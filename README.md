# Intelligent Railway Platform Conflict Manager (IRPCM)

**QNX E-HACK 2026 | Problem Statement #42**

IRPCM is a real-time embedded prototype designed to detect and resolve railway platform conflicts before they cause operational disruptions. Built on the **QNX Neutrino RTOS**, the system replaces manual timetable checking with a continuous, event-driven loop that automatically reallocates platforms during delays, schedule overlaps, or hardware failures.

## ⚙️ Core Technologies
* **OS:** QNX Neutrino RTOS
* **Hardware:** Raspberry Pi 4 (Target) / POSIX-compliant x86 (Simulation)
* **Language:** C/C++
* **Key RTOS Concepts:** Synchronous Message Passing (`MsgSend`/`MsgReceive`), Preemptive Priority Scheduling (`SCHED_FIFO`), Process Isolation, Hardware Interrupts (GPIO).

## 🏗️ System Architecture
The architecture is divided into isolated processes communicating via QNX IPC channels. This ensures that a fault in one component (e.g., the UI dashboard) does not crash the critical conflict-resolution logic.

1. **Train Simulator:** Generates real-time arrival, departure, and delay events.
2. **Train Manager (Priority 15):** Maintains active schedules and applies timing shifts.
3. **Conflict Manager (Priority 22):** Continuously computes time-window overlaps.
4. **Allocation Manager (Priority 20):** Evaluates platform suitability and resolves priority tie-breaks.
5. **Platform Manager (Priority 20):** Tracks hardware occupancy and physical GPIO status.
6. **Dashboard & Logger (Priority 8):** Renders terminal UI and logs system events.

## 🚀 Demonstration Scenarios
The system is tested against four distinct real-time operational disruptions:
* **Scenario 1 (Overlap Detection):** Identifies overlapping schedules on a single platform in real-time.
* **Scenario 2 (Auto-Reallocation):** Dynamically reassigns lower-priority trains to available secondary platforms.
* **Scenario 3 (Delay Injection):** Recalculates the entire state when an unexpected +20-minute delay is injected via hardware interrupt.
* **Scenario 4 (Platform Failure):** Safely routes incoming traffic away from a platform marked "offline" via hardware trigger.

## 💻 Build and Run Instructions

### 1. Local Simulation (Windows/Linux)
You can run the core logic in simulation mode using a standard C compiler before deploying to QNX hardware.
```bash
# Compile the simulation
gcc -Iinclude src/main.c src/conflict_mgr.c -o irpcm_sim

# Run the simulation
./irpcm_sim
