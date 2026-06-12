# Node V3 Ultrasonic Anemometer
## Design Changes, Bring-Up Plan, and Production Firmware Guidance

**Date:** 2026-06-12  
**Board:** Node V3  
**Scope:** 22 V boost, ultrasonic TX path, directional transducer switching, RX analog chain, layout/grounding, bring-up, and final firmware architecture  
**Status:** Schematic/layout revision substantially complete; bench validation pending

---

# 1. Executive Summary

The V3 review began as a focused fix for the MT3608 22 V boost brownout, but the full ultrasonic signal-chain review identified a more fundamental transmit problem:

- The previous Q16 high-side gate network was too slow for carrier-frequency switching.
- `TX_PULSE` did not have a sufficiently strong LOW-state discharge path.
- The firmware drove Q16 at approximately 40 kHz, so Q16 was required to switch every carrier half-cycle.
- The directional `REL_*` controls were clarified as transducer-shunt/damping controls, not simple return-to-ground controls.
- The receive path was hardened with local supply filtering, distributed VREF decoupling, improved sensitive-node placement, better ground referencing, and removal of an unnecessary comparator pull-up.

The revised V3 architecture retains the existing overall topology but corrects the highest-risk faults without introducing a full complementary half-bridge or additional GPIO requirements.

---

# 2. Changes Completed Today

## 2.1 22 V MT3608 Boost Converter

### Inductor

The 22 V boost inductor was changed to:

- **LCSC:** C41406986
- **Value:** 22 µH
- **Saturation current:** approximately 2.5 A
- **Purpose:** replace the previous low-margin inductor that saturated during cold enable and collapsed VSYS.

### Input capacitance

The MT3608 input on `VSYS` now has:

- 100 µF ceramic
- 100 nF ceramic

These are placed locally around the converter input.

### Output capacitance

The output bank was revised from:

- 100 nF + 22 µF + 100 µF

to:

- **100 nF + 22 µF + 22 µF**

Reason:

- reduce cold-start charging energy;
- retain sufficient local storage for the short TX burst;
- reduce the probability of VSYS collapse during converter startup.

The two 22 µF output capacitors must be rated at least 35 V, preferably 50 V. The 100 nF output capacitor should preferably be 50 V.

### U49 enable inverter retained

U49 remains in the design because it provides boot-safe active-low control:

- `GPIO5 / TX_22V_EN_N = HIGH` → U49 output LOW → `EN_22` LOW → boost OFF
- `GPIO5 / TX_22V_EN_N = LOW` → U49 output HIGH → `EN_22` HIGH → boost ON

Changes around U49:

- 1 µF local supply decoupling added
- 100 nF retained
- capacitors moved close to U49 VCC/GND
- `EN_22` test point retained
- no EN hold capacitor added
- no 22 V minimum-load resistor added
- no U49 removal or Schottky replacement implemented

### Layout

The MT3608 layout was reviewed and accepted:

- L1, U22 and D7 are tightly grouped.
- The SW node is compact and limited to:
  - U22 SW pin
  - L1 switching-side pad
  - D7 anode
- No SW test point or long branch is present.
- Feedback divider remains close to the FB pin and outside the SW copper.
- Input/output capacitor return paths were reviewed.
- Ground vias were added or checked around the converter and capacitor grounds.

### Status

- Schematic: approved
- Layout: approved
- Cold-start validation: pending

---

## 2.2 Global 40 kHz TX High-Side Switch

The previous block name "Transmit Burst Enable Switch" was misleading because Q16 switches at the actual carrier frequency. It should be documented as:

> **40 kHz High-Side TX Pulse Switch**

### Confirmed firmware behaviour

`TX_BURST_PWM` on GPIO25 is the actual carrier:

- bit-banged HIGH/LOW
- approximately 12 µs per half-cycle
- nominal 41.7 kHz before software overhead
- 12 cycles in the current bring-up firmware
- approximately 300 µs burst duration

Therefore Q16 must turn ON and OFF during every carrier cycle.

### Q16 gate-drive correction

Previous problem:

- R112 = 100 kΩ was the only Q16 turn-off path.
- The resulting gate time constant was tens of microseconds.
- This was too slow for a 12 µs LOW interval.

Applied changes:

- **R112 changed from 100 kΩ to 4.7 kΩ**
- **R5 = 1 kΩ added between `HS_GATE` and Q21 drain**
- D10 = BZT52C15 retained as Q16 gate-source protection
- Q21 = 2N7002 retained
- R113 = 33 Ω retained
- R114 = 100 kΩ retained

Expected operation:

- Q21 OFF:
  - R112 pulls `HS_GATE` toward `22V_SYS`
  - Q16 turns OFF
- Q21 ON:
  - R5 limits current
  - D10 limits negative Q16 VGS
  - Q16 turns ON

### Strong TX_PULSE LOW state

The old 220 kΩ resistor was only a slow bleed and could not discharge the TX node during a 12 µs LOW half-cycle.

Applied change:

- **R6 = 1 kΩ from `TX_PULSE` to GND**
- Existing R182 = 220 kΩ retained as an off-state bleed
- R6 should use at least a 1206 footprint and suitable pulse rating

Reason:

- gives the single-ended TX architecture a deterministic LOW without:
  - another GPIO;
  - complementary PWM;
  - dead-time circuitry;
  - shoot-through risk.

Expected initial performance with an estimated 2.5 nF TX capacitance:

- RC ≈ 2.5 µs
- `TX_PULSE` should fall close to ground within a 12 µs LOW interval

### Local 22 V capacitors

- 100 nF verified for suitable voltage
- 1 µF upgraded to 0805 and verified for suitable voltage

### Status

- Q16 gate network: approved
- passive TX pull-down: approved
- PCB placement and ground return: approved in principle
- oscilloscope validation: pending

---

## 2.3 Directional Transducer Driver Channels

The North channel was reviewed in detail and the same changes were replicated across East, South and West.

### Correct functional interpretation

The directional controls are:

- `DRV_x`: connects `TX_PULSE` to the selected transducer terminal through the directional P-channel MOSFET
- `REL_x`: turns on a shunt MOSFET across the transducer terminals for damping

`REL_x` is therefore better understood as:

- `DAMP_x`
- `SHUNT_x`

The physical net names may remain `REL_*`, but the firmware and documentation must describe their true purpose.

### Required directional truth table

| DRV_x | REL_x | State |
|---:|---:|---|
| LOW | LOW | Idle / receive |
| HIGH | LOW | Transmit |
| LOW | HIGH | Damping / transducer short |
| HIGH | HIGH | Forbidden while TX_PULSE is active |

### High-side directional gate-network changes

For each directional P-channel MOSFET:

- source pull-up changed from **10 kΩ to 1 kΩ**
- new **1 kΩ resistor** added between the P-MOS gate node and the 2N7002 drain
- 15 V gate-source zener retained
- GPIO-side 100 Ω gate resistor retained
- 100 kΩ gate pulldown retained

For North:

- R116 changed to 1 kΩ
- R7 added as 1 kΩ
- Q17 retained
- Q22 retained
- D11 retained

Equivalent changes were applied to East, South and West.

Reason:

- inactive P-MOS gates must track the fast `TX_PULSE` source to stay OFF;
- selected P-MOS gates need a controlled and predictable ON drive;
- the zener should act as protection rather than an uncontrolled current path.

### Shunt/damping MOSFETs

Retained:

- 100 Ω gate resistor
- 100 kΩ pulldown
- CJ2310 shunt MOSFET
- hardware-safe OFF state at boot

### R121-style transducer ground resistor

The 1 kΩ resistor from one transducer terminal to GND was retained.

This is a tuning component affecting:

- transmit amplitude;
- receive sensitivity;
- electrical damping;
- ring-down duration;
- transducer loading.

Do not change it until acoustic bring-up data is available.

### Status

- North topology: signed off
- East/South/West replication: accepted
- visual consistency/ERC check still required
- acoustic truth table validation pending

---

## 2.4 Comparator Supply and Output

### Local comparator supply filter

Added:

- 4.7 Ω series resistor from `3V3_SYS`
- 100 nF to GND after the resistor
- 1 µF to GND after the resistor

The capacitors are placed on the comparator side of the resistor.

Recommended local net name:

- `3V3_COMP`

### Local VREF capacitor

Added:

- 100 nF from VREF to GND at the comparator reference input

### Comparator pull-up removed

U44 is an MCP6561-family push-pull comparator.

Therefore:

- **R110 = 10 kΩ pull-up on COMP_RAW was removed**

Reason:

- push-pull output does not need an external pull-up;
- the pull-up unnecessarily coupled raw `3V3_SYS` into `COMP_RAW`;
- it created an unnecessary load when COMP_RAW was LOW.

### Hysteresis

No change was made to current hysteresis:

- R109 = 1 MΩ retained
- R179 retained unchanged
- no extra footprint added

Decision:

- first validate the improved TX waveform, supply filtering and VREF stability;
- tune hysteresis only if COMP_RAW still chatters.

### Digital blanking

Retained:

- U50 inverter:
  - `RX_WINDOW_EN = NOT RX_EN_N`
- U51 AND gate:
  - `TOF_EDGE = COMP_RAW AND RX_WINDOW_EN`
- 100 nF local decoupling for U50 and U51
- R178 = 0 Ω gated output path

### Status

- comparator block signed off
- hysteresis tuning pending only if required by measurements

---

## 2.5 TLV9062 RX Amplifier Supply

Added local supply filtering:

- 4.7 Ω series resistor from `3V3_SYS`
- 100 nF local capacitor
- 1 µF local capacitor

Both capacitors are on the TLV9062 side of the resistor and close to the shared VCC/GND pins of the dual op-amp.

Recommended local net name:

- `3V3_RXAMP`

No additional capacitance was added to signal nodes such as:

- `ST1_IN`
- `ST1_OUT`
- `ST2_IN`
- `RX_AMP`

This avoids unintentionally changing filter gain, phase or timing.

---

## 2.6 74HC4052 RX Mux Supply

Added:

- 4.7 Ω series resistor between `3V3_SYS` and U42 VCC
- existing 100 nF retained on the mux side of the resistor

Recommended local net name:

- `3V3_MUX`

The RX clamp upper rails remain connected to normal `3V3_SYS`, not the filtered mux/op-amp/comparator islands. This avoids dumping transient clamp current into a quiet local analog supply.

---

## 2.7 VREF Network

### Main VREF source

Revised to:

- 3V3_SYS
- 220 Ω series feed resistor
- 47 kΩ / 47 kΩ divider
- VREF approximately 1.65 V

Main VREF decoupling now includes:

- 100 nF
- 1 µF
- 4.7 µF

### Distributed local VREF decoupling

Added:

- 100 nF at comparator VREF input
- 100 nF at the first amplifier VREF bias area
- 100 nF at the second amplifier VREF bias area

Reason:

- the VREF route is relatively long;
- local capacitors reduce reference movement and TX-correlated pickup at each consumer.

### Firmware implication

Because total VREF capacitance is now several microfarads, allow VREF to settle before the first acoustic measurement.

Initial conservative recommendation:

- 500 ms after board power-up before the first ultrasonic measurement
- later reduce based on measured VREF settling time

---

## 2.8 RX Protection

The RX protection topology was retained:

### Hot side

- `RX_HOT`
- R100 = 2.2 kΩ series resistor
- BAV99 clamp to GND and `3V3_SYS`
- C103 = 220 pF to GND
- R174 = 1 MΩ to VREF
- C102 = 1 nF coupling capacitor to `ST1_IN`

### Cold side

- `RX_COLD`
- R152 = 2.2 kΩ series resistor
- BAV99 clamp to GND and `3V3_SYS`
- R107 = 100 kΩ to VREF
- C109 = 1 nF in parallel with R107

The series resistors remain before the clamps.

No extra comparator-input clamp or additional signal-node capacitance was added.

---

## 2.9 RX Layout and Ground Referencing

### C102 moved

C102 was moved next to the TLV9062 first-stage input.

Result:

- `ST1_IN` is now extremely short
- no long high-impedance trace remains after the coupling capacitor
- the longer route is now `RX_IN`, which is the better trade-off

### RX_IN routing

`RX_IN` runs primarily on the bottom layer.

The 4-layer stack remains:

- L1 Top signal
- L2 GND
- L3 Power
- L4 Bottom signal

The 5 V pour beneath the long RX_IN route was moved and replaced with a local GND corridor on L3.

Added/verified:

- GND beneath most/all of the bottom RX_IN trace
- GND stitching beside RX_IN layer-transition vias
- local GND corridor stitched to the main ground plane
- no floating GND island
- 5 V pour continuity retained

This removed the immediate need to move to a 6-layer board.

### RX_AMP and COMP_RAW

Both routes were reviewed and accepted:

- `RX_AMP` remains short and local between TLV9062 and comparator
- `COMP_RAW` remains short and local between U44 and U51
- neither loops back beside the comparator inputs or VREF
- test points are acceptable because both are actively driven, low-impedance nodes

### TX/RX zoning

Reviewed and accepted:

- 22V_SYS remains in the TX area
- TX_PULSE remains in the centre/upper TX area
- RX analog circuitry remains lower-right
- no major TX route crosses the RX front end
- no major TX_PULSE reroute was required

---

## 2.10 Ground Via Additions

Ground vias were added or reviewed at these critical locations:

- TLV9062 100 nF and 1 µF capacitor grounds
- comparator 100 nF and 1 µF capacitor grounds
- mux 100 nF ground
- U49 100 nF and 1 µF grounds
- U50/U51 decoupling grounds
- TC4427 decoupling ground
- main and local VREF capacitor grounds
- D8/D9 clamp ground returns
- both RX_IN signal layer transitions
- MT3608 GND
- MT3608 input capacitor grounds
- MT3608 output capacitor grounds
- Q16 TX_PULSE 1 kΩ pulldown ground
- Q21 and directional gate-driver source grounds
- local GND corridor stitching

---

# 3. Deliberate Non-Changes

The following were considered but intentionally not implemented:

- U49 removal
- Schottky-isolated EN_22 replacement
- EN_22 hold capacitor
- 22V_SYS minimum-load resistor
- 470 µF–1000 µF optional VSYS bulk capacitor
- L7 / 5 V boost inductor replacement
- full shared `3V3_A` bus
- new comparator hysteresis footprint
- active low-side TX MOSFET / full half-bridge
- complementary PWM hardware
- analog RX mute switch
- major TX_PULSE reroute
- 6-layer board conversion

These may be revisited only if bring-up measurements justify them.

---

# 4. Mandatory Schematic and Layout Checks Before Ordering

## Schematic/ERC

- Confirm GPIO-side boost net is consistently named `TX_22V_EN_N`.
- Confirm MT3608-side enable net is `EN_22`.
- Confirm U49 is not bypassed.
- Confirm Q16 source/drain/gate mapping.
- Confirm all four directional P-MOSFET source/drain/gate mappings.
- Confirm D10/D11-style zener cathodes connect to their P-MOS source nodes.
- Confirm BAV99 symbol-to-footprint mappings.
- Confirm R110 is removed from BOM/netlist.
- Confirm R6 = 1 kΩ TX_PULSE pulldown is populated.
- Confirm all four directional channels use the same revised resistor values.
- Confirm `DRV_* HIGH + REL_* HIGH` is documented as forbidden during TX.
- Confirm C84/U1/U2 final output capacitor values are two 22 µF capacitors, not 100 µF.
- Confirm all 22 V capacitors have suitable voltage ratings.

## PCB/DRC

- Repour all copper zones.
- Run DRC after every final route change.
- Confirm no isolated GND or power islands.
- Confirm no narrow 5 V bottleneck after moving the pour.
- Confirm RX_IN remains above the new L3 GND corridor.
- Confirm GND vias are close to RX_IN layer transitions.
- Confirm ST1_IN remains very short and without a test point.
- Confirm the MT3608 SW node remains compact.
- Confirm no sensitive net is routed beneath the MT3608 SW node.
- Confirm feedback does not run near SW/L1.
- Confirm R6 ground return is local and does not pass through the RX analog ground area.
- Confirm all new 0402/0603/0805/1206 footprints match JLC assembly capability and BOM parts.

---

# 5. Detailed Ultrasonic Bring-Up Plan

## Phase 0 — Equipment and Safety

Recommended equipment:

- current-limited bench supply or known-good Li-ion battery
- two- or four-channel oscilloscope
- 10× probes rated above 30 V
- DMM
- optional differential probe
- reference 40 kHz ultrasonic transducer or microphone
- physical acoustic blocker
- reference anemometer for final calibration

Scope safety:

- connect all ordinary probe ground clips only to board GND
- never connect a ground clip to `22V_SYS`
- never connect a ground clip to either transducer terminal
- use two probes plus math for:
  - `HS_GATE - 22V_SYS`
  - `TD_x_A - TD_x_B`

Initial scope settings for carrier work:

- 5–10 µs/div
- 5 or 10 V/div for 22 V nodes
- 2 V/div for logic nodes
- trigger on `PWM_5V`

---

## Phase 1 — Unpowered Inspection

With power disconnected:

1. Check resistance from:
   - VSYS to GND
   - 3V3_SYS to GND
   - 5V_SYS to GND
   - 22V_SYS to GND
   - TX_PULSE to GND
2. Confirm TX_PULSE reflects approximately the new 1 kΩ pulldown.
3. Check no short exists across Q16 source/drain.
4. Check each directional channel:
   - DRV gate pulldown present
   - REL gate pulldown present
   - no unintended short across transducer terminals
5. Check all new filter resistors have expected continuity.
6. Check filtered local rails are not accidentally labelled or shorted directly to `3V3_SYS`.

---

## Phase 2 — Low-Voltage System Only

Keep the 22 V boost disabled.

Validate:

- VSYS
- 3V3_SYS
- 5V_SYS
- `3V3_COMP`
- `3V3_RXAMP`
- `3V3_MUX`
- VREF

Expected:

- 3V3 local filtered nodes close to 3.3 V
- VREF approximately 1.65 V
- boost OFF at boot
- `EN_22` LOW
- `22V_SYS` near 0 V
- `TX_PULSE` near 0 V
- all DRV/REL outputs LOW
- `RX_EN_N` HIGH
- `TOF_EDGE` blocked

Measure VREF startup settling and use this result to refine the initial firmware wait.

---

## Phase 3 — 22 V Cold-Start Validation

Test with:

- battery or bench supply near 4.0–4.1 V
- ESP32 fully running
- no TX burst initially

Probe:

- VSYS
- 3V3_SYS
- EN_22
- 22V_SYS

Sequence:

1. Boot with boost disabled.
2. Enable boost once from cold.
3. Hold enabled for at least 100 ms.
4. Disable.
5. Repeat from a fully discharged output.

Pass criteria:

- no `POWERON_RESET`
- no ESP32 brownout
- VSYS does not collapse
- 3V3_SYS remains stable
- EN_22 remains steady
- 22V_SYS reaches approximately 22 V
- converter does not hiccup
- MT3608 and L1 do not heat rapidly

Also test at:

- 4.2 V battery
- approximately 3.7 V
- low usable battery voltage

If cold start still fails:

1. scope inductor/SW behaviour;
2. test with one output 22 µF removed;
3. reconsider inductor current margin;
4. only then reconsider extra VSYS bulk capacitance or enable strategy.

---

## Phase 4 — Carrier Chain Without Directional Load

Generate a short carrier burst with all directional channels disabled.

Probe:

- GPIO25 / TX_BURST_PWM
- PWM_5V
- HS_GATE
- TX_PULSE
- 22V_SYS

Expected:

### GPIO25

- stable hardware-generated carrier once production firmware is implemented
- during temporary bring-up, bit-banged waveform may show timing variation

### PWM_5V

- clean 0–5 V square wave
- no excessive edge distortion

### HS_GATE

- OFF state near 22V_SYS
- ON state approximately 7–11 V depending on clamp operation
- returns to OFF before the next carrier edge
- no slow accumulation across cycles

### TX_PULSE

- HIGH preferably above 20 V
- LOW preferably below 1–2 V before the next rising edge
- no DC accumulation over the burst
- no severe ringing or overshoot

### 22V_SYS

- stable during the complete burst
- no large collapse or oscillation

Fail conditions:

- TX_PULSE rises once and remains high
- TX_PULSE LOW remains several volts high
- HS_GATE does not return to the source level
- 22V_SYS collapses during burst
- Q16/Q21/R6 heat unexpectedly

---

## Phase 5 — One Directional Channel at a Time

Start with North.

Required transmit state:

- `DRV_N = HIGH`
- `REL_N = LOW`
- all other DRV/REL outputs LOW

Probe:

- TX_PULSE
- `DRV_GATE_N`
- `TD_N_A`
- `TD_N_B`
- differential transducer voltage using scope math

Expected:

- Q17 gate approximately 11 V below/relative to source conditions needed for ON
- TD_N_A follows TX_PULSE
- TD_N_B remains referenced through the existing 1 kΩ network
- clear 40 kHz differential waveform across the transducer

Repeat with North disabled while another channel is active.

Inactive-channel pass criterion:

- only small capacitive spikes
- no near-full-amplitude pulse on inactive transducer terminals

Repeat for East, South and West.

---

## Phase 6 — Damping State

After a transmit burst:

1. set `DRV_x = LOW`
2. allow a short non-overlap delay
3. set `REL_x = HIGH` for a configurable damping interval
4. set `REL_x = LOW`
5. enter idle/receive state

Never allow:

- `DRV_x = HIGH`
- `REL_x = HIGH`
- while TX_PULSE is active

Initial damping interval to test:

- 10–50 µs

Tune using transducer ring-down measurements.

Compare:

- no damping
- 10 µs
- 20 µs
- 50 µs

Select the shortest interval that reduces ring-down without suppressing the desired receive event.

---

## Phase 7 — RX Baseline Validation

Keep TX disabled.

Test:

- RX mux disabled
- RX mux enabled
- each mux direction
- comparator raw output
- gated TOF output

Probe:

- VREF
- RX_IN
- ST1_OUT
- RX_AMP
- COMP_RAW
- TOF_EDGE

Pass targets:

- VREF stable
- RX_IN settles toward VREF when disconnected
- baseline COMP_RAW edges near zero in the expected acoustic window
- TOF_EDGE zero when RX is disabled
- no TX-correlated event because TX is inactive

Do not adjust hysteresis yet unless the improved hardware still produces comparator chatter.

---

## Phase 8 — Electrical Coupling Test

Disconnect or acoustically/electrically isolate the RX transducer.

Transmit normally.

Measure:

- COMP_RAW event count
- TOF_EDGE detection rate
- first-edge time distribution

Target:

- detection rate should drop substantially
- desirable initial target: less than 10% false detection in the expected TOF gate
- no stable "arrival" time should remain when the RX transducer is disconnected

If detection remains high:

- inspect TX_PULSE edge ringing
- inspect VREF movement
- inspect RX_IN pickup
- compare COMP_RAW and TOF_EDGE
- test lower burst cycles
- test longer/shorter damping
- only then tune hysteresis

---

## Phase 9 — Open vs Blocked Acoustic Test

With the transducers installed:

1. record at least 50–100 open-path shots
2. physically block the acoustic path
3. record the same number of blocked-path shots

Compare:

- detection rate
- median first valid TOF
- jitter
- edge count
- signal/noise separation

Pass target:

- blocked and open distributions clearly separate
- blocked detection rate should fall strongly
- median separation should exceed normal open-path jitter by a large margin

---

## Phase 10 — Reciprocal Direction Test

For one axis:

- measure N→S
- measure S→N

At still air:

- corrected reciprocal TOFs should be nearly equal
- the difference should be stable around a calibration offset

With directed airflow:

- reciprocal TOF difference should change sign when direction reverses
- sign should be stable across repeated shots
- open-path median difference should exceed jitter

Acceptance target:

- direction sign correct in at least 90–95% of valid reciprocal pairs
- no persistent same-time detection across every direction

---

## Phase 11 — Wind Calibration

Known constants:

- path length: 146.70 mm
- nominal still-air TOF: approximately 427 µs
- projection factor: 0.5344

For reciprocal path times:

- `t_down = L / (c + v_path)`
- `t_up = L / (c - v_path)`

Path-axis velocity:

`v_path = (L / 2) × (1 / t_down - 1 / t_up)`

Projected wind-axis velocity:

`v_axis = v_path / 0.5344`

Use seconds and metres in calculations.

Calibration requirements:

- per-direction electronic timing offsets
- per-axis zero-wind offset
- path-length calibration
- temperature compensation
- outlier rejection
- reference-anemometer comparison

---

# 6. Production Firmware Design

## 6.1 Replace bit-banged PWM

Do not use `digitalWrite()` and `delayMicroseconds()` in production.

Recommended architecture:

### TX burst

Use **ESP32 RMT TX** for:

- exact HIGH/LOW durations
- exact cycle count
- deterministic final LOW state
- configurable carrier frequency
- minimal CPU jitter

Alternative:

- MCPWM or LEDC, but RMT is especially convenient for a finite burst with exact pulse count.

Initial carrier parameters:

- start around 40 kHz
- sweep approximately 38–42 kHz during bring-up
- 50% duty initially
- start with 6 cycles
- tune between 4 and 10 cycles based on acoustic SNR and ring-down

The current 12-cycle burst is long relative to the 146.7 mm acoustic path and current blanking strategy.

---

## 6.2 Use Hardware Edge Capture

Do not use `micros()` in a GPIO ISR for final wind measurement.

Reason:

- 1 m/s produces only about a 1.33 µs reciprocal split
- useful accuracy needs sub-microsecond timing
- ISR and software timestamp jitter are too large

Recommended:

- MCPWM capture input on TOF_EDGE
- or RMT RX configured at a sufficiently high clock resolution

Target capture resolution:

- 100 ns or better preferred
- 250 ns may be usable
- 1 µs is not sufficient for high-quality wind measurements

Capture multiple edges during bring-up, not only the first edge. This helps diagnose comparator chatter and select a robust event.

---

## 6.3 Correct the TX/RX Timing Reference

Current firmware waits approximately:

- 300 µs burst
- 320 µs blanking after burst
- 20 µs mux settle
- 20 µs guard

This opens the receive path approximately 640 µs after the first TX edge.

The nominal acoustic arrival begins around 427 µs after the first transmitted edge, so the present sequence can miss the first acoustic arrival and capture later ring-down or noise.

Production timing must reference a defined TX edge.

Recommended initial approach:

1. record the timestamp of the first TX rising edge;
2. generate 4–8 cycles;
3. keep RX blocked during the electrical burst;
4. optionally apply transmitter damping;
5. open RX sufficiently before the expected arrival;
6. accept TOF only inside an absolute gate referenced to the first TX edge.

Initial expected acoustic gate:

- approximately 350–550 µs from the first TX edge

Refine after measuring:

- temperature range
- filter group delay
- comparator delay
- real transducer ring-up
- mechanical path length

Do not define `MIN_VALID_TOF_US` relative only to `listenStartUs`. Use the hardware TX timestamp as the common time origin.

---

## 6.4 Recommended Measurement State Machine

### `SAFE_IDLE`

- TX_BURST_PWM LOW
- TX_22V_EN_N HIGH
- all DRV LOW
- all REL LOW
- RX_EN_N HIGH
- capture disabled

### `SELECT_PATH`

- set RX mux direction while RX remains disabled
- set selected TX DRV LOW/REL LOW
- clear all other channels
- wait for control settling

### `BOOST_PRECHARGE`

- enable 22 V boost
- wait until stable
- initially use 20–50 ms
- later reduce using measurements or optional rail validation

### `ARM_TIMING`

- clear hardware capture
- record/prepare TX time origin
- keep RX blocked

### `TRANSMIT`

- set selected `DRV_x = HIGH`
- keep selected `REL_x = LOW`
- emit exact RMT burst
- finish carrier LOW

### `DAMP`

- set `DRV_x = LOW`
- wait short non-overlap interval
- set `REL_x = HIGH`
- hold for configurable damping interval
- set `REL_x = LOW`

### `RX_PREPARE`

- wait until configured receive-open time
- set RX_EN_N LOW
- allow mux/analog settle
- arm hardware capture before expected arrival

### `CAPTURE`

- accept edges only inside expected absolute TOF window
- capture multiple edges for diagnostics
- classify first valid acoustic event
- stop at timeout

### `CLEANUP`

- RX_EN_N HIGH
- all DRV LOW
- all REL LOW
- TX_BURST_PWM LOW
- disable boost
- save raw measurement and diagnostics

---

## 6.5 Firmware Safety Rules

Enforce these invariants:

- boost must not enable until all DRV/REL outputs are configured
- TX_BURST_PWM must always idle LOW
- only one DRV channel may be HIGH
- `DRV_x HIGH + REL_x HIGH` is forbidden while TX_PULSE is active
- all DRV and REL pins return LOW after every shot
- RX remains disabled during the electrical TX burst
- boost-only test firmware with inverted GPIO5 logic must not run on a board with U49 fitted
- production firmware must not use legacy V1 RX polarity
- any timeout/error must call a single fail-safe cleanup function

Suggested assertion:

```cpp
bool txStateSafe() {
  return activeDrvCount() <= 1 &&
         !(selectedDrvHigh() && selectedRelHigh() && txCarrierActive());
}
```

---

## 6.6 Configurable Bring-Up Parameters

Expose these through a debug menu or serial command interface:

- carrier frequency
- half-period
- burst cycles
- boost precharge duration
- damping duration
- RX enable time
- RX settle time
- expected TOF window start/end
- comparator edge selection
- direction selection
- DRV/REL state
- inter-shot delay
- number of warm-up shots
- number of scored shots

Log all settings with each test result.

---

## 6.7 Required Raw Data Per Shot

Store:

- TX direction
- RX direction
- carrier frequency
- cycle count
- TX first-edge timestamp
- TX final-edge timestamp
- RX enable timestamp
- all captured TOF edge timestamps within the gate
- selected edge index
- edge count
- timeout status
- 22V_SYS if available
- battery voltage
- temperature
- path blocked/open test flag
- firmware version
- PCB revision

Do not store only the final wind speed during bring-up. Preserve raw TOF data.

---

## 6.8 Filtering and Outlier Handling

Use reciprocal shot pairs.

Recommended initial processing:

1. reject shots with no edge in the expected gate;
2. reject shots with abnormal edge count;
3. reject TOFs outside the physical path window;
4. calculate median of repeated shots;
5. use MAD or robust percentile limits for outliers;
6. pair opposite directions close in time;
7. calculate reciprocal velocity after per-direction offset correction.

Do not average feedthrough-dominated detections into a wind result.

---

## 6.9 Calibration Data

Store in non-volatile configuration:

- path length per axis
- projection factor
- carrier frequency per transducer pair if needed
- zero-wind TOF offset per direction
- comparator/filter timing offset
- temperature correction coefficients
- minimum signal-quality criteria
- acceptable edge-count range
- per-channel damping duration

---

# 7. Bring-Up Acceptance Criteria

The system is not acoustically validated until all of these are met:

## Electrical

- no cold-start ESP32 reset
- 22V_SYS stable
- Q16 gate fully returns each cycle
- TX_PULSE >20 V HIGH
- TX_PULSE <1–2 V LOW before next rising edge
- inactive channels show only minor capacitive coupling
- no sustained excessive heating

## RX baseline

- TOF_EDGE = 0 when RX disabled
- baseline edges near zero inside expected TOF gate
- VREF shows no significant TX-correlated movement
- local filtered rails remain stable

## Acoustic discrimination

- disconnected RX transducer dramatically reduces detection
- blocked path clearly differs from open path
- reciprocal direction deltas are stable in sign
- reversed airflow reverses delta sign
- open-path median separation exceeds jitter
- detections occur in the physically plausible arrival window

## Timing

- first valid acoustic arrival is captured near expected 410–450 µs range at normal temperatures
- timing is referenced to hardware TX edge
- capture resolution is sub-microsecond

---

# 8. Immediate Next Actions

1. Complete final ERC/DRC.
2. Verify all four directional driver blocks match.
3. Verify BOM values and capacitor voltage ratings.
4. Create a formal V3 bring-up firmware branch.
5. Implement safe-state initialization.
6. Implement RMT TX burst.
7. Implement hardware TOF capture.
8. Implement corrected DRV/REL truth table.
9. Implement absolute acoustic timing gate.
10. Bench-test:
    - 22 V cold start
    - Q16 gate
    - TX_PULSE
    - one directional channel
    - RX baseline
    - disconnected receiver
    - open/blocked path
    - reciprocal directions

---

# 9. Key Firmware Corrections to Existing Bring-Up Code

The existing authoritative bring-up code must eventually be revised because:

- bit-banged PWM is not sufficiently deterministic;
- the present 320 µs post-burst blanking is too late for the first acoustic arrival with a 12-cycle burst;
- the current test sweep allows `DRV_x = HIGH` and `REL_x = HIGH`, which is an invalid TX state;
- TOF is timestamped relative to the receive/listen sequence rather than a precise hardware TX edge;
- production ultrasonic code is currently only a stub;
- the platformio.ini corruption around `PIN_DRV_W` / `PIN_REL_N` must be fixed;
- the boost-only inverted-GPIO5 test must remain isolated from normal U49-equipped firmware.

---

# 10. Final Design Position

The recommended V3 direction is now:

- keep the existing single-ended TX architecture;
- keep GPIO25 as the only carrier control;
- use the corrected Q16 gate network;
- use the passive 1 kΩ TX_PULSE pulldown;
- retain directional P-MOS switching;
- use REL signals only as post-TX damping controls;
- retain hardware output blanking;
- retain local analog supply islands;
- retain distributed VREF decoupling;
- keep the revised RX_IN/ST1_IN layout;
- use hardware-generated TX timing and hardware capture in production firmware;
- validate acoustics before further analog/hysteresis redesign.

This limits PCB risk while addressing the most likely causes of V1/V2 feedthrough-dominated behaviour.