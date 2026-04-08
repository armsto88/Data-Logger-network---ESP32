# PCB Bring-Up Troubleshooting Record

**Power-path and 3.3 V rail fault isolation leading to successful first flash**

**Board status:** Powered and flashed successfully  
**Primary root cause:** D8/D9 clamp diode footprint wired with pins 1 and 2 swapped  
**Interim fix:** Removed D8 and D9; 3V3 rail recovered

This note documents the first bring-up troubleshooting sequence for the custom PCB, from the initial power-path symptom through to identification of the failed protection network and successful firmware flashing. The wording is structured so it can be reused in a methods appendix, lab notebook, or internal design record.

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

**What confirmed it:** Both D8 and D9 heated strongly under 3V3 rail injection. Removing them restored correct 3.3 V behaviour. The board could then be flashed successfully.

## Corrective action and recommended PCB revision items

- Remove D8 and D9 for current-board bring-up; replace only after correcting the footprint/pin mapping.
- Audit every other instance where the same diode symbol-footprint pairing was used, because the same library error may be repeated elsewhere on the board.
- Correct the protection diode implementation in the next PCB revision so the intended clamp topology matches the actual BAV99 pinout.
- Add labelled test points for `RAW_BAT`, `VSYS`, `3V3_SYS`, `EN`, and key regulator outputs to reduce ambiguity during future bring-up.
- Keep a short rail-isolation checklist in the project notes: verify upstream path, isolate the suspect rail, inject current-limited power, and use thermal inspection before removing major ICs.

## Current status

**Power state:** 3.3 V rail recovered after D8/D9 removal.  
**Firmware status:** ESP32 board was flashed successfully.  
**Open item:** Protection diodes should be re-implemented correctly in the next board revision or with a bodge fix if needed.

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
