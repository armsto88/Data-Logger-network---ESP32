# 6 V / 3 W solar-charger modification

## Decision and design limits

Replace the CN3163 linear charger stage with a CN3791 buck solar charger.
The CN3163 must not be used with an unregulated nominal-6 V panel because the
panel's open-circuit voltage can exceed the charger's 6 V operating limit.

This design is for:

- one 4.2 V, 1S Li-ion/LiPo protected battery pack;
- 4 Ah to 10 Ah capacity;
- a nominal-6 V panel rated no higher than 3 W;
- panel maximum-power voltage (`Vmp`) of approximately 6 V;
- panel cold open-circuit voltage (`Voc`) below 12 V; and
- a nominal 600 mA maximum charger output.

The final MPPT-divider value must be checked against the data printed on the
actual panel. `Vmp`, `Voc`, `Imp`, and `Isc` are needed; the words "6 V, 3 W"
alone are not sufficient to finalize the divider.

The CN3791 does not provide a load-sharing/power-path output or battery
temperature sensing. `BAT_BUS` remains both the battery and system-power node,
as in the present design. The protected pack's allowed charge current must be
at least 660 mA (allowing for charger and resistor tolerances). Pack protection
does not necessarily prevent charging below 0 C or at excessive temperature.

## Schematic connections

Use `CN3791`, LCSC **C154992**, SSOP-10.

### Panel input and protection

```text
panel connector +
    -> 1.1 A / 16 V resettable fuse
    -> SOLAR_FUSED
    -> reverse-polarity PMOS M2 drain

panel connector - -> GND

SMF12A TVS: cathode -> SOLAR_FUSED, anode -> GND

M2 source -> SOLAR_IN
M2 gate   -> 22 kOhm -> GND
```

For an AO3407A in its normal SOT-23 pinout, M2 is:

- pin 3/drain to `SOLAR_FUSED`;
- pin 2/source to `SOLAR_IN`; and
- pin 1/gate through 22 kOhm to GND.

This orientation is important: it allows the body diode to start the circuit
with correct polarity and blocks a reversed input. No series blocking diode is
then required.

Place 10 uF/16 V X5R and 100 nF/25 V ceramic capacitors from `SOLAR_IN` to GND,
immediately beside the charger switching stage.

### CN3791 pins

| Pin | Name | Connection |
| ---: | --- | --- |
| 1 | VG | 100 nF directly to pin 9/VCC; do not connect this capacitor to GND |
| 2 | GND | Charger quiet ground |
| 3 | CHRG | Charge LED cathode, or GND if unused |
| 4 | DONE | Full LED cathode, or GND if unused |
| 5 | COM | 120 Ohm in series with 220 nF to GND |
| 6 | MPPT | Junction of the MPPT divider |
| 7 | BAT | Kelvin trace to battery side of the current-sense resistor; this node is `BAT_BUS` |
| 8 | CSP | Kelvin trace to inductor side of the current-sense resistor |
| 9 | VCC | `SOLAR_IN` |
| 10 | DRV | Switching PMOS M1 gate |

If the status LEDs are retained, power their anodes through individual 4.7 kOhm
resistors from `SOLAR_IN`, not `BAT_BUS`. This prevents the DONE LED from
continuously draining the battery at night.

### Buck power stage

```text
SOLAR_IN -> M1 source
M1 drain -> SW
M1 gate  -> CN3791 pin 10/DRV

catch Schottky: anode -> GND, cathode -> SW

SW -> 22 uH inductor -> CHARGE_SENSE
CHARGE_SENSE -> 0.200 Ohm current-sense resistor -> BAT_BUS

CN3791 pin 8/CSP -> Kelvin connection to CHARGE_SENSE
CN3791 pin 7/BAT -> Kelvin connection to BAT_BUS

BAT_BUS -> 22 uF/10 V plus 100 nF -> GND
```

For an AO3407A in its normal SOT-23 pinout, M1 is:

- pin 2/source to `SOLAR_IN`;
- pin 3/drain to `SW`; and
- pin 1/gate to CN3791 pin 10/DRV.

The 0.200 Ohm resistor sets nominal charge current to:

```text
ICH = 120 mV / 0.200 Ohm = 600 mA
```

Use a 1%, 1206 current-sense resistor. Its nominal dissipation is only 72 mW,
but the larger footprint makes Kelvin routing practical. Do not join either
sense trace to a power trace before the resistor pad.

The existing 22 uH `SMMS0420-220M` part type is suitable at this current. Its
1.2 A RMS and 1.5 A saturation ratings provide adequate margin. This refers to
reusing the part type/BOM item; the solar charger needs its own physical
inductor.

### MPPT divider

Connect `RMPPT_TOP` from `SOLAR_IN` to pin 6/MPPT and `RMPPT_BOTTOM` from pin 6
to GND. The equation is:

```text
Vmp = 1.205 V * (1 + RMPPT_TOP / RMPPT_BOTTOM)
```

Use 100 kOhm for the bottom resistor. Select the top resistor after checking
the panel:

| Actual panel Vmp | Top resistor | Programmed Vmp |
| ---: | ---: | ---: |
| 5.5 V | 357 kOhm | 5.507 V |
| 6.0 V | 402 kOhm | 6.049 V |

Populate 402 kOhm only if the selected panel really specifies Vmp close to
6.0 V. `Voc` is not the value used for this calculation.

## Suggested LCSC parts

| Function | Suggested part | LCSC |
| --- | --- | --- |
| Charger | CN3791, SSOP-10 | C154992 |
| M1 and M2 | AO3407A, -30 V P-channel MOSFET | C15155 |
| Catch diode | SL14PL-TP, 1 A/40 V Schottky, SOD-123FL | C725469 |
| Inductor | SMMS0420-220M, 22 uH | C2894720 |
| Current sense | RL1206FR-070R2L, 0.200 Ohm 1% 1206 | C309630 |
| Input fuse | MINISMDC110F/16-2, 1.1 A/16 V, 1812 | C2649915 |
| Input TVS | SMF12A, 12 V standoff, SOD-123FL | C178259 |
| 10 uF input capacitor | CL21A106KOQNNNE, 10 uF/16 V X5R, 0805 | C1713 |
| 22 uF output capacitor | TCC0805X5R226M100FT, 22 uF/10 V X5R, 0805 | C380338 |
| Compensation capacitor | 220 nF/16 V X7R, 0402 | C16772 |

Stock and JLCPCB assembly class must be rechecked immediately before ordering.

## PCB placement rules

These are functional requirements, not cosmetic preferences:

1. Put the input ceramic capacitor, M1, and catch diode together. The loop
   `input capacitor -> M1 -> catch diode -> input-capacitor ground` must be as
   small as possible.
2. Keep the `SW` copper short and small. Route it directly from M1/D2 to the
   inductor and keep it away from the ADC inputs, antennas, RTC crystal, and
   I2C traces.
3. Put the current-sense resistor immediately after the inductor. Route pin 8
   and pin 7 as a close Kelvin pair directly to opposite resistor pads.
4. Put the 120 Ohm/220 nF compensation network beside pin 5 and return it
   directly to the IC ground.
5. Join catch-diode ground and output-capacitor ground to the input-capacitor
   ground before this power ground joins the quiet/system ground.
6. Place the fuse and TVS at the panel connector. The TVS ground path must be
   short and wide.
7. Reserve unpopulated 0402 footprints for a series RC snubber from `SW` to
   GND. Populate only if oscilloscope testing shows excessive ringing.

## Bring-up tests

Use a current-limited bench supply before connecting a panel or the final
battery pack.

1. Verify reverse input produces no charger current and no negative voltage on
   `SOLAR_IN`.
2. Sweep input from 4.5 V to the panel's measured cold `Voc`; verify no part
   exceeds its rating.
3. With a partly discharged protected 1S pack, verify approximately 600 mA
   maximum charge current and check M1, D2, L, and the sense resistor for heat.
4. Emulate the panel with a supply set to 6 V and a 500 mA current limit. Verify
   that input voltage settles close to the programmed MPPT voltage instead of
   repeatedly collapsing and restarting.
5. Verify 4.2 V regulation, current taper, termination, and automatic recharge.
6. Repeat while the complete Hub is awake, sleeping, and transmitting over
   LTE; this charger has no separate power path, so system load affects the
   current observed by the charger.
7. Check the `SW` node with a short-ground-spring oscilloscope probe. Fit and
   tune the optional snubber only if necessary.

