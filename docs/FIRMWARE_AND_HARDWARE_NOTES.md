# Firmware & Hardware Engineering Notes

Consolidated reference for PCB bring-up, flashing, firmware architecture, sync workflow, and robustness assessment.  
Merged from: `PCB_BRINGUP_TROUBLESHOOTING.md`, `pcb_bringup_troubleshooting_record.md`, `ESP32-WROOM_NODE_HARDWARE_REVISION_CHECKLIST.md`, `FIRMWARE_BUILD_LOG.md`, `FIRMWARE_BRINGUP_LOG.md`, `NODE-FIRMWARE_NOTES.md`, `FIRMWARE_SYNC_WORKFLOW_AND_TESTING.md`, `19.4.26 fireware review_sonnet.md`, `ERS381_Bring_Up_Section.md`

---

## Contents

1. [PCB Hardware Bring-Up](#1-pcb-hardware-bring-up)
2. [Flashing Reference](#2-flashing-reference)
3. [Firmware Architecture — Build Log](#3-firmware-architecture--build-log)
4. [Ultrasonic Anemometer Node Firmware Architecture](#4-ultrasonic-anemometer-node-firmware-architecture)
5. [System Bring-Up Log](#5-system-bring-up-log)
6. [Sync Workflow and Testing Strategy](#6-sync-workflow-and-testing-strategy)
7. [Firmware Robustness Assessment (19 April 2026)](#7-firmware-robustness-assessment-19-april-2026)
8. [Field Commissioning Procedure](#8-field-commissioning-procedure)

---

## 1. PCB Hardware Bring-Up

**Power-path and 3.3 V rail fault isolation leading to successful first flash**

### 1.1 Executive Summary

- Root cause identified and confirmed: D8/D9 clamp diode footprint pin mapping error (pins 1 and 2 swapped), causing unintended 3V3-to-GND conduction.
- Interim hardware recovery on current board: D8/D9 removed; 3.3 V rail recovered; board flashing and core bring-up restored.
- Key subsystem validations completed: power gating, RTC alarm gating, charging paths, battery sensing, I2C mux/sensor path, and preliminary ultrasonic diagnostics.
- ESP-NOW field results:
  - 1 m LOS: strong reliability (~98.5% ACK)
  - 30 m LOS: near-perfect reliability (~99.7% ACK)
  - 100 m weak LOS / obstructed: usable but degraded (~84–87% ACK)

**Board status:** Powered and flashed successfully  
**Primary root cause:** D8/D9 clamp diode footprint wired with pins 1 and 2 swapped  
**Interim fix:** Removed D8 and D9; 3V3 rail recovered

### 1.2 Initial Symptoms and Field Observations

| Symptom | Observation | Implication at the time |
|---|---|---|
| 5 V applied to `RAW_BAT` but only ~2.8 V seen after button press | Rail appeared to collapse under load. | Suggested a power-path fault or a downstream overload. |
| Battery/status lights dimmed when connected to `VSYS` | Voltage source looked healthy off-load but sagged when the switched bus was engaged. | Indicated excessive current draw or an unintended conduction path. |
| On-board LED did not illuminate | No normal visual power-on confirmation. | Consistent with the switched 3.3 V rail not coming up correctly. |
| Q2 PMOS gate measured 0 V when button pressed, source 5 V, drain ~2.8 V | Q2 was being commanded on. | Initially made Q2 or the downstream rail the prime suspects. |

### 1.3 Troubleshooting Sequence and Decision Logic

| Step | Test / action | Result | Interpretation |
|---|---|---|---|
| 1 | Checked Q2 high-side PMOS during button press. | Source = 5 V, gate = 0 V, drain = ~2.8 V. | Gate drive was correct; the problem was downstream or across the pass element. |
| 2 | Measured `VSYS` to ground with power removed. | ~14 kΩ (button not pressed). | No hard short was present; fault likely active only when powered. |
| 3 | Bypassed Q2. | Issue remained; `VSYS` still collapsed and external indicators dimmed. | Q2 was ruled out as the root cause. |
| 4 | Compared rail resistances. | 22 V rail ~1 MΩ, 5 V rail ~0.7 MΩ, 3V3 rail ~14 kΩ. | The switched 3.3 V domain became the main suspect. |
| 5 | Investigated keep-alive domain interaction. | Weak ghost voltages observed after press, but only ~177.5 kΩ coupling to `3V3_SYS`/`VSYS`. | Backfeed existed, but it was too weak to explain the main collapse. |
| 6 | Lifted the LDO `EN` pin on the 3V3 regulator. | `VSYS` recovered and behaved normally. | Confirmed that enabling the 3.3 V branch was what pulled the system down. |
| 7 | Injected 3.3 V directly onto `3V3_SYS`. | The rail still showed the fault and the MCU area felt warm. | Fault was definitely on the `3V3_SYS` side, not in the upstream power path. |
| 8 | Thermally inspected the 3.3 V analog input protection area. | D8 became very hot; then D9 was also found to heat strongly. | Pointed to a local clamp network fault rather than a generic MCU short. |
| 9 | Checked BAV99 pin mapping against PCB implementation. | PCB footprint had pins 1 and 2 swapped for D8/D9. | Created an unintended DC path from `3V3_SYS` to GND through two forward-biased diodes. |
| 10 | Removed D8 and D9 completely. | 3.3 V rail recovered; board then flashed successfully. | Root cause confirmed. |

### 1.4 Root Cause Statement

The primary bring-up failure was caused by D8 and D9, implemented as BAV99 dual diodes, having pins 1 and 2 swapped in the PCB realization relative to the intended clamp-to-rails arrangement. As assembled, each device created a DC conduction path from `3V3_SYS` to `GND` through two forward-biased diode junctions. This loaded the switched 3.3 V rail heavily enough to drag `VSYS` down when the LDO was enabled.

**What confirmed it:** Both D8 and D9 heated strongly under 3V3 rail injection. Removing them restored correct 3.3 V behavior. The board could then be flashed successfully.

### 1.5 Corrective Action and Recommended PCB Revision Items

- Remove D8 and D9 for current-board bring-up; replace only after correcting the footprint/pin mapping.
- Audit every other instance where the same diode symbol-footprint pairing was used, because the same library error may be repeated elsewhere on the board.
- Correct the protection diode implementation in the next PCB revision so the intended clamp topology matches the actual BAV99 pinout.
- Add labelled test points for `RAW_BAT`, `VSYS`, `3V3_SYS`, `EN`, and key regulator outputs to reduce ambiguity during future bring-up.
- Keep a short rail-isolation checklist in the project notes: verify upstream path, isolate the suspect rail, inject current-limited power, and use thermal inspection before removing major ICs.

### 1.6 Current Status

**Power state:** 3.3 V rail recovered after D8/D9 removal.  
**Firmware status:** ESP32 board was flashed successfully.  
**Open item:** Protection diodes should be re-implemented correctly in the next board revision or with an interim bodge fix if needed.

### 1.7 Bring-Up Lessons Captured

- A rail that looks acceptable in resistance mode can still fail badly under live power if the fault is through active silicon or a miswired protection network.
- Thermal inspection under low-voltage, current-limited injection is a high-value non-destructive method for localizing power faults.
- A board-level symptom that first looks like a PMOS or regulator problem can actually originate in a small protection network much further downstream.
- Dual-diode packages used as rail clamps must be checked against both the schematic symbol and the physical footprint pinout; a swapped outer-pin mapping can create a direct conduction path.

### 1.8 Detailed Run Logs

#### Run log: 2026-04-07 — EN capacitor (1 µF), no RAW_BAT

- D8/D9 removed; 3.3 V injected to rail (LDO bypass); 5 V not connected to RAW_BAT
- Firmware: `esp32wroom-i2c-scan`
- Flash: PASS (auto-reset upload on COM3)
- UART/boot: PASS
- I2C scan: No devices found — peripheral visibility affected by absent 5 V rail

#### Run log: 2026-04-07 — EN capacitor (1 µF), 5 V connected

- Same board state; 5 V connected to RAW_BAT
- Firmware: `esp32wroom-serial-counter`
- Flash: PASS; counter output `count=0` through `count=11`
- I2C scan: `0x48`, `0x68`, `0x71` (repeatable) — devices visible with 5 V present

#### Observation: TX gate path debug (2026-04-08)

- TX gate firmware confirmed active on `IO25` (`TX_PWM`) with long ON/OFF windows.
- No corresponding activity at the 22 V switching regulator `EN` test point.
- Root cause: diode in TX gate → EN control path installed in reverse orientation; blocks control propagation to 22 V regulator `EN`.
- Corrective action: diode removed, pads bridged as temporary bypass.
- After rework: `EN` reaches 3.3 V; boost output reaches ~18 V (below nominal 22 V — investigation ongoing).

#### Run log: 2026-04-08 — PWR_HOLD gate validation

- Firmware: `esp32wroom-pwrhold-gate`; test pin IO23 (`PWR_HOLD`)
- Result: PASS — state transitions observed in serial; rail probing confirmed gate behavior

#### Run log: 2026-04-08 — I2C mux + SHTC3 validation

- Firmware: `esp32wroom-i2c-mux-shtc3`; SDA=18, SCL=19; mux at `0x71`, SHTC3 at `0x70`
- Result: PASS — valid SHTC3 temperature + RH readings on all mux channels during sequential move test

#### Run log: 2026-04-08 — ADC connector VIN selector + ADS1015 sanity

- Firmware: `esp32wroom-ads1015-analog`; VIN selector toggled between 5 V and 22 V rails
- Result: PASS — both supply rails present at connector; ADS1015 acquisition path alive (floating channels show expected low-level values)

#### Run log: 2026-04-08 — Ultrasonic route-finder A/B + isolation

- Firmware: `esp32wroom-ultrasonic-first-test`
- Pins: `TOF_EDGE=34`, `RX_EN=4`, `MUX_A=16`, `MUX_B=17`, `DRV_N/E/S/W=26/27/14/13`, `TX_PWM=25`
- Geometry: ~10 cm and 20 cm N↔S spacing
- Result: All combos reported `DET=24/24` in both OPEN and BLOCKED conditions — measurement feedthrough/noise dominated, not acoustically discriminative.
- Interpretation: D8/D9 absent → increased comparator overdrive sensitivity; RX front-end protection/threshold cleanup needed before further TOF work.

#### Run log: 2026-04-09 — DS3231 RTC + alarm-driven gate validation

- Firmware: `esp32wroom-ds3231-alarm-10s`; RTC at `0x68`; A1 every 10 s; 8 s hold window for probing
- Result: PASS — RTC time set/readback confirmed; alarm events fire at correct schedule; flag clear and re-arm stable; alarm-driven gate trigger confirmed

#### Run log: 2026-04-10 — Charging path validation

- Solar charging: PASS (confirmed at 5–6 V input)
- USB charging: PASS
- Dual-source (simultaneous solar + USB): PASS

#### Run log: 2026-04-10 — Battery sense (IO35)

- Firmware: `esp32wroom-battery-io35`; divider 220k/100k; calibrated scale 3.6200
- Reference: DMM = 3.88 V; firmware output = 3.8828–3.8857 V
- Result: PASS

#### Run log: 2026-04-24 — SHT40 via PCA9548A mux validation

- Firmware: `esp32wroom-sht40-as7343-mux`; SDA=18, SCL=19; mux at `0x71`, SHT40 at `0x44` on mux ch 0
- AS7343 on mux ch 1 physically damaged — init skipped gracefully (`g_as7343_ok=false`)
- Result: PASS — stable SHT40 readings confirmed:
  - AIR_TEMP ≈ 18.44–18.47 °C, AIR_RH ≈ 44.4–45.0 %
- Notes: 2 s sample interval; no queue, no ESP-NOW; pure sensor read loop

#### Next session focus: noise-source isolation

1. Baseline comparator noise with TX disabled — count edges in fixed listen window.
2. TX-only coupling check with receive transducer disconnected — classify feedthrough vs acoustics.
3. OPEN vs BLOCKED re-check after threshold hardening.

### 1.9 Bring-Up Test Run Template

Use this section for each new board or power-rail configuration so results can be compared quickly.

**Run metadata**
- Date:
- Board ID / serial:
- Hardware revision:
- Firmware target:
- USB-UART bridge and COM port:
- Power source used:
- Notes on rail modifications / bodges:

**Pre-power checks**
- Visual inspection complete (bridges, polarity, orientation):
- Continuity check of main rails to GND:
- Strap pins verified (GPIO0, GPIO2, GPIO12, GPIO15):
- EN RC network fitted (value and footprint):

**Power-up observations**
- RAW input voltage:
- VSYS voltage idle:
- 3V3 voltage idle:
- Any components heating:
- LED indicators state:

**Flash and UART checks**
- esptool chip probe result:
- Flash command used:
- Flash success/failure:
- Serial monitor output summary:

**I2C scan checks**
- SDA/SCL pins used:
- Scan result addresses:
- Repeatability across resets:

**Result classification**
- Status (PASS / FAIL / PARTIAL):
- Primary issue found:
- Immediate corrective action:
- Follow-up action for next revision:

---

## 2. Flashing Reference

### 2.1 ESP32-WROOM Node Hardware Revision Checklist

**Target board:** ESP32-WROOM-32D + CH340C USB-UART  
**Purpose:** Ensure reliable UART flashing, reset behaviour, and deterministic boot configuration.

#### 1. Programming / UART Interface

```
CH340 TXD → ESP U0RXD (GPIO3)
CH340 RXD → ESP U0TXD (GPIO1)
```

Optional 330Ω protection resistors (reduces contention; protects MCU during external UART debugging).

#### 2. Auto-Reset / Auto-Programming Circuit

Use standard Espressif DevKit logic (DTR and RTS).

```
CH340 RTS → transistor → GPIO0
CH340 DTR → transistor → EN
```

Each transistor stage:
```
CH340 signal → 10k → B (NPN)
C → target pin (EN or GPIO0)
E → GND
```

**Important:** EN and GPIO0 circuits must remain **independent**. Reference: ESP32-DevKitC schematic.

#### 3. Reset Circuit

```
3.3V → 10k → EN → 1µF → GND
EN → button → GND
```

Purpose: ensures reset pulse is long enough; prevents unstable boot behaviour.

#### 4. Boot Mode Button

```
3.3V → 10k → GPIO0 → BOOT button → GND
```

Manual flash sequence: Hold BOOT → Press RESET → Release RESET → Release BOOT.

#### 5. ESP32 Strapping Pins (Critical — must not float)

| Pin | Function | Recommended |
|---|---|---|
| GPIO0 | Download mode | 10k pull-up |
| GPIO2 | Boot strap | pull-up |
| GPIO5 | Boot strap | pull-up |
| GPIO12 (MTDI) | Flash voltage strap | pull-down |
| GPIO15 (MTDO) | Boot log config | pull-up |

#### 6. Power and Decoupling

Minimum near module:
- 10 µF bulk capacitor
- 100 nF decoupling capacitors

#### 7. Programming Header (Recommended)

```
GND | 3V3 | TX | RX | EN | BOOT
```

Allows emergency programming with external USB-UART if onboard bridge fails.

#### 8. USB-UART Bridge Power

```
3.3V_SYS → VCC; 100nF decoupling
```

#### 9. Layout Recommendations

- Keep CH340 ↔ ESP32 UART traces short.
- Route EN and GPIO0 carefully — avoid long traces and nearby noisy switching signals.
- Place decoupling capacitors close to the ESP module.

#### 10. Lessons Learned from Previous Board

- Floating strap pins cause random boot modes.
- Incorrect auto-reset circuits prevent flashing.
- Missing reset capacitor causes unreliable resets.
- Native USB can interfere with UART flashing if misconfigured.

#### 11. Pre-Fabrication Sanity Checks

- [ ] GPIO0 pull-up present
- [ ] EN pull-up and capacitor present
- [ ] UART0 routed correctly
- [ ] No strap pins floating
- [ ] Auto-reset transistors wired correctly
- [ ] Power decoupling placed near module

---

## 3. Firmware Architecture — Build Log

*Status: Active tracking document*

### 3.1 Versioning Policy

- **V1:** Original workflow — live-send model before queue-first and scheduled sync changes.
- **V2:** New workflow — queue-first architecture, radio duty-cycling, scheduled sync.
- This section is the running changelog for firmware behaviour changes and design intent.

### 3.2 V1 Workflow Baseline (Previous — logged 2026-04-12)

**Node behaviour:**
- Measured sensors and sent data directly to mothership during alarm-driven wake cycles.
- Single schedule control (wake interval) governed both sampling and transmission cadence.
- WiFi/ESP-NOW stack was generally active in runtime (not strictly duty-cycled in deployed state).

**Mothership behaviour:**
- Ran WiFi AP + web UI continuously.
- Discovery, pair, deploy, unpair, and schedule commands managed via ESP-NOW + web routes.
- Data logged to SD as rows when packets arrived.

**Operational summary:** Simpler live-send model. Limited outage tolerance. No local queue contract for delayed sync replay.

### 3.3 V2 Build Log (Current Direction)

#### 2026-04-12 — Queue-first node and scheduled sync foundation

- Added node local persistent queue module:
  - `firmware/nodes/sensor-node/src/storage/local_queue.h/.cpp`
- Node alarm path now captures sensor samples to local queue first.
- Added `SET_SYNC_SCHED` message to shared protocol for fleet timing alignment.
- Added mothership broadcast support for sync schedule and web control endpoint.
- Added periodic mothership re-broadcast of sync schedule to keep nodes aligned.
- Node deployed behavior now duty-cycles radio:
  - WiFi/ESP-NOW enabled for sync windows and required control interactions only.
- Added mock mothership bring-up target: `firmware/nodes/bringup/bringup_mock_mothership_sync.cpp` (env: `esp32s3-mock-mothership-sync`)

**Build validation:** Mothership, node, and mock mothership envs all built successfully.

**Notes:**
- Current queue flush removes records after successful ESP-NOW send.
- Full application-level ACK semantics planned for stronger delivery guarantees.

### 3.4 V2 Open Items

- Mothership low-power scheduled wake architecture: not yet implemented.
- Many-node upload collision management (per-node slot offsets, jitter windows, airtime fairness): not yet implemented.
- Per-node backlog telemetry and sync health visibility in UI/logs.

### 3.5 BLE Wake App for Mothership — Nice-to-Have

**Concept:**
- Keep mothership WiFi off by default.
- BLE as low-power control channel (ESP32-S3 supports BLE).
- User app sends local wake command → mothership enables WiFi AP for a timed session, then auto-shuts down.

**MVP scope:** App button: Wake Mothership WiFi. Status: sleeping / waking / WiFi ready / session time left. Optional: extend session.

**Security expectations:** Authenticated wake command; rate limiting; wake event logging.

---

## 4. Ultrasonic Anemometer Node Firmware Architecture

### 4.1 System Architecture Summary

Firmware controls a bidirectional ultrasonic TOF measurement pipeline and computes axis wind components using reciprocal timing equations. The sequence coordinates direction control, boosted transmit energy, first-arrival edge capture, and robust filtering before producing wind outputs.

### 4.2 Key Subsystems

1. Ultrasonic transmit control: select direction and generate controlled 40 kHz bursts.
2. Ultrasonic receive timing: gate ring-down and capture first valid comparator edge.
3. MCU scheduling logic: coordinate measurements with low jitter and deterministic timing.
4. Power coordination: enable/disable ~22 V stage around each burst window.
5. Sensor-hub behaviour: integrate ultrasonic reads with other node sensor tasks.
6. Measurement algorithm: solve wind and speed-of-sound from bidirectional TOF pairs.

### 4.3 Measurement Cycle (Per Direction Pair)

1. Select transducer direction (`REL_N`).
2. Enable TX power path (`TX_PWM`).
3. Wait for boost warm-up (~5–20 ms).
4. Generate ultrasonic burst (`DRV_N`, ~40 kHz carrier).
5. Disable transmit.
6. Apply RX blanking window (200–800 µs).
7. Capture first comparator edge on timer input.
8. Repeat in opposite direction.

Multiple cycles are collected and filtered before solving wind.

### 4.4 Sampling and Filtering

- Collect 16–64 samples per direction.
- Median-filter TOF set.
- Compute reciprocal-time velocity solve.
- Derive speed-of-sound estimate for health checks.
- Reject captures outside valid timing gate; flag runs with missing first-arrival capture.

### 4.5 Measurement Equations

For one axis pair with bidirectional TOF (`tAB`, `tBA`) and path length `L`:

$$U_{axis} = \frac{L}{2p}\left(\frac{1}{t_{AB}} - \frac{1}{t_{BA}}\right)$$

Speed-of-sound estimate:

$$c = \frac{L}{2}\left(\frac{1}{t_{AB}} + \frac{1}{t_{BA}}\right)$$

where `p = |sin(θ)|` from pod tilt.

### 4.6 Geometry Constants (Current Validated)

| Parameter | Value |
|---|---|
| Path length L | 0.14670 m |
| Pod tilt θ | ~32.3° |
| p = \|sin(θ)\| | 0.5344 |
| t₀ at 20°C | ~427 µs |

Reference split magnitudes: 1 m/s → ~1.33 µs; 5 m/s → ~6.65 µs; 10 m/s → ~13.30 µs.

### 4.7 Timing and Noise Constraints

- Sub-microsecond timestamp stability strongly preferred.
- RX blanking must suppress TX ring-down edges.
- WiFi activity should be paused during active TOF capture windows.

### 4.8 Calibration and Validation Workflow

1. Zero-wind calibration: capture stationary runs and store axis offsets.
2. Gain calibration: fit linear correction (`Utrue = a × Umeas + b`).
3. Temperature consistency check: compare inferred `c` against `331.3 + 0.606T`.
4. Runtime telemetry: log raw `tAB`, `tBA`, inferred `c`, and solved `Uaxis` for debug.

---

## 5. System Bring-Up Log

### 5.1 Current Status (as of 2026-04-18)

| Subsystem | Status |
|---|---|
| Mothership AP | Working |
| Mothership Web UI | Working |
| Node Discovery | Working — fast refresh |
| ESP-NOW | Working on shared channel |
| BLE | Disabled intentionally |

### 5.2 Known-Good Configuration

**Mothership:**
- Environment: `esp32s3`
- Upload port: `COM9`
- AP mode: Enabled
- AP SSID pattern: `Logger` + `DEVICE_ID` (e.g. `Logger001`)
- AP channel: `11`
- ESP-NOW runtime: Enabled
- BLE GATT: Disabled
- Captive portal: Enabled

**Nodes:**
- Environment: `firmware/nodes/sensor-node` → `esp32wroom`
- Upload port: `COM3` (during bring-up)
- Shared ESP-NOW channel: `11`
- Node identity: auto-derived from MAC; no per-node firmware build variants required

### 5.3 Bring-Up Decisions

1. Use shared RF channel (11) for AP + ESP-NOW coexistence.
2. Keep BLE disabled for stability during this phase.
3. Use MAC-based identity to avoid duplicate logical ID collisions.
4. User-friendly naming/assignment kept in UI metadata, separate from hardware identity.

### 5.4 Flash History

**Mothership (esp32s3 → COM9)** — validated: AP visibility, channel alignment, captive portal, discovery burst, UI refresh.

**Nodes (standard esp32wroom firmware):**

| MAC | User ID | Name |
|---|---|---|
| `D4:E9:F4:94:5D:C4` | 001 | NODE 1 |
| `D4:E9:F4:94:DF:54` | 003 | NODE 3 |
| `D4:E9:F4:92:F9:60` | 002 | NODE 2 |
| `D4:E9:F4:94:E3:8C` | 004 | NODE 4 |

### 5.5 Discovery Validation Snapshot (2026-04-18 20:51)

- Mothership booted clean on AP channel 11; fleet started empty.
- All 4 nodes discovered (`ENV_945DC4`, `ENV_94E38C`, `ENV_92F960`, `ENV_94DF54`).
- Repeated discovery packets from same MACs handled without duplicate-node aliasing.
- `Fleet TIME_SYNC: no PAIRED/DEPLOYED nodes registered` expected while nodes are UNPAIRED.
- Nodes marked asleep after idle timeout is expected behaviour, not a fault.

### 5.6 Pairing/Commissioning Validation (2026-04-18 20:58–21:03)

- One-node-at-a-time commissioning flow used.
- Each node: discovered → user ID/name assigned → paired → time-synced.
- Final registry: 4 nodes (paired/deployed), all saved to NVS.

Key validation lines per node:
- `pairNode(...): PAIR_NODE=OK ... PAIRING_RESPONSE=OK`
- `TIME_SYNC -> <nodeId> ... : OK`

---

## 6. Sync Workflow and Testing Strategy

*Date: 2026-04-21 | Scope: Mothership + sensor-node power, wake, sample, and sync behaviour*

### 6.1 Intended Operating Model

**Mothership (target field behaviour):**
- Mostly off in the field; powers on for scheduled fleet sync windows or user wake triggers.
- During sync windows: WiFi/ESP-NOW available to receive queued uploads from all nodes.
- Writes received records to SD card CSV.

**Node (target field behaviour):**
- At each interval wake: wake from RTC A1 alarm → take sensor readings (radio off) → store to local queue → re-arm A1 → power down.
- At scheduled sync times: wake from RTC A2 alarm → bring up radio → flush queued records to mothership → return to low power.

**Conflict rule:** If sync and sample wakes align, do one combined cycle (sample first, then sync flush, then sleep).

### 6.2 Current Implementation Status

**Implemented (confirmed working):**
1. Node queue-first design — samples to local queue every alarm cycle; flushes only at sync window on marker receipt.
2. Node radio duty-cycling — ESP-NOW/WiFi brought up when needed, shut down after flush.
3. Pull-based config convergence — NODE_HELLO → CONFIG_SNAPSHOT → CONFIG_ACK.
4. Queue overflow policy: DROP_OLDEST.
5. Mothership node-liveness messaging distinguishes sleeping deployed nodes from absent/unpaired cases.
6. Sync-stale recovery is bilateral — node performs local recovery; mothership independently assists stale nodes.

**Partially implemented / gaps:**
1. Mothership low-power operation — currently runs continuously.
2. Many-node sync staggering — no contention management yet.
3. Mothership power-state machine — no scheduled wake/sleep architecture yet.

### 6.3 DS3231 Dual-Alarm Strategy

- **Alarm1 (A1):** data-cycle wake path — fires at `now + data interval`.
- **Alarm2 (A2):** global sync-window wake path — fires at next global sync phase boundary.
- Both A1F and A2F checked and cleared each wake.
- Combined A1+A2 wake: sample first, sync flush second, re-arm both, power down.

**Sync window defaults (bench):**
- Pre-sync wake buffer: 0 s (minute-resolution, RTC armed to slot boundary)
- Post-sync listen buffer: 60 s (SYNC_LISTEN_WINDOW_MS)
- Mothership burst: 3 × broadcasts, 200 ms spacing; fires 5 s into slot boundary

### 6.4 Deploy Workflow

**Mothership side:**
1. Operator submits action `start` with interval.
2. Mothership writes desired config: `wakeIntervalMin`, `syncIntervalMin` (auto-derived as `wakeIntervalMin × 18`), `syncPhaseUnix` (next fleet slot), incremented `configVersion`.
3. If UNPAIRED: sends PAIR_NODE + PAIRING_RESPONSE.
4. Sends DEPLOY_NODE with config + RTC timestamp embedded in payload (atomic — no separate CONFIG_SNAPSHOT race).
5. Sets `gLastSyncIntervalSlot = -1`, persists to NVS (slot guard reset for clean anchor).
6. Marks node DEPLOYED locally; awaits DEPLOY_ACK.

**Node side:**
1. Receives DEPLOY_NODE; sets RTC from payload; marks `rtcSynced=true`, `deployedFlag=true`.
2. Sends DEPLOY_ACK.
3. Executes immediate first sample cycle; appends to queue.
4. Arms A1 to `now + dataInterval`; arms A2 to `syncPhaseUnix` (direct arm if phase is in future; advance to next slot if past).
5. Powers down.

### 6.5 Node Wake Behaviour (Deployed)

| Wake source | Action |
|---|---|
| A1F=1 only | Clear A1 flag; sample to queue; radio stays off; re-arm A1; power down |
| A2F=1 only | Clear A2 flag; bring radio up; send NODE_HELLO; listen 60 s for SYNC_WINDOW_OPEN; flush queue on marker; radio off; re-arm A2; power down |
| A1F=1 + A2F=1 | Sample first; then sync flush; re-arm both; single finalizeWakeAndSleep call; power down |

### 6.6 Sync Timing and Broadcast Design

- **Broadcast:** `SET_SYNC_SCHED` and `SYNC_WINDOW_OPEN` use true ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`) — 3 transmissions total regardless of fleet size.
- **Slot guard:** `gLastSyncIntervalSlot` prevents duplicate triggers within a slot. Reset to -1 on deploy anchor change. Persisted to NVS so mothership reboots do not re-fire.
- **5-second burst offset:** Mothership fires 5 s into the slot boundary, giving nodes ~2.5 s boot + radio-up time.
- **SYNC_AUDIT log:** Emitted every 30 s; shows `mode`, `nextIn`, `lastAgo`, `count`.

### 6.7 Sync-Stale Recovery (2026-04-21)

**Node-side:** If `lastTimeSyncUnix` age exceeds 24 h during a data wake, node runs bounded recovery:
- Brings up ESP-NOW.
- Sends NODE_HELLO + REQUEST_TIME.
- Listens briefly for TIME_SYNC and/or sync marker.
- Flushes queue if marker seen within the recovery deadline.

**Mothership-side:** Infers stale node from contact age/missed wake estimate; sends bounded unicast `SET_SYNC_SCHED` + `TIME_SYNC` as assist (no forced marker injection).

### 6.8 Practical Testing Strategy

**Bench profile (fast feedback):**
- Node interval: 1 min; sync interval: auto-derived (wakeMin × 18 = **18 min**); post-wake window: 1500–3000 ms
- Acceptance: queue depth increases between syncs (18 samples queued across 18 data wakes); drains at sync slot; mothership CSV shows contiguous rows with expected node timestamps; power cut confirmed (gate drain voltage drop).

**Field profile:**
- Node interval: agronomy cadence (e.g. 5 or 10 min); sync: daily or few-times-per-day.
- Acceptance: no data loss over long idle periods; queue growth and flush match expected cadence; end-to-end SD logs consistent across power cycles.

### 6.9 Single-Node Test Matrix

| Test | Steps | Pass Criteria |
|---|---|---|
| SN-1 Deploy + immediate cycle | Reset node; pair; deploy | DEPLOY_ACK; immediate sample queued; A1+A2 armed; power cut scheduled |
| SN-2 Data wake — radio off | Let A1 fire | `dataWake=1 syncWake=0`; no ESP-NOW init; queue depth +1; alarms re-armed |
| SN-3 Sync wake — marker received | Let A2 fire; mothership running | `syncWake=1`; marker seen; queue drains to mothership CSV; queue=0 after flush |
| SN-4 Sync wake — marker missed | Let A2 fire; kill mothership power | `⚠️ Sync marker not seen`; queue unchanged; alarms re-armed; no data loss |
| SN-5 Both alarms together | Set `syncIntervalMin == intervalMin` | Sample + flush in one cycle; single `finalizeWakeAndSleep`; ESP-NOW shutdown called once |
| SN-6 RTC power loss recovery | Pull DS3231 coin-cell | `⚠️ RTC lost power`; `rtcSynced=0`; node requests TIME_SYNC; resumes after mothership responds |
| SN-7 Queue overflow | Block sync >2 h at 5 min interval | `[QUEUE] full; dropping oldest`; after sync resumes, CSV shows no gap larger than kCapacity × interval |
| SN-8 SET_SYNC_SCHED mid-run | Send new sync interval mid-deployment | `[SET_SYNC_SCHED] immediate re-arm`; A2 fires at new interval; A1 unaffected |
| SN-9 Reboot mid-cycle | Pull power during capture | `loadNodeConfig` restores state; checksum detects partial write; no duplicate sensor record |
| SN-10 Power cut model validation | PWR_HOLD_PIN=-1 | Continuous loop; alarm check fires every 1 s; `handleRtcWakeEvents` called on flag set |

### 6.10 4-Node Fleet Test Matrix

| Test | Steps | Pass Criteria |
|---|---|---|
| FN-1 Global sync alignment | Deploy 4 nodes, same interval/phase | All 4 A2 alarms armed to same HH:MM ±1 min |
| FN-2 Wake interval change propagates sync | Change global wake interval from UI | `gSyncIntervalMin` recomputed to `wakeMin × 18`; all 4 nodes receive SET_SYNC_SCHED with new auto-derived syncMin; re-arm logged; configVersion incremented for all 4 |
| FN-3 Fleet simultaneous flush | All 4 wake at same A2; mothership running | All 4 queues flush; no duplicates in CSV; total flush < SYNC_LISTEN_WINDOW_MS per node |
| FN-4 One node out of sync | Power-cut one node mid-cycle; restore | Restored node requests TIME_SYNC; within 2 sync cycles A2 is phase-aligned with fleet |
| FN-5 Unpair one node mid-fleet | Unpair node 2 via web UI | Node 2 receives UNPAIR; stops sending; other 3 unaffected; node 2 absent from subsequent CSV |
| FN-6 Discovery of new node with fleet active | Add 5th node while 4 deployed | New node deployed with same syncIntervalMin + syncPhaseUnix as fleet |
| FN-7 Mothership reboot mid-sync-window | Restart mothership during node sync window | Nodes listen full 60 s; log `⚠️ Sync marker not seen`; re-arm without data loss; mothership reloads registry; next sync resumes |
| FN-8 ESP-NOW collision stress | All 4 flush simultaneously (24 queued each) | All 96 records in CSV; any delivery failure leaves queue non-empty for retry next window |

**Explicit pass/fail criteria:** Any sample captured by node (`🧾 queued` in serial) that does not appear in mothership CSV within 2 sync cycles = **FAIL**. Double-fire of `handleRtcWakeEvents` for same alarm event = **FAIL**. Node radio remaining active indefinitely after sync wake = **FAIL**.

### 6.11 Immediate Recommended Next Steps

1. Keep developing against bench profile until stable.
2. Add explicit mothership power-state design doc (always-on vs scheduled-on).
3. Implement many-node sync staggering before large fleet tests.
4. Add a one-page test checklist and run log template for repeatable validation.

---

## 7. Firmware Robustness Assessment (19 April 2026)

**Reviewer:** Claude Sonnet 4.6  
**Scope:** Node firmware (ESP32-WROOM) + mothership firmware (ESP32-S3)

### 7.1 Findings Summary

All Critical and High findings have been addressed. Medium findings M1–M3, L1–L2 also addressed. Optional items (M4, L3) deferred.

### 7.2 Critical Findings (All Fixed)

#### C1 — Queue pop before delivery confirmation → **Fixed**
**Risk:** Silent, permanent data loss. `esp_now_send` returns `ESP_OK` when packet enters outgoing buffer, not on delivery. Original code called `local_queue::pop()` after `ESP_OK` without waiting for `onDataSent` callback.

**Fix:** Added `waitForSendDelivery(200ms)` — spins on `g_lastSendDone` before popping. Record stays in queue until confirmed delivery.

#### C2 — Deploy race between CONFIG_SNAPSHOT and DEPLOY_NODE → **Fixed**
**Risk:** CONFIG_SNAPSHOT and DEPLOY_NODE were sent as separate ESP-NOW messages with no ordering guarantee. Node could enter power-cut countdown before CONFIG_SNAPSHOT arrived.

**Fix:** Config fields (`wakeIntervalMin`, `syncIntervalMin`, `syncPhaseUnix`, `configVersion`) embedded directly in `deployment_command_t`. Single `esp_now_send` call — race eliminated.

#### C3 — Double A1 arm on boot producing timing drift → **Fixed**
**Risk:** `setup()` called `ds3231ArmNextInNMinutes()` then `armDeploymentWakeAlarms()`, with log printing the first call's time but hardware set to the second.

**Fix:** Single boot arm authorityvia `armDeploymentWakeAlarms()`. Standalone `ds3231ArmNextInNMinutes` call removed from `setup()`.

### 7.3 High Findings (All Fixed)

| Finding | Description | Fix |
|---|---|---|
| H1 | `SET_SCHEDULE` enabled A2IE without programming A2 registers — could cause spurious/never sync wake | `SET_SCHEDULE` handler now calls `armDeploymentWakeAlarms()` atomically |
| H2 | Two-variable volatile sync marker not safely ordered on dual-core ESP32 — possible underflow in timing log | Replaced with single `volatile uint32_t g_syncWindowMarkerMs`; zero = not seen |
| H3 | Daily sync guard advanced `gLastSyncBroadcastEpochDay` even on failed broadcast — day's sync permanently lost | Guard only advances `if (sent || markerSent)`; 15-minute retry window added |
| H4 | Flush had no wall-clock timeout — could consume entire sync listen window | `flushDeadline = windowStart + SYNC_LISTEN_WINDOW_MS - 2000` passed to flush function; loop breaks near deadline |
| H5 | NODE_HELLO was unicast-only — mothership could miss HELLO during brief busy period | `sendNodeHello()` now sends to both unicast MAC and broadcast address |

### 7.4 Medium Findings (All Fixed)

| Finding | Description | Fix |
|---|---|---|
| M1 | `configVersion` as `uint8_t` wraps at 256, blocking all future config updates | Changed to `uint16_t` in all protocol structs, NVS stores, registry, and comparison paths |
| M2 | NVS key collision for node IDs sharing a 14-char prefix | `fnv1a32NodeId()` produces deterministic 9-char hash prefix; legacy key fallback for existing data |
| M3 | `RTC_DATA_ATTR` decorators non-functional in hard power-cut architecture | Removed from all 9 node state/config variables; comment added: NVS is sole persistence |

### 7.5 Low Findings (L1, L2 Fixed; L3 Deferred)

| Finding | Status |
|---|---|
| L1 — Silent queue overflow with no quality flag | Fixed — `QF_DROPPED = 0x0001` defined in `local_queue.h`; set on DROP_OLDEST eviction |
| L2 — `sendSensorData()` dead code | Fixed — removed from node firmware |
| L3 — Back-to-back TIME_SYNC to fleet causes collision potential | Deferred — negligible at ≤8 nodes |

### 7.6 What Is Already Robust

1. State upgrade guard in `registerNode` — explicit block on DEPLOY_ACK promotion when conflicting pending target exists.
2. `pendingTargetState` replay on every node contact — missed command retransmission automatic and bounded.
3. Queue checksummed on NVS write/read — FNV-1a catches partial writes; corrupted blobs reinitialise cleanly.
4. `espnowSendWithRecover` one-shot reinit — handles `ESP_ERR_ESPNOW_NOT_INIT` mid-session.
5. RTC lost-power detection at boot — clears `rtcSynced` + `deployedFlag`, forces re-sync.
6. `g_syncWindowMarkerSeen` reset at start of sync wake — prevents stale marker triggering immediate flush.
7. A2 pre-wake correctly computed from shared `syncPhaseUnix` anchor — all nodes compute same `nextSyncUnix`.
8. Burst broadcast (×3) for `SET_SYNC_SCHED` and `SYNC_WINDOW_OPEN`.
9. `SET_SYNC_SCHED` immediately re-arms both alarms when deployed+synced.
10. Queue `peek + pop` separation — record accessible until `pop` succeeds.

### 7.7 Prioritised Hardening Table (completed)

| Priority | Action | Addresses |
|---|---|---|
| 1 | Delivery-gate pop (200ms callback wait before pop) | C1, H4 |
| 2 | Atomic sync marker (single `uint32_t`) | H2 |
| 3 | Embed config in DEPLOY_NODE struct | C2 |
| 4 | Single arm authority in `armDeploymentWakeAlarms()` | C3, H1, M5 |
| 5 | Flush window deadline passed to flush function | H4 |
| 6 | NODE_HELLO broadcast fallback | H5 |
| 7 | Daily sync retry — only advance guard on success | H3 |
| 8 | `configVersion` widened to `uint16_t` | M1 |
| 9 | NVS key hashing via FNV-1a | M2 |
| 10 | Remove `RTC_DATA_ATTR` | M3 |

### 7.8 Third-Pass: UI, Mothership Orchestration & Fleet Continuity (19 April 2026)

**New Critical:** None found.

**High findings from this pass:**

| Sev | Finding | Recommended Fix |
|---|---|---|
| High | SD write inside `OnDataRecv` callback — SD SPI write (10–100 ms) holds ESP-NOW receive path; packets from other nodes dropped during write | Decouple: push rows to in-RAM FIFO (32 rows) in `OnDataRecv`; drain to SD in main `loop()` |
| High | Sync-trigger slot guards not NVS-persisted — reboot resets `gLastSyncIntervalSlot = -1`, causing spurious immediate sync broadcast | Persist `gLastSyncBroadcastEpochDay` and `gLastSyncBroadcastUnix`; load on boot; add 2-minute boot quiet period |

**Medium UI/Mothership findings:**

| Finding | Recommended Fix |
|---|---|
| Queue depth from NODE_HELLO never shown in UI | Add `lastReportedQueueDepth` to `NodeInfo`; display in node manager |
| AWAKE/ASLEEP hard cap at 45 s regardless of configured interval | Change threshold to ~50% of `wakeIntervalMin × 60s`; or rename to "Recently heard" with relative timestamp |
| Pending command duration not shown | Show age (`millis() - pendingSinceMs`) when `stateChangePending == true` |
| No RTC validity check in deploy path | Add year plausibility guard: `if (year < 2024 || year > 2099) → abort deploy` |
| `buildDataStatusSectionHtml()` scans entire CSV on every root page load | Cache record count and last-row timestamp in a global; update on each `logCSVRow` |
| CONFIG_ACK loss leaves "Config pending" permanently | Add time-based fallback after `kConfigAckTimeoutMs` (e.g. 10 min) |

**Fleet continuity stress scenarios:**

| Scenario | Expected failure if broken |
|---|---|
| CS-1: Mothership reboots 3 min after sync | Spurious second `SYNC_WINDOW_OPEN`; node re-arms from unexpected phase |
| CS-2: Mothership DS3231 not set (reads year 2000); operator deploys node | Node time-syncs to year 2000; no alarms fire; no CSV data ever appears |
| CS-3: 4 nodes flush simultaneously while operator loads web dashboard | SD write blocks `OnDataRecv`; some node packets dropped silently from CSV |

### 7.9 Important Compatibility Note

Node and mothership firmware must be deployed as a **matched pair**. The `deployment_command_t` struct was extended during hardening. Flashing only one side results in mismatched struct sizes and silently malformed deploy commands.

### 7.10 Recommended Bench Tests Before Field Deployment

Execute these four tests before any field deployment:
- **SN-3** — Sync wake with marker received
- **SN-4** — Sync wake with marker missed
- **SN-5** — Both alarms firing together
- **FN-3** — Fleet simultaneous flush

---

### 7.11 Post-Deployment Fixes (24 April 2026)

Two bugs identified during live unpair testing were fixed in node firmware.

#### Bug 1 — `finalizeWakeAndSleep` re-armed alarms after UNPAIR mid sync-window

**Symptom:** Node received UNPAIR during a sync wake flush (via broadcast burst), then `finalizeWakeAndSleep` called `armDeploymentWakeAlarms()` unconditionally — re-enabling both alarm interrupts with the old schedule. A1 fired again at the next data-wake interval with the node in UNPAIRED state, and A2 was re-armed to the next sync slot.

**Root cause:** `finalizeWakeAndSleep` had no guard on deploy state before arming the DS3231.

**Fix:** `finalizeWakeAndSleep` now checks `currentNodeState() == STATE_DEPLOYED && rtcSynced && hasMothershipMAC()` before calling `armDeploymentWakeAlarms()`. If the node is not deployed, it calls `ds3231DisableAlarmInterrupt()` + `ds3231DisableAlarm2Interrupt()` + `clearDS3231_AlarmFlags()` instead. Expected log after fix:
```
🔁 [FINALIZE] Not deployed – alarms disabled, no re-arm
💤 [FINALIZE] Power cut scheduled – reason: wake cycle complete + next alarms armed
```

#### Bug 2 — UNPAIRED/PAIRED loop had no power-cut path

**Symptom:** After waking on a stale A1 alarm in UNPAIRED state, the main loop logged `"🟡 Unpaired – idle…"` every 15 seconds indefinitely. No `schedulePowerCut` was ever called, so the node remained powered until the battery was exhausted.

**Fix:** The loop now tracks idle entry time for UNPAIRED and PAIRED states. After 15 minutes with no deploy, it calls `shutdownEspNow()` + `schedulePowerCut()`. The idle timer resets correctly if the node transitions back to STATE_DEPLOYED. Expected log:
```
⏰ Idle timeout (unpaired) – powering off
```

**Note:** The 4× UNPAIR message processing observed in logs is a harmless side-effect of the 3-burst broadcast — the handler processes each burst copy but the state is already wiped after the first, making subsequent applications idempotent.

**Build status:** Both changes compiled successfully (node `esp32wroom` env, 24 April 2026).

---

### 7.12 FN-3 Test Results and CS-3 Architecture Fix (24 April 2026)

#### FN-3 Test Execution

FN-3 (fleet simultaneous flush, 4 nodes, wakeMin=1, syncMin=5) was run and **failed** — confirming the CS-3 scenario predicted in §7.8.

**Observed behaviour across 3 sync cycles:**

| Cycle | Rows expected | Rows in CSV | Dropped |
|---|---|---|---|
| 1 | 20 | 12 | 8 |
| 2 | 20 | 11 | 9 |
| 3 | 20 | 10 | ~10 |

Approximately 27 rows lost across 3 cycles. Node `ENV_94E38C` showed a "Config updated" badge that never cleared — its `lastNodeTimestamp` was stale because enough of its packets were dropped that cadence inference failed. All dropped rows were from the simultaneous-flush burst phase, not the queue-fill phase, confirming the root cause was SD write latency inside `OnDataRecv`.

#### Root Cause (CS-3 Confirmed)

Calling `SD.open()` / write / `SD.close()` inside the ESP-NOW `OnDataRecv` callback takes 20–50 ms per row. With 4 nodes sending simultaneously, packets arriving during that blocked window were silently discarded by the ESP-NOW driver. Per-row open/close compounded the problem: a 4-node burst of 5 records each = 20 SD operations, each one blocking the receive path.

#### Fix — NODE_SNAPSHOT Architecture

The data path was redesigned end-to-end to eliminate the SD latency problem structurally.

**Protocol change — `node_snapshot_t` (protocol.h):**
- One 124-byte packed struct per node per wake cycle replaces N per-sensor packets.
- All sensor fields as `float` (NaN for sensors not fitted on that node).
- `sensorPresent` bitmask, `seqNum`, `qualityFlags`, `configVersion` included.
- `static_assert(sizeof(node_snapshot_t) == 124)` enforced at compile time.
- New sensor IDs added: `SPECTRAL_415`–`SPECTRAL_680` (×8), `WIND_DIR` (1202), `BAT_V` (4001), `AUX2` (3002).

**Node firmware changes:**
- `captureSensorsToQueue()`: builds one `node_snapshot_t` per wake, maps sensor ID → struct field via switch, NaN-initialises all floats, calls `local_queue::enqueue(snap)`.
- `flushQueuedToMothership()`: peeks `node_snapshot_t`, tags `configVersion`, sends 124-byte packet via `espnowSendWithRecover`.

**Node queue redesign (local_queue.h / local_queue.cpp):**
- Queue now stores raw `node_snapshot_t` structs (124 B each) instead of pre-formatted strings (320 B each).
- Capacity increased from 12 → **24 slots** (NVS blob ~3004 B, well under the 4000 B key limit).
- kMagic = `0x4E514D32`, kVersion = 3.
- `static_assert(sizeof(QueueBlob) < 4000)` enforced.

**Mothership changes (espnow_manager.cpp):**
- Old 10 KB pre-formatted string queue removed.
- New `SnapEntry` ring buffer: `g_snapQueue[128]`, each entry holds 124-byte snapshot + MAC string + CSV ID/name + timestamps. Enqueue/dequeue protected by critical section.
- `OnDataRecv` callback: enqueues `SnapEntry` in ~1 µs — no SD access, does not block the receive path.
- `drainCsvQueueToSd()` (called from `loop()`): opens SD once → writes all queued entries → closes once. Eliminates per-row open/close overhead.
- `appendFloat()` helper writes `"NaN"` for absent sensors.
- Deferred UNPAIR dispatch: `kUnpairFlushSettleMs = 1500 ms`. UNPAIR suppressed while packets are flowing; dispatched from `espnow_loop` after 1.5 s packet silence.

**Mothership CSV format (sd_manager.cpp):**
New wide-format header — one row per wake event, 30 columns:
`ms_datetime`, `ms_sync_unix`, `node_id`, `node_name`, `node_mac`, `fw_id`, `node_datetime`, `node_unix`, `bat_v`, `air_temp_c`, `air_hum_pct`, `spectral_415nm`–`spectral_680nm` (×8), `wind_speed_ms`, `wind_dir_deg`, `soil1_vwc`, `soil1_temp_c`, `soil2_vwc`, `soil2_temp_c`, `aux1`, `aux2`, `sensor_present`, `quality_flags`, `seq_num`.

**Effective packet reduction:** 4 nodes × N sensors/wake → 4 nodes × 1 packet/wake. A 20-node fleet produces 20 packets per sync vs 250+ previously.

#### Queue Safety Rule

With a 24-slot queue and 2 slots held as headroom, the maximum safe number of snapshots between syncs is **22**.

The enforced constraint is:

$$\text{syncMin} \leq \text{wakeMin} \times 22$$

Equivalently, the minimum safe wake interval for a given sync period is $\lceil \text{syncMin} / 22 \rceil$ minutes.

Rather than a runtime clamp, queue overflow risk is eliminated structurally via the **K=18 auto-derive rule**: `syncMin = wakeMin × 18`. This always lands at 82% of the theoretical 22-wake limit, giving a 4-wake soft headroom buffer and making a correctly-configured node incapable of overflowing its queue under normal operation.

`computeAutoSyncMin(wakeMin)` in `mothership/src/main.cpp` returns `(wakeMin > 0) ? (wakeMin * kSyncFillK) : 0` where `kSyncFillK = 18`. This is called in:
- `setup()` — immediately after `loadWakeIntervalFromNVS()`.
- `handleSetWakeInterval()` — HTTP POST handler for the global wake interval control.
- The `set_wake_interval` WebSocket command handler.

#### UI Safety Enforcement

Sync interval is **never user-configurable**. The main page shows a read-only auto-sync info panel derived from the current wake interval (e.g. *"Sync every 90 min (auto: wake × 18 — ~75% queue fill, 4+ wakes headroom)"*). No form, no select, no manual entry.

The per-node config page (`/node-config`) similarly shows the derived sync interval as a read-only display alongside the fleet-global wake interval. There is no per-node sync interval override.

Queue overflow is structurally impossible when using the standard scheduling path: at K=18, a node would need to miss 6 consecutive sync windows (accumulating 24+ wakes) before overflow could occur — safely outside the 22-wake hard limit.

**Build status:** All changes compiled successfully (mothership `esp32s3` env + node `esp32wroom` env, 24 April 2026).

---

## 8. Field Commissioning Procedure

*Adapted from methods documentation (Section 4.4 — Bring-Up and Commissioning Workflow)*

To support reproducibility across research teams with varying electronics experience, system bring-up is formalised as a staged commissioning workflow conducted before ecological deployment. The objective is not sensor calibration against reference-grade instruments, but verification that each node and the mothership can operate as an integrated measurement system under realistic field constraints.

### 8.1 Bring-Up Goals

1. Confirm electrical and communication integrity of each assembled node.
2. Verify that core sensors produce plausible, stable outputs.
3. Confirm reliable node discovery, pairing, and deployment through the mothership interface.
4. Verify scheduled wake/sync behaviour and data logging continuity.
5. Record a repeatable pass/fail record for each unit prior to field use.

### 8.2 Staged Procedure

**Stage A — Hardware and Power Integrity Check**
- Visual inspection of solder joints, connector seating, enclosure seals, and cable routing.
- Verification of supply rails and regulator behaviour under expected load.
- Confirmation of microcontroller boot, serial output, and stable reset behaviour.

**Stage B — Sensor and Interface Verification**
- I2C bus scan and device detection for all connected digital sensors.
- Analog channel checks for expected range and response direction (soil channels where fitted).
- Functional response checks: hand shading for PAR, ambient thermal change for air temperature/humidity.
- For ultrasonic hardware: confirm acquisition pathway functionality and timing stability under bench conditions.

**Stage C — Network Bring-Up and Control-Path Validation**
- Node discovery from mothership dashboard.
- Pairing and deployment transitions through node lifecycle states.
- Confirm schedule commands (wake/sync intervals) propagated correctly and retained after reset.
- Time synchronisation checks between mothership and nodes.

**Stage D — Logging and Persistence Checks**
- Verify measurements transmitted and written to central CSV logs.
- Confirm records include expected node/sensor metadata.
- Power-cycle test to confirm deterministic restart and state persistence.
- Short unattended run to check for missed cycles, stalled logging, or communication dropouts.

### 8.3 Pass Criteria

A unit is considered bring-up complete only when all of the following are satisfied:
- Node can be discovered, paired, deployed, and reverted without manual firmware intervention.
- Core sensors produce plausible non-null signals over repeated acquisition cycles.
- Data packets received and archived by mothership without sustained packet-loss.
- Scheduled operation persists across reset/power interruption.
- No recurring watchdog resets, bus lockups, or storage write failures during commissioning window.

### 8.4 Failure Handling

Failures classified into three categories:
- **Assembly issues** (wiring, connector, solder defects)
- **Configuration issues** (addressing, schedule settings, node state mismatches)
- **Firmware/runtime issues** (communication edge cases, queue/logging behaviour)

Units failing any stage: remove from deployment, correct, re-test from Stage A. Full re-run approach avoids partial acceptance of units with unresolved upstream issues.

### 8.5 Documentation Outputs

Each unit generates a brief commissioning record containing:
- Hardware identifier and firmware build information.
- Date/time of bring-up and operator initials.
- Stage outcomes (A–D), pass/fail status, and observed anomalies.
- Corrective actions applied (if any).
- Final deployment readiness decision.

This record creates an auditable link between physical unit history and field data streams, supporting transparent replication.

### 8.6 Scope of Interpretation

The bring-up process establishes functional readiness and systems integration confidence. It does not replace formal metrological calibration, uncertainty quantification, or long-duration durability testing. Those activities are separate components of full validation.
