# Hub v2 engineering review — 2026-10-01 release

- **Review date:** 2026-10-01
- **Release source:** `/home/tom/Downloads/HUB_v2_1.10.26`
- **Imported to:** `hardware/FieldHub_v2/revisions/2026-10-01`

**Recommendation:** **Do not order a production assembly run from this revision.**

The design is substantially complete and the exported files agree with each
other, but one battery-protection error should be corrected before fabrication.
Several field-interface, RF, battery-safety, and assembly issues also need an
explicit decision before this can be considered production-ready.

## Review scope

This was a desktop review of:

- the EasyEDA Pro source project and all seven separately exported schematic sheets;
- schematic connectivity and component pin mapping;
- the four-layer PCB stack, copper routing, planes, vias, placement, and board outline;
- the Gerber and drill archive;
- the BOM and pick-and-place exports; and
- the repository's existing power, pinout, and pre-order notes.

This is not a substitute for EasyEDA ERC/DRC, the assembler's DFM check, an RF
simulation, regulatory review, or physical bring-up testing.

## Release-integrity checks

| Check | Result |
|---|---|
| Supplied files against download folder | Exact byte-for-byte match; this review is the only added release file |
| Gerber ZIP integrity | Pass; no corrupt files |
| Copper/drill set | Four copper layers, top/bottom mask, top paste, silkscreen, outline, PTH/NPTH/via drills present |
| BOM versus placement references | Pass; 234 references in each, with an exact set match |
| BOM grouped quantities | Pass |
| Standalone schematics versus main project | All seven rendered sheets match the main-project sheets |
| Bottom-side assembly | No bottom-side components; absence of bottom paste is therefore expected |

Key imported-file hashes:

| File | SHA-256 |
|---|---|
| `main_1_10_26_HUB_v2.epro2` | `f0aa1ad28ef95e068f101b11961859392dd2fbb682406254d99bec2452450478` |
| `HUB_Gerber_PCB1_2026-10-01.zip` | `0fb05f0d2720d341ae23c3b96280ab66ff1c28cfc21d0caea23fdd9d49adc9f4` |
| `HUB_BOM_Board1_PCB1_2026-10-01.csv` | `0a5e172e0ce900dfea2f707868d18f56f2ba233f563097f6ae55b743968286f5` |
| `HUB_PickAndPlace_PCB1_2026_10_01.csv` | `8fba0be4824a6f93f375dfb7df53fc851d6ae4652eb11400b470e0fd83276b16` |

## Findings

### F-01 — Blocker: Q37 does not provide reverse-battery protection in its present orientation

`Q37` is an AO3407A P-channel MOSFET. Its footprint mapping is pin 1 gate,
pin 2 source, and pin 3 drain. The design connects:

```text
CN1 battery positive -> Q37 pin 2/source
Q37 pin 3/drain      -> BAT_BUS
Q37 gate             -> 100 kOhm pull-down, with 1 MOhm source-to-gate
```

That source/drain direction is appropriate for a normal high-side load switch,
but not for a single-PMOS reverse-battery circuit. With the battery reversed,
the MOSFET is off but its body diode can still complete a damaging current path
through the load. For the usual single-PMOS protection circuit, battery positive
must enter the **drain** and the protected load must be on the **source**.

**Action:** swap Q37 source and drain in the schematic and PCB (battery to pin 3,
`BAT_BUS` to pin 2), re-run ERC/DRC, and check the gate-source maximum rating over
all expected battery/fault voltages. Verify with a current-limited bench supply:
correct polarity must have a low drop; reverse polarity must have negligible
current. `Q2`, the intentional `RAW_BAT` to `VSYS` controlled high-side switch,
should retain its present source-at-supply orientation.

The checked item for input-protection MOSFET orientation in
`mothership/docs/MOTHERSHIP_PCB_PREORDER_CHECKS.md` is therefore stale and should
not be relied upon for this revision.

### F-02 — High: the reed input's series resistor is on the wrong side of the branch

There is an external pull-up/filter, but the exact connectivity is:

```text
reed connector signal ----+---- ESP32 GPIO36 / REED_SIG
                          |
                          +---- R8 1 kOhm ----+---- R9 10 kOhm ---- 3V3_SYS
                                             +---- C2 100 nF ---- GND
```

The connector and ESP32 pin are directly connected. `R8` therefore does not
limit cable ESD/fault current into GPIO36, and `C2` is not a conventional
MCU-side RC filter. The opening edge is filtered through `R8`; closing the switch
pulls the GPIO down directly.

GPIO36 is input-only and has no software-controlled internal pull-up, so the
external bias is necessary. It also has ESP32 interrupt/ADC caveats that must be
accounted for in firmware.

**Action:** rewire as:

```text
connector -> R8 -> REED_SIG/GPIO36
                    +-> R9 to 3V3_SYS
                    +-> C2 to GND
```

Add an appropriately selected low-leakage/low-capacitance TVS at the connector,
with a very short return to ground. Keep firmware debounce. Test open circuit,
closed circuit, wet/dirty cable leakage, contact bounce, ESD, and the maximum
intended cable length. Avoid relying on GPIO36 interrupts during operating modes
affected by the ESP32 GPIO36/39 ADC and Wi-Fi/Bluetooth sleep limitation.

### F-03 — High: outdoor connector interfaces have no defined ESD/surge or port-fault containment

The four muxed I2C sockets expose `3V3_SYS`, ground, SDA, and SCL. Each branch has
33 Ohm series resistors and 4.7 kOhm pull-ups, which helps ringing and establishes
logic levels, but the sockets do not have connector-side transient suppression or
individual supply current limiting.

The two ADC sockets expose the common 5 V boost rail and analog inputs. The analog
channels do have 1 kOhm series resistors, 1 MOhm pull-downs, and 1 nF capacitors,
but there are no connector-side clamps and no per-port protection for a shorted
5 V cable. The ADS1015 inputs must remain within their absolute input limits.

For an outdoor logger, cable ESD, induced transients, wiring mistakes, and water
ingress are foreseeable rather than exceptional.

**Action:**

- add connector-side TVS arrays selected for the bus voltage, leakage, and bus capacitance;
- add per-port current limiting/load switches or deliberately sized resettable fuses on exposed power pins;
- define the allowed sensor current and cable length;
- keep TVS return paths short and away from logic ground paths; and
- test short-to-ground, short-to-adjacent-pin, ESD, hot-plug, and long-cable rise time.

For multi-metre I2C runs, validate at 100 kHz with the actual harness and all
sensors fitted. If the rise-time/noise margin is poor, use an I2C cable extender
or a differential physical layer instead of simply lowering pull-up values.

### F-04 — High: the repository cable pinout is not a direct match for the PCB sockets

The PCB connector numbering recovered from the netlist is:

| PCB connector | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| `CN17`–`CN20` I2C | 3V3 | GND | SDAx | SCLx |
| `CN15` ADC | 5V | GND | A1 | A0 |
| `CN16` ADC | 5V | GND | A2 | A3 |

`hardware/SENSOR_WIRING_AVATION_PLUGS.md` instead numbers an I2C aviation plug
as pin 1 ground, pin 2 SDA, pin 3 SCL, pin 4 power. A same-number-to-same-number
harness would put PCB power onto the aviation connector's ground pin and is not
safe. A custom crossover harness may be intended, but it is not documented.

**Action:** add an explicit table for each cable showing PCB reference/pin, wire
colour, aviation connector pin, and signal. Include connector-view orientation
(mating face versus solder face). Continuity-test and label every harness before
connecting a sensor. Do the same for the reed/anemometer and both ADC sensor types.

### F-05 — High: the LTE RF route is not yet a controlled, connector-protected 50 Ohm path

The `LTE_RF_MOD` route from the A7670G to `JP1` is approximately 40 mm long. About
35 mm is on the bottom layer and the signal changes layers through two vias. The
nearest visible ground-return vias at the signal transitions are roughly 1.6 mm
and 2.1 mm away. `D4`, the RF ESD device, is beside the modem rather than beside
the exposed U.FL connector.

The source stack-up does not contain usable dielectric-constant/loss-tangent
values, so the 0.42 mm RF trace width is not evidence of a calculated 50 Ohm
impedance. A layer transition without tightly coupled return vias also creates a
return-path discontinuity at LTE frequencies.

**Action:** obtain the actual fabricator stack-up and calculate a controlled
50 Ohm geometry. Prefer a short outer-layer route with no signal vias. If a
transition is unavoidable, place one or more ground-return vias immediately
beside it. Add a ground-via fence/grounded coplanar structure appropriate to the
calculated geometry, preserve the reference plane, and keep copper/other signals
out of the RF keepout. Move the RF ESD part to the U.FL end with a near-zero-length
ground path. Consider a pi-match footprint and validate conducted/radiated
performance in the final enclosure.

### F-06 — High: battery temperature and cell protection are system assumptions, not board features

The CN3163 `TEMP` pin is tied to ground, which disables its battery-temperature
monitoring. That is electrically permitted by the charger, but it removes a
valuable charge-inhibit mechanism in an outdoor product. The board also does not
show a separate single-cell under-voltage/over-voltage/short-circuit protection
IC and protection FET pair; the fuses do not provide those functions.

**Action:** document and enforce a **protected 1S Li-ion/LiPo pack** requirement,
including the protection thresholds and current rating. Do not connect an
unprotected bare cell. For field use, provide battery-temperature-qualified
charging (pack NTC/appropriate charger), or justify and validate a system-level
method that prevents charging outside the cell manufacturer's allowed range.
Confirm that the solar panel's cold, open-circuit voltage and all transients stay
inside the CN3163 input ratings. The 3.6 kOhm `ISET` value implies about 330 mA;
verify that against both the panel and the chosen cell's charge specification.

### F-07 — Medium/high: verify the ESP32 PCB-antenna keepout and module lifecycle

The ESP32-WROOM-32D is placed at the upper edge over a 20 mm by about 6.7 mm board
notch. This is directionally good, but the current files do not demonstrate the
manufacturer-recommended clearance around the complete antenna region. Nearby
base-board copper, components, enclosure hardware, cable bundles, and the final
enclosure can all detune it.

**Action:** compare the placement against Espressif's current module placement
and antenna keepout drawing using the exact module origin. Prefer placing the
module antenna entirely beyond the base-board edge. Otherwise apply the full
specified all-layer keepout/cutout and ground-via treatment. Perform Wi-Fi range,
RSSI, throughput, and current tests in the final enclosure. ESP32-WROOM-32D is
marked NRND by Espressif; evaluate the footprint-compatible WROOM-32E for a new
production design and confirm certification/firmware implications.

### F-08 — Medium: the SD series resistors are at the receiver end

The SD SPI mapping and biasing are correct: CS=GPIO13, SCK=GPIO18, MISO=GPIO19,
MOSI=GPIO23; CS is pulled up and the socket has local 10 uF/100 nF decoupling.
However, the SCK, MOSI, and CS routes are approximately 41–47 mm with two vias,
and their 22 Ohm damping resistors are placed beside the SD socket. Source-series
termination is most effective close to the driving ESP32 pin.

**Action:** move the SCK/MOSI/CS resistors close to the ESP32 and keep the route on
one referenced layer where practical. If the socket is user-accessible, add SD
line ESD protection near it. Start bring-up at a conservative SPI clock and scope
SCK at both ends before increasing speed. The card-detect contact is unused, so
firmware cannot report physical insertion unless this is changed.

### F-09 — Medium: rework the 5 V boost hot loop before relying on ADC accuracy/EMI

The MT3608, inductor, Schottky diode, and output capacitors are spread across an
elongated region; the switch-current loop is larger than necessary and is close
to the ADC section. This raises ringing, emissions, ground-noise, and analog-noise
risk even if the circuit regulates correctly.

**Action:** place the IC, inductor, diode, and ceramic input/output capacitors as
a tight reference-layout cluster. Minimise the SW copper area, keep feedback away
from SW/inductor nodes, and return the feedback divider to a quiet output/ground
point. Measure 5 V ripple and all four ADC channels with inputs shorted and with
worst-case sensor load. In firmware, select an ADS1015 PGA range that covers the
intended input; the power rail being 5 V does not make the default +/-2.048 V PGA
range measure 5 V without saturation.

### F-10 — Medium: mounting holes are floating plated holes

The four 3.4 mm mounting holes are exported as plated through-holes with copper
annuli but no intentional electrical net. Metal screws/standoffs can capacitively
or intermittently couple these floating rings and can introduce ESD into nearby
copper.

**Action:** if the holes are purely mechanical, make them NPTH and add the required
all-layer copper/component keepout for the chosen hardware. If they are intended
for chassis/shield grounding, connect them deliberately using the selected
chassis/ESD strategy rather than leaving them floating.

### F-11 — Medium: BOM/placement data need an assembly-specific cleanup

The exports match each other, but they are not yet a clean contract-manufacturing
package:

- 17 test points are listed as placement parts;
- solder jumpers and items without supplier parts need an explicit fit/DNP status;
- the capacitor reference `1uf1` should be renamed to a normal `Cxxx` reference;
- the reed connector reference `edaimagecopyoccupy` should be renamed to `CNxxx`;
- test points should use stable `TPxxx` references rather than signal names;
- 117 rows are Extended parts, increasing assembly cost;
- `U54` USB-C, `U47` terminal block, `U1`, and `U2` are marked non-SMD and may be
  omitted from an SMT-only order; and
- all components are on the top, so the single top paste file is expected.

**Action:** generate assembly-only BOM/CPL files with explicit `FIT`, `DNP`, and
manual/THT instructions. Inspect the assembler's component mapping and 3D/rotation
preview, especially USB-C, SIM, microSD, battery holder, diodes/LEDs, electrolytic
capacitors, modem, ESP32, and fine-pitch ICs. Confirm how the USB-C through-hole
shield anchors and other non-SMD parts will be soldered.

### F-12 — Medium: a clean DRC does not currently cover component-body spacing

The EasyEDA design-rule data have component/device and through-hole-to-SMD
clearance fields set to zero. Trace/space DRC can therefore pass while courtyard,
body, nozzle, rework, and enclosure conflicts remain unchecked.

**Action:** set realistic courtyard/component clearances, run EasyEDA ERC/DRC
again, and run the chosen fabricator/assembler's DFM analysis. Review all board
edges and the ESP32 notch for component-to-route, component-to-edge, tool access,
and enclosure clearance.

### F-13 — Low/medium: USB routing and documentation can be cleaned up

USB D+ is an all-top route of roughly 13 mm; D- is roughly 17 mm and changes layer
twice. USB Full Speed may work at these lengths, and the ESD device is correctly
near the connector, but the pair is not routed as a consistent differential pair.
On the next layout pass, keep D+/D- together on one reference plane, with similar
length and minimal stubs/vias.

Some schematic title blocks still say `MotherShip_v1`, and page/total counts are
not consistent across the seven sheets. Correct the board name, revision, date,
sheet titles, and page totals so screenshots and PDFs cannot be confused with an
older release.

## Items that look correct

- The board outline is closed and measures approximately 100 mm by 90 mm, with
  four copper layers and a continuous inner ground layer.
- The BOM, placement file, project, and Gerber generation dates are coherent.
- The TCA9546A address straps resolve to `0x71`; each downstream branch has its
  own pull-ups and 33 Ohm series resistors.
- The ADS1015 address is `0x48`; the PCA9306 level-shifter topology and 3.3 V/5 V
  pull-up domains are correct.
- The root I2C devices do not have an obvious address conflict: DS3231=`0x68`,
  TCA9546A=`0x71`, ADS1015=`0x48` behind the level shifter.
- ESP32 flash GPIO6–GPIO11 are not used. GPIO34/35/36 are used as inputs.
- ESP32 local bulk/ceramic decoupling is present.
- The microSD SPI pin mapping, CS pull-up, and local decoupling are correct.
- The TPS63020 feedback network targets about 3.9 V and has local bulk capacitance
  at the modem. UART level-shifter directions are consistent with ESP32 3.3 V and
  modem 1.8 V domains.
- USB-C CC1 and CC2 have independent 5.1 kOhm pull-downs; USB ESD protection is
  close to the connector.
- No bottom-side parts are present, matching the paste and placement outputs.

These positives do not close the findings above; they identify areas that do not
currently need schematic rework.

## Recommended release sequence

1. Correct Q37 and prove reverse-polarity behaviour on the bench.
2. Correct the reed network and add connector-side protection/current limiting.
3. Define and document every PCB-to-aviation cable mapping.
4. Rework/verify the LTE RF route and ESP32 antenna keepout using the actual fab stack-up.
5. Decide and document the protected-pack and temperature-qualified charging strategy.
6. Move the SD source resistors and tighten the MT3608 hot loop during the same PCB pass.
7. Convert/define mounting holes and set meaningful component-clearance rules.
8. Re-run ERC, DRC, Gerber inspection, netlist comparison, and manufacturer DFM.
9. Produce cleaned assembly BOM/CPL/DNP/THT instructions and inspect the assembly preview.
10. Build a very small prototype quantity and use current-limited, staged bring-up.

Minimum prototype acceptance tests should cover: rail resistance before power,
reverse battery, quiescent current, power latch/wake/shutdown, solar charge across
temperature limits, 3.3 V/5 V/3.9 V rail ripple, modem burst droop, LTE conducted
performance, Wi-Fi range in the enclosure, SD writes at temperature, all I2C
ports with production cables, ADC gain/calibration/noise, reed bounce/counting,
port shorts, hot-plug, and ESD.

## Primary references

- [AO3407A datasheet](https://www.aosmd.com/sites/default/files/res/datasheets/AO3407A.pdf)
- [TI reverse-polarity protection application report](https://www.ti.com/lit/an/snoaa23a/snoaa23a.pdf)
- [Espressif ESP32 hardware design guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html)
- [Espressif ESP32 GPIO restrictions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html)
- [ESP32-WROOM-32D/32U datasheet and lifecycle notice](https://documentation.espressif.com/esp32-wroom-32d_esp32-wroom-32u_datasheet_en.html)
- [TI ADS1015 datasheet](https://www.ti.com/lit/ds/symlink/ads1015.pdf)
- [TI TPS63020 datasheet](https://www.ti.com/lit/ds/symlink/tps63020.pdf)
- [SIMCom A7670 hardware-document page](https://en.simcom.com/product/A7670X.html)
- [CN3163 manufacturer page](https://www.consonance-elec.com/68.html)
