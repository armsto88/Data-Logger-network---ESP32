# CWT TH-A Soil Sensor Implementation Notes

Source: user-provided manual text for ComWinTop TH-A analog soil sensor.

## 1. Model Summary

Vendor: CWT CO., LIMITED (ComWinTop)
Model family: TH-A (analog type)
Measured parameters per probe:
- Soil moisture (0-100 %RH output scale in manual wording)
- Soil temperature (-40 C to 80 C)

Node mapping target (2 probes total):
- A0 -> Probe1 moisture output
- A1 -> Probe2 moisture output
- A2 -> Probe1 temperature output
- A3 -> Probe2 temperature output

## 2. Electrical Notes from Manual

Power supply:
- 0-5V and 4-20mA variants: DC 10-30V
- 0-10V variant: DC 18-30V

Outputs:
- Current: 4-20mA
- Voltage: 0-5V or 0-10V

Wire color (per manual):
- Brown: Power+
- Black: GND
- Yellow: Moisture signal output
- Blue: Temperature signal output

## 3. Conversion Formulas (from Manual)

For 0-5V variant:
- Moisture = (Pmax - Pmin) / (5 - 0) * V
- For moisture 0..100, this simplifies to:
  - Moisture = 20 * V

- Temperature = (Pmax - Pmin) / (5 - 0) * V - 40
- For temperature -40..80, this simplifies to:
  - Temperature_C = 24 * V - 40

For 4-20mA variant:
- Moisture = (Pmax - Pmin) / (20 - 4) * (I_mA - 4)
- Temperature_C = (Pmax - Pmin) / (20 - 4) * (I_mA - 4) - 40

## 4. Firmware Integration Status

Implemented in node firmware:
- Compile-time mode flag in
  - firmware/nodes/sensor-node/src/sensors/sensors_soil_ads_calib.h
  - SOIL_CWT_THA_MODE (default 1)
- ADS input voltage scaling factor:
  - SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN (default 1.0)
- Soil backend conversion path in
  - firmware/nodes/sensor-node/src/sensors/soil_moist_temp.cpp

When SOIL_CWT_THA_MODE=1:
- A0/A1 use linear moisture conversion from voltage
- A2/A3 use linear temperature conversion from voltage
- values are clamped to:
  - Moisture: 0..100
  - Temperature: -40..80 C

## 5. Important Hardware Integration Caution

ADS1115 full-scale input is much lower than 10-30V supply rails.
If using TH-A 0-5V outputs directly, ensure analog front-end is valid for ADS1115 input range and gain settings.

If a divider is used before ADS1115:
- Set SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN accordingly.
- Example: divider halves 0-5V to 0-2.5V at ADC input -> gain = 2.0

For 4-20mA sensors:
- Use precision shunt resistor and convert measured voltage across shunt to current, then apply current formula.
- This path is not yet implemented in current firmware.

## 6. Recommended Field Validation

1. Bench inject known voltages and confirm conversion:
- 0.0V -> moisture 0, temp -40C
- 2.5V -> moisture 50, temp 20C
- 5.0V -> moisture 100, temp 80C

2. Verify per-channel mapping physically:
- A0 moisture1, A1 moisture2, A2 temp1, A3 temp2

3. Record serial outputs and compare against handheld reference meter during soak tests.

4. If bias is observed, add per-channel gain/offset trim constants.
