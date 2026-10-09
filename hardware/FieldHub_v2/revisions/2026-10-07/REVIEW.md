# FieldHub v2 engineering review — 2026-10-07 release

- **Review date:** 2026-10-07
- **Compared with:** `hardware/FieldHub_v2/revisions/2026-10-01`
- **EasyEDA source:** `Hub_ProPrj_MotherShip_v2_2026-10-07.epro2`
- **Release directory:** `hardware/FieldHub_v2/revisions/2026-10-07`

**Recommendation:** **Do not order a production assembly run from this
revision yet.**

This release is a substantial improvement over the 2026-10-01 revision. It
corrects the battery reverse-polarity MOSFET and reed-input resistor topology,
adds connector-side ESD protection, and introduces protected sensor-power
rails. It also replaces the CN3163 linear solar charger with a CN3791 MPPT buck
charger.

Before fabrication, correct the solar-input blocking circuit and the input
capacitor voltage rating. The CN3791 current-sense routing should also be
reworked as a Kelvin pair. Several connector, RF, battery-safety, layout-rule,
and assembly-package findings from the previous review remain open.

## Review scope and limitations

This was a desktop review of:

- the supplied EasyEDA Pro source project;
- schematic connectivity and component pin mapping;
- the four-layer PCB, placement, board outline, and selected critical routes;
- the Gerber and drill archive;
- the BOM and pick-and-place exports; and
- differences from the repository's 2026-10-01 release and review.

The review does not replace EasyEDA ERC/DRC, the fabricator's DFM checks,
controlled-impedance review, RF simulation, regulatory review, or physical
bring-up and environmental testing.

## Release-integrity checks

| Check | Result |
|---|---|
| Source used | The later `Hub_ProPrj_MotherShip_v2_2026-10-07.epro2`; the earlier download named `MotherShip_v2.epro2` was intentionally disregarded |
| Imported files | Exact byte-for-byte copies of the four supplied October 7 files |
| Gerber ZIP integrity | Pass; all archived files test successfully |
| Layer/drill set | Four copper layers, top/bottom mask, top paste, top/bottom silkscreen, outline, PTH, NPTH, and via drill data present |
| BOM versus placement references | Pass; 260 references in each, with an exact set match |
| Source PCB versus BOM | Pass; all 260 designators and component unique IDs match |
| Source PCB versus placement data | Pass; reference coordinates match; four asymmetric parts use centroid rather than source-origin coordinates, as expected |
| Flying-probe data | All 260 placement references are represented; additional EasyEDA pad pseudo-components are expected |
| Bottom-side assembly | No bottom components; absence of a bottom-paste file is expected |

Key imported-file hashes:

| File | SHA-256 |
|---|---|
| `Hub_ProPrj_MotherShip_v2_2026-10-07.epro2` | `a8602a01b1e135e1a6b63e9cac61e66465e2dc15de8da9f9e7ff747c59848cab` |
| `HUB_Gerber_PCB1_2026-10-07.zip` | `ef107829cd4854d7d4c8f7b698f998d601de20702c663be3cb1c8aa6133cbc44` |
| `HUB_BOM_Board1_PCB1_2026-10-07.csv` | `a6ef2c9bb759a2fec66f9c971ac58fc023d809b20f7763bdce8432be207ab846` |
| `HUB_PickAndPlace_PCB1_2026_10_07.csv` | `aecb9367eb590896720faedd72218a1ae12654b5d4de1f7461fe1ab24de7b529` |

## Revision summary

### Manufacturing-data delta

| Metric | 2026-10-01 | 2026-10-07 | Change |
|---|---:|---:|---:|
| BOM groups | 86 | 96 | +10 |
| BOM/placement references | 234 | 260 | +26 |
| PCB vias | 356 | 371 | +15 |
| Extended-part placements | 117 | 139 | +22 |
| Board outline | Approximately 100 x 90 mm | Approximately 100 x 90 mm | No material change |

Thirty-five references were added:

```text
C5-C12
D5, D7-D13
F3
L2
Q1, Q5
R11, R13, R14, R17, R18, R20-R25
RN3
U3-U5
```

Nine references were removed:

```text
C88, C90
F5
R78, R81, R181
RN8, RN9
U23
```

Approximately 29 retained components changed placement, primarily around the
lower charger and sensor-interface region. The much larger bottom-silkscreen
Gerber is primarily due to the new `FieldHub V2` graphic, not bottom-side
assembly.

### Schematic delta

The following functional sheets changed materially:

- `CHARGE`;
- `ADC`;
- `MUX`;
- `MCU`; and
- `Input + Logic`.

`MODEM`, `PWR + RTC`, and `Overview` have no comparable functional change.

Major electrical changes are:

- `Q37` battery reverse-polarity MOSFET source/drain correction;
- `R8` moved into the actual reed-input signal path;
- four I2C connector ESD arrays and two ADC connector ESD arrays;
- `3V3_SEN` and `5V_SEN` switched/current-limited sensor rails;
- replacement of ADC pull-down resistor arrays with discrete 1 MOhm parts;
- replacement of the CN3163 linear charger with a CN3791 buck/MPPT charger;
- a 1.1 A resettable solar-input fuse and SMF12A TVS;
- nominal 6.05 V MPPT setting and nominal 600 mA charge-current setting; and
- status LEDs powered from `SOLAR_IN` rather than `BAT_BUS`, reducing night-time
  battery drain.

## Findings requiring action

### F-01 — Blocker: Q1 is not the CN3791 reverse-current blocking circuit

The solar input is presently:

```text
panel -> F3 -> TVS node -> Q1 drain
Q1 source              -> SOLAR_IN
Q1 gate                -> R24 22 kOhm -> GND
```

This is a conventional P-channel reverse-input-polarity arrangement. It is not
the CN3791 manufacturer's optional reverse-current blocking MOSFET circuit,
whose gate is controlled from the charger `VG` drive. A gate held at ground can
turn Q1 on whenever its `SOLAR_IN` source is raised above ground, so Q1 cannot
serve as the required night-time backflow blocker.

**Action:** choose and document one of these supported approaches:

1. Fit the CN3791 application circuit's series blocking Schottky diode; or
2. Implement the datasheet's `VG`-controlled P-channel MOSFET replacement for
   that diode, verifying the exact source/drain orientation against the final
   selected MOSFET symbol and footprint.

Then measure current into the disconnected/dark panel port with a charged
battery and with the complete Hub in sleep, idle, and LTE-transmit states.

The existing `hardware/FieldHub_v2/SOLAR_CHARGER_6V_3W.md` description of a
ground-gated reverse-polarity PMOS as eliminating the blocking diode must be
corrected before it is reused as design guidance.

References:

- [CN3791 manufacturer product page](https://www.consonance-elec.com/69.html)
- [CN3791 manufacturer datasheet](https://www.consonance-elec.com/static/upload/file/20231218/1702881078530301.pdf)

### F-02 — Blocker: C89 is underrated for the documented panel-input envelope

`C89` is Samsung `CL10A106KP8NNNC`, a 10 uF, 10 V, 0603 capacitor connected
from `SOLAR_IN` to ground. The existing charger design envelope allows panel
cold open-circuit voltage up to, but below, 12 V. A nominal-12 V-standoff TVS
does not keep a 10 V capacitor within rating during normal open-circuit panel
operation.

This does not prove that the selected panel exceeds 10 V, but the released part
is incompatible with the repository's stated design envelope and lacks useful
voltage margin.

**Action:** replace C89 with at least a 16 V X5R/X7R part, preferably 25 V if
space permits. An 0805 or larger package is preferable to retain useful
capacitance under DC bias. Confirm the actual panel's `Voc` at minimum expected
temperature and account for tolerance and transients.

### F-03 — High: CN3791 current sensing is not Kelvin-routed

The route from CN3791 `CSP` to the R25 sense pad is approximately 15 mm long,
uses two vias, and changes layer. The CN3791 `BAT` pin joins the shared
`BAT_BUS` routing instead of running as a close, direct companion trace to the
opposite R25 pad. R25 is approximately 8 mm from U5 and is not immediately
adjacent to the inductor.

The CN3791 measures roughly 120 mV across this resistor. Shared charge/system
current copper and switching noise in either sense path can therefore create a
material charge-current error or unstable/noisy regulation.

**Action:** place R25 immediately after L2. Route `CSP` and `BAT` as a close,
same-layer Kelvin pair directly to the two resistor pads. Join the high-current
`BAT_BUS` only after the battery-side pad; do not share sense copper with the
charger or system-current path. Follow the layout guidance in the
[CN3791 datasheet](https://www.consonance-elec.com/static/upload/file/20231218/1702881078530301.pdf).

### F-04 — High: sensor protection is shared by domain, not isolated per port

This revision adds two TPS2553DBVR-1 load switches:

- U3 generates `3V3_SEN` for all four I2C connectors; R11=133 kOhm gives an
  approximate 202 mA nominal limit, with a datasheet spread of roughly
  175-232 mA.
- U4 generates `5V_SEN` for both ADC connectors; R13=88.7 kOhm gives an
  approximate 299 mA nominal limit, with a spread of roughly 265-339 mA.

This is a strong improvement over exposing the system rails directly. However,
the `-1` variant latches off after a persistent overcurrent. Each enable is tied
to its input and each fault output is unused, so one faulty cable or sensor can
disable every port in that voltage domain until the upstream rail is power
cycled. There is no per-port fault containment or software-visible fault state.

**Action:** confirm that shared-domain latch-off is the intended product
behaviour. Validate total normal current and simultaneous sensor inrush against
the minimum current-limit threshold. If continued operation of healthy ports is
required, use per-port protection. Otherwise expose the fault outputs and add a
controlled reset mechanism if practical.

**Resolution decision (2026-10-08):** shared protection by voltage domain is
accepted because the board does not have space for per-port switches. Replace
the latch-off `TPS2553DBVR-1` at U3 and U4 with the pin-compatible auto-retry
`TPS2553DBVR`; retain R11=133 kOhm and R13=88.7 kOhm. The accepted limitation is
that a persistent short interrupts every sensor on the affected voltage domain,
while the other domain remains available. Bench verification of steady-state
current, simultaneous startup, cable-end voltage, short-circuit behaviour, and
automatic recovery remains required.

**Firmware follow-up:** the two TH-V5 probes have 0-5 V outputs, but the node's
ADS1015 helper currently selects the +/-4.096 V PGA range and uses 4.096 V for
conversion scaling. Change it to the +/-6.144 V range and 6.144 V scale before
using the full probe output range; add a near-full-scale test to confirm readings
above 4.096 V no longer clip.

Reference: [TPS2553 datasheet](https://www.ti.com/lit/ds/symlink/tps2553.pdf).

### F-05 — High: aviation-connector pin mapping remains unsafe unless a crossover harness is documented

PCB connector numbering remains:

| PCB connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| `CN17`-`CN20` I2C | 3V3_SEN | GND | SDAx | SCLx |
| `CN15` ADC | 5V_SEN | GND | A1 | A0 |
| `CN16` ADC | 5V_SEN | GND | A2 | A3 |

The repository aviation-plug documentation numbers an I2C connector as pin 1
ground, pin 2 SDA, pin 3 SCL, and pin 4 power. A same-number cable would connect
PCB power to the aviation connector ground assignment.

**Action:** create a controlled cable schedule for each sensor type showing PCB
reference and pin, wire colour, intermediate connector pin, aviation pin, and
signal. State whether views are from the mating face or solder face. Add a
continuity and short-circuit check to cable production and incoming inspection.

**Resolution plan (2026-10-08):** retain the PCB and aviation-connector pin
assignments and use documented crossover harnesses. The harnesses will be wired,
labelled, and continuity/short tested after the hardware is built, after which
`hardware/SENSOR_WIRING_AVATION_PLUGS.md` will be updated with the actual wire
colours and mating-face pin views. This finding remains open until that controlled
cable schedule and inspection result exist.

### F-06 — High: battery temperature and cell protection remain external assumptions

Changing to CN3791 improves compatibility with the nominal-6 V solar panel, but
does not add battery-temperature sensing or cell over-voltage, under-voltage,
over-current, and short-circuit protection. `BAT_BUS` remains both the battery
and system-power node; there is no separate load-sharing/power-path output.

**Action:** require and document a protected 1S Li-ion/LiPo pack whose allowed
charge current exceeds the worst-case charger current. Add battery-temperature-
qualified charging, or document and validate a system-level method that prevents
charging outside the cell manufacturer's temperature range. Test charger
termination and recharge with the Hub asleep, awake, and transmitting because
system load changes the current measured at the battery node.

### F-07 — High: exposed interfaces are improved but reed/wind protection is incomplete

The new USBLC6-2SC6 arrays `D5`, `D7`, `D8`, and `D9` protect the four I2C
connectors; `D10` and `D11` protect the two ADC connectors. They are placed
approximately 4-6 mm from their connectors and ahead of the existing series
resistors. This substantially improves the previous revision.

The reed/anemometer connector still has no connector-side TVS. R8 is now in the
correct series position and the MCU-side R9/C2 bias/filter topology is correct,
but R8 alone is not a defined cable ESD or surge solution.

**Action:** add a suitably low-leakage TVS at the reed connector with a short,
direct return. Validate all external ports for cable ESD, hot plug, adjacent-pin
shorts, wet/dirty cable leakage, and the maximum harness length. Validate I2C
rise time with all intended sensors at 100 kHz.

**Resolution implementation reported (2026-10-08):** `D14` has been added as an
ESDS311 between the connector-side `REED_SIG` node and ground, ahead of R8, with
the cathode on `REED_SIG` and the anode on ground. The PCB placement and short
ground return were also completed. The design correction is complete; cable ESD,
hot-plug, leakage, maximum-harness, pulse-counting, and I2C rise-time validation
remain bring-up tests.

### F-08 — High: LTE RF route and connector-side protection are unchanged

The approximately 40 mm LTE route, two signal-layer transitions, return-via
spacing, undefined controlled impedance, and RF ESD placement near the modem
rather than the exposed U.FL remain unchanged from the previous review.

**Action:** obtain the fabricator's actual four-layer stack-up and calculate a
50 Ohm route. Prefer one uninterrupted outer-layer route. If a transition is
unavoidable, place tightly coupled return vias beside it. Put RF ESD at the U.FL
connector, reserve a matching network, maintain an appropriate coplanar/ground
reference, and verify performance in the final enclosure.

**Resolution decision (2026-10-08):** retain the existing LTE route and D4
placement for this prototype revision because the assembled implementation has
already operated successfully with its intended antenna. The unverified
controlled impedance and connector-side ESD exposure are accepted prototype
risks, not a claim that the route meets a formal 50 Ohm or immunity requirement.
Repeat registration and upload testing with the final antenna, cable, enclosure,
power system, and weak-signal conditions; record RSRP, RSRQ, and SINR. Do not
hot-plug the U.FL connector, and revisit the route before certification or volume
production if testing shows inadequate margin.

### F-09 — Medium/high: charger switching layout needs bench validation and may need another pass

The Q5, D12, and L2 switching cluster is reasonably local, but the switch path
is still several millimetres long and the charger is close to the ADC region.
The input has 10 uF plus 100 nF ceramic capacitance and the output has 22 uF plus
100 nF, with no electrolytic/bulk reserve footprint and no optional switch-node
snubber footprint.

The programmed values are internally coherent:

```text
Vmp = 1.205 V * (1 + 402 kOhm / 100 kOhm) = 6.049 V nominal
Ichg = 120 mV / 0.200 Ohm = 600 mA nominal
```

The 22 uH inductor's 1.2 A RMS and 1.5 A saturation ratings are adequate for
the nominal charge current, although ripple and tolerance should be measured.

**Action:** after correcting F-01 through F-03, scope `SW` with a short ground
spring; measure input/output ripple, ringing, component temperature, charge
current, and ADC noise. Test with the battery absent and with long panel leads.
Add bulk-capacitor and RC-snubber footprints if the layout is revised.

**Resolution decision (2026-10-08):** no additional bulk-capacitor or RC-snubber
footprints will be added in this prototype revision. The existing charger layout
is retained. Switching-node ringing, input/output ripple, temperature, charge
current, long-panel-lead behaviour, battery-absent behaviour, and ADC-noise
coupling remain mandatory bring-up measurements; any failed measurement requires
rework or a later layout revision.

### F-10 — Medium/high: ESP32 antenna keepout and module lifecycle remain open

The module remains at the board edge over a notch, which is directionally good,
but the release still does not demonstrate the full manufacturer-specified
all-layer antenna keepout relative to the exact module origin. Nearby hardware,
cables, the enclosure, and copper can detune it. ESP32-WROOM-32D lifecycle risk
also remains relevant for a new production design.

**Action:** check the exact keepout against the selected module documentation,
prefer an antenna entirely beyond the carrier-board edge, and test RSSI,
throughput, and range in the final enclosure. Evaluate a footprint-compatible
current module before volume production.

**Resolution implementation reported (2026-10-08):** U45 was changed from
`ESP32-WROOM-32D-N4` to the footprint-compatible `ESP32-WROOM-32E-N4`, and the
antenna-area all-layer keepout was checked/updated against the module land
pattern. The design correction is complete; final-enclosure RSSI, throughput,
range, and current testing remains required with the production battery, cables,
fasteners, and enclosure fitted.

### F-11 — Medium: SD, USB, and 5 V boost layout findings remain open

- SD clock, MOSI, and chip-select damping resistors remain near the socket rather
  than the ESP32 source. The long routes and unused card-detect contact are
  unchanged, and there is no socket-side SD ESD protection.
- USB D+/D- still use different route lengths/topologies, with D- changing layer
  twice. This may operate at Full Speed but is not a clean differential-pair
  implementation.
- The MT3608 boost hot loop remains spread out near the ADC section. The new
  charger introduces an additional switching source in the same broad area.

**Action:** address these in the next layout pass and validate SD signal quality,
USB enumeration, 5 V ripple, and ADC noise at worst-case radio and sensor load.

**Partial resolution implementation reported (2026-10-08):** the existing 22
Ohm SD source-series resistors R1 (CS), R2 (SCK), and R3 (MOSI) were moved from
the socket end to the corresponding ESP32 source pins and the routes were
updated. Conservative-clock SD read/write testing and signal-quality validation
remain bring-up work. Socket-side ESD and card detect remain conditional on
whether the finished enclosure makes the card user-accessible. The USB and 5 V
boost portions of this finding remain open.

**USB resolution decision (2026-10-08):** retain the existing USB D+/D- routing
for this prototype because enumeration, programming, and communication have been
confirmed on hardware. Its unequal route topology is accepted as a prototype
layout risk. Revisit controlled differential routing if failures appear or
before certification/volume production.

**5 V boost resolution implementation reported (2026-10-08):** the U21/L4/D6
power stage and its input/output capacitors were repacked to minimise the high-
current and switch-node loops, with the feedback divider kept close to U21 and
away from the switch node. The 22 uF input and output capacitors were specified
as 10 V X5R/X7R parts in larger packages to preserve effective capacitance. U30
is now supplied from a separately named `5V_ADC` rail through a 10 Ohm series
resistor, with 10 uF plus 100 nF local decoupling; C115-C118 were increased from
1 nF to 10 nF, and 10 uF local bulk capacitance was added at `5V_SEN`. The design
changes are complete. Startup overshoot, 5 V regulation/ripple, converter
temperature, sensor short/recovery behaviour, and ADC noise remain mandatory
bring-up measurements at minimum battery voltage and maximum sensor load.

### F-12 — Medium: mounting holes remain floating plated holes

All four 3.4 mm mounting holes remain plated through-holes with copper annuli
and no intentional net. Metal hardware can capacitively or intermittently couple
these floating rings and provide an uncontrolled ESD path.

**Action:** use NPTH holes with appropriate all-layer clearance when they are
purely mechanical. If chassis bonding is intentional, connect them through a
documented chassis/ESD strategy.

**Resolution implementation reported (2026-10-08):** plating was disabled for
all four 3.4 mm mechanical mounting holes, converting them to NPTH holes while
retaining their locations. Before ordering, confirm in the regenerated Gerber
and drill preview that each hole is present in the NPTH drill output, absent
from the plated drill output, has no copper annulus on any layer, and retains
adequate copper and screw-head clearance.

### F-13 — Medium: assembly outputs still require production cleanup

The BOM, source, and placement file agree, but the exports are not yet a clean
contract-manufacturing instruction set:

- 17 test points are present as placement components;
- 139 placements use Extended parts, up from 117;
- 20 placed references have no supplier part number;
- `U47`, `U54`, `U1`, and `U2` are marked non-SMD and may be omitted from an
  SMT-only quotation/order;
- two solder jumpers require explicit fitted/open configuration;
- `1uf1` and `edaimagecopyoccupy` are still nonstandard production references;
- test points do not consistently use stable `TPxxx` designators; and
- all components are on the top side, which agrees with the single paste layer.

**Action:** produce assembly-specific BOM and CPL files with explicit `FIT`,
`DNP`, and manual/THT instructions. Inspect the assembler's component mapping,
polarity, centroid, and rotation preview, especially USB-C, SIM, microSD, the
battery holder, diodes, LEDs, modem, ESP32, CN3791, MOSFETs, and fine-pitch ICs.

### F-14 — Medium: DRC does not enforce useful component-body spacing

The EasyEDA design-rule data still contain zero component/device and
through-hole-to-SMD clearance values. Trace/space DRC can therefore pass while
component bodies, courtyards, nozzles, rework access, hardware, or enclosure
features interfere.

**Action:** define fabrication- and assembly-appropriate clearances, run ERC and
DRC again, and run the selected manufacturer's DFM analysis. Review all edges,
the antenna notch, connectors, tall parts, and mounting hardware manually.

### F-15 — Low/medium: schematic release metadata are only partly cleaned up

Most changed pages now identify `MotherShip_v2`, but sheet totals remain
inconsistent: changed sheets show a total of eight while at least the unchanged
`PWR + RTC` sheet still reports a total of six. Mixed metadata can cause old and
new prints to be combined accidentally.

**Action:** standardize project name, board revision, date, page number, total
page count, and sheet title before generating the released schematic PDF.

## Disposition of the 2026-10-01 findings

| Previous finding | 2026-10-07 status |
|---|---|
| F-01 Q37 reverse-battery orientation | **Closed in design:** battery input is now on Q37 pin 3/drain and `BAT_BUS` on pin 2/source; bench verification still required |
| F-02 reed resistor on wrong branch | **Circuit corrected:** connector now passes through R8 before the MCU-side pull-up/filter; connector TVS still required |
| F-03 no field-port ESD/current containment | **Partly closed:** I2C/ADC ESD and shared protected rails added; reed TVS and per-port isolation remain open |
| F-04 PCB/aviation pin mapping mismatch | **Open** |
| F-05 LTE RF path | **Open** |
| F-06 battery temperature/cell protection | **Open:** charger changed, but these safety functions were not added |
| F-07 ESP32 keepout/lifecycle | **Open** |
| F-08 SD termination/ESD/card detect | **Open** |
| F-09 MT3608 hot loop/ADC noise | **Open;** CN3791 adds another switching-noise source to validate |
| F-10 floating plated mounting holes | **Open** |
| F-11 assembly cleanup | **Open:** component count and Extended-part count increased |
| F-12 zero component-clearance rules | **Open** |
| F-13 USB routing/documentation | **Partly open:** USB routing unchanged; title blocks improved but page totals remain inconsistent |

## Items that look correct

- The October 7 project, BOM, placement file, Gerber archive, and flying-probe
  data identify the same PCB population.
- The board remains a closed, approximately 100 x 90 mm, four-layer design.
- All placed components are on the top, consistent with the paste data.
- Q37 now has the correct single-PMOS reverse-battery orientation.
- The reed contact now feeds the MCU through R8, with R9 and C2 on the MCU side.
- Connector ESD arrays are placed on the connector side of the series elements.
- ADC 1 MOhm bias resistors remain present after replacement of RN8/RN9 with
  discrete resistors.
- The four muxed I2C branches retain their individual pull-ups and 33 Ohm series
  resistors.
- The 402 kOhm/100 kOhm MPPT divider targets approximately 6.05 V.
- The 0.200 Ohm current-sense resistor targets approximately 600 mA.
- Charger status LEDs are supplied from the solar input rather than the battery.
- The solar fuse, input TVS, charger power stage, compensation network, output
  capacitors, and main component voltage/current classes are present.
- No obvious ESP32 flash-pin conflict was introduced.

## Required pre-order sequence

1. Correct the Q1 blocking topology and update the charger design note.
2. Replace C89 with a suitably rated capacitor.
3. Re-route the CN3791 sense connections as a true Kelvin pair.
4. Decide whether shared latch-off sensor domains are acceptable and verify
   current/inrush margins.
5. Resolve and document every cable pin mapping.
6. Confirm protected-battery and charge-temperature requirements.
7. Clean the assembly BOM/CPL and define all DNP/manual-fit states.
8. Standardize schematic metadata and generate a released schematic PDF.
9. Run EasyEDA ERC/DRC with nonzero assembly clearances and obtain PCB/PCBA DFM
   approval from the selected manufacturer.
10. Repeat this comparison against the regenerated source, BOM, CPL, and Gerbers.

## Minimum bring-up tests

Use a current-limited bench supply and a protected test cell before connecting
the production panel or battery:

1. Verify correct-polarity and reverse-polarity battery behaviour at Q37.
2. With a charged battery and dark/disconnected panel, measure current flowing
   out of the solar connector.
3. Sweep solar input from minimum startup voltage to the panel's measured
   cold-temperature `Voc`; verify all component voltages and temperatures.
4. Verify the programmed MPPT voltage with a current-limited panel simulator.
5. Verify charge-current limit, 4.2 V regulation, taper, termination, and
   automatic recharge while the Hub sleeps, operates, and transmits over LTE.
6. Scope the charger switch node and measure ripple and ADC noise under all major
   system-load combinations.
7. Short and hot-plug every sensor rail/port; verify latch-off, recovery, fault
   containment, and that no connector or load switch exceeds its rating.
8. Validate reed operation, contact bounce, leakage margin, and cable ESD.
9. Validate I2C rise time/noise with the final harnesses and all intended sensors.
10. Inspect the assembled-board rotations and polarity before powering the first
    unit, then run thermal and enclosure-level RF tests.

## Overall assessment

The October 7 revision is coherent and materially closer to a robust FieldHub
v2. The prior Q37 and reed-topology errors are corrected, and the added ESD and
sensor-power protection are worthwhile improvements. The CN3791 conversion is
electrically plausible and its programmed voltage/current values are sensible,
but the solar blocking topology, C89 voltage rating, and current-sense routing
must be corrected before the revision should be treated as production-ready.
