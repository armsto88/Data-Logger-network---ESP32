# Node V3 Overview

**Project:** NODE_v3  
**Board:** ESP32-based low-power environmental and ultrasonic wind sensor node  
**Status:** Ready for a small prototype fabrication run, subject to final ERC, DRC, BOM, footprint, polarity, and Gerber checks  
**Recommended first order:** 3–5 boards

---

## 1. Purpose and Scope

Node V3 is a low-power, battery-operated ESP32 sensor node designed primarily for ultrasonic wind measurement while also supporting:

- environmental sensing;
- analogue sensor inputs;
- auxiliary reed-switch wind sensing;
- RTC-controlled wake-up;
- hardware power latching;
- USB programming;
- solar charging;
- USB charging;
- modular I²C expansion;
- local deployment and configuration workflows.

The design has now reached the prototype-fabrication stage. The major schematic and PCB weaknesses identified in V2 have been addressed. The remaining risks are primarily:

- electrical bring-up;
- oscilloscope validation;
- acoustic timing;
- firmware sequencing;
- ultrasonic calibration;
- environmental validation.

The board should not yet be considered ready for a large production run.

---

# 2. Core Controller Architecture

## ESP32

The main controller is an ESP32-WROOM-32 operating from `3V3_SYS`.

The design includes:

- RTC-controlled wake-up;
- hardware power latch;
- USB programming through CH340C;
- manual BOOT and RESET controls;
- battery-voltage monitoring;
- ultrasonic timing capture;
- directional TX control;
- directional damping control;
- RX mux selection;
- 22 V boost control.

## Current GPIO Map

| GPIO | Function |
|---:|---|
| GPIO0 | `PROG_BOOT` |
| GPIO1 | `ESP_TX` |
| GPIO3 | `ESP_RX` |
| GPIO4 | `RX_EN_N` or mutually exclusive AUX wind input |
| GPIO5 | `TX_22V_EN_N` |
| GPIO13 | `DRV_W` |
| GPIO14 | `DRV_S` |
| GPIO16 | `MUX_A` |
| GPIO17 | `MUX_B` |
| GPIO18 | `SDA` |
| GPIO19 | `SCL` |
| GPIO21 | `REL_S` |
| GPIO22 | `REL_W` |
| GPIO23 | `PWR_HOLD` |
| GPIO25 | `TX_PWM` |
| GPIO26 | `DRV_N` |
| GPIO27 | `DRV_E` |
| GPIO32 | `REL_E` |
| GPIO33 | `REL_N` |
| GPIO34 | `TOF_EDGE` |
| GPIO35 | `VOLT_ESP` |

The `REL_*` outputs drive transducer-shunting MOSFETs and should be treated in firmware as **damping controls**, not simple relays or ground returns.

---

# 3. Battery Input and Power Latching

## Battery Connector

The board now uses a 1.25 mm two-pin battery connector compatible with the selected battery assembly.

Electrical assignment:

```text
Pin 1: BAT+
Pin 2: GND
```

The physical battery cable polarity must still be confirmed with a multimeter before assembly.

## Reverse-Polarity Protection

Q37 is a P-channel MOSFET used for reverse-polarity protection.

Final intended orientation:

```text
Battery connector BAT+
        │
        ▼
Q37 drain

Q37 source
        │
        ▼
BAT_BUS
```

Gate network:

```text
Q37 source / BAT_BUS ── 1 MΩ ── Q37 gate
Q37 gate             ── 100 kΩ ── GND
```

This arrangement:

- allows normal discharge;
- allows charging through `BAT_BUS`;
- blocks a reversed battery when the charger is not already energising the board side;
- does not provide full bidirectional reverse blocking under every powered-charger fault condition.

For a keyed internal battery connector, this is acceptable for V3.

## Battery Fuse

The battery input includes a resettable fuse after the reverse-polarity stage.

The selected battery and connector are expected to remain below approximately 1 A in normal use, with the highest transient expected during 22 V cold startup.

## Main Power Latch

The main system is switched by a high-side P-channel MOSFET latch.

Wake sources include:

- RTC alarm;
- manual wake switch;
- USB/configuration wake.

The ESP32 holds the node on through `PWR_HOLD`.

Required boot sequence:

```text
1. Wake source enables the main rail.
2. ESP32 boots.
3. ESP32 asserts PWR_HOLD.
4. Firmware confirms the latch is held.
5. Firmware clears the RTC alarm.
```

Clearing the RTC alarm before asserting `PWR_HOLD` could shut the node down during boot.

## VSYS Bulk Capacitance

A 470 µF bulk capacitor remains on `VSYS`.

Purpose:

- support short converter startup transients;
- reduce brief VSYS droop;
- reduce 3.3 V disturbance;
- buffer connector and cable resistance.

It does not reduce total energy consumption and will introduce a short plug-in or wake inrush event.

Retain it for the first prototype and measure:

- battery current;
- `RAW_BAT`;
- `VSYS`;
- Q2 voltage drop;
- 3.3 V stability.

---

# 4. Battery Monitoring

The battery divider was moved to `VSYS` so it is not continuously connected to the battery while the main system is off.

Final network:

```text
VSYS
 │
R157 220 kΩ
 │
 ├──────── VOLT
 │
 ├── R158 100 kΩ ── GND
 │
 └── C 100 nF ───── GND

VOLT
 │
R159 2.2 kΩ
 │
VOLT_ESP / GPIO35
```

At 4.2 V:

```text
VOLT ≈ 4.2 × 100 / (220 + 100)
     ≈ 1.31 V
```

Firmware should:

- allow the divider/filter to settle;
- take multiple samples;
- use averaging or median filtering;
- calibrate the divider and ADC;
- make low-battery decisions using under-load voltage.

Initial bring-up should determine:

- warning threshold;
- radio-disable threshold;
- hard-shutdown threshold.

---

# 5. 3.3 V Regulation

## AP2112K-3.3

The main 3.3 V rail is generated by an AP2112K-3.3 LDO.

Current arrangement:

```text
VSYS → AP2112K → 3V3_SYS
```

Decoupling:

```text
Input:
10 µF + 100 nF

Output:
10 µF + 100 nF
```

The AP2112K is retained for V3 because it is:

- simple;
- low noise;
- already routed;
- capable of the expected ESP32 load;
- lower risk than introducing a new buck-boost design late in the revision.

## Low-Battery Limitation

A linear regulator cannot maintain 3.3 V once the battery voltage approaches the regulator output plus dropout.

The practical low-voltage limit must therefore be measured.

Bring-up targets:

```text
Test VSYS at:
4.2 V
3.8 V
3.6 V
3.4 V
3.3 V
```

Measure `3V3_SYS` during:

- boot;
- radio transmission;
- ultrasonic activity;
- RTC wake;
- USB programming.

A reasonable initial target is to keep `3V3_SYS` above approximately 3.05 V during the worst transient.

The final firmware cutoff should be based on measured behaviour rather than the battery protection cutoff.

---

# 6. 5 V Boost Converter

The 5 V rail uses an MT3608 boost converter.

Final key features:

- corrected feedback divider for approximately 5.0 V;
- 22 µH inductor;
- SS14 diode;
- local input capacitance;
- local output capacitance;
- enabled whenever `VSYS` is on.

The 5 V rail is not currently firmware-switchable.

Therefore, power sequencing is:

```text
VSYS rises
→ 5V_SYS starts
→ firmware later enables 22V_SYS
```

The 5 V converter should be validated for:

- correct output voltage;
- startup behaviour;
- ripple;
- heating;
- operation at low battery.

---

# 7. 22 V Boost Converter

## Purpose

The 22 V rail powers the ultrasonic transmitter.

## Final Architecture

The 22 V converter uses an MT3608 with:

- upgraded 22 µH inductor;
- LCSC C41406986;
- approximately 2.5 A or greater saturation-current margin;
- 100 µF + 100 nF input capacitance;
- 100 nF + 22 µF + 22 µF output capacitance;
- 360 kΩ / 10 kΩ feedback divider;
- approximately 22.2 V output.

The output bank was reduced from the earlier larger configuration to limit cold-start energy while retaining burst support.

## Enable Control

The converter is controlled through U49.

Logic:

```text
TX_22V_EN_N HIGH
→ U49 output LOW
→ EN_22 LOW
→ boost OFF

TX_22V_EN_N LOW
→ U49 output HIGH
→ EN_22 HIGH
→ boost ON
```

U49 was retained because it gives the desired boot-safe default.

U49 has local:

```text
100 nF + 1 µF
```

decoupling.

## Layout

The switching node is intentionally compact:

```text
MT3608 SW pin
↔ inductor switching pad
↔ diode anode
```

No long SW branch or test point should be added.

The feedback network should remain:

- close to FB;
- away from SW;
- away from the inductor switching node.

## Cold-Start Expectations

The 22 V cold-start event is expected to be the largest short battery-current transient.

Normal ultrasonic operation is expected to remain comfortably below 1 A, while cold startup may briefly approach the upper hundreds of milliamps.

Staggered startup reduces the total system peak by avoiding overlap with other major loads.

---

# 8. Charging Architecture

## Solar Charger

The solar charger uses a CN3163.

Current configuration:

- single-cell LiPo charging;
- approximately 330 mA charge current;
- local input decoupling;
- local battery-side decoupling;
- `TEMP` grounded;
- status LED;
- output connected to `BAT_BUS`.

Important constraints:

- panel open-circuit voltage must remain inside the CN3163 input range;
- a nominal 9 V panel must not be connected directly;
- low-temperature LiPo charging protection is disabled when `TEMP` is grounded;
- an optional NTC provision would improve future outdoor safety;
- an optional input TVS footprint is useful for long solar cables.

If the unused `DONE#` output is not used, it should be handled according to the final datasheet-recommended configuration.

## USB Charger

The USB charger uses a TP5100.

Final corrected configuration includes:

- USB-C input;
- independent 5.1 kΩ pull-down resistors on CC1 and CC2;
- USB input decoupling;
- TP5100;
- external Schottky catch diode;
- 10 µH output inductor;
- VS-side 10 µF + 100 nF;
- BAT-side 22 µF + 100 nF;
- R180 = 0.20 Ω;
- approximately 500 mA charge current;
- 0.75 A hold PPTC fuse.

Selected current-sense resistor:

```text
FOJAN FRL0805JR200TS
LCSC C2934277
0.20 Ω
±5%
125 mW
0805
```

Expected resistor dissipation at 500 mA:

```text
P = I²R
  = 0.5² × 0.2
  = 0.05 W
```

The 125 mW rating provides acceptable margin.

Selected USB fuse:

```text
Bourns MF-MSMF075-2
LCSC C84140
0.75 A hold
1.5 A trip
1812
```

## Dual-Charger Interaction

Both chargers connect to `BAT_BUS`.

For prototype bring-up, isolation links are useful:

```text
Solar charger output → 0 Ω link / solder jumper → BAT_BUS
USB charger output   → 0 Ω link / solder jumper → BAT_BUS
```

This allows one charger to be isolated during fault finding.

---

# 9. RTC and Keep-Alive Power

## RTC

The node uses a DS3231M RTC.

Connections include:

- `KEEP_ALIVE` supply;
- CR1220 backup cell;
- `SDA`;
- `SCL`;
- `INT/SQW`;
- local decoupling.

The alarm output is inverted through Q38 to create the wake signal used by the power latch.

## Keep-Alive LDO

The always-on rail uses a TPL720F33-3TR.

Decoupling:

```text
Input:
10 µF + 100 nF

Output:
10 µF + 100 nF
```

The keep-alive rail powers the RTC while the main system is off.

---

# 10. USB Programming

The programming interface uses:

- USB-C;
- CH340C;
- automatic EN/BOOT transistor circuit;
- manual RESET and BOOT controls;
- UART series resistors;
- six-pin programming header;
- USB data ESD protection.

The automatic programming circuit uses DTR and RTS to control:

- `PROG_EN`;
- `PROG_BOOT`.

The CH340C implementation is retained because the current board already works reliably.

The `V3` arrangement is documented as:

```text
Existing proven CH340C implementation retained.
V3 is not used to power any external load.
```

Programming-header power behaviour should be documented clearly.

If the 3.3 V header pin is output-only, label it accordingly.

---

# 11. Environmental Sensor Power Selection

Two sensor outputs support selectable supply voltage.

Each channel can provide:

```text
approximately 5 V
or
approximately 22 V
```

The corrected high-side 22 V switch uses:

- P-channel MOSFET;
- 100 kΩ source-to-gate pull-up;
- 12 V gate-source zener;
- 4.7 kΩ gate-drive resistor;
- NPN control transistor;
- 10 kΩ NPN base resistor;
- 100 kΩ NPN base pulldown.

The gate network now correctly places:

```text
Zener cathode → P-MOS source / 22V_SYS
Zener anode   → P-MOS gate
```

The 5 V and switched 22 V sources are diode-ORed.

This is a selector, not a full power-off circuit:

```text
22 V switch OFF:
sensor receives approximately 5 V

22 V switch ON:
sensor receives approximately 22 V
```

Only sensors explicitly compatible with the selected voltage should be attached.

---

# 12. ADS1015 Analogue Inputs

The ADS1015 is powered from `5V_SYS`.

The analogue front end includes:

- 1 kΩ series resistor per channel;
- 1 nF input capacitor;
- 1 MΩ input pulldown;
- 100 nF + 1 µF supply decoupling.

Important limitation:

```text
The ADS1015 PGA range does not allow the physical input pins
to exceed the supply rails.
```

Connected sensor outputs must remain within approximately:

```text
0 V to 5 V
```

unless external dividers or clamps are added.

---

# 13. I²C Level Shifting

## PCA9306

The PCA9306 now follows the recommended NXP configuration.

Final network:

```text
VREF1 → 3V3_SYS

VREF2 ─┐
        ├── joined bias node
EN    ──┘

5V_SYS → 200 kΩ → joined VREF2/EN node
joined VREF2/EN node → 100 nF → GND
```

Pull-ups remain:

```text
3.3 V side:
SCL1 → 4.7 kΩ → 3V3_SYS
SDA1 → 4.7 kΩ → 3V3_SYS

5 V side:
SCL2 → 4.7 kΩ → 5V_SYS
SDA2 → 4.7 kΩ → 5V_SYS
```

Benefits:

- datasheet-aligned biasing;
- correct 3.3 V ↔ 5 V translation;
- reduced back-powering when 5 V is absent;
- controlled startup of the translator bias.

---

# 14. I²C Expansion Multiplexer

## TCA9546A

The node includes a four-channel I²C multiplexer.

Key features:

- powered from `3V3_SYS`;
- 100 nF + 1 µF decoupling;
- RESET# pull-up;
- reset capacitor;
- configurable A0/A1 straps;
- A2 tied LOW;
- four downstream I²C connectors.

Manufacturing rule:

```text
Only one strap option per address pin may be populated.
Never fit both HIGH and LOW links for the same address pin.
```

Firmware should normally enable only one downstream mux channel at a time.

## Expansion Ports

Each connector provides:

```text
Pin 1: 3V3_SYS
Pin 2: GND
Pin 3: SDA
Pin 4: SCL
```

The current series resistors can remain for this prototype.

For future revisions, 47–100 Ω may provide better low-level margin than 330 Ω if longer cables or faster I²C are required.

---

# 15. AUX Wind Input

The AUX wind input uses a two-pin JST connector:

```text
Pin 1: GND
Pin 2: REED_SIG
```

The input shares GPIO4 with `RX_EN_N`.

The functions are mutually exclusive.

## Ultrasonic Mode

```text
AUX sensor disconnected
GPIO4 configured as RX_EN_N output
```

## AUX Wind Mode

```text
Ultrasonic RX not used
GPIO4 configured as input with pull-up
reed switch closes GPIO4 to GND
```

A 1 kΩ series resistor at the connector is recommended as protection against:

- wrong firmware mode;
- accidental connection;
- GPIO contention;
- external cable transients.

---

# 16. Ultrasonic System Overview

The ultrasonic system uses separate TX and RX transducers.

Example North nets:

```text
TX:
TD_N_A
TD_N_B

RX:
RX_N_A
RX_N_B
```

The TX and RX nets are distinct.

Therefore, the RX mux is not directly connected to the 22 V TX nodes, and the previously considered eight additional pre-mux 2.2 kΩ resistors are not required.

---

# 17. Ultrasonic Transmitter

## Carrier Path

```text
ESP32 GPIO25 / TX_PWM
→ TC4427
→ PWM_5V
→ Q21
→ Q16
→ TX_PULSE
```

The TC4427 is powered from `5V_SYS`.

One channel is used to drive the TX carrier chain.

The unused channel input is held LOW.

## Global 40 kHz High-Side Switch

Q16 is the global high-side TX pulse switch.

Important V3 changes:

```text
R112: 100 kΩ → 4.7 kΩ
R5:   new 1 kΩ between HS_GATE and Q21 drain
R6:   new 1 kΩ from TX_PULSE to GND
R182: existing 220 kΩ bleed retained
D10:  15 V gate-source clamp retained
```

The earlier 100 kΩ gate return was too slow for carrier-frequency switching.

The 1 kΩ `TX_PULSE` pulldown creates a real LOW state between carrier pulses.

Expected initial waveform:

```text
TX_PULSE HIGH: preferably >20 V
TX_PULSE LOW:  preferably <1–2 V
before the next rising edge
```

## Directional TX Channels

The North, East, South and West channels each include:

- P-channel MOSFET connecting `TX_PULSE` to the selected TX transducer;
- 2N7002 gate driver;
- 1 kΩ source-to-gate pull-up;
- 1 kΩ gate-drive resistor;
- 15 V gate-source zener;
- 100 Ω GPIO gate resistor;
- 100 kΩ GPIO pulldown;
- CJ2310 damping MOSFET across the transducer;
- 1 kΩ grounding/loading resistor on the second terminal.

Control truth table:

| DRV | REL/DAMP | Function |
|---:|---:|---|
| LOW | LOW | Idle |
| HIGH | LOW | Transmit |
| LOW | HIGH | Damping / transducer short |
| HIGH | HIGH | Forbidden while TX is active |

Firmware must never allow:

```text
DRV HIGH + REL HIGH + active TX carrier
```

---

# 18. Ultrasonic Receiver Multiplexer

The RX transducers are selected through a 74HC4052.

Inputs:

```text
RX_N_A / RX_N_B
RX_E_A / RX_E_B
RX_S_A / RX_S_B
RX_W_A / RX_W_B
```

Selection:

| MUX_B | MUX_A | Direction |
|---:|---:|---|
| 0 | 0 | North |
| 0 | 1 | East |
| 1 | 0 | South |
| 1 | 1 | West |

Enable:

```text
RX_EN_N HIGH → mux disabled
RX_EN_N LOW  → selected pair connected
```

The mux supply is locally filtered:

```text
3V3_SYS → 4.7 Ω → local mux VCC
                     │
                   100 nF
                     │
                    GND
```

---

# 19. RX Protection and Signal Conditioning

## Hot Path

```text
RX_HOT
→ R100 2.2 kΩ
→ RX_IN
→ BAV99 clamps to GND and 3V3_SYS
→ C103 220 pF to GND
→ R174 1 MΩ to VREF
→ C102 1 nF
→ ST1_IN
```

C102 was moved next to `ST1_IN` so the high-impedance input node is extremely short.

## Cold Path

```text
RX_COLD
→ R152 2.2 kΩ
→ BAV99 clamps
→ R107 100 kΩ || C109 1 nF
→ VREF
```

The clamp upper rails remain on normal `3V3_SYS`, not on the filtered analogue rails.

This prevents clamp-current injection into the quiet local amplifier or comparator supply islands.

---

# 20. RX Amplifier

The RX amplifier uses a TLV9062 dual op-amp.

Supply:

```text
3V3_SYS → 4.7 Ω → local amplifier VCC
                         │
                  100 nF + 1 µF
                         │
                        GND
```

## Stage 1

```text
R102 = 20 kΩ
R103 = 2 kΩ
C105 = 150 pF
```

Approximate low-frequency gain:

```text
Av1 = 1 + 20k / 2k
    = 11
```

Approximate upper shaping pole:

```text
~53 kHz
```

## Stage 2

```text
R106 = 22 kΩ
R105 = 3.3 kΩ
C108 = 150 pF
```

Approximate low-frequency gain:

```text
Av2 = 1 + 22k / 3.3k
    ≈ 7.67
```

Approximate upper shaping pole:

```text
~48 kHz
```

## Combined Gain

Approximate gain near 40 kHz:

```text
~52 V/V
~34 dB
```

The design is a broad ultrasonic conditioning chain rather than a narrow high-Q resonant filter.

This should reduce excessive timing uncertainty caused by filter ringing.

---

# 21. VREF

VREF is generated from `3V3_SYS`.

Network:

```text
3V3_SYS
→ 220 Ω
→ 47 kΩ
→ VREF
→ 47 kΩ
→ GND
```

Main decoupling:

```text
100 nF
1 µF
4.7 µF
```

Additional local 100 nF capacitors are placed at:

- TLV9062 bias points;
- comparator VREF input.

Expected VREF:

```text
approximately 1.65 V
```

The total capacitance and divider resistance give a relatively slow startup.

Allow approximately:

```text
700 ms after initial 3V3_SYS startup
```

before trusting the first ultrasonic measurement.

This delay is not required before every 22 V burst while the 3.3 V rail remains active.

---

# 22. Comparator and Hardware Blanking

## Comparator

The RX comparator uses an MCP6561.

Signal path:

```text
RX_AMP
→ R108 1 kΩ
→ comparator non-inverting input

VREF
→ comparator inverting input
```

The comparator output is push-pull, so no external output pull-up is required.

The comparator supply is locally filtered:

```text
3V3_SYS → 4.7 Ω → local comparator VCC
                         │
                  100 nF + 1 µF
                         │
                        GND
```

## Hysteresis

R109 is the normal hysteresis resistor.

R179 is an optional second hysteresis resistor.

Assembly recommendation:

```text
R109 = 1 MΩ, populated
R179 = 1 MΩ, DNP
```

Populate R179 only if comparator chatter is observed.

Approximate behaviour:

```text
R109 only:
lower external hysteresis

R109 + R179:
approximately double the external hysteresis
```

## Digital Blanking

Logic:

```text
RX_WINDOW_EN = NOT RX_EN_N

TOF_EDGE = COMP_RAW AND RX_WINDOW_EN
```

Therefore:

```text
RX_EN_N HIGH:
mux disabled
TOF_EDGE forced LOW

RX_EN_N LOW:
selected RX path enabled
COMP_RAW allowed through
```

U50 and U51 have local 100 nF decoupling.

---

# 23. Ultrasonic PCB Layout

## TX Region

The following remain grouped in the noisy TX region:

- 22 V MT3608;
- TC4427;
- Q16 global TX switch;
- `TX_PULSE`;
- directional P-MOSFETs;
- damping MOSFETs;
- high-current return paths;
- 22 V local decoupling.

## RX Region

The following remain grouped in the quiet RX region:

- 74HC4052;
- BAV99 clamp network;
- TLV9062;
- VREF consumers;
- MCP6561;
- U50/U51 blanking logic.

## Important Layout Improvements

Completed improvements include:

- C102 moved beside `ST1_IN`;
- `ST1_IN` kept extremely short;
- RX_IN routed over a local stitched GND corridor;
- 5 V pour moved away from RX_IN;
- RX_AMP kept short;
- COMP_RAW kept short;
- VREF locally decoupled;
- local filtered rails created for mux, amplifier, and comparator;
- TX and RX zones physically separated.

## Layer Stack

```text
L1  Top signal/components
L2  Solid GND
L3  Power pours and local GND corridor
L4  Bottom signal
```

A six-layer board is no longer required for this version.

## Ground Vias

Important GND vias have been added at:

- amplifier decoupling capacitors;
- comparator decoupling capacitors;
- mux decoupling capacitor;
- VREF capacitor bank;
- local VREF capacitors;
- clamp ground returns;
- RX_IN layer transitions;
- MT3608 capacitor grounds;
- Q16 pulldown return;
- TC4427 decoupling;
- directional MOSFET driver grounds;
- local RX ground corridor.

---

# 24. Recommended Ultrasonic Firmware Architecture

## Carrier Generation

Do not use `digitalWrite()` and `delayMicroseconds()` for production carrier generation.

Preferred:

```text
ESP32 RMT TX
```

Alternatives:

```text
MCPWM
LEDC with deterministic burst control
```

Initial test settings:

```text
Carrier frequency: approximately 40 kHz
Duty cycle:        50%
Burst cycles:      6
```

Then test:

```text
4 cycles
6 cycles
8 cycles
10 cycles
```

Use the shortest burst that provides reliable acoustic detection.

## TOF Capture

Do not rely on a normal GPIO interrupt with `micros()` for final wind measurement.

Preferred:

```text
MCPWM capture
or
RMT RX
```

Target resolution:

```text
100 ns preferred
250 ns potentially usable
1 µs insufficient for high-quality wind measurement
```

---

# 25. Ultrasonic Measurement State Machine

Recommended states:

```text
SAFE_IDLE
SELECT_PATH
BOOST_PRECHARGE
ARM_TIMING
TRANSMIT
DAMP
RX_PREPARE
CAPTURE
CLEANUP
```

## SAFE_IDLE

```text
TX_PWM LOW
22 V OFF
all DRV outputs LOW
all REL outputs LOW
RX disabled
capture disabled
```

## SELECT_PATH

```text
Set MUX_A and MUX_B
Select TX direction
Confirm all other DRV and REL outputs are LOW
```

## BOOST_PRECHARGE

```text
Enable 22 V
Wait for rail stability
```

Initial conservative delay:

```text
20–50 ms
```

## ARM_TIMING

- clear capture state;
- prepare the hardware timing origin;
- keep RX blocked.

## TRANSMIT

```text
Selected DRV HIGH
Selected REL LOW
Emit exact hardware-generated burst
Finish TX_PWM LOW
```

## DAMP

```text
DRV LOW
short non-overlap delay
REL HIGH
hold for configurable damping interval
REL LOW
```

Initial damping interval:

```text
10–50 µs
```

## RX_PREPARE

The same `RX_EN_N` edge enables the mux and opens the digital gate.

Use:

```text
1. RX_EN_N HIGH
2. Set MUX_A and MUX_B
3. Clear capture hardware
4. RX_EN_N LOW
5. Wait 10–20 µs
6. Clear any mux-settling edge
7. Start the valid acoustic capture gate
```

## CAPTURE

Accept edges only inside a physical timing window referenced to the first transmitted edge.

Initial estimated gate:

```text
approximately 350–550 µs
after the first transmitted edge
```

This must be refined using real hardware.

## CLEANUP

```text
RX disabled
TX_PWM LOW
all DRV LOW
all REL LOW
22 V OFF
capture disabled
```

Every fault and timeout should call the same cleanup function.

---

# 26. Power Sequencing

The radio and ultrasonic 22 V system should never operate at the same time.

Recommended sequence:

```text
Wake ESP32
Disable radios
Allow analogue rails and VREF to settle
Enable 22 V once
Perform the complete ultrasonic measurement batch
Disable 22 V
Complete other sensor measurements
Enable radio
Transmit data
Return to deep sleep
```

The 22 V boost should be enabled once per measurement batch, not once per individual shot.

This reduces:

- cold-start energy;
- connector peak current;
- converter stress;
- measurement-to-measurement rail variation.

---

# 27. Expected Current Draw

With radios off during ultrasonic operation:

```text
ESP32 active, radios off:
roughly tens of milliamps to approximately 100 mA

5 V and analogue circuits:
additional tens of milliamps

Ultrasonic TX:
likely total node current in the few-hundred-milliamp range

22 V cold start:
largest short transient
potentially upper hundreds of milliamps
```

Sustained operation near 1 A is not expected.

The 3000 mAh battery provides better current margin than the earlier 2000 mAh battery.

The 1.25 mm connector should be validated for:

- voltage drop;
- heating;
- cold-start current;
- repeated wake events.

---

# 28. Ultrasonic Calibration Model

For path length `L`:

```text
t_down = L / (c + v_path)

t_up = L / (c - v_path)
```

Path-axis airflow velocity:

```text
v_path = (L / 2) × (1 / t_down − 1 / t_up)
```

The physical wind-axis velocity is corrected using the transducer geometry projection factor.

Current known values:

```text
Path length:        approximately 146.70 mm
Still-air TOF:      approximately 427 µs
Projection factor:  approximately 0.5344
```

Calibration must include:

- temperature compensation;
- per-direction electronic timing offsets;
- zero-wind offsets;
- path-length correction;
- reciprocal pairing;
- robust outlier rejection;
- comparison with a reference anemometer.

---

# 29. Prototype Bring-Up Plan

## Stage 1 — Unpowered Inspection

Measure resistance from:

```text
VSYS to GND
3V3_SYS to GND
5V_SYS to GND
22V_SYS to GND
TX_PULSE to GND
```

Verify:

- MOSFET pin mappings;
- zener orientations;
- diode orientations;
- connector polarity;
- DNP parts;
- no accidental net shorts.

## Stage 2 — Low-Voltage Rails

With 22 V disabled, verify:

```text
VSYS
3V3_SYS
5V_SYS
KEEP_ALIVE
VREF
local mux supply
local amplifier supply
local comparator supply
```

## Stage 3 — 22 V Cold Start

Measure:

```text
battery current
VSYS
3V3_SYS
EN_22
22V_SYS
```

Test at:

```text
4.2 V battery
approximately 3.7 V
low usable battery voltage
```

Acceptance:

- no ESP32 reset;
- no brownout;
- no converter hiccup;
- stable `EN_22`;
- stable `22V_SYS`;
- acceptable temperature.

## Stage 4 — Global TX Waveform

Scope:

```text
GPIO25 / TX_PWM
PWM_5V
HS_GATE
TX_PULSE
22V_SYS
```

Acceptance:

```text
TX_PULSE HIGH > approximately 20 V
TX_PULSE LOW  < approximately 1–2 V
before the next rising edge
```

## Stage 5 — Directional TX

Test each direction individually.

Verify:

- only the selected transducer receives the main carrier;
- inactive directions show only small coupled spikes;
- DRV/REL truth table is correct;
- damping reduces ring-down.

## Stage 6 — RX Baseline

With TX disabled, observe:

```text
VREF
RX_IN
ST1_OUT
RX_AMP
COMP_RAW
TOF_EDGE
```

Acceptance:

- stable VREF;
- no unexplained baseline edge train;
- TOF_EDGE forced LOW when RX is disabled.

## Stage 7 — Electrical Coupling Test

Disconnect or isolate the RX transducer.

Transmit normally.

A persistent stable detection time with the receiver disconnected indicates electrical coupling or logic artefacts rather than an acoustic arrival.

## Stage 8 — Open vs Blocked Path

Collect repeated shots with:

```text
open acoustic path
blocked acoustic path
```

Compare:

- valid detection rate;
- median TOF;
- jitter;
- edge count.

The two conditions should clearly separate.

## Stage 9 — Reciprocal Direction Test

For each axis:

```text
A → B
B → A
```

At still air:

- reciprocal corrected values should be close;
- residual offset should be stable.

With directed airflow:

- the difference should change sign when airflow reverses.

## Stage 10 — Reference Calibration

Compare the node against a reference wind instrument.

Store:

- timing offsets;
- path corrections;
- temperature coefficients;
- carrier settings;
- damping settings;
- valid timing windows;
- quality thresholds.

---

# 30. Final Pre-Order Checklist

## Schematic

```text
[ ] All four TX directional channels identical
[ ] All four damping channels identical
[ ] TX and RX nets remain separate
[ ] Q16 source/drain/gate mapping verified
[ ] Directional P-MOS mappings verified
[ ] CJ2310 mappings verified
[ ] 2N7002 mappings verified
[ ] BAV99 clamp mappings verified
[ ] Zener cathodes face P-MOS sources
[ ] U49 polarity and net names verified
[ ] 5 V feedback divider updated
[ ] 22 V feedback divider verified
[ ] Battery divider connected to VSYS
[ ] Battery divider capacitor connected VOLT to GND
[ ] Q37 orientation verified
[ ] TP5100 catch diode polarity verified
[ ] TP5100 VS and BAT capacitors verified
[ ] R180 = 0.20 Ω
[ ] USB fuse = 0.75 A hold
[ ] PCA9306 VREF2 and EN joined
[ ] PCA9306 200 kΩ bias resistor fitted
[ ] PCA9306 filter capacitor fitted
[ ] AUX mode documented as mutually exclusive
[ ] R179 marked DNP
[ ] CH340 V3 implementation documented
[ ] No accidental duplicate net labels
```

## PCB

```text
[ ] All copper zones repoured
[ ] No isolated copper islands
[ ] RX_IN remains over GND corridor
[ ] RX_IN transition vias have nearby GND stitching
[ ] MT3608 SW node remains compact
[ ] No sensitive traces under SW nodes
[ ] TX_PULSE routing remains separated from RX
[ ] ST1_IN remains extremely short
[ ] R6 ground return is local
[ ] Decoupling capacitors have local GND vias
[ ] Charger switching loops are compact
[ ] Connector orientations checked
[ ] Battery polarity silkscreen checked
[ ] Diode and zener markings visible
[ ] Test points accessible
```

## Manufacturing

```text
[ ] ERC reviewed
[ ] DRC clean
[ ] Gerbers reviewed layer by layer
[ ] Drill files checked
[ ] Board outline checked
[ ] BOM checked
[ ] CPL checked
[ ] DNP parts excluded correctly
[ ] Address strap population notes added
[ ] Optional hysteresis part marked DNP
[ ] Correct JLC/LCSC parts assigned
```

---

# 31. Current Readiness Assessment

```text
Schematic architecture:        strong
Power system:                  ready for prototype
Charging circuits:             ready after final checks
Ultrasonic TX hardware:        ready for prototype
Ultrasonic RX hardware:        ready for prototype
Analogue layout:               substantially improved
Environmental sensor support:  ready for prototype
I²C architecture:              ready for prototype
Firmware architecture:         defined
Production firmware:           still required
Electrical validation:         pending
Acoustic validation:           pending
Mass-production readiness:     not yet
```

Recommended first fabrication quantity:

```text
3–5 prototype boards
```

---

# 32. Final Design Position

Node V3 is now a credible and substantially hardened ultrasonic environmental sensor-node prototype.

The main V2 weaknesses addressed in V3 include:

- improved 22 V inductor current margin;
- reduced 22 V cold-start capacitance;
- retained boot-safe boost control;
- corrected Q16 carrier-frequency gate drive;
- deterministic `TX_PULSE` LOW state;
- corrected directional source-tracking gate networks;
- clarified damping logic;
- locally filtered analogue rails;
- improved VREF stability;
- shortened high-impedance RX nodes;
- improved RX ground reference;
- corrected USB charger freewheel and output network;
- corrected USB current-sense and fuse sizing;
- corrected sensor high-side gate-clamp topology;
- corrected PCA9306 reference/enable configuration;
- relocated and filtered battery ADC divider;
- improved connector selection and pin documentation.

The remaining work is predominantly:

- formal file checks;
- prototype assembly;
- electrical bring-up;
- ultrasonic waveform validation;
- acoustic discrimination;
- timing calibration;
- production firmware implementation.

No further major ultrasonic schematic redesign is recommended before the first V3 prototype is built and measured.