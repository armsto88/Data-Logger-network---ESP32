// src/soil_moist_temp.cpp
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "soil_moist_temp.h"
#include "ads1115_helper.h"
#include "sensors_soil_ads_calib.h"   // your SOIL1_A0, SOIL2_A1, theta_v_from_mv

// Use the same I2C bus as RTC / rest of sensors
extern TwoWire WireRtc;

namespace {

  // One global ADS instance on the root I2C bus
  static ADS1115 ads(WireRtc);

  // ---- Thermistor divider + Steinhart–Hart (from MicroPython main.py) ----
  constexpr float V_DIV_SUPPLY = 4.910f;
  constexpr float R_FIXED_A2   = 9880.0f;   // A2 → soil1 NTC
  constexpr float R_FIXED_A3   = 9970.0f;   // A3 → soil2 NTC

  // Fallback SH coefficients (from fit on your logs)
  constexpr float A2_A = -0.0036485006f;
  constexpr float A2_B =  0.00096359095f;
  constexpr float A2_C = -2.4188805e-06f;

  constexpr float A3_A = -0.0047102991f;
  constexpr float A3_B =  0.00112009362f;
  constexpr float A3_C = -2.9164770e-06f;

  constexpr float A2_TRIM_GAIN = 1.000f;
  constexpr float A2_TRIM_OFF  = 0.0f;
  constexpr float A3_TRIM_GAIN = 1.000f;
  constexpr float A3_TRIM_OFF  = 0.0f;

  // Cached sample so we don’t hammer the ADS every time read() is called
  bool     haveSample    = false;
  uint32_t lastSampleMs  = 0;
  float    lastThetaV1   = NAN;  // SOIL1_VWC
  float    lastThetaV2   = NAN;  // SOIL2_VWC
  float    lastTemp1C    = NAN;  // SOIL1_TEMP
  float    lastTemp2C    = NAN;  // SOIL2_TEMP

  float r_from_vnode(float vnode_v, float vsup_v, float r_fixed_ohm) {
    float v = vnode_v;
    if (v < 0.001f)          v = 0.001f;
    if (v > vsup_v - 0.001f) v = vsup_v - 0.001f;
    return r_fixed_ohm * ((vsup_v - v) / v);
  }

  float sh_temp_c(float R_ohm, float A, float B, float C) {
    float lnR  = logf(R_ohm);
    float invT = A + B * lnR + C * lnR * lnR * lnR;
    float T    = 1.0f / invT;
    return T - 273.15f;
  }

  void sampleAdsIfNeeded() {
    uint32_t now = millis();
    // if we sampled within the last 250 ms, reuse
    if (haveSample && (now - lastSampleMs) < 250) {
      return;
    }

    int16_t raw0, raw1, raw2, raw3;
    float   mv0,  mv1,  mv2,  mv3;

    bool ok0 = ads.readChannelMv(0, raw0, mv0); // SOIL1 moisture
    bool ok1 = ads.readChannelMv(1, raw1, mv1); // SOIL2 moisture
    bool ok2 = ads.readChannelMv(2, raw2, mv2); // SOIL1 thermistor
    bool ok3 = ads.readChannelMv(3, raw3, mv3); // SOIL2 thermistor

    if (!ok0 || !ok1 || !ok2 || !ok3) {
      Serial.println(F("[SOIL] ADS1115 read failed on one or more channels"));
      haveSample = false;
      return;
    }

    // Moisture → θv (using your soil_calibration.h helpers)
    lastThetaV1 = theta_v_from_mv(mv0, SOIL1_A0, SOIL1_B0, SOIL1_C0);
    lastThetaV2 = theta_v_from_mv(mv1, SOIL2_A1, SOIL2_B1, SOIL2_C1);

    // Thermistors → °C
    float r2 = r_from_vnode(mv2 / 1000.0f, V_DIV_SUPPLY, R_FIXED_A2);
    float t2 = sh_temp_c(r2, A2_A, A2_B, A2_C);
    lastTemp1C = A2_TRIM_GAIN * t2 + A2_TRIM_OFF;

    float r3 = r_from_vnode(mv3 / 1000.0f, V_DIV_SUPPLY, R_FIXED_A3);
    float t3 = sh_temp_c(r3, A3_A, A3_B, A3_C);
    lastTemp2C = A3_TRIM_GAIN * t3 + A3_TRIM_OFF;

    // Debug
    Serial.printf("[SOIL] ch0 raw=%d mv=%.1f → θv1=%.4f\n", raw0, mv0, lastThetaV1);
    Serial.printf("[SOIL] ch1 raw=%d mv=%.1f → θv2=%.4f\n", raw1, mv1, lastThetaV2);
    Serial.printf("[SOIL] ch2 raw=%d mv=%.1f → Tsoil1=%.2f °C\n", raw2, mv2, lastTemp1C);
    Serial.printf("[SOIL] ch3 raw=%d mv=%.1f → Tsoil2=%.2f °C\n", raw3, mv3, lastTemp2C);

    haveSample   = true;
    lastSampleMs = now;
  }

} // anonymous namespace


// ----------------- Public API -----------------
namespace soil_moist_temp_backend {

  bool init() {
    Serial.println(F("[SOIL] soil_moist_temp_backend::init() — probing ADS1115 on WireRtc"));
    if (!ads.begin()) {
      Serial.println(F("[SOIL] ⚠️ ADS1115 not found at 0x48 on WireRtc"));
      return false;
    }
    Serial.println(F("[SOIL] ✅ ADS1115 ready (soil_moist_temp backend)"));
    haveSample = false;
    return true;
  }

  size_t count() {
    // 0: SOIL1_VWC
    // 1: SOIL2_VWC
    // 2: SOIL1_TEMP
    // 3: SOIL2_TEMP
    return 4;
  }

  const char* label(size_t index) {
    switch (index) {
      case 0: return "SOIL1_VWC";
      case 1: return "SOIL2_VWC";
      case 2: return "SOIL1_TEMP";
      case 3: return "SOIL2_TEMP";
      default: return "UNKNOWN";
    }
  }

  const char* type(size_t index) {
    switch (index) {
      case 0:
      case 1:
        return "SOIL_VWC";
      case 2:
      case 3:
        return "SOIL_TEMP";
      default:
        return "UNKNOWN";
    }
  }

  bool read(size_t index, float &outValue) {
    if (index >= count()) return false;

    sampleAdsIfNeeded();
    if (!haveSample) return false;

    switch (index) {
      case 0: outValue = lastThetaV1; break;
      case 1: outValue = lastThetaV2; break;
      case 2: outValue = lastTemp1C;  break;
      case 3: outValue = lastTemp2C;  break;
      default: return false;
    }
    return true;
  }
}
