// Soil analog conversion helpers.
#pragma once
#include <Arduino.h>

// If 1, use the ComWinTop TH-A linear conversion for temperature outputs.
// If 0, use the legacy thermistor model from the older probe setup.
#ifndef SOIL_CWT_THA_MODE
#define SOIL_CWT_THA_MODE 1
#endif

// If the probe analog output is voltage-divided before ADS1115 input,
// set this gain so sensorVoltage = adcVoltage * gain.
#ifndef SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN
#define SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN 1.0f
#endif

// Moisture calibration is intentionally not performed on the node. Moisture
// channels are emitted as sensor output volts so backend calibration can evolve
// without reflashing field nodes.

// TH-A manual temperature range (0-5V output mode).
constexpr float CWT_THA_VOLT_FS = 5.0f;
constexpr float CWT_THA_TEMP_MIN_C = -40.0f;
constexpr float CWT_THA_TEMP_MAX_C = 80.0f;

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float cwt_tha_temp_c_from_sensor_volts(float sensorV) {
    float tempC = ((CWT_THA_TEMP_MAX_C - CWT_THA_TEMP_MIN_C) / CWT_THA_VOLT_FS) * sensorV + CWT_THA_TEMP_MIN_C;
    return clampf(tempC, CWT_THA_TEMP_MIN_C, CWT_THA_TEMP_MAX_C);
}
