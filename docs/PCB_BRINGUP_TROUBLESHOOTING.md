# PCB Bring-Up Troubleshooting Record

**Power-path and 3.3 V rail fault isolation leading to successful first flash**

## Executive summary

- Root cause identified and confirmed: D8/D9 clamp diode footprint pin mapping error (pins 1 and 2 swapped), causing unintended 3V3-to-GND conduction.
- Interim hardware recovery on current board: D8/D9 removed; 3.3 V rail recovered; board flashing and core bring-up restored.
- Key subsystem validations completed: power gating, RTC alarm gating, charging paths, battery sensing, I2C mux/sensor path, and preliminary ultrasonic diagnostics.
- ESP-NOW field results:
	- 1 m LOS: strong reliability (about 98.5% ACK)
	- 30 m LOS: near-perfect reliability (about 99.7% ACK)
	- 100 m weak LOS / obstructed: usable but degraded (about 84% to 87% ACK)

**Board status:** Powered and flashed successfully  
**Primary root cause:** D8/D9 clamp diode footprint wired with pins 1 and 2 swapped  
**Interim fix:** Removed D8 and D9; 3V3 rail recovered

This note documents the first bring-up troubleshooting sequence for the custom PCB, from the initial power-path symptom through identification of the failed protection network and successful firmware flashing. The content is structured so it can be reused in a methods appendix, lab notebook, or internal design record.

## Initial symptoms and field observations

| Symptom | Observation | Implication at the time |
|---|---|---|
| 5 V applied to `RAW_BAT` but only ~2.8 V seen after button press | Rail appeared to collapse under load. | Suggested a power-path fault or a downstream overload. |
| Battery/status lights dimmed when connected to `VSYS` | Voltage source looked healthy off-load but sagged when the switched bus was engaged. | Indicated excessive current draw or an unintended conduction path. |
| On-board LED did not illuminate | No normal visual power-on confirmation. | Consistent with the switched 3.3 V rail not coming up correctly. |
| Q2 PMOS gate measured 0 V when button pressed, source 5 V, drain ~2.8 V | Q2 was being commanded on. | Initially made Q2 or the downstream rail the prime suspects. |

## Troubleshooting sequence and decision logic

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

## Root cause statement

The primary bring-up failure was caused by D8 and D9, implemented as BAV99 dual diodes, having pins 1 and 2 swapped in the PCB realization relative to the intended clamp-to-rails arrangement. As assembled, each device created a DC conduction path from `3V3_SYS` to `GND` through two forward-biased diode junctions. This loaded the switched 3.3 V rail heavily enough to drag `VSYS` down when the LDO was enabled.

**What confirmed it:** Both D8 and D9 heated strongly under 3V3 rail injection. Removing them restored correct 3.3 V behavior. The board could then be flashed successfully.

## Corrective action and recommended PCB revision items

- Remove D8 and D9 for current-board bring-up; replace only after correcting the footprint/pin mapping.
- Audit every other instance where the same diode symbol-footprint pairing was used, because the same library error may be repeated elsewhere on the board.
- Correct the protection diode implementation in the next PCB revision so the intended clamp topology matches the actual BAV99 pinout.
- Add labelled test points for `RAW_BAT`, `VSYS`, `3V3_SYS`, `EN`, and key regulator outputs to reduce ambiguity during future bring-up.
- Keep a short rail-isolation checklist in the project notes: verify upstream path, isolate the suspect rail, inject current-limited power, and use thermal inspection before removing major ICs.

## Current status

**Power state:** 3.3 V rail recovered after D8/D9 removal.  
**Firmware status:** ESP32 board was flashed successfully.  
**Open item:** Protection diodes should be re-implemented correctly in the next board revision or with an interim bodge fix if needed.

## Active test note (current board)

The next experiment will keep the existing board setup unchanged and add an EN delay capacitor to evaluate reset/programming stability.

Current board configuration before EN capacitor test:

- D8 and D9 are removed.
- 3.3 V is injected directly onto the 3V3 rail (LDO bypassed).
- 5 V is applied to RAW_BAT.
- USB switch is set to PROG.
- Same board and wiring are retained to isolate only the EN capacitor effect.

Planned observation focus:

- Flash entry reliability (auto-reset/manual BOOT sequence behavior).
- UART stability during upload.
- Boot consistency after reset and power-cycle.

### Run log: 2026-04-07 (1 uF EN cap, same board)

- Setup retained:
	- D8/D9 removed
	- 3.3 V injected directly to 3V3 rail (LDO bypass)
	- 5 V not connected to RAW_BAT for this run
	- USB switch set to PROG
	- EN capacitor fitted: 1 uF
- Firmware flashed: `esp32wroom-i2c-scan`
- Flash result: PASS (auto-reset upload succeeded on COM3)
- UART/boot result: PASS (normal boot and scanner startup)
- I2C scan result: No devices found (repeated scans)

Interpretation note:

- Auto-flash and serial boot behavior are stable in this configuration.
- I2C device visibility changed versus earlier scans; likely wiring/power-path state on peripherals or bus connectivity, not core flash path.

Next run condition:

- Repeat the same flash + I2C scan workflow with 5 V connected to RAW_BAT.

### Run log: 2026-04-07 (1 uF EN cap, same board, 5 V connected)

- Setup retained:
	- D8/D9 removed
	- 3.3 V injected directly to 3V3 rail (LDO bypass)
	- 5 V connected to RAW_BAT
	- USB switch set to PROG
	- EN capacitor fitted: 1 uF
- Firmware flashed: `esp32wroom-serial-counter`
- Flash result: PASS (auto-reset upload succeeded on COM3)
- UART/boot result: PASS (normal boot and incrementing counter output)
- Counter output observed: `count=0` through `count=11`

Interpretation note:

- Auto-flash remains stable with 5 V connected to RAW_BAT in this board state.

### Run log: 2026-04-07 (1 uF EN cap, same board, 5 V connected, I2C scan)

- Setup retained:
	- D8/D9 removed
	- 3.3 V injected directly to 3V3 rail (LDO bypass)
	- 5 V connected to RAW_BAT
	- USB switch set to PROG
	- EN capacitor fitted: 1 uF
- Firmware flashed: `esp32wroom-i2c-scan`
- Flash result: PASS (auto-reset upload succeeded on COM3)
- UART/boot result: PASS (normal boot and scanner startup)
- I2C scan result: `0x48`, `0x68`, `0x71` (repeatable over multiple scans)

Interpretation note:

- With 5 V connected to RAW_BAT, I2C devices are detected reliably.
- Combined with the prior no-5V run, this indicates the 5 V rail state changes peripheral visibility while flash stability remains good.

### Observation: TX gate path debug (2026-04-08)

- TX gate firmware confirmed active on `IO25` (`TX_PWM`) with long ON/OFF windows.
- `TX_PWM` test pad shows expected 3.3 V logic activity.
- Signal is also observed near `TC4427EOA` input region.
- No corresponding activity observed at the 22 V switching regulator `EN` test point.

Confirmed root cause update:

- A diode in the TX gate to EN control path is installed in reverse orientation.
- This blocks expected control propagation to the 22 V regulator `EN` node.

Immediate corrective action:

- Rework diode orientation to match intended schematic polarity.
- Re-run TX gate firmware (`esp32wroom-txpwm-gate`) and verify:
	- `EN` node toggles in sync with TX gate state.
	- 22 V rail follows ON/OFF gate windows.

Rework status update:

- Diode removed and pads bridged as a temporary bypass fix.
- Measured `EN` on 22 V regulator now reaches 3.3 V.
- Measured boost output now reaches approximately 18 V on the 22 V rail node.

Interpretation:

- The TX control path to regulator `EN` is now functional.
- Boost stage is switching and producing high voltage, though currently below nominal 22 V target.

### Run log: 2026-04-08 (PWR_HOLD gate validation)

- Firmware flashed: `esp32wroom-pwrhold-gate`
- Test pin: `IO23` (`PWR_HOLD`)
- Pattern used: ON/OFF gate toggling with long dwell windows
- Result: PASS
	- `PWR_HOLD` state transitions observed in serial output
	- Rail probing confirmed hold-gate behavior is responsive to firmware control

### Run log: 2026-04-08 (I2C mux + SHTC3 validation)

- Firmware flashed: `esp32wroom-i2c-mux-shtc3`
- I2C wiring: SDA=18, SCL=19
- Mux address: `0x71`
- Sensor address: `0x70` (SHTC3)
- Driver path: SparkFun SHTC3 library integrated for measurement calls

Result summary:

- Baseline checks confirmed 3.3 V present at I2C connectors.
- Single-channel baseline produced valid SHTC3 readings on active channel (temperature + RH).
- Sequential validation completed by moving the same sensor across mux channels.
- User-confirmed outcome: valid data obtained on all mux channels during per-channel move test.

### Run log: 2026-04-08 (ADC connector VIN selector + ADS1015 sanity)

- Firmware flashed: `esp32wroom-ads1015-analog`
- Test condition: physical VIN selector switch on ADC connectors toggled between 5 V and 22 V input rails
- Observation: both 5 V and 22 V rails present at connector VIN when selected
- ADS1015 readings: stable low-level floating values on unconnected analog inputs (expected behavior for floating channels)

Interpretation:

- Connector VIN selector hardware is functioning for both source rails.
- ADS1015 acquisition path is alive and reading channels correctly; next step should use known driven voltages to validate scaling/linearity.

### Run log: 2026-04-08 (Ultrasonic route-finder A/B + isolation)

- Firmware flashed: `esp32wroom-ultrasonic-first-test`
- Test mode: command-driven rounds (`OPEN`/`BLOCKED`) with per-combo scoring
- Pin map under test: `TOF_EDGE=34`, `RX_EN=4`, `MUX_A=16`, `MUX_B=17`, `DRV_N/E/S/W=26/27/14/13`, `TX_PWM=25`
- Geometry tested: approximately 10 cm and 20 cm spacing (N <-> S transducers)
- Isolation test: one cable removed from South transducer receive side

Observed behavior summary:

- All `REL/DRV` combinations repeatedly reported `DET=24/24` in both `OPEN` and `BLOCKED` conditions.
- `listen_only` also repeatedly reported events, including early edges.
- Median TOF values shifted only modestly and inconsistently between `OPEN` and `BLOCKED` conditions.
- With one South transducer cable removed, detections still remained saturated (`24/24`) with similar TOF/jitter bands.

Interpretation:

- Current capture is feedthrough/noise dominated; not yet a clean first-acoustic-arrival measurement.
- Direction/polarity discrimination cannot be trusted yet because all combinations look similarly valid.
- The RX front-end state likely contributes: D8/D9 protection diodes are currently removed due to earlier footprint/pinout fault.
- Running without the intended clamp/protection network can increase comparator overdrive sensitivity and TX->RX coupling susceptibility.

Short ultrasonic testing summary:

- Ultrasonic TX/RX chain is active and repeatable, but measurements are not yet acoustically discriminative.
- Data indicates strong non-acoustic triggering path (electrical feedthrough and/or comparator threshold sensitivity).
- Next work should prioritize RX front-end protection/threshold cleanup before further TOF accuracy validation.

### Run log: 2026-04-09 (DS3231 RTC + alarm-driven gate validation)

- Firmware flashed: `esp32wroom-ds3231-alarm-10s`
- RTC device: DS3231 at `0x68`
- Test mode: set RTC time from firmware build time, arm Alarm1 every 10 s, clear/re-arm in loop
- Hardware topology note: DS3231 alarm line is not connected to ESP32 GPIO; it drives the gate path directly (active-low at DS3231 output, then inverted in hardware for gate trigger)
- Hold-window test: alarm active hold before clear set to `8000 ms` for voltage probing

Observed behavior summary:

- RTC time set and readback confirmed in serial output.
- Alarm events fired repeatedly at expected schedule points.
- Alarm flag clear and re-arm behavior confirmed each cycle.
- Extended hold window observed in runtime logs (`holding active for 8000 ms before clear`), enabling gate-path voltage probing.

Result:

- PASS: RTC functionality confirmed.
- PASS: alarm-driven power gate trigger path behavior confirmed.

### Run log: 2026-04-10 (Charging path validation: solar + USB)

- Solar charging path: PASS
	- Charging confirmed when solar input is in the 5 V to 6 V range.
- USB charging path: PASS
	- USB charging confirmed in both switch positions tested.
- Dual-source condition: PASS
	- Charging behavior remained functional when solar and USB sources were applied simultaneously.

Interpretation:

- Both charging circuits are operational on this board.
- Solar input requires practical minimum headroom near 5 V before charge behavior is observed.
- No immediate contention/fault behavior observed under simultaneous USB + solar charging during this test.

### Run log: 2026-04-10 (Battery sense validation on IO35)

- Firmware flashed: `esp32wroom-battery-io35`
- ADC pin under test: `IO35`
- Divider network: top `220k`, bottom `100k`
- Calibrated divider scale used in firmware: `3.6200`
- Reference measurement: DMM battery voltage `3.88 V`
- Serial measurement after calibration: `3.8828 V` to `3.8857 V`

Result:

- PASS: battery voltage measurement path on `IO35` validated and calibrated.
- Firmware battery estimate now matches DMM within a few millivolts in this operating condition.

### Next session focus: noise-source isolation plan

Stop functional TOF validation for now and run targeted noise isolation only.

Priority sequence:

1. Baseline comparator noise with TX disabled

- Keep RX path enabled and count comparator edges in a fixed listen window.
- Record edge count and first-edge timing over multiple repeats.

2. TX-only coupling check with receive transducer disconnected

- Enable TX burst path while keeping receive acoustic path physically disconnected.
- If detections remain high, classify as electrical feedthrough dominated.

3. OPEN vs BLOCKED re-check after threshold hardening

- Increase blanking and minimum valid TOF thresholds.
- Re-run one OPEN and one BLOCKED round at fixed geometry.
- Require a clear detection-rate or median/jitter separation before resuming functional TOF testing.

4. Protection network review (D8/D9 context)

- Current board runs with D8/D9 removed due to verified footprint pinout fault.
- Treat absence of clamps as a likely contributor to overdrive/noise susceptibility.
- Prioritize corrected clamp implementation (or temporary bodge-equivalent behavior) before final ultrasonic validation.

5. Escalation criterion

- If noise still dominates after firmware threshold/gating changes, move to scope-based probing of RX mux output, amplifier stages, and comparator input/output to locate dominant coupling node.

## Bring-up lessons captured from this fault

- A rail that looks acceptable in resistance mode can still fail badly under live power if the fault is through active silicon or a miswired protection network.
- Thermal inspection under low-voltage, current-limited injection is a high-value non-destructive method for localizing power faults.
- A board-level symptom that first looks like a PMOS or regulator problem can actually originate in a small protection network much further downstream.
- Dual-diode packages used as rail clamps must be checked against both the schematic symbol and the physical footprint pinout; a swapped outer-pin mapping can create a direct conduction path.

## Bring-up test run template

Use this section for each new board or power-rail configuration so results can be compared quickly.

### Run metadata

- Date:
- Board ID / serial:
- Hardware revision:
- Firmware target:
- USB-UART bridge and COM port:
- Power source used:
- Notes on rail modifications / bodges:

### Pre-power checks

- Visual inspection complete (bridges, polarity, orientation):
- Continuity check of main rails to GND:
- Strap pins verified (GPIO0, GPIO2, GPIO12, GPIO15):
- EN RC network fitted (value and footprint):

### Power-up observations

- RAW input voltage:
- VSYS voltage idle:
- 3V3 voltage idle:
- Any components heating:
- LED indicators state:

### Flash and UART checks

- esptool chip probe result:
- Flash command used:
- Flash success/failure:
- Serial monitor output summary:

### I2C scan checks

- SDA/SCL pins used:
- Scan result addresses:
- Repeatability across resets:

### Result classification

- Status (PASS / FAIL / PARTIAL):
- Primary issue found:
- Immediate corrective action:
- Follow-up action for next revision:

## 22 V rail validation procedure

This procedure verifies that the ultrasonic TX boost rail behaves correctly and only enables when commanded.

### Why this matters

- Architecture notes define the ~22 V rail as a gated TX domain.
- Firmware notes define `TX_PWM` as the control signal that enables this stage before burst generation.
- Expected behavior is: rail enabled for TX windows, then disabled for RX windows.

### Safety setup

- Use a bench supply with current limit enabled for initial tests.
- Start with low current limit and increase gradually only if rails look healthy.
- Keep probe grounds short and use a stable board ground reference.
- Do not leave the 22 V stage forced on for long soak tests until thermal behavior is known.

### Static pre-checks (power off)

- Measure resistance from `22V` net to `GND` and log the value.
- Compare against known-good board trend rather than a single absolute threshold.
- Verify no unintended continuity from `22V` net into low-voltage domains.

### Dynamic checks (power on)

1. Bring board up in known-good flash configuration (USB switch in `PROG`, stable 3.3 V/5 V setup).
2. Probe these points with scope or DMM + scope:
	 - `22V` rail node (boost output)
	 - `TX_PWM` control net (if accessible)
3. Trigger a firmware flow that performs TX bursts.
4. Confirm sequence:
	 - `TX_PWM` asserts
	 - 22 V rail rises after warm-up delay
	 - burst window occurs
	 - 22 V rail drops back after TX window

### Pass/fail criteria

- PASS:
	- 22 V rail rises only during TX-enable windows.
	- Rail does not remain latched on between bursts.
	- No excessive droop on 3.3 V during 22 V activity.
- FAIL examples:
	- 22 V never rises when TX is commanded.
	- 22 V is always on regardless of TX command.
	- Large coupling/noise causes resets or UART instability.

### If no TX firmware is available yet

- Add a temporary bring-up firmware that periodically toggles `TX_PWM` with a visible interval (for example 1 s ON / 2 s OFF) and no ultrasonic burst.
- Then verify the 22 V rail follows the gate command cleanly.
- After verification, revert to normal measurement firmware.

## Run log: 2026-04-11 (ESP-NOW range and reliability characterization)

- Firmware targets:
	- TX: `esp32wroom-espnow-range-tx`
	- RX: `esp32wrover-espnow-range-rx`
- Test window: 60 s timed runs
- TX packet interval: 100 ms
- Payload: 64 bytes
- Trigger model: `G` on TX starts local timed run and sends remote start control to RX
- Monitor stabilization note:
	- Most reliable monitor command on COM3 was `device monitor -p COM3 -b 115200 --dtr 0 --rts 0`

### Firmware/statistics fix applied during test session

- Issue found:
	- `send_ok_pct` could exceed 100% because startup control packets were counted in send callbacks while excluded from the `sent` data counter.
- Fix applied:
	- Control-start callback events are tracked separately and excluded from data send success/fail counters.
- Verification:
	- Post-fix TX runs show `send_ok_pct` pinned to 100.0 with `send_fail=0` under clean links.

### Measured results summary

1) 1 m baseline (LOS)

- TX final summary:
	- `sent=595`
	- `send_ok=595` (`send_ok_pct=100.0`)
	- `ack_rx=586` (`ack_pct=98.5`)
	- `rtt_ms_avg=5.7`, `rtt_ms_max=22`

2) 30 m LOS

- TX final summary:
	- `sent=595`
	- `send_ok=595` (`send_ok_pct=100.0`)
	- `ack_rx=593` (`ack_pct=99.7`)
	- `rtt_ms_avg=6.7`, `rtt_ms_max=31`

3) 100 m (reduced LOS quality), run A

- TX final summary:
	- `sent=595`
	- `send_ok=595` (`send_ok_pct=100.0`)
	- `ack_rx=509` (`ack_pct=85.5`)
	- `rtt_ms_avg=6.1`, `rtt_ms_max=27`

4) 100 m (reduced LOS quality), run B

- TX final summary:
	- `sent=595`
	- `send_ok=595` (`send_ok_pct=100.0`)
	- `ack_rx=519` (`ack_pct=87.2`)
	- `rtt_ms_avg=6.2`, `rtt_ms_max=25`

5) 100 m with wall obstruction

- TX final summary:
	- `sent=595`
	- `send_ok=595` (`send_ok_pct=100.0`)
	- `ack_rx=504` (`ack_pct=84.7`)
	- `rtt_ms_avg=6.4`, `rtt_ms_max=27`

### Interpretation

- ESP-NOW link and firmware are validated as functional and stable.
- Short-to-mid LOS range (1 m to 30 m) shows near-perfect reliability.
- At 100 m in weaker or obstructed paths, reliability drops into the mid-80% range.
- TX path health remained strong in all runs (`send_fail=0`), indicating losses are RF propagation/channel effects rather than TX firmware transport faults.

### Practical deployment guidance from this dataset

- Strong-link operating band (current hardware/layout): LOS up to at least 30 m.
- Usable but degraded band in difficult paths: around 100 m with weak LOS and/or wall obstruction.
- For delivery-critical telemetry at harder range:
	- add higher-level retries/queueing,
	- log ACK-rate trends,
	- and verify with longer soak tests under representative interference conditions.

## Ultrasonic hardware tweaks TODO (next revision)

- [ ] Re-implement RX input clamp stage with corrected diode mapping and series resistor placement.
- [ ] Add hardware blanking of comparator digital output (`TOF_EDGE`) during TX burst and blanking window.
- [ ] Add optional analog mute/clamp footprint on RX path for TX-period overload suppression.
- [ ] Redesign `VREF` network with lower divider impedance and local decoupling (`100 nF + 1 uF`, optional `4.7 uF`).
- [ ] Evaluate buffered `VREF` distribution for analog section stability.
- [ ] Increase comparator hysteresis to intentional tens-of-mV range; add tuning resistor footprints.
- [ ] Partition layout into quiet RX analog zone and noisy TX/boost zone.
- [ ] Feed analog receive section from filtered `3V3_A` rail (bead/resistor + local decoupling).
- [ ] Force RX-disabled state to a true quiet condition (no floating analog nodes, no spurious comparator edges).
- [ ] Add test points for: `TX_PWM`, `TX_PULSE`, `PWM_5V`, `22V_SYS`, `3V3_A`, `VREF`, `RX_HOT`, `RX_IN`, `RX_AMP`, `TOF_EDGE`.
- [ ] Add optional tuning footprints: comparator RC filter, hysteresis options, TX snubber, TX damping resistor, analog mute transistor.
- [ ] Re-run bring-up acceptance checks after rework: baseline noise (`N`), RX-disabled baseline (`D`), coupling (`C`), aggressor (`A`), sweep (`S`), paired direction (`P`), open/blocked (`O`/`B`).
