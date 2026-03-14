# Hardware Design Document — Ultrasonic Anemometer Node

## System Architecture Summary

The node is an environmental sensing platform built around an ESP32-class MCU and includes an ultrasonic anemometer subsystem, shared power rails, and external sensor interfaces. The ultrasonic channel uses a burst-transmit path, analog receive path, and timer-capture timing method to solve bidirectional time-of-flight (TOF).

Core architecture (ultrasonic subsystem):

- Sensors used: 40 kHz ultrasonic transducer set (bidirectional paths per axis)
- MCU platform: ESP32 node controller (see firmware notes for scheduling and capture flow)
- Transmit system: MCU burst generation + ~22 V boosted drive stage
- Receive system: transducer input + analog mux + protection + amplification + comparator + timer capture
- Timing method: first-arrival comparator edge capture with firmware blanking window
- Mechanical geometry: roof-reflection path (`pod -> roof -> opposite pod`) with constants from `TOF_WORKOUT_GUIDE.md`

## Key Subsystems and Purpose

1. Ultrasonic transmit system: generate controlled acoustic energy for TOF measurement.
2. Ultrasonic receive system: detect first-arrival edge with low-noise analog conditioning.
3. MCU control logic: sequence direction switching, burst timing, capture, and filtering.
4. Power system: provide stable 3.3 V/5 V rails and gated ~22 V TX energy.
5. Mechanical sensor housing: enforce acoustic geometry while protecting electronics.
6. Measurement algorithm: convert reciprocal TOF pairs into wind and speed-of-sound estimates.

---

## MCU Subsystem

ESP32 is used for:

- waveform generation
- timing capture
- sensor interface
- measurement scheduling

Important ultrasonic control signals:

- `DRV_N` (40 kHz ultrasonic carrier drive control)
- `REL_N` (transducer direction selector)
- `TX_PWM` (transmit power enable envelope controlling ~22 V boost regulator)

Assumption / To Be Verified:

- Final GPIO mapping and active polarity of `DRV_N`, `REL_N`, and `TX_PWM` on the latest PCB revision.

## Ultrasonic Transmit Architecture

Transmit path uses:

- 40 kHz ultrasonic transducers
- short burst excitation
- high-voltage transmit drive (~22 V)

Signal flow:

`ESP32 -> driver stage -> transducer`

Burst characteristics:

- Carrier frequency: ~40 kHz
- Burst length: 0.5 to 5 ms

Burst purpose:

- excite resonant transducer behavior and maximize acoustic output for robust first-arrival detection.

## Ultrasonic Receive Architecture

Receive chain:

`transducer -> analog multiplexer -> protection network -> amplification stages -> comparator -> ESP32 timer capture`

Detection method:

- first valid acoustic arrival edge is used for TOF timing.

Comparator timing interface:

- comparator output is routed to a hardware timer-capture input on the MCU.

Reference threshold:

- `VREF ~= 1.65 V` (derived from 3.3 V divider).

Assumption / To Be Verified:

- Comparator part number and exact hysteresis network values for current build.

## Analog Multiplexer

A `74HC4052` analog mux selects which transducer path feeds the shared RX chain.

Design intent:

- reduce component count by sharing one receive chain
- keep raw transducer routing centralized
- rely on firmware RX blanking to ignore transmit ring-down period

Assumption / To Be Verified:

- Whether all production variants remain on `74HC4052` versus alternate mux footprints.

## Transmit Power Stage

A boost regulator generates approximately 22 V for ultrasonic transmit bursts.

Higher TX voltage improves:

- signal-to-noise ratio
- timing precision
- low-wind measurement robustness

Design rule:

- 22 V stage should be active only for TX windows and disabled during RX capture windows.

## Power and Mixed-Signal Integration

Node rails and domains:

- 3.3 V digital domain for MCU and control logic
- 5 V analog/sensor domain for auxiliary interfaces
- ~22 V pulsed TX domain for ultrasonic excitation

Integration goals:

- keep switching noise out of comparator/timer capture path
- maintain stable references during first-arrival detection
- preserve deterministic timing under field power variation

## Ultrasonic Geometry Constants (Current Snapshot)

From `TOF_WORKOUT_GUIDE.md` validated snapshot (Mar 5, 2026):

- Path length: `L = 146.70 mm`
- Pod tilt: `~32.3 deg`
- Projection factor: `p = |sin(32.3 deg)| = 0.5344`
- Nominal no-wind TOF: `t0 ~ 427 us`

Expected reciprocal TOF split magnitude:

- 1 m/s -> `~1.33 us`
- 5 m/s -> `~6.65 us`
- 10 m/s -> `~13.30 us`

These constants are the current hardware/firmware alignment baseline until geometry changes.
