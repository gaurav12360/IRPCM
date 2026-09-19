# IRPCM --- Intelligent Railway Platform Conflict Manager

A QNX 8.0 real-time mini project for Raspberry Pi 4 that uses RFID-based
train identification, platform allocation, a waiting queue, periodic
platform release, and an LED matrix status display.

## Project Overview

IRPCM (**Intelligent Railway Platform Conflict Manager**) is designed to
manage railway-platform allocation using an MFRC522 RC522 RFID reader
and a MAX7219 display connected to a Raspberry Pi 4.

The implementation runs on **QNX 8.0** and demonstrates:

-   QNX threads with different priorities
-   QNX native message passing
-   QNX pulses and timers
-   Mutex and condition-variable synchronization
-   Direct BCM2711 GPIO/SPI register access using `mmap_device_memory()`
-   RFID-based train identification
-   Dedicated and shared platform allocation
-   FIFO waiting for unavailable platforms
-   Periodic platform release after a configured dwell time
-   MAX7219 LED matrix/status display

## Hardware

### Raspberry Pi 4

The source uses the BCM2711 peripheral address space and maps GPIO and
SPI0 registers directly.

  Function            GPIO   Raspberry Pi physical pin Device
  -------------- --------- --------------------------- -----------------
  SPI CE0 / CS      GPIO 8                      Pin 24 RC522
  SPI MOSI         GPIO 10                      Pin 19 RC522 + MAX7219
  SPI MISO          GPIO 9                      Pin 21 RC522 + MAX7219
  SPI SCLK         GPIO 11                      Pin 23 RC522 + MAX7219
  SPI CE1 / CS      GPIO 7                      Pin 26 MAX7219
  RC522 RST        GPIO 25                      Pin 22 RC522

CE0 and CE1 are **chip-select lines on the same SPI0 bus**. MOSI, MISO,
and SCLK are shared. The implementation therefore protects SPI
transactions with `spi_lock`.

## QNX Architecture

The application is split into four main threads:

1.  **RFID thread**
    -   Polls the RC522 at the configured RFID polling interval.
    -   Detects a UID.
    -   Sends the detected UID to the platform-management server with
        `MsgSend()`.
2.  **Platform Manager / Server thread**
    -   Registers the `irpcm` QNX service using `name_attach()`.
    -   Receives client messages using `MsgReceive()`.
    -   Owns the platform-allocation decisions.
    -   Allocates known trains to platforms 1--4.
    -   Allocates other valid RFID groups to platforms 5--8 when
        available.
    -   Places arrivals into the waiting queue when the shared pool is
        full.
    -   Sends replies using `MsgReply()`.
    -   Updates the MAX7219 status display.
3.  **Timer thread**
    -   Creates a QNX timer and receives periodic timer pulses.
    -   Checks for expired platform assignments.
    -   Sends platform-release requests to the server.
    -   Sends a periodic status pulse to request a display/status
        update.
4.  **Display thread**
    -   Waits on a condition variable.
    -   Refreshes display-related output without owning the railway
        decision logic.

## IPC Flow

The main request/reply path is:

``` text
Server:
name_attach("irpcm")
        ↑
        │ service registration
        │
RFID:
name_open("irpcm")
        │
        │ MsgSend()
        ▼
Platform Manager:
MsgReceive()
        │
        │ process + update state
        │
        └──────────────► MsgReply()
```

`MsgReply()` replies to the exact client represented by the receive ID
(`rcvid`) for that request.

The timer thread also uses QNX timer pulses internally. When a platform
must be released, the timer thread sends a normal message to the server.

## Platform Allocation Logic

The source configures eight platforms:

-   **Platforms 1--4:** dedicated to the four configured known train
    UIDs.
-   **Platforms 5--8:** shared pool for other valid/unknown train UIDs.

The configured train mapping is:

  Train     UID               Platform
  --------- --------------- ----------
  Train 1   `62:89:36:07`            1
  Train 2   `50:43:D2:55`            2
  Train 3   `43:63:9C:C9`            3
  Train 4   `09:2E:FE:06`            4

When a valid unknown UID arrives:

1.  Search platforms 5--8.
2.  Assign the first available platform.
3.  If all four are occupied, place the train in the FIFO waiting queue.
4.  When a platform is released, queued work can be serviced by the
    server.

The implementation also checks duplicate/active UID events and handles
re-arrival timing using the configured gap.

## Timing Configuration

The source contains these project-level settings:

  Parameter                                          Value
  ----------------------- --------------------------------
  Platform dwell time                                 60 s
  RFID polling interval                             100 ms
  Duplicate message gap                                3 s
  Re-arrival gap                                       3 s
  Full status interval                       5 timer ticks
  SPI clock divider         256 (commented as about 2 MHz)

The **100 ms RFID interval is a software polling interval**, not a
statement of guaranteed end-to-end response latency.

## Thread Priorities

The source assigns these QNX thread priorities:

  Thread                        Priority
  --------------------------- ----------
  RFID                                30
  Platform Manager / Server           25
  Timer                               20
  Display                             10

The program uses `SCHED_FIFO` scheduling when setting thread scheduling
parameters.

These numbers are scheduling priorities, **not milliseconds**.

## Synchronization

Two mutexes protect different shared resources:

### `spi_lock`

Protects the shared SPI0 bus so that RC522 and MAX7219 transactions do
not overlap.

Memory trick:

> `spi_lock` → who can use the SPI bus?

### `data_lock`

Protects shared railway-state data such as platform, train, and queue
information from concurrent access.

Memory trick:

> `data_lock` → who can access the railway data?

The display uses a condition variable (`disp_cond`) so that it can wait
efficiently until display work is requested.

## Real-Time Concepts Demonstrated

This project demonstrates several QNX real-time concepts:

### 1. Priority-based scheduling

Critical threads are assigned higher scheduling priorities than
lower-importance display work.

### 2. Deterministic event handling

RFID and timer events are handled through defined message/pulse paths
instead of an uncontrolled shared-data design.

### 3. Message passing

Train/arrival information and platform-release requests are transferred
using QNX native IPC.

### 4. Pulses

QNX pulses are used for lightweight asynchronous timer/status
notifications.

### 5. Synchronization

Mutexes and a condition variable protect shared resources and coordinate
threads.

### 6. Centralized platform state

The server acts as the decision point for allocation/release requests,
which helps serialize platform-state changes and avoid conflicting
updates.

## Source Layout

``` text
IRPCM/
├── README.md
├── .gitignore
├── src/
│   └── irpcm.c
└── docs/
    └── architecture.md
```

## Important QNX / BSP Note

The source uses:

``` c
#define PERIPHERAL_BASE   0xFE000000UL
```

and maps BCM2711 GPIO/SPI registers directly.

The source comments explicitly note that the peripheral base may need
adjustment depending on the QNX BSP configuration. When porting the
program to a different BSP or hardware configuration, verify the
addresses used by the board support package.

## Simulation Mode

The program contains a compile-time simulation option:

``` c
#define SIMULATION_MODE 0
```

The source comments define:

-   `0` → real RC522
-   `1` → fake scans for testing

The source also contains a separate `SIM_USE_MATRIX` setting for
simulation display behavior.

## Source File

The main implementation is:

``` text
src/irpcm.c
```

It contains the QNX threads, IPC definitions, BCM2711 SPI/GPIO handling,
RC522 access, MAX7219 handling, platform allocation logic, timer
handling, display handling, and shutdown logic.

## GitHub Presentation

For GitHub, keep the C implementation as a normal `.c` file so it can be
syntax-highlighted and reviewed directly. Use this README for the
project explanation and `docs/architecture.md` for the evaluator-facing
architecture notes.

## Project Status

This repository is a project source/documentation snapshot based on the
supplied QNX IRPCM implementation. Hardware-specific behavior should be
validated on the target Raspberry Pi 4 + QNX BSP before presenting
measured timing or hardware guarantees.
