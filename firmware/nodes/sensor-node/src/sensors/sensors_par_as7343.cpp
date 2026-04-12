#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_AS7343.h>

#include "sensors_par_as7343.h"

extern TwoWire WireRtc;

namespace {

SfeAS7343ArdI2C g_par;
bool g_ready = false;
float g_lastPar = NAN;
uint32_t g_lastSampleMs = 0;
bool g_haveSample = false;

void sampleIfNeeded() {
  uint32_t now = millis();
  if (g_haveSample && (now - g_lastSampleMs) < 300) {
    return;
  }

  if (!g_par.readSpectraDataFromSensor()) {
    g_haveSample = false;
    Serial.println(F("[PAR] AS7343 read failed"));
    return;
  }

  // PAR proxy from visible channels. Replace with calibrated PAR conversion later.
  float red = (float)g_par.getRed();
  float green = (float)g_par.getGreen();
  float blue = (float)g_par.getBlue();
  g_lastPar = red + green + blue;

  g_lastSampleMs = now;
  g_haveSample = true;

  Serial.printf("[PAR] RED=%.1f GREEN=%.1f BLUE=%.1f PAR_PROXY=%.1f\n", red, green, blue, g_lastPar);
}

} // namespace

namespace par_as7343_backend {

bool init() {
  if (g_ready) return true;

  if (!g_par.begin(kAS7343Addr, WireRtc)) {
    Serial.println(F("[PAR] AS7343 not found on WireRtc"));
    return false;
  }

  if (!g_par.powerOn()) {
    Serial.println(F("[PAR] AS7343 powerOn failed"));
    return false;
  }

  if (!g_par.setAutoSmux(AUTOSMUX_18_CHANNELS)) {
    Serial.println(F("[PAR] AS7343 setAutoSmux failed"));
    return false;
  }

  if (!g_par.enableSpectralMeasurement()) {
    Serial.println(F("[PAR] AS7343 enableSpectralMeasurement failed"));
    return false;
  }

  g_ready = true;
  g_haveSample = false;
  Serial.println(F("[PAR] AS7343 ready"));
  return true;
}

size_t count() {
  return g_ready ? 1 : 0;
}

const char* label(size_t index) {
  if (index == 0) return "PAR";
  return "UNKNOWN";
}

const char* type(size_t index) {
  if (index == 0) return "PAR";
  return "UNKNOWN";
}

bool read(size_t index, float& outValue) {
  if (!g_ready || index != 0) return false;

  sampleIfNeeded();
  if (!g_haveSample) return false;

  outValue = g_lastPar;
  return true;
}

} // namespace par_as7343_backend
