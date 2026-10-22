# Node V4 solar charger wiring guide

## Decision

Use a CN3791 buck solar charger with a TMP390A2 board-mounted hot/cold
temperature switch. Remove the TP5100 USB battery charger and the previous
CN3163 solar-charger circuit.

This circuit is intended for:

- one protected 1S Li-ion/LiPo pack with a 4.2 V charge voltage;
- approximately 2 Ah battery capacity;
- a nominal 6 V / 3 W solar panel;
- 600 mA nominal charge current; and
- top-side PCB assembly only, with the battery attached below the PCB.

The battery manufacturer's maximum charge current must be at least 660 mA.
If that cannot be confirmed, use the 500 mA sense-resistor alternative given
below.

The panel's actual `Vmp`, `Voc`, `Imp`, and `Isc` must be checked before the
MPPT divider is finalized. For the component ratings in this guide, the cold
open-circuit panel voltage must remain below 12 V.

This is a cost-conscious charger, not a full power-path controller. The
CN3791 datasheet does not specify a charge safety timer, and Node load current
passes through its charge-current sense path while the charger is active, so
an awake Node can delay charge termination. The external TMP390 shutdown
provides the missing hot/cold interlock, but it is a board-to-pouch thermal
proxy rather than an in-pack sensor. Those limitations must be validated on
the complete mechanical assembly.

## Proposed reference designators

These references start above the highest references in the 2026-10-12 Node V4
schematic export. They may be changed by a later full-project annotation, but
the connections must remain the same.

| Reference | Function | Suggested part/value |
|---|---|---|
| CN21 | Two-pin solar input | Pin 1 `PANEL+`, pin 2 GND |
| F9 | Panel-input resettable fuse | 1.1 A / 24 V PPTC, `1812L110/24DR`, LCSC C207066 |
| F10 | Repositioned existing battery fuse (currently U7) | 1.1 A / 8 V PPTC, `1812L110PR`, LCSC C163516 |
| D20 | Panel-input bidirectional TVS | `SMF12CA`, LCSC C353317 |
| Q40 | Panel reverse-polarity protection | `AO3407A`, LCSC C15155 |
| R181 | Q40 gate pull-down | 22 kOhm |
| D23 | Q40 gate protection | 8.2 V Zener, `BZT52C8V2`, LCSC C7305282 |
| C130 | Charger input bulk capacitor | 47 uF, 25 V, low-ESR SMD aluminium electrolytic, at least 0.3 A ripple rating |
| C131 | Charger input ceramic | 4.7 uF, 25 V, X5R/X7R |
| C139 | Charger input high-frequency bypass | 100 nF, 25 V, X7R |
| U46 | Solar charger | `CN3791`, LCSC C154992 |
| C132 | U46 VG bootstrap capacitor | 100 nF, 25 V, X7R |
| Q41 | CN3791 switching PMOS | `AO3407A`, LCSC C15155 |
| D21 | Series reverse-current blocking diode | `PMEG4010CEH,115`, LCSC C193342 |
| D22 | Buck catch diode | `PMEG4010CEH,115`, LCSC C193342 |
| L7 | Buck inductor | 33 uH, shielded, at least 1.0 A saturation and 0.8 A RMS; `XRNR5040-33uH/M`, LCSC C5339711 |
| R182 | Charge-current sense | 0.200 Ohm, 1%, 1206, `RL1206FR-070R2L`, LCSC C309630 |
| C133 | Battery-side bulk capacitor | 10 uF, 10 V, low-ESR SMD aluminium electrolytic |
| C134 | Battery-side ceramic capacitor | 4.7 uF, 10 V, X5R/X7R |
| R183 | Loop compensation resistor | 120 Ohm |
| C135 | Loop compensation capacitor | 220 nF, 16 V or greater, X7R |
| R184 | MPPT upper resistor | 402 kOhm, 1%, provisional for a 6.0 V Vmp panel |
| R185 | MPPT lower resistor | 100 kOhm, 1% |
| U47 | Hot/cold temperature switch | `TMP390A2DRLR`, LCSC C5219772 |
| C136 | U47 supply bypass | 100 nF, X7R |
| U48 | Solar-powered 3.3 V temperature-safety supply | `HT7533-1`, SOT-89-3, LCSC C14289 |
| C137 | U48 input capacitor | 10 uF, 25 V, X5R/X7R |
| C138 | U48 output capacitor | 10 uF, 10 V, X5R/X7R |
| R186 | Hot threshold setting | 2.49 kOhm, 1%, 100 ppm/degC or better |
| R187 | Cold threshold and hysteresis setting | 18.7 kOhm, 1%, 100 ppm/degC or better |
| R188 | Temperature-output pull-up | 10 kOhm |
| Q42 | Normal-temperature clamp-release switch | `MMBT3904`, LCSC C20526 |
| R189 | Default-off Q43 base feed/current limiter | 100 kOhm |
| R190 | Q42 base-current limiter | 47 kOhm |
| R191 | Q42 base pull-down | 100 kOhm |
| Q43 | MPPT shutdown clamp | `MMBT3904`, LCSC C20526 |

Do not substitute the TMP390A3 for the A2 without accepting its lower trip
accuracy. Check the selected vendor's MOSFET pin mapping rather than relying
only on a generic symbol.

## Net names

Use these distinct net names to make the schematic and PCB review unambiguous:

```text
PANEL_RAW
PANEL_FUSED
SOLAR_IN
BAT_FUSED
SW_RAW
SW
CHARGE_SENSE
MPPT
BAT_BUS
TEMP_3V3
TEMP_FAULT_N
TEMP_ENABLE_DRIVE
TEMP_CLAMP_DRIVE
TMP_SETA
TMP_SETB
GND
```

## Step 1: remove the former charging circuits

Delete the entire former charger page, including:

- CN3163 and its current-setting, status and input/output components;
- TP5100 and its inductor, catch diode and current-sense circuit;
- the USB-to-battery charging path; and
- any obsolete solar/USB charger isolation jumpers.

USB may remain for programming and communications, but USB VBUS must not be
connected to `BAT_BUS` through a charger. Keep the battery connector, battery
reverse-polarity protection, battery fuse, `BAT_BUS`, and the existing system
power circuitry.

### Existing battery interface: retain, but correct the fuse position

The current Node V4 export uses U3, a Molex 532610271 1.25 mm PicoBlade
connector, for the battery. Retain it with this electrical assignment:

```text
U3 pin 1 = battery positive
U3 pin 2 = GND
```

Pins 3 and 4 in the EasyEDA symbol are the connector's mechanical hold-down
pads, not battery contacts. Follow the Molex footprint specification for them.
Do not assume that every third-party 1.25 mm battery lead has the same polarity;
measure the actual pack before connecting it.

The existing schematic places its 1.1 A PPTC between `BAT_BUS` and `RAW_BAT`.
That does not protect a short on the new charger branch, because the charger
also joins `BAT_BUS`. Move that existing fuse to the connector side of Q37 and
re-annotate it as F10:

```text
U3 pin 1 -> F10 1.1 A PPTC -> BAT_FUSED -> Q37 drain
Q37 source -> BAT_BUS -> new charger and all existing battery-fed circuitry
U3 pin 2 -> GND
```

Keep the existing Q37 gate network. Replace the old `RAW_BAT` labels on the
system side with `BAT_BUS`; do not leave a second, unfused path around F10.
The charger output at R182 must connect to `BAT_BUS`, never directly to U3.

Q37 is not guaranteed to block a reversed battery when the solar charger is
already energising `BAT_BUS`. Treat the keyed connector and verified lead
polarity as part of the safety design, and always disconnect the panel before
handling the battery connector.

## Step 2: wire the panel input and protection

1. Connect CN21 pin 1 to `PANEL_RAW`.
2. Connect CN21 pin 2 to GND.
3. Connect F9 from `PANEL_RAW` to `PANEL_FUSED`.
4. Connect D20 from `PANEL_FUSED` to GND. D20 is bidirectional, so it has no
   functional cathode orientation.
5. Connect Q40 drain to `PANEL_FUSED`.
6. Connect Q40 source to `SOLAR_IN`.
7. Connect Q40 gate through R181, 22 kOhm, to GND.
8. Connect D23 cathode to Q40 source/`SOLAR_IN` and anode to Q40 gate.
9. Connect C130, C131 and C139 in parallel from `SOLAR_IN` to GND.

C130 is polarised: connect its positive terminal to `SOLAR_IN` and its
negative terminal to GND. C131 and C139 are ceramic and non-polarised.

For the normal AO3407A SOT-23 mapping, pin 1 is gate, pin 2 is source, and pin
3 is drain. With Q40 oriented as specified, its body diode starts the circuit
for a correctly connected panel and blocks a reversed panel. D23 prevents the
Q40 gate-to-source voltage approaching its absolute maximum if panel voltage
rises in cold, bright conditions.

Place CN21 and F9 together at the board edge, with D20 immediately after F9
and Q40 immediately after D20. The bidirectional TVS remains off for a reversed
6 V panel connection, so it can sit ahead of Q40 and clamp a cable transient
without sending the clamp current through the small MOSFET. D20 must have a
short, wide return to ground.

C130/C131/C139 implement the manufacturer's recommended input combination of
bulk, ceramic and high-frequency capacitance. The 47 uF starting value assumes
a cabled 6 V / 3 W panel and must be confirmed during bring-up.

F9 must be the `/24` part or another part rated for at least 24 V. The
non-suffixed `1812L110PR` used on the battery input is only an 8 V part and is
not suitable at the panel connector.

## Step 3: wire the CN3791 control pins

Wire U46 as follows:

| U46 pin | Name | Connection |
|---:|---|---|
| 1 | VG | One side of C132 |
| 2 | GND | Charger quiet ground |
| 3 | CHRG | GND when no charge-status LED is fitted |
| 4 | DONE | GND when no full-status LED is fitted |
| 5 | COM | R183 in series with C135 to GND |
| 6 | MPPT | `MPPT` net |
| 7 | BAT | Kelvin connection to the `BAT_BUS` side of R182 |
| 8 | CSP | Kelvin connection to the `CHARGE_SENSE` side of R182 |
| 9 | VCC | `SOLAR_IN` |
| 10 | DRV | Q41 gate |

Connect the other side of C132 directly to U46 pin 9/VCC. C132 is connected
between VG and VCC, not between VG and ground.

For the compensation network, connect:

```text
U46 pin 5/COM -> R183 120 Ohm -> C135 220 nF -> GND
```

Put R183 and C135 directly beside pins 5 and 2. Return C135 to the quiet U46
ground rather than through the switching-current ground path.

## Step 4: wire the buck power stage

1. Connect Q41 source to `SOLAR_IN`.
2. Connect Q41 gate to U46 pin 10/DRV.
3. Connect Q41 drain to `SW_RAW`.
4. Connect D21 anode to `SW_RAW` and cathode to `SW`.
5. Connect D22 anode to GND and cathode to `SW`.
6. Connect L7, 33 uH, from `SW` to `CHARGE_SENSE`.
7. Connect R182 from `CHARGE_SENSE` to `BAT_BUS`.
8. Connect C133 and C134 in parallel from `BAT_BUS` to GND.

C133 is polarised: connect its positive terminal to `BAT_BUS` and its negative
terminal to GND. C134 is ceramic and non-polarised. For the selected
PMEG4010CEH diode, pin 1 is cathode and pin 2 is anode; verify that mapping in
both EasyEDA symbols before routing.

D21 is the manufacturer's optional reverse-current blocking diode. Retaining
it avoids the additional night-time battery current that otherwise flows back
through the inductor, Q41 body diode and U46 VCC pin.

D21 and D22 use the lower-leakage PMEG4010CEH instead of a generic SS14-class
part because catch-diode leakage flows from the battery in darkness. Both
diodes still require thermal verification at maximum charge current.

The output capacitor pair follows the manufacturer's preferred 10 uF
electrolytic plus 1 uF to 10 uF ceramic arrangement. Do not replace both with
a small high-capacitance ceramic without checking its effective capacitance at
4.2 V and verifying the no-battery overshoot.

The 33 uH inductor keeps the calculated ripple near 25% of 600 mA at a 6 V
panel operating point and a 2.8 V battery, before component tolerance. Use a
shielded type; do not select it by inductance alone. Its saturation current,
RMS-current rating and DCR must all be checked against the final part.

The charge current is set by:

```text
ICHARGE = 120 mV / R182
```

With R182 = 0.200 Ohm:

```text
ICHARGE = 600 mA nominal
```

Allowing for U46's 110 mV to 130 mV current-sense range and a 1% resistor,
the approximate possible range is 545 mA to 657 mA. The protected battery
must therefore permit at least 660 mA continuous charging.

If the battery's permitted charge current cannot be confirmed, fit 0.240 Ohm
instead:

```text
ICHARGE = 500 mA nominal
approximate maximum including tolerance = 547 mA
```

Use a 1206 sense resistor and route U46 pins 7 and 8 directly to the two R182
pads as a close Kelvin pair. Do not take either sense connection from a wider
power trace before it reaches the resistor pad.

## Step 5: wire the MPPT divider

Connect:

```text
SOLAR_IN -> R184 -> MPPT -> R185 -> GND
```

The CN3791 relationship is:

```text
Vmp = 1.205 V * (1 + R184 / R185)
```

For a panel whose specified maximum-power voltage is 6.0 V:

```text
R184 = 402 kOhm
R185 = 100 kOhm
programmed Vmp = 6.049 V
```

Do not select R184 from the panel's open-circuit voltage. If the selected
panel specifies another Vmp, recalculate R184 before ordering the PCB.

## Step 6: wire the solar-powered temperature monitor

Power the temperature-safety circuit from `SOLAR_IN`, not from the battery.
This makes the temperature decision available whenever charging energy is
present, including when a deeply discharged pack has opened its protection
switch. It also prevents the monitor and a persistent fault indication from
draining the battery overnight.

Wire U48 as follows:

| U48 pin | Name | Connection |
|---:|---|---|
| 1 | GND | GND |
| 2 | VIN | `SOLAR_IN` |
| 3 | VOUT | `TEMP_3V3` |

These pin numbers are for the Holtek HT7533-1 in SOT-89-3; confirm the EasyEDA
symbol-to-footprint pin mapping before routing. Connect C137 from U48 VIN to
GND and C138 from U48 VOUT to GND. Place both capacitors directly beside U48.
The 10 uF values follow the manufacturer's basic application circuit.

Wire U47 as follows:

| U47 pin | Name | Connection |
|---:|---|---|
| 1 | SETA | R186, 2.49 kOhm, to GND |
| 2 | SETB | R187, 18.7 kOhm, to GND |
| 3 | GND | GND |
| 4 | OUTB | `TEMP_FAULT_N` |
| 5 | VDD | `TEMP_3V3` |
| 6 | OUTA | `TEMP_FAULT_N` |

Connect C136 directly between U47 pins 5 and 3. Connect R188, 10 kOhm, from
`TEMP_3V3` to `TEMP_FAULT_N`.

Name the pin-1/R186 node `TMP_SETA` and the pin-2/R187 node `TMP_SETB`; these
names are only for small manufacturing test pads and do not connect elsewhere.

The two open-drain outputs are deliberately joined. They are high impedance
inside the allowed temperature window and pull `TEMP_FAULT_N` low if either
the hot or cold threshold is crossed.

The selected conservative thresholds are:

| Condition | Trip | Reset |
|---|---:|---:|
| Too cold | +5 degC | +10 degC |
| Too hot | +42 degC | +37 degC |

These thresholds add margin around a typical 0 degC to 45 degC Li-ion charging
range because U47 senses the pouch through the PCB rather than from inside the
battery pack. If the battery manufacturer supplies a narrower charging range,
change the thresholds to match it.

## Step 7: wire hardware charger shutdown

The CN3791 datasheet states that the charger operates only when its MPPT pin
is above approximately 1.23 V during startup. Q43 therefore provides a
hardware shutdown by clamping `MPPT` to ground. The interface is deliberately
default-off when the temperature-safety supply is absent: an unpowered U47 or
U48 cannot accidentally release the clamp.

Wire the interface as follows:

1. Connect R189, 100 kOhm, from `SOLAR_IN` to `TEMP_CLAMP_DRIVE`.
2. Connect Q43 base to `TEMP_CLAMP_DRIVE`.
3. Connect Q43 emitter to GND.
4. Connect Q43 collector to `MPPT`.
5. Connect R190, 47 kOhm, from `TEMP_FAULT_N` to `TEMP_ENABLE_DRIVE`.
6. Connect R191, 100 kOhm, from `TEMP_ENABLE_DRIVE` to GND.
7. Connect Q42 base to `TEMP_ENABLE_DRIVE`.
8. Connect Q42 emitter to GND.
9. Connect Q42 collector to `TEMP_CLAMP_DRIVE`.

For both selected MMBT3904 devices, verify that the EasyEDA symbol and
footprint use pin 1/base, pin 2/emitter and pin 3/collector before routing.
R189 both limits Q43 base current and turns Q43 on by default whenever the
panel is present. A normal-temperature high level on `TEMP_FAULT_N` turns Q42
on; Q42 then pulls Q43's base low and permits charging.

The resulting logic is:

| Monitor state | `TEMP_FAULT_N` | Q42 | Q43 | Charger |
|---|---|---|---|---|
| +5 degC to +42 degC | High | On | Off | Enabled |
| Below +5 degC | Low | Off | On | Disabled |
| Above +42 degC | Low | Off | On | Disabled |
| U47/U48 unpowered or R188 open | Low/floating | Off | On | Disabled |

When the panel is dark, `TEMP_3V3` is off and this circuit draws no current
from the battery. When useful panel voltage appears, R189 initially holds the
charger off while U48 powers U47 from `SOLAR_IN`. U47 has a 10 ms reset
interval and updates its output every 0.5 seconds, so allow at least one
complete update interval in shutdown tests. The interlock also works when a
deeply discharged protected pack initially presents nearly 0 V on `BAT_BUS`.
The TMP390 datasheet does not fully specify the joined-output state throughout
an arbitrarily slow supply ramp, so cold and hot sunrise-ramp tests remain
mandatory.

This shutdown is hardware-only and must not depend on the ESP32 being awake.

## Functional connection overview

```text
PANEL+
  |
 CN21
  |
 F9
  |
 PANEL_FUSED ---- D20 bidirectional TVS ---- GND
  |
 Q40 reverse-polarity PMOS
  |
SOLAR_IN -----+---- R184 ----+---- R185 ---- GND
  |           |              |
  |           |             MPPT <--- Q43 collector
  |           |                       Q43 emitter -> GND
  |           |
  |           +---- U48 HT7533-1 ---- TEMP_3V3
  |                                      |
  |                                   U47 TMP390
  |                                      |
  |                                 TEMP_FAULT_N
  |
 Q41 switching PMOS <- U46 DRV
  |
 D21 blocking Schottky
  |
 SW ---- D22 catch Schottky ---- GND
  |
 L7 33 uH
  |
 CHARGE_SENSE ---- R182 0.200 Ohm ---- BAT_BUS ---- existing system loads
       |                                      |
       +---- U46 CSP                     U46 BAT

 protected 1S battery + ---- U3 pin 1 ---- F10 ---- BAT_FUSED ---- Q37 ---- BAT_BUS
 protected 1S battery - ---- U3 pin 2 ----------------------- GND

 SOLAR_IN -> R189 -> TEMP_CLAMP_DRIVE -> Q43 base
 TEMP_FAULT_N -> R190 -> TEMP_ENABLE_DRIVE -> Q42 base
 Q42 collector -> TEMP_CLAMP_DRIVE
 Q42 emitter and Q43 emitter -> GND
 Q43 collector -> MPPT
```

## PCB placement requirements

### Switching charger

1. Place C130/C131/C139, Q41, D21 and D22 as one compact switching group.
2. Keep `SW_RAW` and `SW` copper short and small.
3. Keep the switching nodes away from the ESP32 antenna, ADC inputs, RTC,
   sensor connectors and I2C traces.
4. Place L7 and R182 immediately after the switching group.
5. Route U46 CSP and BAT as a short, close Kelvin pair to the R182 pads.
6. Join the C130/C131/C139 ground, D22 anode and C133/C134 ground locally before
   connecting that power ground to the system ground.
7. Keep U46 pin 2 and the C135 ground return out of the switching-current path.
8. Add unpopulated series-RC snubber pads from `SW` to GND for prototype tuning.

### Battery-temperature sensing

1. Put U47 on the top side directly above the centre region of the battery
   pouch mounted below the PCB.
2. Put U48 near U47 but outside the thermally isolated sensor island.
3. Keep U47 away from U46, Q41, D21, D22, L7, power regulators and the ESP32.
4. Make a small thermally isolated copper island around U47 with a copper
   keepout separating it from the main ground and power planes.
5. Use several small thermal vias inside that island to a solder-mask-covered
   bottom copper island.
6. Couple the bottom copper island to the battery pouch with a thin,
   electrically insulating silicone thermal pad.
7. Tent the vias and leave no exposed or sharp copper against the pouch.
8. Do not put a bottom-side component under the battery.

The board-mounted TMP390 is a thermal proxy, not an in-pack sensor. Its
temperature error and the PCB-to-pouch temperature difference must be measured
on the assembled prototype before production.

## Required test points

Provide accessible pads for:

```text
PANEL_RAW
SOLAR_IN
BAT_FUSED
MPPT
SW
CHARGE_SENSE
BAT_BUS
TEMP_3V3
TEMP_FAULT_N
TEMP_ENABLE_DRIVE
TEMP_CLAMP_DRIVE
TMP_SETA
TMP_SETB
GND
```

Do not probe `SW` with a long oscilloscope ground lead. Use a short ground
spring.

## Bring-up and safety tests

1. With no battery fitted, check resistance from `BAT_BUS` to GND and confirm
   there is no assembly short.
2. Confirm U3 pin 1 reaches `BAT_BUS` only through F10 and Q37, and that no
   unfused path bypasses F10.
3. Use a current-limited bench supply in place of the panel. Begin at 5 V with
   a 50 mA current limit.
4. Confirm correct input polarity produces `SOLAR_IN` and reversed polarity
   does not.
5. Confirm U46 pin 1 has C132 to pin 9, not to ground.
6. Confirm U48 produces approximately 3.3 V on `TEMP_3V3` whenever useful
   panel voltage is present.
7. Ground `TEMP_ENABLE_DRIVE` to simulate loss of the temperature-monitor
   supply. Confirm Q43 pulls MPPT below 1.0 V and U46 does not switch.
8. Force `TEMP_FAULT_N` low and confirm the same default-off state.
9. Power the panel supply off. Verify the pack polarity, connect the battery,
   then power the panel supply. Do not connect a battery while U46 is running.
10. With a partly discharged protected pack and the 0.200 Ohm resistor fitted,
    raise the supply current limit gradually and verify charge current does
    not exceed 657 mA.
11. Verify normal operation places the MPPT pin near 1.205 V when the emulated
    panel reaches its current limit.
12. During active charging, force `TEMP_FAULT_N` low. Confirm Q43 pulls MPPT
    below 1.0 V and battery charging stops without ESP32 involvement.
13. Use the TMP390 manufacturing trip-test method to test OUTA and OUTB
   separately. Do not use the trip-test inputs as an operational control.
14. Temperature-test the complete assembly. Confirm cold shutdown, hot
   shutdown and both hysteretic restart temperatures while measuring the
   actual pouch temperature with an independent thermocouple.
15. At both the hot and cold fault temperatures, ramp the emulated panel very
    slowly from 0 V through its cold `Voc`. Confirm charging remains off during
    U48/U47 power-up and the complete TMP390 update interval. If any charge
    pulse occurs, add a qualified power-good/startup-delay stage ahead of Q42;
    do not waive this test for production.
16. Confirm 4.2 V regulation, current taper, termination and automatic
    recharge.
17. Confirm the safe service sequence: connect the battery before the panel,
    and disconnect the panel before unplugging or reconnecting the battery.
    Do not hot-plug the battery while U46 is operating.
18. Disconnect the panel in darkness and confirm U48/U47 draw no current from
    `BAT_BUS`. Measure the remaining battery current into the entire charger
    block and investigate if it is materially above the CN3791 sleep-current
    expectation.
19. Test with the Node asleep, awake and transmitting. System load is connected
    to `BAT_BUS`, so it changes the battery current and can delay charge
    termination.
20. Test at the panel's measured cold `Voc` and inspect Q40, Q41, D21, D22, L7
    and R182 for temperature rise.
21. Scope `SW` with a ground spring and populate/tune the optional snubber only
    if ringing requires it.

Do not treat the battery pack's protection PCB as a substitute for charge
temperature control. Do not release the board for unattended charging until
the temperature shutdown has been verified against the actual installed
battery and mechanical stack-up.

## Datasheet basis

- CN3791 manufacturer datasheet:
  <https://www.consonance-elec.com/static/upload/file/20231218/1702881078530301.pdf>
- TMP390 manufacturer datasheet:
  <https://www.ti.com/lit/ds/symlink/tmp390.pdf>
- HT7533-1 manufacturer datasheet:
  <https://www.holtek.com/webapi/116711/HT75xx-1v281.pdf>
- Molex 532610271 connector drawing:
  <https://www.molex.com/content/dam/molex/molex-dot-com/products/automated/en-us/salesdrawingpdf/532/53261/532610271_sd.pdf>
- Littelfuse 1812L PPTC datasheet:
  <https://www.littelfuse.com/~/media/electronics/datasheets/resettable_ptcs/littelfuse_ptc_1812l_datasheet.pdf.pdf>
- Nexperia PMEG4010CEH datasheet:
  <https://assets.nexperia.com/documents/data-sheet/PMEG4010CEH.pdf>

