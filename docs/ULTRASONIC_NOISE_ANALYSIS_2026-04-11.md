# Ultrasonic Noise Analysis Report (2026-04-11)

## Scope

This report analyzes ultrasonic bring-up results collected on 2026-04-11 from the ESP32-WROOM route-finder test firmware.

Primary goal:

- Determine whether current detections are true acoustic first-arrival events or electrically coupled false triggers.

Secondary goal:

- Identify which subsystem contributes most to false triggering.

## Test Context

- Firmware target: `esp32wroom-ultrasonic-first-test`
- Board state: D8 and D9 receive-path clamp diodes removed (known temporary state from prior bring-up)
- Active serial diagnostics used:
  - `N` noise baseline
  - `C` coupling round
  - `A` aggressor matrix
  - `S` blanking/guard/min-TOF sweep
  - `P` paired-direction round
  - `O` / `B` open vs blocked route-finder rounds

## Key Observations

### 1) Baseline comparator activity exists without intended acoustic event

From repeated `N` runs:

- `MEAN_EDGE_COUNT` around 5 to 6 edges in a 2.5 ms window
- `FIRST_EDGE_MED_US` around 3 to 4 us

Interpretation:

- The comparator is active almost immediately after capture arm.
- Background/event-free edge rate is already non-trivial.

### 2) Coupling test remains saturated

From `C`:

- `DET=24/24` in all tested REL/DRV combinations
- `DET_PCT=100%` in all combinations
- Median TOF values remain tightly clustered rather than showing strong route dependence

Interpretation:

- Detection remains fully saturated under coupling-focused conditions.
- This is inconsistent with robust acoustic discrimination.

### 3) Aggressor matrix identifies TX burst path as dominant disturbance source

From `A` summary:

- `BASE_RX_ONLY` mean edge count: 5.10
- `RX_EN_TOGGLE`: 4.10
- `MUX_SWITCH`: 5.10
- `DRVREL_SWITCH`: 5.30
- `TX_PWM_ONLY`: 10.70
- `TX_PWM_WITH_ROUTE`: 12.10

Relative increase versus baseline:

- `TX_PWM_ONLY`: about +110%
- `TX_PWM_WITH_ROUTE`: about +137%

Interpretation:

- TX burst switching is the strongest electrical aggressor.
- Routing enabled during burst increases coupling further.

### 4) Sweep behavior indicates timing is dominated by gating/electrical path

From `S` at `MIN_TOF_US=220`, `GUARD_US=0`:

- `BLANK_US=320` -> `MED_TOF_US=664`
- `BLANK_US=500` -> `MED_TOF_US=838`
- `BLANK_US=800` -> `MED_TOF_US=1078`
- `BLANK_US=1100` -> `MED_TOF_US=1460`

Interpretation:

- Median measured time shifts strongly with blanking changes.
- A true acoustic TOF should not move this closely with listen-window timing.
- Current timing metric is strongly coupled to firmware gating and electrical transient behavior.

### 5) Directional and obstruction discrimination remains weak

From `P`, `O`, and `B`:

- Paired-direction deltas (`DELTA_US`) are near zero or sign-variable.
- `OPEN` and `BLOCKED` remain `DET=24/24` with similar medians/jitter ranges.

Interpretation:

- No stable directional asymmetry (N->S vs S->N) is present.
- No reliable open-vs-blocked separation is present.

## Conclusion

Current ultrasonic detections are not acoustically trustworthy.

Evidence converges on an electrical-dominant trigger path:

1. Early baseline edge activity with no intended acoustic event.
2. TX burst path is the largest aggressor by a wide margin.
3. Measured TOF tracks blanking window changes strongly.
4. Direction/blocking tests fail to show robust physical-path dependence.

## Probable Root Causes (ranked)

1. TX burst feedthrough into RX/comparator path (dominant)
2. Comparator threshold/hysteresis sensitivity
3. Receive front-end overdrive susceptibility
4. Missing clamp behavior due to D8/D9 removal
5. VREF / analog rail disturbance during TX burst activity

## Recommended Next Actions

### Priority 1: Hardware/electrical validation with scope

Probe simultaneously:

- TX burst gate path (`TX_PWM` and post-driver burst node)
- Comparator output (`TOF_EDGE`)
- Comparator input / RX_AMP node
- VREF node
- Analog 3.3 V rail near receive chain

Objective:

- Confirm temporal alignment between TX edges and false comparator edges.

### Priority 2: Front-end hardening

- Restore corrected clamp/protection behavior (D8/D9 equivalent corrected implementation)
- Increase comparator hysteresis if currently too low
- Improve local VREF decoupling and analog return isolation from TX switching currents

### Priority 3: Firmware guard strategy (interim)

- Keep strict capture arming (already implemented)
- Increase post-enable guard range for diagnostics and temporary suppression
- Continue robust multi-shot statistics (median/trimmed logic), avoid single-shot trust

### Priority 4: Re-validation gate before acoustic claims

Require all of the following before declaring acoustic TOF valid:

- Reduced baseline noise edge rate
- Coupling test no longer saturated with RX disconnected
- Paired direction deltas stable in sign under fixed geometry
- Open vs blocked rounds show repeatable separation in medians and/or detection rates

## Suggested Acceptance Criteria for "Acoustic Ready"

- Baseline (`N`) mean edge count near zero or very low in listen window
- Coupling (`C`) detection rate substantially below 100% when RX transducer is disconnected
- Paired direction (`P`) produces consistent sign and magnitude trend over repeated runs
- Open/Blocked (`O`/`B`) medians separate beyond jitter overlap for at least one validated route setting

## Test Evidence Snapshot (2026-04-11)

- Baseline noise: persistent, first edges around 3 to 4 us
- Coupling: all combinations 24/24 detections
- Aggressor ranking: `TX_PWM_WITH_ROUTE` > `TX_PWM_ONLY` >> others
- Sweep trend: median timing increases strongly with blanking interval
- Paired/open/blocked: no robust physical-path discrimination yet

## Notes for Next Session

Run order:

1. `N`
2. `C` (with RX transducer physically disconnected)
3. `A`
4. `S`
5. `P`
6. `O` and `B`

Record outputs exactly and compare to this report to detect whether noise source shifts after hardware changes.
