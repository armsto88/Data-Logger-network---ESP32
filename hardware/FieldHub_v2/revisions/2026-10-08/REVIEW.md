# FieldHub v2 engineering review — 2026-10-08 release

- **Review date:** 2026-10-08
- **Compared with:** `hardware/FieldHub_v2/revisions/2026-10-07`
- **EasyEDA source:** `HUB_ProPrj_MotherShip_v2_2026-10-08.epro2`
- **Release directory:** `hardware/FieldHub_v2/revisions/2026-10-08`

**Recommendation:** **Do not order this revision yet.**

The October 8 export contains the intended solar-input correction, higher-rated
solar capacitor, auto-retry sensor switches, reed-input TVS, current ESP32
module, source-side SD damping resistors, and useful 5 V/ADC filtering changes.
The source, BOM, placement data, Gerbers, and drill files are internally
consistent.

Two PCB items still require another layout/export cycle:

1. The four mounting holes are now in the NPTH drill file, but 6.0 mm floating
   copper pads remain around every 3.4 mm hole on **all four copper layers**.
2. The MT3608 boost converter was not physically compacted as reported. Its
   switch-node path is still about 9.4 mm long, and its diode, output capacitor,
   and feedback divider remain remote from U21.

The CN3791 sense routing is much better than the October 7 release, but the two
sense traces diverge instead of running as a close Kelvin pair. This should be
tightened during the same layout pass. Battery safety, the out-of-spec 5 V
operation of the selected soil probes, harness documentation, DRC evidence, and
assembly instructions also remain release requirements.

## Review scope and limitations

This was a desktop review of:

- the supplied EasyEDA Pro source project;
- schematic connectivity, part selection, and selected programmed values;
- the four-layer PCB, placement, board outline, and critical power routes;
- the generated Gerber, plated-drill, non-plated-drill, via-drill, and
  flying-probe data;
- the BOM and pick-and-place exports; and
- changes from the October 7 release.

The review does not replace EasyEDA ERC/DRC, the selected fabricator's PCB and
PCBA DFM checks, controlled-impedance review, formal ESD/surge testing, RF
testing, certification work, or physical bring-up. No ERC, DRC, schematic PDF,
manufacturer DFM report, stack-up, battery datasheet, or test results were
included in this release.

## Release-integrity checks

| Check | Result |
|---|---|
| Imported files | Exact copies of the four supplied October 8 files were placed in this revision directory |
| Source archive integrity | Pass; the EasyEDA project ZIP tests successfully |
| Gerber ZIP integrity | Pass; all 17 archived files test successfully |
| BOM groups | 99 |
| BOM references | 262; no duplicate references |
| Placement references | 262; exact match to the BOM and source PCB |
| Source unique IDs | All 262 source PCB component IDs match the BOM |
| Placement coordinates | All source origins agree; SJ4, SJ5, U1, and U2 use expected footprint centroids |
| Assembly side | All 262 placements are top-side; no bottom-paste file is required |
| Layer/drill set | Four copper layers, top/bottom mask, top paste, top/bottom silk, outline, PTH, NPTH, and via drill files are present |
| Flying-probe data | Contains every placed reference, plus expected EasyEDA pad pseudo-components |
| Board outline | Closed, approximately 100 x 90 mm, with the ESP32 antenna notch retained |

Key imported-file hashes:

| File | SHA-256 |
|---|---|
| `HUB_BOM_Board1_PCB1_2026-10-08.csv` | `a904e7d6003d60e00bed4a543fc9edf7f65abe698bb6b6b35fbc1754b3a3b17c` |
| `HUB_PickAndPlace_PCB1_2026_10_08.csv` | `21113c4fca2439db06fb809ebe5e7644e124b3457054f555a4516ed8cc75ae1b` |
| `HUB_Gerber_PCB1_2026-10-08.zip` | `87bd03be13158e1e6d68c78e2d006313312e85d3cc54213a51586939c3d901e8` |
| `HUB_ProPrj_MotherShip_v2_2026-10-08.epro2` | `6f5b402951b7f7e243f2c242fcd5f2f27e7d535e37b3e9365f2e0748f499e722` |

## Revision summary

| Metric | 2026-10-07 | 2026-10-08 | Change |
|---|---:|---:|---:|
| BOM groups | 96 | 99 | +3 |
| PCB/BOM/placement references | 260 | 262 | +2 |
| PCB vias | 371 | 370 | -1 |
| Extended-part placements | 139 | 141 | +2 |
| Basic-part placements | 101 | 101 | None |
| Placements with no JLCPCB class | 20 | 20 | None |
| Board outline | Approx. 100 x 90 mm | Approx. 100 x 90 mm | None |

Added references:

```text
C13, D14, D15, R26, R27, R28
```

Removed references:

```text
Q1, R176, R177, R178
```

Notable part/value changes:

- C89: 10 uF, 10 V, 0603 to 10 uF, 25 V, 0805.
- C3 and C76: 22 uF, 10 V parts moved from 0603 to 0805.
- C114: 1 uF to 10 uF on the new ADC supply filter.
- C115-C118: 1 nF to 10 nF ADC input capacitors.
- R22: 402 kOhm to 365 kOhm for the revised MPPT setpoint.
- U3/U4: latch-off `TPS2553DBVR-1` to auto-retry `TPS2553DBVR`.
- U45: `ESP32-WROOM-32E-N4` with its corresponding footprint.
- The SD series resistors are now R24, R26, and R27 after reannotation.

## Findings requiring action

### F-01 — Required: NPTH mounting holes still contain copper on every layer

The four 3.4 mm mounting holes are correctly present only in
`Drill_NPTH_Through.DRL` and are absent from the plated-hole file. Disabling
plating therefore changed the drill classification as intended.

However, each hole location is still flashed with a 6.0 mm circular copper pad
on the top, bottom, Inner 1, and Inner 2 Gerbers. This leaves an approximately
1.3 mm radial floating copper ring around every non-plated hole on each copper
layer. The holes are at approximately:

```text
(158.2243, -10.6539) mm
(246.2243, -10.6539) mm
(246.2243, -88.6539) mm
(158.2243, -88.6539) mm
```

This does not provide a controlled chassis connection and can couple ESD or
metal fastener noise into floating copper. The inner-layer rings are also easy
to miss in a normal top/bottom preview.

**Action:** replace each mounting-pad object with a true mechanical NPTH hole,
or explicitly remove copper on every layer and add an appropriate screw-head
and copper keepout. In the regenerated output, verify all four holes in NPTH,
none in PTH, and no copper flash or annulus at those coordinates on any copper
layer.

### F-02 — Required: the MT3608 boost hot loop remains physically spread out

The capacitor and ADC filtering changes are good, but U21, L4, and D6 were not
meaningfully moved. The switch-node copper from U21 through L4 to D6 spans
about 9.4 mm. D6 is approximately 10.5 mm from the U21 body centre, C76 is not
adjacent to D6/U21, and feedback resistors R72/R73 remain roughly 8 mm from U21.

This is the high-di/dt portion of the 5 V converter. Its present geometry raises
ringing, EMI, ground-noise, regulation, and ADC-noise risk. Merely widening or
rerouting the existing long connection will not reduce the loop area enough.

The programmed output is coherent:

```text
Vout = 0.6 V * (1 + 549 kOhm / 75 kOhm) = 4.992 V nominal
```

The 22 uH inductor is within the MT3608's stated 4.7-22 uH range and is not the
primary concern at the intended load. Placement is.

**Action:** repack U21, L4, D6, the input bypass capacitor, and C76 as one tight
power stage. Minimise the input-current loop and especially the SW/diode/output
loop. Put the feedback divider beside U21, sense the output after C76, keep its
trace away from SW, and preserve a quiet ground return. Then scope SW, 5V_SYS,
and the ADC supply under minimum battery voltage and maximum sensor load.

Reference: [MT3608 datasheet](https://www.mouser.com/datasheet/2/306/MT3608-3223743.pdf).

### F-03 — High: CN3791 Kelvin routing is improved but not yet a close pair

R25 has moved from roughly 8 mm to roughly 4.1 mm from U5. Both sense
connections are now direct, same-layer traces without vias, and the high-current
`BAT_BUS` joins at the battery-side R25 pad. Those are material corrections.

The two traces nevertheless leave adjacent CN3791 pins in opposite directions
and approach opposite R25 pads from widely separated sides. They therefore do
not behave as a close companion pair and can still pick up unequal switching or
load-current voltage.

The nominal charge-current target remains:

```text
Ichg = 120 mV / 0.200 Ohm = 600 mA
```

**Action:** while performing the boost/mounting-hole layout pass, place R25 as
close as practical to the charger output path and route `CSP` and `BAT` together
as a short same-layer Kelvin pair directly to opposite R25 pads. Keep shared
load and charge current out of both sense traces.

Reference: [CN3791 manufacturer datasheet](https://www.consonance-elec.com/static/upload/file/20231218/1702881078530301.pdf).

### F-04 — High: battery safety remains dependent on the selected pack/system

The board does not measure cell temperature and the CN3791 does not provide
battery-temperature-qualified charging. `BAT_BUS` is also the system supply
node rather than a separate load-sharing output, so operating current can affect
charge termination and recharge behaviour.

The previously discussed large 1S LiPo is not qualified by an Amazon listing
alone. The connector having worked with another pack establishes mechanical and
basic current compatibility, not cell provenance, protection thresholds,
temperature limits, charge-current rating, or pack quality.

**Action:** obtain a manufacturer datasheet for the exact pack and require
documented over-charge, over-discharge, over-current, and short-circuit
protection. Confirm that its continuous and peak ratings exceed the Hub loads
and that 600 mA charging is permitted. Add a system-level method that prevents
charging outside the cell maker's allowed temperature range. Validate
termination and recharge while the Hub sleeps, samples, writes SD, and uses the
radio.

### F-05 — High: selected soil probes are being operated outside their stated supply range

The two ADC connectors provide `5V_SEN`. The selected Comwintop TH-V5-type soil
temperature/moisture probes are reported to specify a 10-30 V supply but to
appear functional at 5 V. That observation is useful for a prototype, but 5 V
is outside the reported specification and provides essentially no headroom for
a nominal 0-5 V output. Accuracy, linearity, startup, temperature behaviour, and
full-scale output cannot be assumed from apparent operation.

**Action:** identify the exact probe part/output variant and retain its
datasheet with the release. Either provide a compliant supply and safe ADC
interface or explicitly qualify operation at 5 V across all expected soil,
temperature, cable-length, battery-voltage, and unit-to-unit conditions. Compare
against a reference instrument and check the entire output range.

The ADS1015 firmware must select the +/-6.144 V PGA range and use 6.144 V in the
conversion scale before accepting a 0-5 V signal. Verify near-full-scale inputs
and add over-range detection.

Product page supplied for the selected sensor family:
[Comwintop soil sensor](https://store.comwintop.com/products/rs485-4-20ma-soil-temperature-humidity-moisture-conductivity-ec-ph-sensor?variant=42250573316323).

### F-06 — High: the aviation-connector crossover is not yet a controlled build document

The PCB-side assignments remain:

| PCB connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| CN17-CN20 I2C | 3V3_SEN | GND | SDAx | SCLx |
| CN15 ADC | 5V_SEN | GND | A1 | A0 |
| CN16 ADC | 5V_SEN | GND | A2 | A3 |

The existing aviation-plug documentation uses different numbering, so a
same-pin-number harness could short or reverse power and ground. The decision to
use crossover harnesses is acceptable, but the control document does not yet
describe the as-built cables.

**Action:** after wiring the first unit, update
`hardware/SENSOR_WIRING_AVATION_PLUGS.md` with PCB reference/pin, wire colour,
intermediate connections, aviation pin and signal, and explicit mating-face or
solder-face views. Continuity-test every conductor and test for unintended
shorts before attaching a sensor.

### F-07 — Medium/high: assembly outputs need explicit fit and manual-placement instructions

The design exports agree, but they are not yet an unambiguous PCBA order:

- 141 placements use JLCPCB Extended parts and 101 use Basic parts.
- Twenty placed references have no supplier part number: 17 test points, SJ4,
  SJ5, and U1.
- U47, U54, U1, and U2 are marked `SMD=No`. U54 is the USB-C connector and has
  SMT contacts even though its shield stakes may have caused this
  classification; it could be omitted from an SMT-only order.
- SJ4/SJ5 still appear in the BOM and placement file with no supplier part.
  They are configuration jumpers and must be explicitly left open unless their
  opposite zero-ohm option is removed.
- `1uf1` and `edaimagecopyoccupy` remain nonstandard references.
- Test points use signal names rather than stable `TPxxx` designators.
- U1 and U2 require a clear DNP, mechanical-feature, or manual-fit disposition.

**Action:** generate assembly-specific BOM/CPL files with explicit `FIT`, `DNP`,
and `MANUAL/THT` status. Confirm the U54 order classification and define the
open/closed state of SJ4/SJ5. Inspect the assembler preview for centroid,
rotation, polarity, and footprint mapping, especially the modem, ESP32, USB-C,
SIM, microSD, diodes, LEDs, MOSFETs, charger, and fine-pitch parts.

### F-08 — Medium: design rules and release evidence remain incomplete

The source rule data still set component/device-body and through-hole-to-SMD
clearances to zero. A routing DRC can therefore pass while bodies, courtyards,
pick-and-place nozzles, rework access, fasteners, or enclosure parts interfere.
No ERC/DRC or manufacturer DFM results were supplied.

**Action:** define appropriate nonzero fabrication and assembly clearances, run
ERC and DRC, and save the reports with the release. Run the chosen fabricator's
PCB/PCBA DFM checks and manually review the antenna notch, mounting hardware,
connectors, tall parts, edge clearance, mask slivers, and polarity.

### F-09 — Low/medium: schematic release metadata are inconsistent

Updated sheets show a total of eight pages, but the overview still states page
1 of 1. The modem sheet still identifies `MotherShip_v1`, page 6 of 6, and an
older 2026-06-21 update date. This can cause stale sheets to be mixed into a
release package.

**Action:** standardise project name, hardware revision, page number, page
count, sheet title, and issue date on every page. Export and archive a complete
released schematic PDF.

### F-10 — Low: name the filtered ADC supply net

R28 correctly feeds U30 from `5V_SYS` through 10 Ohm, with C114=10 uF and
C119=100 nF at U30. In the source, however, the filtered rail still has an
anonymous generated net name rather than `5V_ADC`.

The nominal RC pole is:

```text
fc = 1 / (2 * pi * 10 Ohm * 10 uF) = 1.59 kHz
```

**Action:** label the filtered net `5V_ADC` so schematic review, PCB inspection,
firmware notes, and test procedures refer to the same rail.

## Verified corrections and sound design changes

### Solar charger input and setpoint

- The incorrect Q1 circuit is removed.
- D14 (`SL14PL-TP`) is in series from the protected panel input to `SOLAR_IN`
  with the correct polarity to conduct into the charger and block battery-side
  reverse current.
- D13 remains the input TVS.
- C89 is now a 10 uF, 25 V, 0805 part rather than the previous 10 V capacitor.
- R22=365 kOhm and R23=100 kOhm give a charger-side MPPT target of:

```text
Vmp(charger) = 1.205 V * (1 + 365 kOhm / 100 kOhm) = 5.603 V
```

Allowing roughly 0.4-0.6 V for D14 puts the corresponding panel-side operating
point around 6.0-6.2 V. This is coherent for the intended nominal-6 V panel, but
must be validated against the actual panel's voltage/current curve and cold
open-circuit voltage. Measure reverse current into a dark or disconnected panel
with a charged battery.

### Sensor rail protection

- U3/U4 are now the auto-retry `TPS2553DBVR` rather than latch-off `-1` parts.
- Their pin-to-net mapping is correct.
- R11=133 kOhm and R13=88.7 kOhm retain approximately 200 mA and 300 mA
  nominal current-limit targets.
- C13 adds 10 uF local bulk capacitance to `5V_SEN`; U3 retains 1 uF on
  `3V3_SEN`.

The accepted limitation remains that a persistent short disables and repeatedly
retries the complete voltage domain rather than only one port. Bench-test
simultaneous sensor startup, cable-end voltage, short behaviour, recovery, and
temperature.

### Reed input

D15 is an ESDS311 from connector-side `REED_SIG` to ground, placed about 4.8 mm
from the reed connector and ahead of R8. Its signal/ground connection and
placement are appropriate. Cable ESD, leakage, hot-plug, contact bounce, and
pulse-counting tests are still required.

### ESP32 module and SD interface

- U45 is now `ESP32-WROOM-32E-N4` and uses the corresponding footprint.
- The antenna end projects into the carrier-board notch, preserving the intended
  all-layer keepout approach.
- R24 (`SD_CS`), R26 (`SD_MOSI`), and R27 (`SD_SCK`) are 22 Ohm resistors at the
  ESP32 source end and have correct net mapping.

Final-enclosure Wi-Fi testing remains necessary. SD socket-side ESD and card
detect remain conditional design choices based on enclosure accessibility;
conservative-clock read/write and removal tests are mandatory.

### 5 V and ADC filtering

- C3/C76 are now specified as 22 uF, 10 V, 0805 parts.
- R28, C114=10 uF, and C119=100 nF create a filtered local supply for U30.
- C115-C118 are now 10 nF, improving high-frequency input filtering.
- C13 provides 10 uF local bulk capacitance on `5V_SEN`.

These changes should remain. They do not close F-02 because filtering after the
converter cannot substitute for a compact switching loop.

## Accepted prototype risks

The following decisions are retained for this prototype and must not be read as
production or certification approval:

- The existing LTE RF route and D4 placement remain because the assembled
  implementation has worked with the intended antenna. Retest registration,
  upload, RSRP, RSRQ, and SINR with the production enclosure, battery, cables,
  and weak-signal conditions. Do not hot-plug U.FL.
- USB D+/D- routing remains unequal and includes layer changes, but programming
  and enumeration have worked. Repeat USB tests on multiple assembled boards
  and cable types.
- No optional charger bulk-capacitor or snubber footprint was added. Switching
  ringing, ripple, temperature, panel-lead, battery-absent, and ADC-coupling
  measurements are therefore mandatory.
- SD socket-side ESD and card-detect handling remain dependent on whether the
  card is externally accessible in the finished enclosure.

## Required pre-order sequence

1. Remove all four mounting-hole copper rings while retaining the NPTH drills.
2. Repack the MT3608 power stage and feedback network into a compact layout.
3. Tighten the CN3791 sense traces into a true Kelvin pair.
4. Name the filtered rail `5V_ADC` and clean the schematic title blocks.
5. Regenerate source, BOM, CPL, Gerbers, drills, and a complete schematic PDF.
6. Recheck mounting-hole Gerbers and all critical routes in the regenerated
   files; do not rely only on the PCB editor view.
7. Select and document the exact protected battery and charge-temperature
   strategy.
8. Resolve the TH-V5 supply-voltage issue or complete a written qualification
   plan for 5 V operation.
9. Finish the assembly-specific fit/DNP/manual-placement instructions.
10. Run and archive ERC, DRC, and the selected manufacturer's PCB/PCBA DFM
    results.
11. Update the aviation-plug cable schedule after the first harness is wired and
    continuity-tested.

## Minimum bring-up tests

Use a current-limited bench supply and a known protected test cell before the
production panel or battery:

1. Inspect placement, orientation, soldering, and rail resistance before power.
2. Verify correct and reverse battery connection behaviour at Q37.
3. With a charged battery and dark/disconnected panel, measure reverse current
   through the solar port.
4. Sweep solar input over the actual panel's operating and cold-`Voc` range;
   confirm D14/C89 voltage margin and the approximate 5.60 V charger-side MPPT
   point.
5. Verify 600 mA current limit, 4.2 V regulation, taper, termination, and
   recharge while asleep, sampling, writing SD, and using LTE.
6. Scope CN3791 and MT3608 switch nodes with a short ground spring. Measure rail
   ripple, ringing, temperatures, startup overshoot, and ADC noise across
   battery voltage and load.
7. Load `5V_SYS` and `5V_SEN` to the intended maximum; verify regulation at the
   minimum allowed cell voltage.
8. Short and hot-plug each sensor domain; confirm current limiting, automatic
   recovery, connector voltage, and that the unaffected domain continues.
9. Characterise every soil probe over the full environmental/output range and
   compare with a reference. Confirm ADS1015 readings above 4.096 V.
10. Validate I2C rise time/noise with both SHT40 and AS734x-class devices on the
    final mux harnesses.
11. Validate reed contact bounce, pulse counting, leakage margin, and cable ESD.
12. Exercise SD read/write/removal, USB enumeration/programming, Wi-Fi, and LTE
    on multiple boards in the final enclosure.
13. Perform a thermal soak with charging, maximum sensor load, SD activity, and
    radio transmissions.

## Overall assessment

The October 8 release is internally coherent and corrects most of the specific
electrical issues identified on October 7. The solar-input circuit, capacitor
rating, sensor-switch variant, reed protection, ESP32 module, SD resistor
placement, and ADC filtering are all credible improvements.

It is not yet ready to order because the generated Gerbers prove that the
mounting-hole copper remains, and the source PCB proves that the MT3608 hot loop
was not repacked. Correct those items, tighten the CN3791 Kelvin pair during the
same pass, and submit the regenerated release for one focused final review.
