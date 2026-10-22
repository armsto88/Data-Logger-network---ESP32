# Node V4 firmware updates

**Status:** Required changes identified; firmware not yet modified  
**Hardware reviewed:** Node V4 schematic and BOM revision 8 dated 2026-10-16,
plus the native netlist exported 2026-10-17  
**Last reviewed:** 2026-10-17

This note records the firmware changes required when moving the current Node
firmware from Node V3 to the Node V4 hardware. It is deliberately separate from
the implementation so the hardware can be finalized and calibrated before the
production constants are committed.

## Required before Node V4 deployment

### 1. Identify the build as Node V4

The production environment in `node/firmware/platformio.ini` currently uses:

```ini
-D FW_HW_TARGET=\"node-v3\"
```

Create a Node V4 production environment or change the production target to:

```ini
-D FW_HW_TARGET=\"node-v4\"
```

Keeping a separate Node V3 environment is preferable if existing Node V3
boards will continue to receive firmware updates.

### 2. Apply the external analog-input divider gain

Every Node V4 ADS1115 channel uses:

- 36 kOhm from the connector input to the ADC node (`18 kOhm + 18 kOhm`);
- 5 kOhm from the ADC node to ground (`10 kOhm || 10 kOhm`).

The nominal reconstruction gain is therefore:

```text
(36 kOhm + 5 kOhm) / 5 kOhm = 8.2
```

Add the following production build flag:

```ini
-D SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN=8.2f
```

The existing conversion in `src/sensors/soil_moist_temp.cpp` already applies
this macro. Without the build flag, the default value remains `1.0f` and Node
V4 reports approximately one eighth of the actual sensor output voltage.

Treat `8.2` as the nominal starting value. Determine the production coefficient
from assembled-board measurements if the absolute voltage accuracy matters.

### 3. Confirm the ADS1115 full-scale range

The current ADS1115 driver uses the +/-4.096 V PGA range at 128 samples/s.
This is safe and should be retained for first bring-up.

With the 8.2:1 divider:

| Connector voltage | ADS1115 pin voltage |
| ---: | ---: |
| 3.3 V | 0.402 V |
| 5.0 V | 0.610 V |
| 10.0 V | 1.220 V |
| 12.0 V | 1.463 V |
| 13.0 V | 1.585 V |

After hardware validation, the +/-2.048 V range may be used to approximately
double the voltage resolution. It still covers connector inputs up to about
16.8 V. Do not make that change until rail overshoot and the intended maximum
external signal voltage have been measured.

### 4. Calibrate battery-voltage measurement

The Node V4 schematic has a nominal battery-divider ratio of:

```text
(220 kOhm + 100 kOhm) / 100 kOhm = 3.20
```

The current build uses:

```ini
-D BAT_DIVIDER_SCALE=3.58f
```

Do not automatically replace `3.58` with `3.20`. The larger value may include
empirical compensation for ESP32 ADC error. Calibrate the assembled Node V4 at
three battery voltages, for example 3.2 V, 3.7 V and 4.2 V, using a calibrated
DMM. Then set the Node V4 coefficient from the measured result.

Where supported by the selected Arduino-ESP32 release, use the ESP32 calibrated
millivolt API instead of converting raw counts with an assumed 3.3 V ADC
reference.

### 5. Preserve the confirmed sensor-channel mapping

The present firmware mapping is:

```text
SOIL1: A0 = temperature, A1 = moisture
SOIL2: A2 = moisture,    A3 = temperature
```

Confirm the final connector-to-harness continuity against this mapping before
release. If the harness pinout changes, update the mapping in firmware rather
than silently swapping backend channel meanings.

### 6. Retain the mux address

The Node V4 TCA9546A straps select address `0x71` when the default zero-ohm
links are fitted. The existing production build already specifies:

```ini
-D MUX_ADDR=0x71
```

No firmware change is required unless the hardware address links are changed.

### 7. Limit the mux configuration to four channels

Node V4 uses a TCA9546A four-channel mux. The shared firmware default is still
eight channels:

```cpp
#define MUX_CHANNELS 8
```

Add the following to the Node V4 production build flags:

```ini
-D MUX_CHANNELS=4
```

The existing mux helper checks requested channels against `MUX_CHANNELS`, so
this flag prevents later code from treating nonexistent channels 4 through 7
as valid. It does not change the mux address or the ADS1115 connection; the
ADS1115 remains directly on the root I2C bus at address `0x48`.

### 8. Remove the stale RTC interrupt GPIO definition

The DS3231 alarm output controls the hardware power-gate path and is not routed
to an ESP32 GPIO on Node V4. GPIO4 is used by the reed/RX-enable signal. The
production build currently still contains the historical definition:

```ini
-D RTC_INT_PIN=4
```

Remove that flag from the Node V4 production environment. Do not replace it
with another GPIO unless the final schematic actually routes the DS3231
interrupt to that pin.

The current production source does not use `RTC_INT_PIN` for normal operation,
so removing the flag should not alter existing behaviour. It prevents a future
deep-sleep or interrupt change from incorrectly treating reed pulses on GPIO4
as RTC alarms. The fallback definition in `shared/protocol.h` should eventually
be removed or restricted to the legacy hardware targets that still use it.

### 9. Reserve the final Node V4 field-interface GPIO mapping

The revision 7 schematic currently assigns `SDI12_TX_EN` to GPIO35. GPIO35 is
input-only on the ESP32-WROOM-32E and therefore cannot drive the transmitter-
enable circuit. It also assigns `REED_SIG_1` to GPIO2 with a 10 kOhm pull-up;
GPIO2 is a boot-strapping pin and holding it high can prevent automatic serial
download when GPIO0 is driven low.

Before PCB release, change those two nets as follows:

- move `SDI12_TX_EN` from GPIO35 to the unused output-capable GPIO13;
- move `REED_SIG_1` from GPIO2 to GPIO35, retaining its external 10 kOhm
  pull-up and 100 nF filter.

After that schematic correction, use this Node V4 mapping:

| Function | ESP32 GPIO | Direction and default |
| --- | ---: | --- |
| `RS485_PWR_EN` | 32 | Output, low/off at boot |
| `RS485_DIR` | 33 | Output, low/receive at boot |
| `RS485_TX` | 17 | UART output |
| `RS485_RX` | 16 | UART input |
| `SDI12_TX_EN` | 13 | Output, low/transmitter disabled at boot |
| `SDI12_TX` | 25 | UART or timed output |
| `SDI12_RX` | 26 | UART or timed input |
| `SDI12_PWR_EN` | 27 | Output, low/off at boot |
| `REED_SIG_1` | 35 | Digital input; input-only GPIO |
| `REED_SIG_2` | 4 | Digital input |
| `VOLT_ESP` | 34 | Analog input; input-only GPIO |

Set all four control outputs to their safe low states before enabling either
field-interface UART or powering an external sensor. GPIO34 and GPIO35 cannot
provide internal pull-ups; the battery-divider and reed-input circuits provide
the required external biasing. Do not restore either reed input to GPIO2.

These assignments conflict with the historical ultrasonic-anemometer GPIO
mapping. The Node V4 build must not compile or initialize the legacy ultrasonic
interface on these pins. Treat removal of that interface as an intentional
Node V4 hardware change rather than trying to support both mappings at run
time.

### 10. Implement the SDI-12 interface polarity and timing

Configure the SDI-12 serial format as 1200 baud, 7 data bits, even parity and
one stop bit (`1200 7E1`). The external data line uses negative logic.

The final receiver comparator is wired as a hardware inverter:

- SDI-12 marking/idle, approximately 0 V, produces a high `SDI12_RX` level;
- SDI-12 spacing, approximately 5 V, produces a low `SDI12_RX` level.

Consequently, do **not** enable receive inversion in the ESP32 UART.

The SN74AHCT1G125 transmitter is non-inverting, so the ESP32 transmit signal
must be inverted. Configure transmit inversion only, or generate the SDI-12
waveform explicitly with the corresponding inverted levels. Confirm on an
oscilloscope that:

- UART idle and stop bits produce a low/marking level on `SDI12_DATA`;
- start bits and transmitted zero bits produce a high/spacing level;
- a break produces a continuous high/spacing level for at least 12 ms.

Use the following transmitter sequence:

1. Configure the transmit pin and inversion while `SDI12_TX_EN` remains low.
2. Establish the marking/idle output level before enabling the transmitter.
3. Set `SDI12_TX_EN` high to drive the bus, then generate the break and command.
4. Return the transmitter to marking after the final stop bit.
5. Set `SDI12_TX_EN` low within 7.5 ms of the final stop bit so the sensor can
   respond.
6. Ignore or flush locally received command echo while the transmitter is
   enabled, then receive the sensor response normally.

Implement the SDI-12 retry and response-time rules rather than treating it as
an ordinary request/response UART. In particular, allow sensors their specified
wake time following a break and validate address, framing, parity and optional
CRC before accepting a response.

Keep `SDI12_PWR_EN` low except while an SDI-12 sensor is deliberately powered.
Allow a configurable sensor startup delay before issuing the first break. Log
power-on, break, command, response and timeout timestamps in the engineering
build so the hardware timing can be verified.

### 11. Implement RS-485/Modbus direction and power control

The THVD2410 driver enable and active-low receiver enable share `RS485_DIR`:

- `RS485_DIR = 0`: transmitter disabled and receiver enabled;
- `RS485_DIR = 1`: transmitter enabled and receiver disabled.

Keep `RS485_DIR` low by default. Set it high immediately before transmitting,
wait for the UART shift register—not merely the software buffer—to become
empty, and then return it low promptly to receive the reply.

Keep `RS485_PWR_EN` low except while the external Modbus sensor is being used.
Baud rate, parity, stop bits, slave address, response timeout and retry count
must be configurable because Modbus sensor settings are not universal.

The 12 V boost rail is not sized for all field ports at their individual fuse
limits simultaneously. Firmware must not enable `RS485_PWR_EN` and
`SDI12_PWR_EN` at the same time. The mechanically selected analog-sensor ports
cannot be interlocked in firmware, so the documented total 12 V load limit
still applies when either digital field port is enabled.

Use the following provisional field-sensor budget for implementation and
bring-up. The budget deliberately uses a reasonably demanding representative
Modbus weather sensor and a common SDI-12 probe rather than the lightest
possible devices.

| Load | Published 12 V requirement | Firmware/hardware budget |
| --- | ---: | ---: |
| CWT TH-A 0-5 V soil probe 1 | 25 mA maximum (0.3 W) | 30 mA |
| CWT TH-A 0-5 V soil probe 2 | 25 mA maximum (0.3 W) | 30 mA |
| Renke RS-FSXCS-N01-3H-T RS-485 weather sensor | 58.3 mA maximum equivalent (0.7 W) | 70 mA |
| Campbell Scientific CS655 SDI-12 probe | 45 mA required/typical active | 60 mA |

The published simultaneous load is approximately 153 mA. The rounded design
budget is 190 mA, which leaves too little startup and thermal margin to make
unrestricted simultaneous operation the production policy.

The two soil probes remain powered whenever their mechanically selected ports
and the 12 V rail are active. Sequence the switchable interfaces as follows:

1. Initialize both `RS485_PWR_EN` and `SDI12_PWR_EN` low.
2. Power and read the RS-485 sensor, then disable its power output.
3. Confirm the RS-485 output is disabled before enabling SDI-12 power.
4. Power and read the SDI-12 sensor, then disable its power output.
5. Reject or queue any request that would make both power enables high.

With the per-sensor allowances above, this limits the intended 12 V load to
about 130 mA during the RS-485 phase and 120 mA during the SDI-12 phase. Sensor
startup delays must be configurable; do not assume that a valid reading is
available immediately after applying power.

These values are an acceptance-test envelope, not sensor-identification logic.
If a deployed sensor has a higher published continuous or startup current, it
must receive a new power-budget review before being enabled on Node V4.

### 12. Add the second reed-switch channel

Node V4 has two protected reed inputs. Retain the existing debounce behaviour
for `REED_SIG_2` on GPIO4 and instantiate the same event/counting logic for
`REED_SIG_1` on GPIO35. Do not enable an ESP32 internal pull-up on GPIO35;
the external schematic pull-up is authoritative.

Make the two channels independently configurable and report their counts or
states under distinct telemetry identifiers. Test simultaneous activity so an
interrupt or debounce interval on one channel does not suppress the other.

### Recommended Node V4 production flags

At minimum, the Node V4 environment should contain:

```ini
-D FW_HW_TARGET=\"node-v4\"
-D MUX_ADDR=0x71
-D MUX_CHANNELS=4
-D SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN=8.2f
```

Do not include `RTC_INT_PIN` in the Node V4 environment. Keep the final
calibrated `BAT_DIVIDER_SCALE` alongside these flags once it has been measured
on assembled hardware.

## No firmware change required

- The TMP390A2 hot/cold outputs and the two-transistor CN3791 MPPT clamp form a
  hardware-only charging interlock. The ESP32 has no control or monitoring GPIO
  on `TEMP_FAULT_N`, so no firmware task, interrupt or startup sequence is
  required. Charging must remain inhibited during a temperature fault even if
  the ESP32 is unpowered, crashed or held in reset. The final confirmed values
  are `R21 = 47 kOhm` and `R22 = 100 kOhm`.
- The selected TMP390 thresholds are approximately +5 degC cold trip/+10 degC
  reset and +42 degC hot trip/+37 degC reset. These are hardware thresholds;
  do not duplicate them as firmware cutoffs or imply in telemetry that the
  ESP32 has measured the battery temperature.
- The proposed TPS63802 3.3 V buck-boost replacement can use `VSYS` directly
  for its enable input. It therefore follows the existing hardware power latch
  and needs no MCU-controlled enable sequence.
- The 3.3/5/12 V mechanical selectors and TPS2121 power multiplexers operate
  entirely in hardware. Firmware does not know which voltage a user selected
  and must not infer it from the measured sensor signal.
- CH340 V3/VCC correction is hardware-only.
- Reed-switch surge/ESD protection is hardware-only. Firmware changes for the
  second reed channel are listed above.

## Recommended Node V4 validation logging

Add a temporary engineering/debug mode that records:

- raw signed ADS1115 counts for A0 through A3;
- ADC-pin millivolts before the 8.2 multiplier;
- reconstructed connector voltage;
- raw battery ADC counts and calibrated battery millivolts;
- mux address probe result and selected channel;
- sensor warm-up time and first-valid-sample time;
- SDI-12 power, transmitter-enable, break, command and response timestamps;
- Modbus power, direction-control, request and response timestamps;
- independent raw and debounced events for both reed inputs.

The current PCB exposes the charger-temperature state only at test points. Do
not add firmware reporting for `TEMP_FAULT_N` unless a future schematic
revision deliberately routes that signal to a suitable ESP32 GPIO. For Node V4
bring-up, record the temperature-shutdown result in the hardware test report.

Do not enable this verbose logging in the final low-power production build.

## Acceptance tests

1. Apply 0 V, 3.3 V, 5 V, 10 V and 12 V to each analog input and compare the
   reported voltage with a DMM.
2. Verify that every channel reconstructs voltage using the same 8.2 nominal
   gain before applying any sensor-specific calibration.
3. Validate both ADS1115 PGA settings and record clipping margin during 12 V
   rail startup and shutdown.
4. Confirm the two CWT TH-A temperature channels produce the expected values at
   at least two known input voltages.
5. Calibrate battery voltage at low, nominal and fully charged pack voltage.
6. Confirm the production image reports `node-v4` in all diagnostics, telemetry
   and update-compatibility checks.
7. Run a complete wake, sensor warm-up, acquisition, radio transmission and
   shutdown cycle at battery voltages from 4.2 V down to the chosen cutoff.
8. While the Node firmware is running, force the charger `TEMP_FAULT_N` test
   point low and confirm externally that charging stops. This is a coexistence
   test only: the shutdown must not depend on firmware and no firmware event is
   expected.
9. Verify SDI-12 `1200 7E1` operation with an oscilloscope: idle low on the
   external data line, break high for at least 12 ms, correct command polarity,
   and transmitter release within 7.5 ms.
10. Verify a real SDI-12 sensor response, including parity checking, timeout and
    retry behaviour. Confirm `SDI12_RX` is not inverted in firmware.
11. Verify RS-485 direction turnaround at every supported baud rate and confirm
    that the receiver is enabled before the Modbus slave begins its reply.
12. Attempt to request RS-485 and SDI-12 power concurrently and confirm the
    firmware rejects or serializes the second request.
13. Pulse both reed inputs independently and simultaneously and verify their
    counts remain independent through debounce and telemetry reporting.
14. With both reed inputs open and again with each one closed, perform an
    automatic USB download/reset cycle. Confirm the final hardware no longer
    uses GPIO2 and always enters the serial bootloader when requested.
15. With two CWT TH-A probes powered at 12 V, measure rail voltage and current
    while separately starting and reading the representative RS-485 and SDI-12
    loads. Repeat at the minimum permitted battery voltage and maximum intended
    enclosure temperature. Confirm that the loads are never powered together,
    the 12 V rail remains in regulation and neither 100 mA field-port PTC
    nuisance-trips.

## Expected implementation files

- `node/firmware/platformio.ini`
- `node/firmware/shared/protocol.h` when retiring the legacy `RTC_INT_PIN`
  fallback
- `node/firmware/src/sensors/sensors_soil_ads_calib.h`
- `node/firmware/src/drivers/ads1115_helper.cpp` if the PGA range changes
- `node/firmware/src/main.cpp` for calibrated battery-millivolt acquisition
- Node V4 SDI-12 and Modbus driver modules, with their GPIO assignments kept in
  the Node V4 hardware configuration rather than duplicated in drivers
- the reed-input module for the second independently debounced channel
- Node V4 bring-up tests under `node/firmware/tests/`

