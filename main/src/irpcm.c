

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

// ************* USER SETTINGS *************

#define SIMULATION_MODE          0     // 0 = real rc522, 1 = fake scans for testing
#define SIM_USE_MATRIX           0     // only for simulation: 1 = also use the led matrix

#define NUM_TRAINS               4     // only 4 known trains now
#define NUM_PLATFORMS            8     // P1-P4 known trains, P5-P8 unknown trains
#define FIRST_UNKNOWN_PLATFORM   5
#define LAST_UNKNOWN_PLATFORM    8
#define MAX_UNKNOWN_WAITING      8     // unknown trains that can wait
#define MAX_SEEN_UIDS            8     // remember recently scanned unknown uids
#define DWELL_TIME_SECONDS       60
#define SUMMARY_INTERVAL_TICKS   5     // full status table every 5 timer pulses
#define RFID_POLL_MS             100
#define DUPLICATE_MSG_GAP        3     // seconds between duplicate messages
#define REARRIVAL_GAP            3     // card must be removed this long before next arrival

#define MATRIX_ROTATED           0     // make it 1 if rows show up as columns

// QNX service name -> /dev/name/local/irpcm
#define SERVER_NAME       "irpcm"

// ---------------- QNX thread priorities ----------------
#define PRIO_RFID                30
#define PRIO_SERVER              25
#define PRIO_TIMER               20
#define PRIO_DISPLAY             10

// ---------------- QNX pulse priorities ----------------
#define TIMER_PULSE_PRIO         20
#define STATUS_PULSE_PRIO        10
#define SHUTDOWN_PULSE_PRIO      20

// uid of the 4 known trains. train N always uses platform N.
// any other tag = unknown train, uses platform 5-8
char *train_uids[NUM_TRAINS] = {
    "62:89:36:07",   // Train 1
    "50:43:D2:55",   // Train 2
    "43:63:9C:C9",   // Train 3
    "09:2E:FE:06"    // Train 4
};

// ************* BCM2711 ADDRESSES (Raspberry Pi 4, low-peripheral mode) *************
// If the program crashes right after "Reading SPI0 CS register",
// your BSP maps peripherals at a different address. Check the
// BSP build file (hw.raspberrypi-bcm2711-rpi4 project) for the
// addresses used by the UART/GPIO drivers and correct this value.
#define PERIPHERAL_BASE   0xFE000000UL
#define GPIO_BASE         (PERIPHERAL_BASE + 0x200000UL)
#define SPI0_BASE         (PERIPHERAL_BASE + 0x204000UL)
#define MAP_BLOCK_SIZE    0x1000

#define GPFSEL0   0
#define GPSET0    7
#define GPCLR0    10

#define GPIO_FUNC_OUTPUT  1
#define GPIO_FUNC_ALT0    4

#define PIN_SPI0_CE1      7
#define PIN_SPI0_CE0      8
#define PIN_SPI0_MISO     9
#define PIN_SPI0_MOSI     10
#define PIN_SPI0_SCLK     11
#define PIN_RC522_RST     25

#define SPI_CS    0
#define SPI_FIFO  1
#define SPI_CLK   2

#define SPI_CS_CLEAR_TX   (1u << 4)
#define SPI_CS_CLEAR_RX   (1u << 5)
#define SPI_CS_TA         (1u << 7)
#define SPI_CS_DONE       (1u << 16)
#define SPI_CS_RXD        (1u << 17)
#define SPI_CS_TXD        (1u << 18)

#define SPI_CLOCK_DIVIDER 256          // about 2 MHz
#define SPI_TIMEOUT_LOOPS 1000000L

#define RC522_CS          0            // CE0
#define MAX7219_CS        1            // CE1

// ************* MFRC522 REGISTERS *************
#define CommandReg      0x01
#define ComIrqReg       0x04
#define ErrorReg        0x06
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define BitFramingReg   0x0D
#define CollReg         0x0E
#define ModeReg         0x11
#define TxModeReg       0x12
#define RxModeReg       0x13
#define TxControlReg    0x14
#define TxASKReg        0x15
#define ModWidthReg     0x24
#define TModeReg        0x2A
#define TPrescalerReg   0x2B
#define TReloadRegH     0x2C
#define TReloadRegL     0x2D
#define VersionReg      0x37

#define PCD_IDLE        0x00
#define PCD_TRANSCEIVE  0x0C
#define PCD_SOFTRESET   0x0F

#define PICC_REQA       0x26
#define PICC_ANTICOLL1  0x93

#define RC_OK           0
#define RC_TIMEOUT      1
#define RC_ERROR        2

// ************* MAX7219 REGISTERS *************
#define MAX_DECODE_MODE   0x09
#define MAX_INTENSITY     0x0A
#define MAX_SCAN_LIMIT    0x0B
#define MAX_SHUTDOWN      0x0C
#define MAX_DISPLAY_TEST  0x0F

// ************* QNX IPC : MESSAGE TYPES, PULSE CODES, MESSAGE STRUCTURES *************

// message types for MsgSend (kept below 0x100 so they dont clash with qnx system msgs)
enum {
    MSG_RFID_DETECTED = 1,
    MSG_PLATFORM_REQUEST,
    MSG_PLATFORM_ALLOCATED,
    MSG_PLATFORM_RELEASE,
    MSG_SYSTEM_STATUS,
    MSG_SHUTDOWN
};

// Reply status codes returned with MsgReply()
enum {
    RPL_OK = 0,
    RPL_ALLOCATED,
    RPL_WAITING,
    RPL_DUPLICATE,
    RPL_UNKNOWN,
    RPL_RELEASED,
    RPL_IGNORED,
    RPL_NOTHING
};

// QNX pulse codes (user codes start at _PULSE_CODE_MINAVAIL)
#define PULSE_TIMER_TICK      (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_STATUS_REQUEST  (_PULSE_CODE_MINAVAIL + 2)
#define PULSE_SHUTDOWN        (_PULSE_CODE_MINAVAIL + 3)

// message sent to the server.
// type must be the first 16 bit field because name_open() sends _IO_CONNECT there
typedef struct {
    uint16_t type;
    uint16_t subtype;
    int      train_number;
    char     uid[32];
    int      platform;
    time_t   event_time;
} Message;

// Reply sent back by the server
typedef struct {
    int  status;
    int  platform;
    int  waiting_trains;
    char text[128];
} Reply;

// Receive buffer: a message, a pulse, or a QNX system message
typedef union {
    uint16_t      type;
    struct _pulse pulse;
    Message  msg;
} RecvBuf;

// ************* RAILWAY DATA STRUCTURES *************
// owner_type of a platform
#define OWNER_FREE      0
#define OWNER_KNOWN     1
#define OWNER_UNKNOWN   2

typedef struct {
    int    number;
    int    occupied;
    int    owner_type;       // OWNER_FREE / OWNER_KNOWN / OWNER_UNKNOWN
    int    train_index;      // index in trains[] (known train), -1 otherwise
    int    train_number;     // 1-4 for known train, 0 otherwise
    char   unknown_uid[32];  // uid of the unknown train
    time_t arrival_time;
    time_t departure_time;
} Platform;

typedef struct {
    int    number;
    char   uid[16];
    int    priority;        // railway priority (not QNX thread priority)
    int    platform;
    int    arrived;
    int    waiting;
    int    active;
    time_t arrival_time;
    time_t departure_time;
    time_t last_scan_time;
    time_t last_msg_time;
} Train;

// ************* GLOBAL VARIABLES *************
Platform platforms[NUM_PLATFORMS];
Train    trains[NUM_TRAINS];
// unknown trains waiting for platform 5-8
char     wait_uid[MAX_UNKNOWN_WAITING][32];
int      wait_cnt = 0;

// recently scanned unknown uids (so a card lying on the reader
// is not taken as a new train again and again)
char     seen_uid[MAX_SEEN_UIDS][32];
time_t   seen_time[MAX_SEEN_UIDS];

// QNX SYNCHRONIZATION
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;   // platforms, trains, queue
pthread_cond_t  disp_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t spi_lock   = PTHREAD_MUTEX_INITIALIZER;   // SPI0 shared by RC522 + MAX7219
int show_flag      = 0;   // protected by data_lock
int show_full = 0;

// hardware
volatile uint32_t *gpio = NULL;
volatile uint32_t *spi  = NULL;
int hw_ok = 0;
int led_ok  = 0;
int rfid_ok     = 0;
uint8_t led_rows[8];

// startup status flags (set by threads, read by main)
volatile int server_ok = 0;    // 0 = starting, 1 = ok, -1 = failed
volatile int timer_ok  = 0;
volatile int started = 0;
volatile sig_atomic_t running = 1;

int timer_coid = -1;               // connection to the timer thread's channel
int prio_ok = 1;

// ************* SMALL HELPERS *************
void delay_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void get_time(time_t t, char *buf, size_t size)
{
    struct tm tm_val;
    localtime_r(&t, &tm_val);
    strftime(buf, size, "%H:%M:%S", &tm_val);
}

void stop_program(int sig)
{
    running = 0;
}

void line(void)
{
    printf("--------------------------------------------\n");
}

void dline(void)
{
    printf("============================================\n");
}

char *policy_str(int policy)
{
    if (policy == SCHED_FIFO) return "SCHED_FIFO";
    if (policy == SCHED_RR)   return "SCHED_RR";
    return "OTHER";
}

// ************* HARDWARE LAYER : GPIO + SPI0 (BCM2711 registers via QNX mmap) *************
void gpio_set_function(int pin, int func)
{
    int reg   = pin / 10;
    int shift = (pin % 10) * 3;
    uint32_t value;

    if (gpio == NULL)
        return;

    value  = gpio[GPFSEL0 + reg];
    value &= ~(7u << shift);
    value |= ((uint32_t)func << shift);
    gpio[GPFSEL0 + reg] = value;
}

void gpio_write(int pin, int level)
{
    if (gpio == NULL)
        return;
    if (level)
        gpio[GPSET0] = (1u << pin);
    else
        gpio[GPCLR0] = (1u << pin);
}

int spi_wait_flag(uint32_t flag)
{
    long count = SPI_TIMEOUT_LOOPS;
    while ((spi[SPI_CS] & flag) == 0) {
        count--;
        if (count == 0)
            return -1;
    }
    return 0;
}

// send/receive len bytes on spi0 (mode 0).
// spi_lock is needed because rfid thread and server thread both use spi0
int spi_transfer(int chip_select, uint8_t *tx, uint8_t *rx, int len)
{
    int i, result = 0;
    uint8_t byte;
    uint32_t cs_bits = (uint32_t)(chip_select & 0x03);

    if (!hw_ok || spi == NULL)
        return -1;

    pthread_mutex_lock(&spi_lock);
    __sync_synchronize();

    spi[SPI_CS] = cs_bits | SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX;
    spi[SPI_CS] = cs_bits | SPI_CS_TA;

    for (i = 0; i < len; i++) {
        if (spi_wait_flag(SPI_CS_TXD) != 0) { result = -1; break; }
        spi[SPI_FIFO] = tx[i];
        if (spi_wait_flag(SPI_CS_RXD) != 0) { result = -1; break; }
        byte = (uint8_t)(spi[SPI_FIFO] & 0xFF);
        if (rx != NULL)
            rx[i] = byte;
    }

    if (result == 0 && spi_wait_flag(SPI_CS_DONE) != 0)
        result = -1;

    spi[SPI_CS] = cs_bits;      // TA = 0, chip select released
    __sync_synchronize();
    pthread_mutex_unlock(&spi_lock);

    if (result != 0)
        printf("SPI ERROR: transfer timeout on CE%d\n", chip_select);
    return result;
}

int hw_init(void)
{
#if defined(__aarch64__)
    void *ptr;

    printf("HW: mapping GPIO registers at 0x%lX\n", (unsigned long)GPIO_BASE);
    ptr = mmap_device_memory(NULL, MAP_BLOCK_SIZE,
                             PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, GPIO_BASE);
    if (ptr == MAP_FAILED) {
        printf("HW ERROR: mmap_device_memory(GPIO) failed: %s\n", strerror(errno));
        printf("          Run the program as root.\n");
        return -1;
    }
    gpio = (volatile uint32_t *)ptr;

    printf("HW: mapping SPI0 registers at 0x%lX\n", (unsigned long)SPI0_BASE);
    ptr = mmap_device_memory(NULL, MAP_BLOCK_SIZE,
                             PROT_READ | PROT_WRITE | PROT_NOCACHE, 0, SPI0_BASE);
    if (ptr == MAP_FAILED) {
        printf("HW ERROR: mmap_device_memory(SPI0) failed: %s\n", strerror(errno));
        munmap_device_memory((void *)gpio, MAP_BLOCK_SIZE);
        gpio = NULL;
        return -1;
    }
    spi = (volatile uint32_t *)ptr;

    // first register read. if it crashes here the base address is wrong
    printf("HW: Reading SPI0 CS register...\n");
    fflush(stdout);
    printf("HW: SPI0 CS register = 0x%08X\n", (unsigned int)spi[SPI_CS]);

    gpio_set_function(PIN_SPI0_CE1,  GPIO_FUNC_ALT0);
    gpio_set_function(PIN_SPI0_CE0,  GPIO_FUNC_ALT0);
    gpio_set_function(PIN_SPI0_MISO, GPIO_FUNC_ALT0);
    gpio_set_function(PIN_SPI0_MOSI, GPIO_FUNC_ALT0);
    gpio_set_function(PIN_SPI0_SCLK, GPIO_FUNC_ALT0);

    gpio_set_function(PIN_RC522_RST, GPIO_FUNC_OUTPUT);
    gpio_write(PIN_RC522_RST, 1);

    spi[SPI_CS]  = SPI_CS_CLEAR_TX | SPI_CS_CLEAR_RX;
    spi[SPI_CLK] = SPI_CLOCK_DIVIDER;

    hw_ok = 1;
    printf("HW: GPIO and SPI0 ready.\n");
    return 0;
#else
    printf("HW ERROR: hardware access is only built for aarch64 (Raspberry Pi 4).\n");
    return -1;
#endif
}

void hw_close(void)
{
    if (spi != NULL) {
        munmap_device_memory((void *)spi, MAP_BLOCK_SIZE);
        spi = NULL;
    }
    if (gpio != NULL) {
        munmap_device_memory((void *)gpio, MAP_BLOCK_SIZE);
        gpio = NULL;
    }
    hw_ok = 0;
}

// ************* MAX7219 DRIVER *************
void max7219_send(uint8_t reg, uint8_t data)
{
    uint8_t tx[2];
    tx[0] = reg;
    tx[1] = data;
    spi_transfer(MAX7219_CS, tx, NULL, 2);
}

int max7219_init(void)
{
    int i;

    if (!hw_ok)
        return -1;

    max7219_send(MAX_DISPLAY_TEST, 0x00);
    max7219_send(MAX_SHUTDOWN,     0x01);
    max7219_send(MAX_SCAN_LIMIT,   0x07);
    max7219_send(MAX_DECODE_MODE,  0x00);
    max7219_send(MAX_INTENSITY,    0x03);

    for (i = 0; i < 8; i++) {
        led_rows[i] = 0x00;
        max7219_send((uint8_t)(i + 1), 0x00);
    }

    printf("MAX7219: init commands sent on CE1 (write-only chip, no readback).\n");
    led_ok = 1;
    return 0;
}

// turns one platform row on/off. only this function knows the matrix direction
void set_platform_row(int platform, int on)
{
    if (platform < 1 || platform > 8)
        return;

#if MATRIX_ROTATED == 0
    led_rows[platform - 1] = on ? 0xFF : 0x00;
    if (led_ok)
        max7219_send((uint8_t)platform, led_rows[platform - 1]);
#else
    {
        int i;
        uint8_t mask = (uint8_t)(0x80 >> (platform - 1));
        for (i = 0; i < 8; i++) {
            if (on)
                led_rows[i] |= mask;
            else
                led_rows[i] &= (uint8_t)~mask;
            if (led_ok)
                max7219_send((uint8_t)(i + 1), led_rows[i]);
        }
    }
#endif

    printf("MAX7219        : ROW %d %s%s\n", platform, on ? "ON" : "OFF",
           led_ok ? "" : "  (terminal only)");
}

void clear_matrix(void)
{
    int i;
    for (i = 0; i < 8; i++) {
        led_rows[i] = 0x00;
        if (led_ok)
            max7219_send((uint8_t)(i + 1), 0x00);
    }
}

// send the MAX7219 setup registers again and rewrite all 8 rows from led_rows[].
// if noise changes the scan-limit register, the chip only shows rows 1-4,
// so this makes sure rows 5-8 are always displayed.
void max7219_refresh(void)
{
    int i;

    if (!led_ok)
        return;

    max7219_send(MAX_DISPLAY_TEST, 0x00);
    max7219_send(MAX_SHUTDOWN,     0x01);
    max7219_send(MAX_SCAN_LIMIT,   0x07);   // all 8 rows
    max7219_send(MAX_DECODE_MODE,  0x00);
    max7219_send(MAX_INTENSITY,    0x03);

    for (i = 0; i < 8; i++)
        max7219_send((uint8_t)(i + 1), led_rows[i]);
}

// ************* RFID DRIVER (MFRC522) *************
void rc522_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2];
    tx[0] = (uint8_t)((reg << 1) & 0x7E);
    tx[1] = value;
    spi_transfer(RC522_CS, tx, NULL, 2);
}

uint8_t rc522_read_reg(uint8_t reg)
{
    uint8_t tx[2];
    uint8_t rx[2] = {0, 0};
    tx[0] = (uint8_t)(((reg << 1) & 0x7E) | 0x80);
    tx[1] = 0x00;
    spi_transfer(RC522_CS, tx, rx, 2);
    return rx[1];
}

void rc522_set_bits(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, (uint8_t)(rc522_read_reg(reg) | mask));
}

void rc522_clear_bits(uint8_t reg, uint8_t mask)
{
    rc522_write_reg(reg, (uint8_t)(rc522_read_reg(reg) & (uint8_t)~mask));
}

int rc522_init(void)
{
    uint8_t version;
    int count;

    if (!hw_ok)
        return -1;

    gpio_write(PIN_RC522_RST, 0);
    delay_ms(10);
    gpio_write(PIN_RC522_RST, 1);
    delay_ms(50);

    rc522_write_reg(CommandReg, PCD_SOFTRESET);
    delay_ms(50);
    count = 0;
    while ((rc522_read_reg(CommandReg) & 0x10) && count < 10) {
        delay_ms(10);
        count++;
    }

    version = rc522_read_reg(VersionReg);
    printf("RFID: RC522 VersionReg = 0x%02X\n", version);
    if (version == 0x00 || version == 0xFF) {
        printf("RFID: 0x00/0xFF means no answer from the RC522 on CE0.\n");
        printf("      Check 3.3V, GND, MISO (pin 21), CE0 (pin 24), RST (pin 22).\n");
        return -1;
    }
    if (version != 0x91 && version != 0x92)
        printf("RFID: unusual version (clone chip?) - continuing.\n");

    rc522_write_reg(TxModeReg, 0x00);
    rc522_write_reg(RxModeReg, 0x00);
    rc522_write_reg(ModWidthReg, 0x26);
    rc522_write_reg(TModeReg, 0x80);
    rc522_write_reg(TPrescalerReg, 0xA9);
    rc522_write_reg(TReloadRegH, 0x03);
    rc522_write_reg(TReloadRegL, 0xE8);
    rc522_write_reg(TxASKReg, 0x40);
    rc522_write_reg(ModeReg, 0x3D);

    if ((rc522_read_reg(TxControlReg) & 0x03) != 0x03)
        rc522_set_bits(TxControlReg, 0x03);

    printf("RFID: RC522 initialised, antenna ON.\n");
    return 0;
}

int rc522_transceive(uint8_t *send, int send_len,
                            uint8_t *back, int *back_len, uint8_t bit_framing)
{
    int i, n, loops;
    uint8_t irq;

    rc522_write_reg(CommandReg, PCD_IDLE);
    rc522_write_reg(ComIrqReg, 0x7F);
    rc522_write_reg(FIFOLevelReg, 0x80);

    for (i = 0; i < send_len; i++)
        rc522_write_reg(FIFODataReg, send[i]);

    rc522_write_reg(BitFramingReg, bit_framing);
    rc522_write_reg(CommandReg, PCD_TRANSCEIVE);
    rc522_set_bits(BitFramingReg, 0x80);

    loops = 0;
    while (1) {
        irq = rc522_read_reg(ComIrqReg);
        if (irq & 0x30)
            break;
        if ((irq & 0x01) || ++loops > 2000) {
            rc522_clear_bits(BitFramingReg, 0x80);
            return RC_TIMEOUT;
        }
    }

    rc522_clear_bits(BitFramingReg, 0x80);

    if (rc522_read_reg(ErrorReg) & 0x1B)
        return RC_ERROR;

    n = rc522_read_reg(FIFOLevelReg);
    if (n > *back_len)
        n = *back_len;
    for (i = 0; i < n; i++)
        back[i] = rc522_read_reg(FIFODataReg);
    *back_len = n;
    return RC_OK;
}

// returns 1 = UID read, 0 = no card, -1 = read error
int rc522_read_uid(uint8_t uid[4])
{
    uint8_t cmd[2];
    uint8_t buf[10];
    int len;

    rc522_clear_bits(CollReg, 0x80);

    cmd[0] = PICC_REQA;
    len = sizeof(buf);
    if (rc522_transceive(cmd, 1, buf, &len, 0x07) != RC_OK || len != 2)
        return 0;

    cmd[0] = PICC_ANTICOLL1;
    cmd[1] = 0x20;
    len = sizeof(buf);
    if (rc522_transceive(cmd, 2, buf, &len, 0x00) != RC_OK || len != 5)
        return -1;

    if (buf[0] == 0x88)            // 7-byte UID tag, not supported
        return -1;
    if ((uint8_t)(buf[0] ^ buf[1] ^ buf[2] ^ buf[3]) != buf[4])
        return -1;

    memcpy(uid, buf, 4);
    return 1;
}

// ************* IRPCM LOGIC (called only by the IRPCM server thread, *************
// always with data_lock locked)
void init_data(void)
{
    int i;

    for (i = 0; i < NUM_TRAINS; i++) {
        memset(&trains[i], 0, sizeof(Train));
        trains[i].number   = i + 1;
        trains[i].priority = 1;
        strcpy(trains[i].uid, train_uids[i]);
    }
    for (i = 0; i < NUM_PLATFORMS; i++) {
        memset(&platforms[i], 0, sizeof(Platform));
        platforms[i].number      = i + 1;
        platforms[i].owner_type  = OWNER_FREE;
        platforms[i].train_index = -1;
    }
    for (i = 0; i < MAX_SEEN_UIDS; i++) {
        seen_uid[i][0] = '\0';
        seen_time[i] = 0;
    }
    wait_cnt = 0;
}

void print_uids(void)
{
    int i;
    printf("RFID UID TABLE:\n");
    for (i = 0; i < NUM_TRAINS; i++)
        printf("  Train %d : %s  -> Platform %d\n", trains[i].number, trains[i].uid, i + 1);
    printf("  any other tag : UNKNOWN TRAIN -> Platform 5-8\n");
}

int find_train_by_uid(char *uid)
{
    int i;
    for (i = 0; i < NUM_TRAINS; i++)
        if (strcmp(trains[i].uid, uid) == 0)
            return i;
    return -1;
}

// first free platform among 5-8 (returns index), -1 if all busy
int find_free_unknown_platform(void)
{
    int i;
    for (i = FIRST_UNKNOWN_PLATFORM - 1; i <= LAST_UNKNOWN_PLATFORM - 1; i++)
        if (!platforms[i].occupied)
            return i;
    return -1;
}

// platform index where this unknown uid is parked, -1 if not inside
int find_unknown_inside(char *uid)
{
    int i;
    for (i = FIRST_UNKNOWN_PLATFORM - 1; i <= LAST_UNKNOWN_PLATFORM - 1; i++)
        if (platforms[i].owner_type == OWNER_UNKNOWN && strcmp(platforms[i].unknown_uid, uid) == 0)
            return i;
    return -1;
}

int is_unknown_waiting(char *uid)
{
    int i;
    for (i = 0; i < wait_cnt; i++)
        if (strcmp(wait_uid[i], uid) == 0)
            return 1;
    return 0;
}

// remember when an unknown uid was last scanned.
// returns the previous scan time (0 = never seen before)
time_t update_seen(char *uid, time_t now)
{
    int i, old = 0;
    time_t prev;

    for (i = 0; i < MAX_SEEN_UIDS; i++) {
        if (seen_uid[i][0] != '\0' && strcmp(seen_uid[i], uid) == 0) {
            prev = seen_time[i];
            seen_time[i] = now;
            return prev;
        }
    }
    // not in the list: use an empty slot or the oldest one
    for (i = 0; i < MAX_SEEN_UIDS; i++) {
        if (seen_uid[i][0] == '\0') {
            old = i;
            break;
        }
        if (seen_time[i] < seen_time[old])
            old = i;
    }
    strncpy(seen_uid[old], uid, 31);
    seen_uid[old][31] = '\0';
    seen_time[old] = now;
    return 0;
}

// QNX SYNCHRONIZATION: wake the display thread (data_lock held)
void wake_display(int full)
{
    show_flag = 1;
    if (full)
        show_full = 1;
    pthread_cond_signal(&disp_cond);
}

void wake_display_lock(int full)
{
    pthread_mutex_lock(&data_lock);
    wake_display(full);
    pthread_mutex_unlock(&data_lock);
}

// put a train on platform index p and start the 60 sec dwell.
// owner = OWNER_KNOWN (t = train index) or OWNER_UNKNOWN (uid used)
int assign_platform(int p, int owner, int t, char *uid)
{
    time_t now = time(NULL);
    char now_s[16], leave[16];

    platforms[p].occupied       = 1;
    platforms[p].owner_type     = owner;
    platforms[p].arrival_time   = now;
    platforms[p].departure_time = now + DWELL_TIME_SECONDS;

    if (owner == OWNER_KNOWN) {
        platforms[p].train_index  = t;
        platforms[p].train_number = trains[t].number;
        platforms[p].unknown_uid[0] = '\0';

        trains[t].platform       = platforms[p].number;
        trains[t].active         = 1;
        trains[t].arrived        = 1;
        trains[t].arrival_time   = now;
        trains[t].departure_time = platforms[p].departure_time;
    } else {
        platforms[p].train_index  = -1;
        platforms[p].train_number = 0;
        strncpy(platforms[p].unknown_uid, uid, 31);
        platforms[p].unknown_uid[31] = '\0';
    }

    get_time(now, now_s, sizeof(now_s));
    get_time(platforms[p].departure_time, leave, sizeof(leave));

    printf("ALLOCATED PLATFORM : %d\n", platforms[p].number);
    printf("ARRIVAL TIME  : %s\n", now_s);
    printf("DWELL TIME    : 1 MINUTE\n");
    printf("WILL LEAVE BY : %s\n", leave);

    return platforms[p].number;
}

// known train scanned (t = 0..3). returns platform row to turn ON (0 = none)
int process_known(int t, Message *msg, Reply *reply)
{
    time_t now = msg->event_time;
    char now_s[16];
    int p = trains[t].number - 1;     // train N -> platform N

    // same train scanned again while inside
    if (trains[t].active) {
        if (now - trains[t].last_msg_time >= DUPLICATE_MSG_GAP) {
            printf("TRAIN %d ALREADY INSIDE STATION (Platform %d)\n",
                   trains[t].number, trains[t].platform);
            trains[t].last_msg_time = now;
        }
        trains[t].last_scan_time = now;
        reply->status   = RPL_DUPLICATE;
        reply->platform = trains[t].platform;
        snprintf(reply->text, sizeof(reply->text), "train %d duplicate scan", trains[t].number);
        return 0;
    }

    // card still lying on the reader after the train left
    if (trains[t].last_scan_time != 0 && now - trains[t].last_scan_time < REARRIVAL_GAP) {
        if (now - trains[t].last_msg_time >= DUPLICATE_MSG_GAP) {
            printf("Train %d card still near reader - remove it for a new arrival.\n",
                   trains[t].number);
            trains[t].last_msg_time = now;
        }
        trains[t].last_scan_time = now;
        reply->status = RPL_IGNORED;
        snprintf(reply->text, sizeof(reply->text), "train %d card not removed", trains[t].number);
        return 0;
    }

    trains[t].last_scan_time = now;
    trains[t].last_msg_time  = now;
    get_time(now, now_s, sizeof(now_s));

    printf("\n");
    dline();
    printf("TRAIN %d ENTERS STATION\n", trains[t].number);
    dline();
    printf("UID            : %s\n", msg->uid);
    printf("QNX EVENT      : MSG_RFID_DETECTED (MsgReceive)\n");
    printf("CURRENT TIME   : %s\n", now_s);

    // known train uses only its own platform
    if (platforms[p].occupied) {
        printf("PLATFORM %d IS OCCUPIED - no platform allocated\n", platforms[p].number);
        dline();
        reply->status = RPL_NOTHING;
        snprintf(reply->text, sizeof(reply->text), "platform %d occupied", platforms[p].number);
        return 0;
    }

    printf("PLATFORM %d     : FREE\n", platforms[p].number);
    reply->platform = assign_platform(p, OWNER_KNOWN, t, NULL);
    reply->status   = RPL_ALLOCATED;
    snprintf(reply->text, sizeof(reply->text), "train %d -> platform %d",
             trains[t].number, reply->platform);
    return reply->platform;
}

// unknown uid scanned. returns platform row to turn ON (0 = none)
int process_unknown(Message *msg, Reply *reply)
{
    static char   last_dup_uid[32] = "";
    static time_t last_dup_time = 0;
    time_t now = msg->event_time;
    time_t prev;
    int i, p;

    prev = update_seen(msg->uid, now);

    // same unknown train already on platform 5-8
    p = find_unknown_inside(msg->uid);
    if (p >= 0 || is_unknown_waiting(msg->uid)) {
        if (strcmp(last_dup_uid, msg->uid) != 0 || now - last_dup_time >= DUPLICATE_MSG_GAP) {
            if (p >= 0) {
                printf("UNKNOWN TRAIN ALREADY INSIDE\n");
                printf("UID : %s\n", msg->uid);
                printf("PLATFORM : %d\n", platforms[p].number);
            } else {
                printf("UNKNOWN TRAIN ALREADY WAITING\n");
                printf("UID : %s\n", msg->uid);
            }
            strcpy(last_dup_uid, msg->uid);
            last_dup_time = now;
        }
        reply->status = RPL_DUPLICATE;
        snprintf(reply->text, sizeof(reply->text), "unknown %s duplicate scan", msg->uid);
        return 0;
    }

    // card still lying on the reader after this unknown train left
    if (prev != 0 && now - prev < REARRIVAL_GAP) {
        reply->status = RPL_IGNORED;
        snprintf(reply->text, sizeof(reply->text), "unknown card not removed");
        return 0;
    }

    p = find_free_unknown_platform();

    // all of platform 5-8 busy -> wait
    if (p < 0) {
        printf("\n");
        dline();
        printf("UNKNOWN TRAIN WAITING\n");
        dline();
        printf("UID : %s\n", msg->uid);
        printf("PLATFORMS 5-8 ARE OCCUPIED\n");
        printf("TRAIN WAITING FOR PLATFORM\n");
        dline();
        if (wait_cnt < MAX_UNKNOWN_WAITING) {
            strncpy(wait_uid[wait_cnt], msg->uid, 31);
            wait_uid[wait_cnt][31] = '\0';
            wait_cnt++;
        }
        reply->status = RPL_WAITING;
        reply->waiting_trains = wait_cnt;
        snprintf(reply->text, sizeof(reply->text), "unknown %s waiting", msg->uid);
        wake_display(0);
        return 0;
    }

    printf("\n");
    dline();
    printf("UNKNOWN TRAIN ENTERS STATION\n");
    dline();
    printf("UID : %s\n", msg->uid);
    printf("QNX EVENT : MSG_RFID_DETECTED (MsgReceive)\n\n");
    for (i = 1; i <= 4; i++)
        printf("Platform %d : reserved for known trains\n", i);
    printf("\nChecking unknown-train platforms...\n\n");
    for (i = FIRST_UNKNOWN_PLATFORM - 1; i < p; i++)
        printf("Platform %d : OCCUPIED\n", platforms[i].number);
    printf("Platform %d : FREE\n\n", platforms[p].number);
    printf("ALLOCATING PLATFORM %d\n\n", platforms[p].number);

    reply->platform = assign_platform(p, OWNER_UNKNOWN, -1, msg->uid);
    reply->status   = RPL_ALLOCATED;
    snprintf(reply->text, sizeof(reply->text), "unknown %s -> platform %d",
             msg->uid, reply->platform);
    return reply->platform;
}

// MSG_RFID_DETECTED from the rfid thread.
// returns platform row to switch ON (0 = none)
int process_scan(Message *msg, Reply *reply)
{
    int t = find_train_by_uid(msg->uid);

    if (t >= 0)
        return process_known(t, msg, reply);     // train 1-4 -> platform 1-4

    return process_unknown(msg, reply);          // any other uid -> platform 5-8
}

// MSG_PLATFORM_RELEASE: dwell time over. returns row to switch OFF (0 = none)
int release_platform(Message *msg, Reply *reply)
{
    time_t now = time(NULL);
    char now_s[16];
    int p = msg->platform - 1;
    int t;

    if (p < 0 || p >= NUM_PLATFORMS || !platforms[p].occupied ||
        now < platforms[p].departure_time) {
        reply->status = RPL_NOTHING;
        snprintf(reply->text, sizeof(reply->text), "platform %d not due", msg->platform);
        return 0;
    }

    get_time(now, now_s, sizeof(now_s));
    printf("TIME : %s\n", now_s);

    if (platforms[p].owner_type == OWNER_KNOWN) {
        t = platforms[p].train_index;
        printf("TRAIN %d LEFT PLATFORM %d\n", platforms[p].train_number, platforms[p].number);
        if (t >= 0 && t < NUM_TRAINS) {
            trains[t].active         = 0;
            trains[t].arrived        = 0;
            trains[t].platform       = 0;
            trains[t].departure_time = now;
        }
    } else if (platforms[p].owner_type == OWNER_UNKNOWN) {
        // no trains[] entry for unknown trains, just print the uid
        printf("TRAIN WITH UID %s LEFT PLATFORM %d\n", platforms[p].unknown_uid, platforms[p].number);
    }
    printf("PLATFORM %d IS NOW FREE\n", platforms[p].number);

    platforms[p].occupied       = 0;
    platforms[p].owner_type     = OWNER_FREE;
    platforms[p].train_index    = -1;
    platforms[p].train_number   = 0;
    platforms[p].unknown_uid[0] = '\0';
    platforms[p].arrival_time   = 0;
    platforms[p].departure_time = 0;

    reply->status         = RPL_RELEASED;
    reply->platform       = msg->platform;
    reply->waiting_trains = wait_cnt;
    snprintf(reply->text, sizeof(reply->text), "platform %d released", msg->platform);
    return msg->platform;
}

// MSG_PLATFORM_REQUEST: give a free platform 5-8 to the first waiting unknown train
int assign_waiting(Reply *reply)
{
    char uid[32];
    int i, p;

    p = find_free_unknown_platform();
    if (wait_cnt == 0 || p < 0) {
        reply->status = RPL_NOTHING;
        snprintf(reply->text, sizeof(reply->text), "nothing to allocate");
        return 0;
    }

    strcpy(uid, wait_uid[0]);
    for (i = 1; i < wait_cnt; i++)
        strcpy(wait_uid[i - 1], wait_uid[i]);
    wait_cnt--;

    printf("\n");
    dline();
    printf("WAITING UNKNOWN TRAIN GETS PLATFORM %d\n", platforms[p].number);
    dline();
    printf("UID : %s\n", uid);
    printf("QNX EVENT : MSG_PLATFORM_REQUEST (MsgReceive)\n");

    reply->platform = assign_platform(p, OWNER_UNKNOWN, -1, uid);
    reply->status   = RPL_ALLOCATED;
    snprintf(reply->text, sizeof(reply->text), "waiting unknown %s -> platform %d",
             uid, reply->platform);
    return reply->platform;
}

void handle_status(Reply *reply)
{
    int i, free_count = 0;
    for (i = 0; i < NUM_PLATFORMS; i++)
        if (!platforms[i].occupied)
            free_count++;
    reply->status         = RPL_OK;
    reply->waiting_trains = wait_cnt;
    snprintf(reply->text, sizeof(reply->text), "%d of %d platforms free, %d waiting",
             free_count, NUM_PLATFORMS, wait_cnt);
}

// ************* QNX IPC : CLIENT HELPERS *************

// QNX name service: connect to /dev/name/local/irpcm
int connect_server(char *who)
{
    int coid = name_open(SERVER_NAME, 0);
    if (coid == -1)
        printf("%s: name_open(\"%s\") failed: %s\n", who, SERVER_NAME, strerror(errno));
    return coid;
}

// QNX message passing: blocking MsgSend() to the server
int send_msg(int coid, int type, char *uid, int platform, Reply *reply)
{
    Message msg;

    memset(&msg, 0, sizeof(msg));
    memset(reply, 0, sizeof(*reply));
    msg.type       = (uint16_t)type;
    msg.subtype    = 0;
    msg.platform   = platform;
    msg.event_time = time(NULL);
    if (uid != NULL)
        strncpy(msg.uid, uid, sizeof(msg.uid) - 1);

    if (MsgSend(coid, &msg, sizeof(msg), reply, sizeof(*reply)) == -1) {
        printf("QNX IPC ERROR: MsgSend(type %d) failed: %s\n", type, strerror(errno));
        return -1;
    }
    return 0;
}

// ************* QNX IPC : IRPCM SERVER THREAD *************
// All railway decisions happen here, after MsgReceive().
// QNX priority inheritance: while serving a message, this thread runs
// at the priority of the client that sent it.
void *server_thread(void *arg)
{
    name_attach_t *attach;
    RecvBuf buf;
    Reply reply;
    int rcvid, row_on, row_off, loop = 1;

    attach = name_attach(NULL, SERVER_NAME, 0);
    if (attach == NULL) {
        printf("SERVER: name_attach(\"%s\") failed: %s\n", SERVER_NAME, strerror(errno));
        printf("        (is another IRPCM instance still running?)\n");
        server_ok = -1;
        return NULL;
    }
    server_ok = 1;

    while (loop) {
        rcvid = MsgReceive(attach->chid, &buf, sizeof(buf), NULL);

        if (rcvid == -1) {
            if (errno == EINTR)
                continue;
            printf("SERVER: MsgReceive failed: %s\n", strerror(errno));
            break;
        }

        // ---------------- QNX PULSES ----------------
        if (rcvid == 0) {
            switch (buf.pulse.code) {
            case _PULSE_CODE_DISCONNECT:
                ConnectDetach(buf.pulse.scoid);      // client closed its connection
                break;
            case PULSE_STATUS_REQUEST:
                wake_display_lock(1);                   // ask display thread for full table
                break;
            default:
                break;
            }
            continue;
        }

        // ---------------- QNX system messages ----------------
        if (buf.type == _IO_CONNECT) {               // sent by name_open()
            MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (buf.type > _IO_BASE && buf.type <= _IO_MAX) {
            MsgError(rcvid, ENOSYS);
            continue;
        }

        // ---------------- IRPCM messages ----------------
        memset(&reply, 0, sizeof(reply));
        row_on  = 0;
        row_off = 0;

        pthread_mutex_lock(&data_lock);
        switch (buf.msg.type) {
        case MSG_RFID_DETECTED:
            row_on = process_scan(&buf.msg, &reply);
            break;
        case MSG_PLATFORM_REQUEST:
            row_on = assign_waiting(&reply);
            break;
        case MSG_PLATFORM_RELEASE:
            row_off = release_platform(&buf.msg, &reply);
            break;
        case MSG_SYSTEM_STATUS:
            handle_status(&reply);
            break;
        case MSG_SHUTDOWN:
            reply.status = RPL_OK;
            snprintf(reply.text, sizeof(reply.text), "server stopping");
            loop = 0;
            break;
        default:
            reply.status = RPL_NOTHING;
            snprintf(reply.text, sizeof(reply.text), "unknown message type %d", buf.msg.type);
            break;
        }
        pthread_mutex_unlock(&data_lock);

        // SPI work done OUTSIDE the state mutex
        if (row_on > 0) {
            set_platform_row(row_on, 1);     // known P1-P4 or unknown P5-P8
            max7219_refresh();
            dline();
            wake_display_lock(0);
        }
        if (row_off > 0) {
            set_platform_row(row_off, 0);    // only this row goes off
            max7219_refresh();
            line();
            wake_display_lock(0);
        }

        if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1)
            printf("SERVER: MsgReply failed: %s\n", strerror(errno));
    }

    name_detach(attach, 0);
    return NULL;
}

// called on every QNX timer pulse (1 sec).
// finds platforms whose 60 sec dwell is over and asks the server to free them.
// it never creates a train arrival.
void check_platforms(int server_coid, unsigned long ticks)
{
    Reply reply;
    int expired[NUM_PLATFORMS];
    int n = 0, i;
    time_t now = time(NULL);

    // read state quickly under the mutex
    pthread_mutex_lock(&data_lock);
    for (i = 0; i < NUM_PLATFORMS; i++) {
        if (platforms[i].occupied && now >= platforms[i].departure_time)
            expired[n++] = platforms[i].number;
    }
    pthread_mutex_unlock(&data_lock);

    // send release messages without holding the mutex
    for (i = 0; i < n; i++) {
        printf("\n");
        line();
        printf("QNX TIMER      : PULSE RECEIVED (tick %lu)\n", ticks);
        printf("QNX MESSAGE    : MSG_PLATFORM_RELEASE -> platform %d\n", expired[i]);

        if (send_msg(server_coid, MSG_PLATFORM_RELEASE, NULL, expired[i], &reply) == 0 &&
            reply.status == RPL_RELEASED && reply.waiting_trains > 0) {
            // unknown train waiting: ask the server to give it the freed platform
            send_msg(server_coid, MSG_PLATFORM_REQUEST, NULL, 0, &reply);
        }
    }
}

// ************* QNX TIMER + QNX PULSES : PLATFORM TIMER THREAD *************
// A QNX timer sends a pulse every 1 second. Each pulse makes this
// thread check the 8 platforms and send MSG_PLATFORM_RELEASE for
// every platform whose 60 s dwell has ended.
void *timer_thread(void *arg)
{
    struct _pulse pulse;
    struct sigevent event;
    struct itimerspec itime;
    timer_t timer_id;
    int chid, server_coid, rcvid;
    unsigned long ticks = 0;

    // private channel for timer pulses
    chid = ChannelCreate(0);
    if (chid == -1) {
        printf("TIMER: ChannelCreate failed: %s\n", strerror(errno));
        timer_ok = -1;
        return NULL;
    }

    timer_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (timer_coid == -1) {
        printf("TIMER: ConnectAttach failed: %s\n", strerror(errno));
        ChannelDestroy(chid);
        timer_ok = -1;
        return NULL;
    }

    // timer expiry -> pulse PULSE_TIMER_TICK on our channel
    SIGEV_PULSE_INIT(&event, timer_coid, TIMER_PULSE_PRIO, PULSE_TIMER_TICK, 0);

    if (timer_create(CLOCK_MONOTONIC, &event, &timer_id) == -1) {
        printf("TIMER: timer_create failed: %s\n", strerror(errno));
        ConnectDetach(timer_coid);
        ChannelDestroy(chid);
        timer_ok = -1;
        return NULL;
    }

    itime.it_value.tv_sec     = 1;      // first pulse after 1 s
    itime.it_value.tv_nsec    = 0;
    itime.it_interval.tv_sec  = 1;      // then every 1 s
    itime.it_interval.tv_nsec = 0;

    if (timer_settime(timer_id, 0, &itime, NULL) == -1) {
        printf("TIMER: timer_settime failed: %s\n", strerror(errno));
        timer_delete(timer_id);
        ConnectDetach(timer_coid);
        ChannelDestroy(chid);
        timer_ok = -1;
        return NULL;
    }

    server_coid = connect_server("TIMER");
    if (server_coid == -1) {
        timer_delete(timer_id);
        ConnectDetach(timer_coid);
        ChannelDestroy(chid);
        timer_ok = -1;
        return NULL;
    }

    timer_ok = 1;

    while (1) {
        rcvid = MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) {
            if (errno == EINTR)
                continue;
            printf("TIMER: MsgReceivePulse failed: %s\n", strerror(errno));
            break;
        }

        if (pulse.code == PULSE_SHUTDOWN)
            break;
        if (pulse.code != PULSE_TIMER_TICK)
            continue;

        ticks++;
        check_platforms(server_coid, ticks);     // dwell check, every 1 sec

        // every few seconds: asynchronous status pulse to the server
        if (ticks % SUMMARY_INTERVAL_TICKS == 0) {
            if (MsgSendPulse(server_coid, STATUS_PULSE_PRIO, PULSE_STATUS_REQUEST, 0) == -1)
                printf("TIMER: MsgSendPulse failed: %s\n", strerror(errno));
        }
    }

    timer_delete(timer_id);
    name_close(server_coid);
    ConnectDetach(timer_coid);
    ChannelDestroy(chid);
    return NULL;
}

// ************* RFID THREAD (real RC522 or simulation) *************
// It never touches platform state. It only sends MSG_RFID_DETECTED
// to the IRPCM server with MsgSend().
#if SIMULATION_MODE
typedef struct {
    int at_second;
    int train_index;   // -1 = unknown tag
} SimEvent;

SimEvent sim_events[] = {
    {  2,  0 },   // Train 1 -> platform 1
    { 10,  1 },   // Train 2 -> platform 2
    { 18,  2 },   // Train 3 -> platform 3
    { 26,  3 },   // Train 4 -> platform 4
    { 34, -1 },   // unknown tag -> platform 5
    { 40,  0 },   // Train 1 duplicate scan
    { 45, -1 },   // same unknown tag again (duplicate)
    { 80,  0 },   // Train 1 again after it left
};
#define NUM_SIM_EVENTS ((int)(sizeof(sim_events) / sizeof(sim_events[0])))
#endif

void print_reply(Reply *reply)
{
    if (reply->status == RPL_ALLOCATED || reply->status == RPL_WAITING ||
        reply->status == RPL_UNKNOWN)
        printf("RFID THREAD    : MsgReply received -> %s\n", reply->text);
}

void *rfid_thread(void *arg)
{
    Reply reply;
    int coid;

    coid = connect_server("RFID");
    if (coid == -1)
        return NULL;

    while (running && !started)
        delay_ms(50);

#if SIMULATION_MODE
    {
        time_t start = time(NULL);
        int next = 0;
        char *uid;

        while (running && next < NUM_SIM_EVENTS) {
            if (time(NULL) - start >= sim_events[next].at_second) {
                uid = (sim_events[next].train_index >= 0)
                      ? train_uids[sim_events[next].train_index] : "DE:AD:BE:EF";
                printf("\n[SIMULATION] simulated RFID scan: %s -> MsgSend()\n", uid);
                if (send_msg(coid, MSG_RFID_DETECTED, uid, 0, &reply) == 0)
                    print_reply(&reply);
                next++;
            }
            delay_ms(RFID_POLL_MS);
        }
        if (running)
            printf("\n[SIMULATION] all simulated scans sent. Timer keeps running (Ctrl+C to exit)\n");
    }
#else
    {
        uint8_t uid[4];
        char uid_str[32];
        int result;
        time_t last_error = 0;

        while (running) {
            result = rc522_read_uid(uid);
            if (result == 1) {
                snprintf(uid_str, sizeof(uid_str), "%02X:%02X:%02X:%02X",
                         uid[0], uid[1], uid[2], uid[3]);
                if (send_msg(coid, MSG_RFID_DETECTED, uid_str, 0, &reply) == 0)
                    print_reply(&reply);
            } else if (result < 0 && time(NULL) - last_error >= 2) {
                printf("RFID READ ERROR (hold the tag steady)\n");
                last_error = time(NULL);
            }
            delay_ms(RFID_POLL_MS);
        }
    }
#endif

    name_close(coid);
    return NULL;
}

// ************* DISPLAY THREAD (low priority) *************
// Sleeps on the condition variable until the server or a status
// pulse reports a change. Holds no railway logic.
void print_status(int full)
{
    char buf[16];
    int i;

    get_time(time(NULL), buf, sizeof(buf));

    if (full) {
        printf("\n");
        line();
        printf("PLATFORM STATUS            TIME: %s\n", buf);
        line();
        for (i = 0; i < NUM_PLATFORMS; i++) {
            if (platforms[i].occupied) {
                get_time(platforms[i].departure_time, buf, sizeof(buf));
                if (platforms[i].owner_type == OWNER_KNOWN)
                    printf("Platform %d : OCCUPIED - Train %d - Until %s\n",
                           platforms[i].number, platforms[i].train_number, buf);
                else
                    printf("Platform %d : OCCUPIED - Unknown UID %s - Until %s\n",
                           platforms[i].number, platforms[i].unknown_uid, buf);
            } else {
                printf("Platform %d : FREE\n", platforms[i].number);
            }
        }
        if (wait_cnt > 0) {
            printf("Waiting    :");
            for (i = 0; i < wait_cnt; i++)
                printf(" [unknown %s]", wait_uid[i]);
            printf("\n");
        }
    }

    printf("LED MATRIX (row = platform):\n");
    for (i = 0; i < 8; i++) {
        if (i < NUM_PLATFORMS && platforms[i].occupied)
            printf("  %d  ********\n", i + 1);
        else
            printf("  %d  ........\n", i + 1);
    }
    if (full)
        line();
}

void *display_thread(void *arg)
{
    int full;

    pthread_mutex_lock(&data_lock);
    while (running) {
        while (!show_flag && running)
            pthread_cond_wait(&disp_cond, &data_lock);
        if (!running)
            break;

        full = show_full;
        show_flag = 0;
        show_full = 0;
        print_status(full);
    }
    pthread_mutex_unlock(&data_lock);
    return NULL;
}

// ************* QNX SCHEDULING *************
// Really calls pthread_setschedparam(), then reads back the result.
void set_priority(pthread_t tid, char *name, int policy, int priority)
{
    struct sched_param param;
    int rc, actual_policy;

    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;

    rc = pthread_setschedparam(tid, policy, &param);
    if (rc != EOK) {
        printf("%-16s: pthread_setschedparam(%s, %d) FAILED: %s\n",
               name, policy_str(policy), priority, strerror(rc));
        prio_ok = 0;
    }

    rc = pthread_getschedparam(tid, &actual_policy, &param);
    if (rc == EOK) {
        printf("%-16s: Policy %-10s Priority %d\n", name,
               policy_str(actual_policy), param.sched_priority);
        if (actual_policy != policy || param.sched_priority != priority)
            prio_ok = 0;
    } else {
        printf("%-16s: pthread_getschedparam failed: %s\n", name, strerror(rc));
        prio_ok = 0;
    }
}

// ************* MAIN *************
int main(void)
{
    pthread_t t_server, t_timer, t_disp, t_rfid;
    int made_server = 0, made_timer = 0, made_disp = 0, made_rfid = 0;
    int threads_ok = 1, msg_ok = 0, coid_main = -1;
    char *rfid_state, *led_state;
    sigset_t sigs;
    Reply reply;
    struct sigaction sa;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (NUM_PLATFORMS < 1 || NUM_PLATFORMS > 8) {
        printf("ERROR: NUM_PLATFORMS must be 1..8\n");
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_program;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    init_data();

    dline();
    printf("IRPCM QNX REAL-TIME SYSTEM - STARTING\n");
    printf("BUILD : %s %s\n", __DATE__, __TIME__);   // shows the new build is running
#if SIMULATION_MODE
    printf("MODE  : SIMULATION (fake scans, RC522 not used)\n");
#else
    printf("MODE  : RFID HARDWARE (only real tags create arrivals)\n");
#endif
    dline();
    print_uids();

    // ---------------- hardware ----------------
#if SIMULATION_MODE
    rfid_state = "SIMULATION";
  #if SIM_USE_MATRIX
    if (hw_init() == 0 && max7219_init() == 0)
        led_state = "READY (commands sent)";
    else
        led_state = "SIMULATION (terminal only)";
  #else
    led_state = "SIMULATION (terminal only)";
  #endif
#else
    if (hw_init() != 0) {
        printf("\nRFID HARDWARE INITIALIZATION FAILED (SPI not available)\n");
        return 1;
    }
    led_state = (max7219_init() == 0) ? "READY (commands sent)" : "NOT AVAILABLE";
    if (rc522_init() != 0) {
        printf("\nRFID HARDWARE INITIALIZATION FAILED\n");
        clear_matrix();
        hw_close();
        return 1;
    }
    rfid_ok = 1;
    rfid_state = "READY";
#endif

    // worker threads must not take SIGINT; only main handles it
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    // ---------------- QNX THREADS ----------------
    printf("\nQNX SCHEDULING:\n");

    if (pthread_create(&t_server, NULL, server_thread, NULL) != EOK) {
        printf("ERROR: cannot create server thread\n");
        pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
        clear_matrix();
        hw_close();
        return 1;
    }
    made_server = 1;
    set_priority(t_server, "IRPCM SERVER", SCHED_FIFO, PRIO_SERVER);

    while (server_ok == 0)
        delay_ms(20);
    if (server_ok < 0) {
        pthread_join(t_server, NULL);
        clear_matrix();
        hw_close();
        return 1;
    }

    if (pthread_create(&t_timer, NULL, timer_thread, NULL) == EOK) {
        made_timer = 1;
        set_priority(t_timer, "TIMER THREAD", SCHED_FIFO, PRIO_TIMER);
        while (timer_ok == 0)
            delay_ms(20);
    } else {
        printf("ERROR: cannot create timer thread\n");
        threads_ok = 0;
        timer_ok = -1;
    }

    if (pthread_create(&t_disp, NULL, display_thread, NULL) == EOK) {
        made_disp = 1;
        set_priority(t_disp, "DISPLAY THREAD", SCHED_RR, PRIO_DISPLAY);
    } else {
        printf("ERROR: cannot create display thread\n");
        threads_ok = 0;
    }

    if (pthread_create(&t_rfid, NULL, rfid_thread, NULL) == EOK) {
        made_rfid = 1;
        set_priority(t_rfid, "RFID THREAD", SCHED_FIFO, PRIO_RFID);
    } else {
        printf("ERROR: cannot create RFID thread\n");
        threads_ok = 0;
    }

    // test message to check MsgSend/MsgReceive/MsgReply works
    // (status message only, it does not create any train)
    coid_main = connect_server("MAIN");
    if (coid_main != -1 &&
        send_msg(coid_main, MSG_SYSTEM_STATUS, NULL, 0, &reply) == 0) {
        msg_ok = 1;
        printf("\nMAIN: MSG_SYSTEM_STATUS reply -> %s\n", reply.text);
    }

    // ---------------- status (from real results) ----------------
    printf("\n");
    dline();
    printf("IRPCM QNX REAL-TIME SYSTEM\n");
    dline();
    printf("QNX IPC          : %s\n", (server_ok == 1 && coid_main != -1) ? "ENABLED" : "FAILED");
    printf("MESSAGE PASSING  : %s\n", msg_ok ? "ENABLED" : "FAILED");
    printf("PULSE EVENTS     : %s\n", (timer_ok == 1) ? "ENABLED" : "FAILED");
    printf("QNX TIMER        : %s\n", (timer_ok == 1) ? "ENABLED" : "FAILED");
    printf("THREADS          : %s\n", threads_ok ? "ENABLED" : "PARTIAL");
    printf("PRIORITY SCHED   : %s\n", prio_ok ? "ENABLED" : "NOT FULLY APPLIED (see errors above)");
    printf("MUTEX            : ENABLED\n");
    printf("CONDITION VAR    : ENABLED\n");
    printf("RFID             : %s\n", rfid_state);
    printf("MAX7219          : %s\n", led_state);
    dline();

#if SIMULATION_MODE
    printf("*** SIMULATION MODE: RFID scans are simulated, NOT read from RC522 ***\n");
#else
    printf("RFID HARDWARE MODE: bring a tag near the reader.\n");
#endif

    wake_display_lock(1);
    started = 1;
    pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);

    // main just waits for Ctrl+C
    while (running)
        delay_ms(200);

    // ---------------- shutdown ----------------
    printf("\nShutting down IRPCM...\n");

    if (timer_ok == 1)
        MsgSendPulse(timer_coid, SHUTDOWN_PULSE_PRIO, PULSE_SHUTDOWN, 0);

    pthread_mutex_lock(&data_lock);
    pthread_cond_broadcast(&disp_cond);
    pthread_mutex_unlock(&data_lock);

    if (made_rfid)    pthread_join(t_rfid, NULL);
    if (made_timer)   pthread_join(t_timer, NULL);

    if (coid_main != -1) {
        send_msg(coid_main, MSG_SHUTDOWN, NULL, 0, &reply);
        name_close(coid_main);
    }
    if (made_server)  pthread_join(t_server, NULL);
    if (made_disp) pthread_join(t_disp, NULL);

    clear_matrix();
    if (rfid_ok)
        rc522_clear_bits(TxControlReg, 0x03);   // antenna off
    hw_close();

    printf("Bye.\n");
    return 0;
}
