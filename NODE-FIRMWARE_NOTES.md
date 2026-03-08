# Firmware Architecture Document — Ultrasonic Anemometer Node

## System Architecture Summary

Firmware controls a bidirectional ultrasonic TOF measurement pipeline and computes axis wind components using reciprocal timing equations. The firmware sequence coordinates direction control, boosted transmit energy, first-arrival edge capture, and robust filtering before producing wind outputs suitable for environmental monitoring.

## Key Subsystems and Purpose

1. Ultrasonic transmit control: select direction and generate controlled 40 kHz bursts.
2. Ultrasonic receive timing: gate ring-down and capture the first valid comparator edge.
3. MCU scheduling logic: coordinate measurements with low jitter and deterministic timing.
4. Power coordination: enable/disable ~22 V stage around each burst window.
5. Sensor-hub behavior: integrate ultrasonic reads with other node sensor tasks.
6. Measurement algorithm: solve wind and speed-of-sound from bidirectional TOF pairs.

---

## Measurement Cycle (Per Direction Pair)

1. Select transducer direction (`REL_N`).
2. Enable TX power path (`TX_PWM`).
3. Wait for boost warm-up (`~5 to 20 ms`).
4. Generate ultrasonic burst (`DRV_N`, ~40 kHz carrier).
5. Disable transmit.
6. Apply RX blanking window (`200 to 800 us`).
7. Capture first comparator edge on timer input.
8. Repeat in opposite direction.

Multiple cycles are collected and filtered before solving wind.

## Sampling and Filtering

Typical processing per direction:

- collect `16 to 64` samples
- median-filter TOF set
- compute reciprocal-time velocity solve
- derive speed-of-sound estimate for health checks

Outlier handling intent:

- reject captures outside valid timing gate
- flag runs with missing first-arrival capture

Assumption / To Be Verified:

- Exact outlier rejection thresholds in production firmware build.

## Timing and Noise Constraints

Critical constraints for low-wind sensitivity:

- sub-microsecond timestamp stability is strongly preferred
- RX blanking must suppress TX ring-down edges
- comparator output should provide a clean digital transition to timer capture
- WiFi activity should be paused/suppressed during active TOF capture windows

## Burst and Direction Control

Burst parameters (current design intent):

- carrier frequency: `~40 kHz`
- burst duration: `0.5 to 5 ms`

Direction-control rules:

- only one direction path active at a time
- insert short dead-time before reversing direction

Assumption / To Be Verified:

- Final dead-time value and burst-cycle count per transducer pair.

## Measurement Equations

For one axis pair using bidirectional TOF (`tAB`, `tBA`) and path length `L`:

Along-beam component:

$$
\nu = \frac{L}{2}\left(\frac{1}{t_{AB}} - \frac{1}{t_{BA}}\right)
$$

Axis wind velocity:

$$
U_{axis} = \frac{L}{2p}\left(\frac{1}{t_{AB}} - \frac{1}{t_{BA}}\right)
$$

where $p = |\sin(\theta)|$ from pod tilt.

Speed-of-sound estimate:

$$
c = \frac{L}{2}\left(\frac{1}{t_{AB}} + \frac{1}{t_{BA}}\right)
$$

## Geometry Constants Used by Firmware (Current)

Current validated constants (from `TOF_WORKOUT_GUIDE.md`):

- `L = 0.14670 m`
- `theta ~ 32.3 deg`
- `p = 0.5344`
- `t0 ~ 427 us` at `20 deg C`

Reference split magnitude:

- 1 m/s -> `~1.33 us`
- 5 m/s -> `~6.65 us`
- 10 m/s -> `~13.30 us`

## Calibration and Validation Workflow

1. Zero-wind calibration: capture stationary runs and store axis offsets.
2. Gain calibration: fit linear correction using reference airflow (`Utrue = a * Umeas + b`).
3. Temperature consistency check: compare inferred `c` against `331.3 + 0.606T`.
4. Runtime telemetry: log raw `tAB`, `tBA`, inferred `c`, and solved `Uaxis` for debug.

## Integration with Node Tasks

Firmware should schedule ultrasonic acquisition so that:

- non-critical tasks do not add capture jitter
- I2C/ADC operations do not overlap critical timing windows
- communication stack activity is reduced during edge capture windows

Assumption / To Be Verified:

- Final real-time scheduling policy (single loop, RTOS task priority, or ISR-driven capture dispatcher).
