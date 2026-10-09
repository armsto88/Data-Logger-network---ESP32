# FieldHub v2 engineering review — 2026-10-08 revision 2

- **Review date:** 2026-10-08
- **Compared with:** `hardware/FieldHub_v2/revisions/2026-10-08`
- **EasyEDA source:** `HUB_ProPrj_MotherShip_v2_2026-10-08_#2.epro2`
- **Release directory:** `hardware/FieldHub_v2/revisions/2026-10-08-r2`

**Recommendation:** **Do not order this revision yet.**

Revision 2 materially improves the 5 V MT3608 switch-node and feedback routing,
but it does not yet close the converter-layout finding. The aggregate routed
length of the switch net has fallen from about 10.04 mm to 5.07 mm, and D6 is
now 3.61 mm from U21 rather than 10.55 mm. However, the bulk input capacitor is
still remote from the inductor input/current loop, the output-capacitor return
is not tight to U21 ground, and the feedback supply trace branches from the D6
cathode before reaching C76 rather than sensing at C76.

The four 3.4 mm mounting holes also still have 6.0 mm floating copper pads on
all four copper layers. That remains an independent pre-order blocker.

This release changed more than the reported 5 V spacing. Eighteen placement
records changed: ten in the 5 V stage and eight around the microSD socket. The
socket moved approximately 2.67 mm and its mechanical holes and local pull-up/
decoupling network moved with it. The regenerated files are internally
consistent, but enclosure fit and SD operation therefore need to be rechecked.

The MT3608 is a boost converter, not a buck converter; this review uses
"5 V boost stage" throughout.

## Review scope and limitations

This was a desktop review of:

- the supplied EasyEDA Pro source project;
- the BOM and pick-and-place exports;
- the four-layer Gerber and drill set;
- source/netlist/placement consistency;
- the changed 5 V and microSD placement/routing; and
- differences from the first October 8 release.

The review does not replace EasyEDA ERC/DRC, the selected fabricator's PCB and
PCBA DFM checks, controlled-impedance review, formal ESD/surge testing, RF
testing, certification work, or physical bring-up. No ERC, DRC, schematic PDF,
manufacturer DFM report, stack-up, battery datasheet, or test results were
included in this release.

## Release-integrity checks

| Check | Result |
|---|---|
| Imported files | Exact copies of the four supplied `#2` files are stored in this separate revision directory |
| Source archive integrity | Pass; the EasyEDA project ZIP tests successfully |
| Gerber ZIP integrity | Pass; all 17 archived files test successfully |
| BOM groups | 99 |
| BOM references | 262; no duplicate or missing references |
| Placement references | 262; exact match to the BOM and source PCB |
| Source unique IDs | All 262 source PCB component IDs occur in the BOM |
| Netlist | All 921 source PCB pad-to-net assignments are unchanged from the first October 8 release |
| Placement coordinates | 258 source origins agree directly; SJ4, SJ5, U1, and U2 use the same expected footprint-centroid offsets as the prior release |
| Assembly side | All 262 placements are top-side; no bottom-paste file is required |
| Changed placements | 18 changed; 244 unchanged |
| PCB vias | 372, up from 370 |
| Layer/drill set | Four copper layers, top/bottom mask, top paste, top/bottom silk, outline, PTH, NPTH, and via drill files are present |
| Flying-probe data | Contains every placed reference, plus expected EasyEDA pad pseudo-components |
| Board outline | Centreline geometry unchanged, approximately 100 x 90 mm, with the ESP32 antenna notch retained |

Key imported-file hashes:

| File | SHA-256 |
|---|---|
| `HUB_BOM_Board1_PCB1_2026-10-08_#2.csv` | `6f9f3e241c098e63bd18d76c958039a262e26a097306cf065720c78706d70f7f` |
| `HUB_PickAndPlace_PCB1_2026_10_08__2.csv` | `5faf63af99e1cde701e706e7f0c94f24bb3874feb22f4cbcb684e5be316cb771` |
| `HUB_Gerber_PCB1_2026-10-08_#2.zip` | `62b63e21ee567b64bef9827735952068197676550daead95a707a1eefc1f565f` |
| `HUB_ProPrj_MotherShip_v2_2026-10-08_#2.epro2` | `39b2b79c55f40cdb8a69ac69937c01cd0ed64b5f1c4a93712b41525d7b2a38cd` |

The BOM has the same components, values, footprints, manufacturer parts, and
supplier parts as the first October 8 release. Its only differences are live
supplier-stock figures captured at export time.

## Revision summary

| Metric | 2026-10-08 | 2026-10-08 revision 2 | Change |
|---|---:|---:|---:|
| BOM groups | 99 | 99 | None |
| PCB/BOM/placement references | 262 | 262 | None |
| PCB vias | 370 | 372 | +2 net |
| PCB routed line objects | 1,572 | 1,569 | -3 |
| Changed placements | — | 18 | See below |
| Board outline | Approx. 100 x 90 mm | Approx. 100 x 90 mm | Centreline unchanged |

Changed 5 V-stage placements:

```text
5V, C3, C4, C75, C76, D6, L4, R72, R73, U21
```

Changed microSD-area placements:

```text
C130, C131, R4, R5, R6, R7, R175, TF1
```

No references, footprints, values, manufacturer parts, or pad-to-net
assignments changed.

## Findings requiring action

### F-01 — Required: NPTH mounting holes still contain copper on every layer

The four holes remain correctly present only in the NPTH drill file as 3.4 mm
holes and remain absent from the plated-hole file. Their centres are unchanged:

```text
(158.2243, -10.6539) mm
(246.2243, -10.6539) mm
(246.2243, -88.6539) mm
(158.2243, -88.6539) mm
```

However, the top, bottom, Inner 1, and Inner 2 Gerbers still flash aperture D12,
a 6.0 mm circular copper pad, at all four coordinates. This leaves an
approximately 1.3 mm radial floating copper ring around every hole on every
copper layer.

**Action:** replace the mounting-pad objects with true mechanical NPTH holes,
or explicitly remove copper on all layers and add the intended fastener/head
keepout. In the regenerated outputs, verify all four holes in NPTH, none in
PTH, and no copper flash at those coordinates on any copper layer.

### F-02 — Required: the 5 V boost layout is improved but not yet a tight power stage

The revision makes measurable progress:

| Measurement | 2026-10-08 | Revision 2 |
|---|---:|---:|
| Aggregate `$9N52` SW-track length | 10.04 mm | 5.07 mm |
| SW-track bounding box | 9.39 x 1.21 mm | 2.28 x 2.61 mm |
| U21-to-D6 body-centre distance | 10.55 mm | 3.61 mm |
| U21-to-L4 body-centre distance | 4.32 mm | 6.02 mm |
| U21-to-C3 body-centre distance | 5.81 mm | 5.37 mm |
| D6-to-C76 body-centre distance | 4.38 mm | 3.85 mm |
| U21-to-C76 body-centre distance | 10.23 mm | 7.09 mm |
| Aggregate `$9N56` FB-track length | 10.18 mm | 3.68 mm |
| U21-to-R72/R73 body-centre distances | 8.17/7.94 mm | 3.86/3.42 mm |

U21, D6, and the SW-facing L4 pad are now much better grouped. The FB midpoint
is also routed on the top layer without the previous two FB vias. These changes
should be retained.

The remaining geometry does not yet satisfy the MT3608 layout requirements:

- C3 is the 22 uF input capacitor, but its positive pad is approximately
  11.98 mm from L4's `VSYS` pad and its ground pad is approximately 7.51 mm
  from U21 ground.
- C75 is a useful 100 nF local input capacitor and is close to U21 `IN`, but its
  positive pad is still approximately 10.30 mm from L4's `VSYS` pad and its
  ground return is not tight to U21 ground.
- D6 cathode reaches C76 positive through about 3.3 mm of routed copper. C76
  ground is approximately 7.16 mm from U21 ground, so the diode/output-cap/
  switch return loop remains spread out.
- The R72 supply/sense route branches from the D6-cathode area, enters an inner
  layer, and reaches R72 before the power route reaches C76. It therefore does
  not sense directly from the quiet side of C76 as requested.
- The two removed FB vias were replaced by two `5V_SYS` and two `VSYS` vias.
  The GND-via count is unchanged, and no dedicated local ground vias were added
  beside U21, C3/C75, or C76.

This means the noisy SW connection is smaller, but the complete high-di/dt
input and output loops are not yet compact.

**Action:** keep the improved U21/D6/SW placement, then repack the stage as a
complete current-loop problem:

1. Put C3/C75 at the L4-input/U21-ground side so their positive and ground
   connections close the local input-current loop.
2. Put C76 and C4 immediately at D6 cathode and the local U21 ground return.
3. Take R72's output sense directly from the C76 positive pad or its quiet
   downstream side, not from the D6-cathode branch.
4. Add short, intentional ground returns and local ground vias beside U21 and
   both capacitor groups while preserving the continuous ground plane.
5. Keep the FB midpoint on the quiet side of U21 and away from the SW copper.

Then regenerate the layout and scope `SW`, `5V_SYS`, and the filtered ADC supply
at minimum battery voltage and maximum sensor load with a short probe ground
spring.

Reference: [MT3608 datasheet](https://www.mouser.com/datasheet/2/306/MT3608-3223743.pdf).

### F-03 — High: CN3791 Kelvin routing is unchanged and is not yet a close pair

The charger region did not change in revision 2. The two sense traces remain
direct and same-layer, but they leave adjacent CN3791 pins in opposite
directions and approach opposite R25 pads from separated sides. They therefore
do not behave as a close companion Kelvin pair.

**Action:** route `CSP` and `BAT` together as a short same-layer pair directly
to opposite R25 pads, with shared load and charge current kept out of both
sense traces.

Reference: [CN3791 manufacturer datasheet](https://www.consonance-elec.com/static/upload/file/20231218/1702881078530301.pdf).

### F-04 — High: battery safety remains dependent on the selected pack/system

No exact battery datasheet or system charge-temperature strategy was included.
The board does not measure cell temperature, and `BAT_BUS` remains both the
battery and system-load node.

**Action:** qualify the exact protected pack, confirm that 600 mA charging is
permitted, implement charge-temperature control at system level, and validate
termination/recharge while the Hub sleeps, samples, writes SD, and uses the
radio.

### F-05 — High: selected soil probes remain outside their stated supply range

The ADC connectors still provide `5V_SEN`, while the selected TH-V5-type probes
are reported to require 10-30 V. Apparent operation at 5 V is not a substitute
for accuracy, linearity, startup, temperature, cable-length, and unit-to-unit
qualification.

**Action:** retain the exact probe datasheet and either provide a compliant
supply/interface or document a complete 5 V qualification. The ADS1015
firmware must use the +/-6.144 V PGA range and 6.144 V conversion scale before
accepting a nominal 0-5 V input.

### F-06 — High: aviation-connector crossover documentation remains incomplete

No revised controlled harness schedule was supplied. A same-pin-number harness
can still reverse or short power and ground because the PCB and aviation-plug
numbering differ.

**Action:** update `hardware/SENSOR_WIRING_AVATION_PLUGS.md` with PCB pin, wire
colour, intermediate connection, aviation pin/signal, and explicit mating- or
solder-face views. Continuity- and short-test every completed harness before a
sensor is connected.

### F-07 — Medium/high: assembly instructions remain incomplete

The assembly data remain unchanged in substance:

- 141 placements use Extended parts and 101 use Basic parts.
- Twenty references have no supplier part number: 17 test points, SJ4, SJ5,
  and U1.
- U47, U54, U1, and U2 are marked `SMD=No`.
- SJ4/SJ5 need explicit open/closed and fit/DNP instructions.
- `1uf1` and `edaimagecopyoccupy` remain nonstandard references.

**Action:** create assembly-specific BOM/CPL instructions with explicit `FIT`,
`DNP`, and `MANUAL/THT` status and inspect the assembler preview.

### F-08 — Medium: DRC and DFM evidence remain absent

The source still contains zero component/device-body and through-hole-to-SMD
clearance rules. No ERC, DRC, or manufacturer DFM reports were supplied.

**Action:** define nonzero assembly/fabrication clearances, run ERC and DRC,
archive their reports, and run the selected fabricator's PCB/PCBA DFM checks.

### F-09 — Low/medium: schematic release metadata remain inconsistent

The schematic-sheet metadata did not change. The overview and modem sheets
still have inconsistent project names, page counts, and dates.

**Action:** standardise the title blocks and archive a complete schematic PDF.

### F-10 — Low: the filtered ADC rail still has an anonymous net name

R28, C114, and C119 still create the intended filtered U30 supply, but the
post-R28 net remains anonymous rather than `5V_ADC`.

**Action:** name the filtered rail `5V_ADC`.

### F-11 — Medium: the microSD socket and local network moved outside the reported scope

TF1 moved from `(221.996, -48.768)` mm to `(221.615, -46.101)` mm: 0.381 mm
left and 2.667 mm upward. Its two 1.0 mm mechanical holes moved by the same
amount. R4-R7, R175, C130, and C131 also moved by approximately 3-4 mm, and the
associated vias and copper were regenerated.

The source, placement file, drills, and Gerbers agree, so this is not an export
mismatch. It is nevertheless a mechanical and signal-layout change, not merely
a converter-spacing change. The ESP32-side SD series resistors R24, R26, and
R27 did not move.

**Action:** confirm that the new TF1 location matches the enclosure/card-access
opening, preserves insertion/ejection clearance, and has adequate courtyard
and rework space. Include the moved SD region in DRC/DFM review and repeat
conservative-clock card detection, read/write, removal, and power-cycle tests.

## Verified changes worth retaining

- U21 and D6 are now close enough to form a much smaller SW region.
- The SW-track bounding area and aggregate length are substantially reduced.
- The FB midpoint no longer changes layers and is much shorter.
- C4 remains the local 100 nF `5V_SYS` output capacitor beside C76.
- The BOM and electrical pad-to-net assignments are unchanged.
- Source, BOM, placement, Gerber, drill, and flying-probe reference sets remain
  internally consistent.
- All verified corrections documented in the first October 8 review remain in
  the design: solar-input polarity/rating changes, auto-retry sensor switches,
  reed TVS, current ESP32 module, source-side SD damping resistors, and ADC
  filtering.

## Required pre-order sequence

1. Remove the four mounting-hole copper rings while retaining the NPTH drills.
2. Finish the MT3608 input/output current-loop placement and move the feedback
   pickoff to C76.
3. Tighten the CN3791 sense routing into a true Kelvin pair.
4. Confirm and document the moved microSD socket's enclosure fit.
5. Name the filtered rail `5V_ADC` and clean the schematic title blocks.
6. Regenerate source, BOM, CPL, Gerbers, drills, and a schematic PDF.
7. Recheck the mounting-hole flashes and critical converter routes in the
   regenerated Gerbers, not only in the PCB editor.
8. Complete battery, probe, harness, and assembly documentation.
9. Run and archive ERC, DRC, and PCB/PCBA DFM results.
10. Complete power-stage, ADC-noise, SD, sensor, radio, and thermal bring-up
    testing on assembled boards.

## Overall assessment

Revision 2 is internally coherent and the switch-node repack is a meaningful
improvement. It cuts the SW routing approximately in half and places D6 close
to U21. It should not yet be ordered because the full input/output current loops
remain spread out, the feedback senses before C76, and the mounting-hole copper
blocker is unchanged. The unreported microSD relocation also requires explicit
mechanical and functional validation before this set can be treated as a
release candidate.
