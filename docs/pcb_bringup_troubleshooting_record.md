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
