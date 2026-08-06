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

// Unclamped conversion. Needed for the presence test below: clamping is what
// turned a disconnected probe's 0 V float into exactly -40.00 C and let it be
// reported as an ordinary reading.
inline float cwt_tha_temp_c_raw_from_sensor_volts(float sensorV) {
    return ((CWT_THA_TEMP_MAX_C - CWT_THA_TEMP_MIN_C) / CWT_THA_VOLT_FS) * sensorV + CWT_THA_TEMP_MIN_C;
}

inline float cwt_tha_temp_c_from_sensor_volts(float sensorV) {
    return clampf(cwt_tha_temp_c_raw_from_sensor_volts(sensorV), CWT_THA_TEMP_MIN_C, CWT_THA_TEMP_MAX_C);
}

// Probe-presence band, in degrees C of the UNCLAMPED conversion above.
//
// A disconnected analog input does not fail the way an I2C read does — it
// floats, and the conversion turns whatever stray voltage is on the pin into a
// number. Temperature is the usable presence signal because, unlike moisture,
// its credible range excludes both rails: a probe lying in open air still
// reports true ambient, whereas a disconnected one lands at a rail (0 V -> -40 C,
// 5 V -> +80 C). Moisture cannot do this job — a probe in air legitimately reads
// near 0 V, which is indistinguishable from an unplugged one.
//
// Deliberately wide. These bound "is a probe physically attached", NOT "is this
// a good measurement" — narrowing them trades a missed fault for the far worse
// failure of declaring a working probe dead and dropping real data.
//
// Caveat this cannot fix in software: with no bias resistor on the ADS1115
// input, a floating pin is undefined and can drift into this band by
// coincidence. A pull-down per input makes "disconnected" a defined 0 V and
// turns this from a strong heuristic into a deterministic test.
#ifndef SOIL_TEMP_PRESENT_MIN_C
#define SOIL_TEMP_PRESENT_MIN_C (-15.0f)
#endif
#ifndef SOIL_TEMP_PRESENT_MAX_C
#define SOIL_TEMP_PRESENT_MAX_C (60.0f)
#endif

// True when this probe's temperature channel looks like a physically attached
// sensor rather than a floating input.
inline bool cwt_tha_probe_present(float tempSensorV) {
    const float t = cwt_tha_temp_c_raw_from_sensor_volts(tempSensorV);
    return (t >= SOIL_TEMP_PRESENT_MIN_C) && (t <= SOIL_TEMP_PRESENT_MAX_C);
}
