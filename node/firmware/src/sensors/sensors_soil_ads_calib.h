// soil_calibration.h
#pragma once
#include <Arduino.h>

// If 1, use ComWinTop TH-A linear conversion from analog voltage outputs.
// If 0, use legacy polynomial + thermistor model from older probe setup.
#ifndef SOIL_CWT_THA_MODE
#define SOIL_CWT_THA_MODE 1
#endif

// If the probe analog output is voltage-divided before ADS1115 input,
// set this gain so sensorVoltage = adcVoltage * gain.
#ifndef SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN
#define SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN 1.0f
#endif

// TH-A manual ranges (0-5V output mode).
constexpr float CWT_THA_VOLT_FS = 5.0f;
constexpr float CWT_THA_MOIST_MIN = 0.0f;
constexpr float CWT_THA_MOIST_MAX = 100.0f;
constexpr float CWT_THA_TEMP_MIN_C = -40.0f;
constexpr float CWT_THA_TEMP_MAX_C = 80.0f;

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline float cwt_tha_moisture_from_sensor_volts(float sensorV) {
    float moist = ((CWT_THA_MOIST_MAX - CWT_THA_MOIST_MIN) / CWT_THA_VOLT_FS) * sensorV;
    return clampf(moist, CWT_THA_MOIST_MIN, CWT_THA_MOIST_MAX);
}

inline float cwt_tha_temp_c_from_sensor_volts(float sensorV) {
    float tempC = ((CWT_THA_TEMP_MAX_C - CWT_THA_TEMP_MIN_C) / CWT_THA_VOLT_FS) * sensorV + CWT_THA_TEMP_MIN_C;
    return clampf(tempC, CWT_THA_TEMP_MIN_C, CWT_THA_TEMP_MAX_C);
}

// Coeffs from your MicroPython script
// Probe on ADS A0:
static constexpr float SOIL1_A0 = 0.982340f;
static constexpr float SOIL1_B0 = -5.249293e-04f;
static constexpr float SOIL1_C0 = 5.973622e-08f;

// Probe on ADS A1:
static constexpr float SOIL2_A1 = 0.694885f;
static constexpr float SOIL2_B1 = -1.2659144e-04f;
static constexpr float SOIL2_C1 = -6.2022472e-08f;

inline float soil_y_from_mv(float mv, float a, float b, float c) {
    return a + b * mv + c * mv * mv;
}

// Your calibration y is already θv, so we just clamp 0–0.6
inline float theta_v_from_mv(float mv, float a, float b, float c,
                             float lo = 0.0f, float hi = 0.6f)
{
    float tv = soil_y_from_mv(mv, a, b, c);
    if (tv < lo) tv = lo;
    if (tv > hi) tv = hi;
    return tv;
}
