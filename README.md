# Intelligent Railway Platform Conflict Manager (IRPCM)

A QNX Neutrino RTOS-based railway platform management prototype for Raspberry Pi 4.  
The system detects trains using an MFRC522 RFID reader, allocates platforms, displays occupied platforms on a MAX7219 8×8 LED matrix, and automatically releases platforms after a configurable dwell time.

## Overview

The **Intelligent Railway Platform Conflict Manager (IRPCM)** is designed to demonstrate how QNX real-time operating system mechanisms can be combined with embedded hardware to coordinate railway platform allocation.

The project uses:

- **QNX Neutrino RTOS**
- **Raspberry Pi 4 Model B**
- **MFRC522 RFID reader**
- **MAX7219 8×8 LED matrix**
- **SPI0**
- **GPIO**
- **QNX message-passing IPC**
- **QNX Name Service**
- **QNX channels and pulses**
- **QNX timers**
- **POSIX/QNX threads**
- **Mutexes and condition variables**
- **Real-time thread scheduling (`SCHED_FIFO`, `SCHED_RR`)**

## Key Features

### Train Detection

The MFRC522 reads a train RFID tag. The UID is converted into a message and sent to the IRPCM server.

### Platform Allocation

Known trains use dedicated platforms:

| Train | Platform |
|---|---:|
| Train 1 | 1 |
| Train 2 | 2 |
| Train 3 | 3 |
| Train 4 | 4 |

Unknown UIDs are handled separately and use platforms **5–8**.

If all unknown-train platforms are occupied, the UID is placed into a waiting queue until a platform becomes available.

### Automatic Platform Release

Each allocated platform receives a configurable dwell time.

The current configuration uses:

```c
#define DWELL_TIME_SECONDS 60
```

A QNX timer generates a pulse every second. The timer thread checks platform expiry and sends a platform-release request to the server.

### LED Platform Indication

The MAX7219 matrix represents platform occupancy:

```text
Platform 1 → Row 1
Platform 2 → Row 2
Platform 3 → Row 3
Platform 4 → Row 4
Platform 5 → Row 5
Platform 6 → Row 6
Platform 7 → Row 7
Platform 8 → Row 8
```

An occupied platform is shown by an ON row; when the platform is released, that row is switched OFF.

---

# System Architecture

```text
                    ┌──────────────────────┐
                    │   MFRC522 RFID       │
                    │   Reader             │
                    └──────────┬───────────┘
                               │ SPI
                               ▼
                    ┌──────────────────────┐
                    │    RFID THREAD       │
                    │  Read UID / MsgSend  │
                    └──────────┬───────────┘
                               │
                         QNX MsgSend()
                               │
                               ▼
              ┌─────────────────────────────────┐
              │        IRPCM SERVER THREAD      │
              │                                 │
              │  MsgReceive()                   │
              │  Train identification           │
              │  Platform allocation             │
              │  Platform release                │
              │  MsgReply()                      │
              └─────────────┬───────────────────┘
                            │
                  ┌─────────┴──────────┐
                  │                    │
                  ▼                    ▼
        ┌──────────────────┐   ┌─────────────────┐
        │ Railway State    │   │ MAX7219 Matrix  │
        │ Platforms/Trains │   │ Platform LEDs   │
        └──────────────────┘   └─────────────────┘

                    ┌──────────────────────┐
                    │    TIMER THREAD      │
                    │                      │
                    │ QNX Timer            │
                    │       ↓              │
                    │ QNX Pulse            │
                    │       ↓              │
                    │ MsgReceivePulse()    │
                    │       ↓              │
                    │ Platform expiry      │
                    └──────────┬───────────┘
                               │
                        MsgSend(RELEASE)
                               │
                               ▼
                         IRPCM SERVER

                    ┌──────────────────────┐
                    │   DISPLAY THREAD     │
                    │                      │
                    │ Condition Variable   │
                    │ Runtime status       │
                    └──────────────────────┘
```

## Software Threads

The application uses four main worker threads:

1. **RFID Thread**
   - Reads the MFRC522.
   - Converts the UID to a string.
   - Sends `MSG_RFID_DETECTED` to the server.

2. **IRPCM Server Thread**
   - Central decision-making thread.
   - Receives QNX messages.
   - Identifies known/unknown trains.
   - Allocates and releases platforms.
   - Controls the LED matrix after state changes.
   - Sends replies to clients.

3. **Timer Thread**
   - Owns a QNX timer and private channel.
   - Receives timer pulses every second.
   - Checks platform dwell-time expiry.
   - Sends release requests to the server.
   - Periodically sends status pulses.

4. **Display Thread**
   - Waits on a condition variable.
   - Prints platform and system status.
   - Does not make railway allocation decisions.

---

# QNX RTOS Mechanisms Used

## 1. QNX Name Service

The server registers the IRPCM service:

```c
name_attach(NULL, "irpcm", 0);
```

Worker threads locate the server using:

```c
name_open("irpcm", 0);
```

This provides QNX service discovery without hard-coding the server channel identifier.

## 2. QNX Message Passing

The main application IPC path is:

```text
Client
   │
   │ MsgSend()
   ▼
Server
   │
   │ MsgReceive()
   ▼
Process request
   │
   │ MsgReply()
   ▼
Client
```

Application message types include:

```c
MSG_RFID_DETECTED
MSG_PLATFORM_REQUEST
MSG_PLATFORM_ALLOCATED
MSG_PLATFORM_RELEASE
MSG_SYSTEM_STATUS
MSG_SHUTDOWN
```

## 3. QNX Channels and Pulses

The timer thread creates a channel and attaches a connection:

```c
ChannelCreate()
ConnectAttach()
```

The timer notification is configured using:

```c
SIGEV_PULSE_INIT()
```

Timer expiry is then received with:

```c
MsgReceivePulse()
```

The project uses pulse codes for:

```c
PULSE_TIMER_TICK
PULSE_STATUS_REQUEST
PULSE_SHUTDOWN
```

## 4. QNX Timers

The platform timer is created using:

```c
timer_create(CLOCK_MONOTONIC, ...)
```

and configured with:

```c
timer_settime(...)
```

The current interval is one second.

## 5. Real-Time Scheduling

Thread scheduling is explicitly configured using:

```c
pthread_setschedparam()
pthread_getschedparam()
```

Current intended policies:

```text
RFID     → SCHED_FIFO
SERVER   → SCHED_FIFO
TIMER    → SCHED_FIFO
DISPLAY  → SCHED_RR
```

## 6. Synchronization

Shared railway state is protected by a mutex:

```c
pthread_mutex_t data_lock;
```

The SPI bus is protected using:

```c
pthread_mutex_t spi_lock;
```

The display thread uses:

```c
pthread_cond_t disp_cond;
```

to sleep until a display update is requested.

---

# Hardware Connections

The current prototype uses SPI0 on the Raspberry Pi 4.

## MFRC522

| MFRC522 | Raspberry Pi |
|---|---|
| 3.3V | Physical Pin 1 |
| GND | Physical Pin 6 |
| MOSI | GPIO10 / Pin 19 |
| MISO | GPIO9 / Pin 21 |
| SCK | GPIO11 / Pin 23 |
| SDA/SS | GPIO8 / CE0 / Pin 24 |
| RST | GPIO25 / Pin 22 |

## MAX7219

| MAX7219 | Raspberry Pi |
|---|---|
| VCC | 5V / Physical Pin 2 |
| GND | Physical Pin 6 |
| DIN | GPIO10 / MOSI / Pin 19 |
| CLK | GPIO11 / SCLK / Pin 23 |
| CS | GPIO7 / CE1 / Pin 26 |

### Important Hardware Note

The MFRC522 uses **3.3 V logic**. Do not connect a 5 V signal directly to the Raspberry Pi GPIO or MFRC522 signal pins.

The project uses the SPI bus with:

```text
CE0 → MFRC522
CE1 → MAX7219
```

---

# QNX Hardware Access

On the AArch64 Raspberry Pi target, the application maps the BCM2711 GPIO and SPI0 register regions using:

```c
mmap_device_memory()
```

The hardware layer then configures GPIO and performs low-level SPI register access.

This implementation is a prototype-oriented low-level hardware approach rather than a full higher-level QNX SPI driver stack.

---

# Train and Platform Logic

## Known Train

A known UID is matched against the configured train table.

Example:

```text
Train 1
UID → known
      ↓
Platform 1
```

Before allocation, the server checks whether that platform is already occupied.

## Unknown Train

An unrecognized UID is treated as an unknown train.

The server searches:

```text
Platform 5
Platform 6
Platform 7
Platform 8
```

and assigns the first available platform.

If all four are occupied:

```text
Unknown UID
     ↓
Waiting Queue
     ↓
Platform becomes free
     ↓
MSG_PLATFORM_REQUEST
     ↓
Waiting train assigned
```

## Duplicate Detection

The application tracks recently scanned UIDs to avoid treating a tag that remains on the reader as multiple arrivals.

---

# Runtime Flow

A normal train arrival follows this path:

```text
1. Train RFID tag enters reader range
           ↓
2. RC522 reads UID
           ↓
3. RFID thread receives UID
           ↓
4. RFID thread sends MSG_RFID_DETECTED
           ↓
5. QNX server receives message
           ↓
6. Server identifies known/unknown train
           ↓
7. Server checks platform availability
           ↓
8. Platform is allocated
           ↓
9. Platform state is updated
           ↓
10. MAX7219 row is switched ON
           ↓
11. Server sends MsgReply()
           ↓
12. QNX timer continues ticking
           ↓
13. Dwell time expires
           ↓
14. Timer sends MSG_PLATFORM_RELEASE
           ↓
15. Server releases platform
           ↓
16. MAX7219 row is switched OFF
```

---

# Error Handling and Shutdown

The application checks initialization and runtime failures including:

- GPIO/SPI mapping failure
- RC522 initialization failure
- QNX server registration failure
- Thread creation failure
- Timer/channel/connection failure
- `MsgSend()` failure
- `MsgReceive()` failure
- `MsgReply()` failure
- RFID read errors
- Scheduling API failure

On shutdown, the program:

1. Stops worker activity.
2. Sends a timer shutdown pulse.
3. Wakes the display thread.
4. Joins created threads.
5. Sends `MSG_SHUTDOWN` to the server.
6. Clears the LED matrix.
7. Disables the RFID antenna.
8. Unmaps GPIO/SPI memory.

---

# Performance and Runtime Benchmarking

A benchmark layer can be integrated into the application to measure observed runtime behaviour.

Recommended metrics include:

### CPU

- Process CPU time
- CPU usage over an interval
- Overall CPU usage
- CPU share relative to available CPUs

### QNX IPC latency

Measure the end-to-end round trip:

```text
MsgSend()
    ↓
QNX scheduling + message delivery
    ↓
Server processing
    ↓
MsgReply()
    ↓
MsgSend() returns
```

Track:

- Current latency
- Minimum latency
- Average latency
- Maximum latency
- Number of samples

### Timer behaviour

Track:

- Timer tick count
- Actual timer period
- Minimum interval
- Average interval
- Maximum interval
- Observed timer jitter

### Runtime application behaviour

Track:

- RFID scans
- Known arrivals
- Unknown arrivals
- Duplicate scans
- Platform allocations
- Platform releases
- Waiting events
- Current queue length
- Maximum queue length
- Active platforms
- Maximum simultaneously occupied platforms
- IPC errors
- Timer errors
- RFID errors

**Important:** measured latency and jitter should be described as **observed runtime behaviour**, not as proof of hard real-time guarantees.

---

# Project Structure

A typical repository can be organized as:

```text
IRPCM/
├── README.md
├── src/
│   └── main.c
├── docs/
│   ├── architecture/
│   ├── wiring/
│   └── screenshots/
├── benchmark/
│   └── results/
└── LICENSE
```

If the project is maintained as a single Momentics source file, keeping the original source layout is also acceptable.

---

# Building

## Requirements

- QNX Neutrino RTOS
- QNX Software Development Platform (SDP)
- QNX Momentics IDE
- QNX Raspberry Pi 4 BSP
- Raspberry Pi 4 Model B
- AArch64 QNX target
- MFRC522
- MAX7219
- SPI/GPIO access

## Build

Import/open the project in QNX Momentics and select an **AArch64** build configuration for the Raspberry Pi 4 target.

Do not use an x86_64 binary on the Raspberry Pi target.

A previously encountered architecture mismatch is:

```text
Binary architecture aarch64 does not match target architecture x86_64
```

Make sure the target architecture and build configuration match.

## Run

Deploy the executable to the Raspberry Pi 4 QNX target from Momentics or the target shell.

Hardware access may require the program to run with appropriate QNX privileges/root permissions.

On startup, verify that the terminal reports the expected states for:

```text
QNX IPC
MESSAGE PASSING
PULSE EVENTS
QNX TIMER
THREADS
PRIORITY SCHED
MUTEX
CONDITION VAR
RFID
MAX7219
```

---

# Example Runtime Output

A typical functional output follows this pattern:

```text
IRPCM QNX REAL-TIME SYSTEM - STARTING

MODE  : RFID HARDWARE

QNX SCHEDULING:
IRPCM SERVER    : Policy SCHED_FIFO ...
TIMER THREAD    : Policy SCHED_FIFO ...
DISPLAY THREAD  : Policy SCHED_RR ...
RFID THREAD     : Policy SCHED_FIFO ...

MAIN: MSG_SYSTEM_STATUS reply -> ...

IRPCM QNX REAL-TIME SYSTEM
QNX IPC          : ENABLED
MESSAGE PASSING  : ENABLED
PULSE EVENTS     : ENABLED
QNX TIMER        : ENABLED
THREADS          : ENABLED
PRIORITY SCHED   : ENABLED
MUTEX            : ENABLED
CONDITION VAR    : ENABLED
RFID             : READY
MAX7219          : READY

RFID HARDWARE MODE: bring a tag near the reader.
```

After a train is detected:

```text
TRAIN 1 ENTERS STATION

UID            : XX:XX:XX:XX
QNX EVENT      : MSG_RFID_DETECTED (MsgReceive)
CURRENT TIME   : HH:MM:SS

PLATFORM 1     : FREE
ALLOCATED PLATFORM : 1
ARRIVAL TIME  : HH:MM:SS
DWELL TIME    : 1 MINUTE
WILL LEAVE BY : HH:MM:SS

MAX7219        : ROW 1 ON
```

When the dwell time expires:

```text
QNX TIMER      : PULSE RECEIVED
QNX MESSAGE    : MSG_PLATFORM_RELEASE -> platform 1
MAX7219        : ROW 1 OFF
```

---

# Why QNX?

This project uses QNX because the railway application benefits from explicit real-time mechanisms:

- deterministic thread scheduling policies
- synchronous message passing
- lightweight pulses
- kernel-managed timers
- channels and connections
- thread synchronization
- separation of hardware access and application logic

The core design is:

```text
Hardware Event
     ↓
QNX Thread
     ↓
QNX IPC
     ↓
Central Server
     ↓
Decision
     ↓
Hardware Output
```

---

# Limitations

This is a **prototype / demonstration system**, not a railway-certified safety system.

Current limitations include:

- RFID is used as the train-arrival identification mechanism.
- Platform allocation rules are simplified for the prototype.
- The known train-to-platform mapping is fixed.
- The unknown-train allocation pool is fixed to platforms 5–8.
- The 60-second dwell time is configurable but not based on a live railway timetable.
- Runtime benchmark values are observational measurements, not formal worst-case execution-time guarantees.
- The current low-level hardware interface directly maps Raspberry Pi registers.
- The implementation does not provide certified railway safety assurance or fail-safe interlocking.

---

# Future Improvements

Possible future work:

- dynamic timetable integration
- multiple railway stations/nodes
- train route conflict prediction
- real GPS/train-position integration
- richer operator dashboard
- persistent event logging
- networked QNX nodes
- formal timing analysis
- watchdog/fault-recovery mechanisms
- higher-level QNX device-driver integration
- authenticated train identification
- railway interlocking interface

---

# Project Goal

The main goal of IRPCM is to demonstrate how a **real-time QNX application can combine hardware I/O, threads, IPC, timers, pulses, synchronization and scheduling to manage a time-sensitive railway platform allocation problem.**

---

## License

Add the license you intend to use for the repository.

Example:

```text
MIT License
```

if appropriate for your project.

---

## Author

**Intelligent Railway Platform Conflict Manager (IRPCM)**

Developed as a QNX/Raspberry Pi embedded systems prototype.
