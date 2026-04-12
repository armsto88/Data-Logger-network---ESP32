#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>

#include "sensors_sht41.h"

extern TwoWire WireRtc;

namespace {

Adafruit_SHT4x g_sht4;
bool g_ready = false;
bool g_haveSample = false;
uint32_t g_lastSampleMs = 0;
float g_lastTempC = NAN;
float g_lastRh = NAN;

void sampleIfNeeded() {
  uint32_t now = millis();
  if (g_haveSample && (now - g_lastSampleMs) < 250) {
    return;
  }

  sensors_event_t humidity;
  sensors_event_t temp;
  if (!g_sht4.getEvent(&humidity, &temp)) {
    g_haveSample = false;
    Serial.println(F("[SHT41] read failed"));
    return;
  }

  g_lastTempC = temp.temperature;
  g_lastRh = humidity.relative_humidity;
  g_lastSampleMs = now;
  g_haveSample = true;

  Serial.printf("[SHT41] AIR_TEMP=%.2f C AIR_RH=%.2f %%\n", g_lastTempC, g_lastRh);
}

} // namespace

namespace sht41_backend {

bool init() {
  if (g_ready) return true;

  if (!g_sht4.begin(&WireRtc)) {
    Serial.println(F("[SHT41] not found on WireRtc"));
    return false;
  }

  g_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  g_sht4.setHeater(SHT4X_NO_HEATER);

  g_ready = true;
  g_haveSample = false;
  Serial.println(F("[SHT41] ready"));
  return true;
}

size_t count() {
  return g_ready ? 2 : 0;
}

const char* label(size_t index) {
  switch (index) {
    case 0: return "AIR_TEMP";
    case 1: return "AIR_RH";
    default: return "UNKNOWN";
  }
}

const char* type(size_t index) {
  switch (index) {
    case 0: return "AIR_TEMP";
    case 1: return "HUMIDITY";
    default: return "UNKNOWN";
  }
}

bool read(size_t index, float& outValue) {
  if (!g_ready || index >= count()) return false;

  sampleIfNeeded();
  if (!g_haveSample) return false;

  if (index == 0) {
    outValue = g_lastTempC;
    return true;
  }

  if (index == 1) {
    outValue = g_lastRh;
    return true;
  }

  return false;
}

} // namespace sht41_backend
