// src/soil_moist_temp.cpp
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "soil_moist_temp.h"
#include "../drivers/ads1115_helper.h"
#include "sensors_soil_ads_calib.h"

// Use the same I2C bus as RTC / rest of sensors.
extern TwoWire WireRtc;

namespace {

constexpr uint8_t CH_SOIL1_TEMP  = 0;
constexpr uint8_t CH_SOIL1_MOIST = 1;
constexpr uint8_t CH_SOIL2_MOIST = 2;
constexpr uint8_t CH_SOIL2_TEMP  = 3;

// One global ADS instance on the root I2C bus.
static ADS1115 ads(WireRtc);

// Legacy thermistor divider + Steinhart-Hart from older probe setup.
constexpr float V_DIV_SUPPLY = 4.910f;
constexpr float R_FIXED_A0   = 9880.0f;  // A0 -> soil1 NTC
constexpr float R_FIXED_A3   = 9970.0f;  // A3 -> soil2 NTC

// Fallback SH coefficients from fit on prior logs.
constexpr float A0_A = -0.0036485006f;
constexpr float A0_B =  0.00096359095f;
constexpr float A0_C = -2.4188805e-06f;

constexpr float A3_A = -0.0047102991f;
constexpr float A3_B =  0.00112009362f;
constexpr float A3_C = -2.9164770e-06f;

constexpr float A0_TRIM_GAIN = 1.000f;
constexpr float A0_TRIM_OFF  = 0.0f;
constexpr float A3_TRIM_GAIN = 1.000f;
constexpr float A3_TRIM_OFF  = 0.0f;

// Cached sample so repeated read() calls for the four logical channels reuse
// the same ADS conversion set.
bool     haveSample    = false;
uint32_t lastSampleMs  = 0;
float    lastMoist1V   = NAN;  // SOIL1_VWC legacy channel: sensor volts
float    lastMoist2V   = NAN;  // SOIL2_VWC legacy channel: sensor volts
float    lastTemp1C    = NAN;  // SOIL1_TEMP
float    lastTemp2C    = NAN;  // SOIL2_TEMP

float r_from_vnode(float vnode_v, float vsup_v, float r_fixed_ohm) {
  float v = vnode_v;
  if (v < 0.001f) v = 0.001f;
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
  if (haveSample && (now - lastSampleMs) < 250) {
    return;
  }

  // Ping the ADS1115 before attempting a full 4-channel read. If the ADC is
  // hung or absent, this returns quickly instead of blocking on each channel.
  WireRtc.beginTransmission(0x48);
  if (WireRtc.endTransmission() != 0) {
    Serial.println(F("[SOIL] ADS1115 not responding - skipping soil this cycle"));
    haveSample = false;
    return;
  }

  int16_t raw0, raw1, raw2, raw3;
  float   mv0,  mv1,  mv2,  mv3;

  bool ok0 = ads.readChannelMv(CH_SOIL1_TEMP,  raw0, mv0);  // SOIL1 temperature
  bool ok1 = ads.readChannelMv(CH_SOIL1_MOIST, raw1, mv1);  // SOIL1 moisture voltage
  bool ok2 = ads.readChannelMv(CH_SOIL2_MOIST, raw2, mv2);  // SOIL2 moisture voltage
  bool ok3 = ads.readChannelMv(CH_SOIL2_TEMP,  raw3, mv3);  // SOIL2 temperature

  if (!ok0 || !ok1 || !ok2 || !ok3) {
    Serial.println(F("[SOIL] ADS1115 read failed on one or more channels"));
    haveSample = false;
    return;
  }

  // CWT TH-A current wiring: SOIL1 A0=temp A1=moisture,
  // SOIL2 A2=moisture A3=temp. Convert ADS input volts to sensor output volts
  // using the configured gain.
  const float v0 = (mv0 / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v1 = (mv1 / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v2 = (mv2 / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;
  const float v3 = (mv3 / 1000.0f) * SOIL_ADC_INPUT_TO_SENSOR_VOLT_GAIN;

  // Moisture calibration lives in the backend. The node emits raw sensor
  // output voltage in the existing VWC channels to preserve protocol/schema
  // compatibility while making calibration curves field-updatable.
  lastMoist1V = v1;
  lastMoist2V = v2;

#if SOIL_CWT_THA_MODE
  lastTemp1C = cwt_tha_temp_c_from_sensor_volts(v0);
  lastTemp2C = cwt_tha_temp_c_from_sensor_volts(v3);
#else
  // Legacy thermistor model.
  float r0 = r_from_vnode(mv0 / 1000.0f, V_DIV_SUPPLY, R_FIXED_A0);
  float t0 = sh_temp_c(r0, A0_A, A0_B, A0_C);
  lastTemp1C = A0_TRIM_GAIN * t0 + A0_TRIM_OFF;

  float r3 = r_from_vnode(mv3 / 1000.0f, V_DIV_SUPPLY, R_FIXED_A3);
  float t3 = sh_temp_c(r3, A3_A, A3_B, A3_C);
  lastTemp2C = A3_TRIM_GAIN * t3 + A3_TRIM_OFF;
#endif

  Serial.printf("[SOIL] ch0 raw=%d mv=%.1f sensorV=%.4fV -> Tsoil1=%.2f C\n",
                raw0, mv0, v0, lastTemp1C);
  Serial.printf("[SOIL] ch1 raw=%d mv=%.1f sensorV=%.4fV -> soil1_voltage=%.4fV\n",
                raw1, mv1, v1, lastMoist1V);
  Serial.printf("[SOIL] ch2 raw=%d mv=%.1f sensorV=%.4fV -> soil2_voltage=%.4fV\n",
                raw2, mv2, v2, lastMoist2V);
  Serial.printf("[SOIL] ch3 raw=%d mv=%.1f sensorV=%.4fV -> Tsoil2=%.2f C\n",
                raw3, mv3, v3, lastTemp2C);

  haveSample   = true;
  lastSampleMs = now;
}

} // namespace

namespace soil_moist_temp_backend {

bool init() {
  Serial.println(F("[SOIL] soil_moist_temp_backend::init() - probing ADS1115 on WireRtc"));
  if (!ads.begin()) {
    Serial.println(F("[SOIL] ADS1115 not found at 0x48 on WireRtc"));
    return false;
  }
  Serial.println(F("[SOIL] ADS1115 ready (soil_moist_temp backend)"));
  haveSample = false;
  return true;
}

size_t count() {
  // 0: SOIL1_VWC legacy channel: moisture sensor volts
  // 1: SOIL1_TEMP
  // 2: SOIL2_VWC legacy channel: moisture sensor volts
  // 3: SOIL2_TEMP
  return 4;
}

const char* label(size_t index) {
  switch (index) {
    case 0: return "SOIL1_VWC";
    case 1: return "SOIL1_TEMP";
    case 2: return "SOIL2_VWC";
    case 3: return "SOIL2_TEMP";
    default: return "UNKNOWN";
  }
}

const char* type(size_t index) {
  switch (index) {
    case 0:
    case 2:
      return "SOIL_VWC";
    case 1:
    case 3:
      return "SOIL_TEMP";
    default:
      return "UNKNOWN";
  }
}

bool read(size_t index, float& outValue) {
  if (index >= count()) return false;

  sampleAdsIfNeeded();
  if (!haveSample) return false;

  switch (index) {
    case 0: outValue = lastMoist1V; break;  // SOIL1_VWC legacy channel: volts
    case 1: outValue = lastTemp1C;  break;  // SOIL1_TEMP
    case 2: outValue = lastMoist2V; break;  // SOIL2_VWC legacy channel: volts
    case 3: outValue = lastTemp2C;  break;  // SOIL2_TEMP
    default: return false;
  }
  return true;
}

} // namespace soil_moist_temp_backend
