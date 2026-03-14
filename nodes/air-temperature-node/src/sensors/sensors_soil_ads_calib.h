// soil_calibration.h
#pragma once
#include <Arduino.h>

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
