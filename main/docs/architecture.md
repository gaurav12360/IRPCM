# IRPCM Architecture --- Evaluator Notes

## 1. System Goal

IRPCM separates railway decision logic from hardware/event handling so
that RFID arrivals, platform releases, and display updates can be
handled by dedicated execution contexts.

## 2. High-Level Data Flow

``` text
                 +--------------------+
                 |   RC522 RFID       |
                 |   CE0 / GPIO8      |
                 +---------+----------+
                           |
                           | SPI0
                           v
                 +--------------------+
                 |    RFID Thread     |
                 |    Priority 30     |
                 +---------+----------+
                           |
                       MsgSend()
                           |
                           v
                 +--------------------+
                 | Platform Manager   |
                 | / QNX Server       |
                 |    Priority 25     |
                 +----+----------+----+
                      |          |
              railway state     SPI0
                      |          |
                      v          v
              +-----------+  +----------------+
              | Platforms |  | MAX7219        |
              | + Queue   |  | CE1 / GPIO7    |
              +-----------+  +----------------+
                      ^
                      |
                  MsgSend()
                      |
              +-------+--------+
              |   Timer Thread |
              |   Priority 20  |
              +-------+--------+
                      ^
                      |
                QNX timer pulse
```

## 3. Shared SPI Bus

RC522 and MAX7219 use the same SPI0 peripheral:

``` text
                 SPI0
        +----------+----------+
        |          |          |
      MOSI       MISO       SCLK
        |          |          |
        +----------+----------+
             shared lines
             /          \
          CE0            CE1
           |              |
         RC522          MAX7219
```

CE0 and CE1 select the target device; they do not create separate SPI
buses.

Because multiple threads can initiate hardware access through the same
SPI peripheral, the implementation uses `spi_lock` around SPI
transactions.

## 4. QNX Server Registration

The platform-management thread registers a named service:

``` c
name_attach(NULL, "irpcm", 0);
```

The RFID client obtains a connection:

``` c
name_open("irpcm", 0);
```

The client then sends the RFID event:

``` c
MsgSend(coid, ...);
```

The server receives it:

``` c
MsgReceive(server_chid, ...);
```

After processing:

``` c
MsgReply(rcvid, ...);
```

### Important distinction

`name_open()` establishes access to the service. It does **not** carry
the RFID UID as the application message.

The actual UID and event data are sent by `MsgSend()`.

## 5. Timer Path

The timer thread creates a QNX timer and uses a pulse event.

The simplified sequence is:

``` text
QNX timer expiry
      |
      v
timer pulse
      |
      v
Timer thread
      |
      | check platform expiry
      v
MSG_PLATFORM_RELEASE
      |
   MsgSend()
      |
      v
Platform Manager
      |
      +--> release platform
      +--> update queue/state
      +--> update display
```

A separate status pulse is also used periodically.

## 6. Railway State Protection

The source declares:

``` c
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
```

It is used for shared platform/train/queue state.

This is different from:

``` c
pthread_mutex_t spi_lock = PTHREAD_MUTEX_INITIALIZER;
```

which protects the shared SPI hardware transaction path.

Using separate locks keeps the protection scope tied to the resource
being protected.

## 7. Allocation Rules

The project configuration defines:

``` text
4 known trains
8 total platforms
P1-P4 -> known trains
P5-P8 -> unknown/shared pool
```

### Known train

``` text
UID match
   |
   v
dedicated platform
```

### Unknown train

``` text
UID does not match known list
          |
          v
search P5-P8
     /          \
free found      all busy
   |               |
   v               v
allocate        FIFO queue
```

### Duplicate event

An already-active/duplicate UID is handled according to the
duplicate/re-arrival logic in the source rather than creating a second
platform assignment.

## 8. Why Message Passing Helps

The important design point is not that `MsgSend()` itself magically
prevents duplicate allocation.

The useful property is:

``` text
multiple event producers
        |
        v
  server request
        |
        v
 centralized platform-state decision
```

This gives the application one main place where allocation/release
decisions are processed.

## 9. Real-Time Design Elements

### Priority

Higher scheduling priority is assigned to RFID processing and platform
management than to display work.

``` text
RFID       30
SERVER     25
TIMER      20
DISPLAY    10
```

### Event-driven handling

RFID communication uses message passing, while timers use QNX pulses.

### Synchronization

Mutexes protect shared state and the SPI bus.

### Thread separation

Each thread has a narrow responsibility:

``` text
RFID    -> detect + send
SERVER  -> decide + manage state
TIMER   -> periodic expiry checks
DISPLAY -> output
```

This makes the execution flow easier to reason about and test.

## 10. Hardware Register Access

The implementation does not rely on Linux `/dev/spidev`.

Instead, it maps BCM2711 peripheral registers through QNX:

``` c
mmap_device_memory()
```

The source defines GPIO and SPI0 base addresses and accesses the
hardware registers directly.

That is a key platform-specific part of this implementation and should
be explained when comparing the project with a Linux Raspberry Pi
implementation.

## 11. Timing Interpretation

The source configuration includes:

``` c
#define RFID_POLL_MS 100
#define DWELL_TIME_SECONDS 60
#define SUMMARY_INTERVAL_TICKS 5
```

Interpret them carefully:

-   **100 ms** = software RFID polling interval.
-   **60 s** = configured platform dwell time before release processing.
-   **5 ticks** = configured status interval; with a 1-second timer
    pulse period in the source, that corresponds to a 5-second status
    cycle.

Do not present these configuration values as measured worst-case timing
unless measurements have been performed on the target system.

## 12. Evaluator-Ready Explanation

> IRPCM uses QNX threads, native message passing, pulses, timers, and
> synchronization primitives to separate RFID detection, platform
> management, timed release, and display handling. RC522 and MAX7219
> share the Raspberry Pi 4 SPI0 bus, using CE0 and CE1 as independent
> chip-selects, so SPI transactions are protected by a dedicated mutex.
> RFID events are sent to a named QNX server, where platform-state
> decisions are centralized. Known trains map to platforms 1--4, while
> other valid UIDs use platforms 5--8 or enter a FIFO waiting queue when
> the shared pool is full. A QNX timer generates periodic pulses, and
> platform release requests are sent back to the server after the
> configured dwell period.
