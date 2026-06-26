#include <Arduino.h>

#include "sensors_ultrasonic_wind.h"

#ifndef ULTRASONIC_WIND_ENABLE_STUB_SLOT
#define ULTRASONIC_WIND_ENABLE_STUB_SLOT 0
#endif

namespace {

bool g_initialized = false;
bool g_haveSample = false;
float g_lastWindMps = NAN;

} // namespace

namespace ultrasonic_wind_backend {

bool init() {
  if (g_initialized) return true;
  g_initialized = true;
  Serial.println(F("[WIND] Ultrasonic backend stub ready"));
  return true;
}

size_t count() {
#if ULTRASONIC_WIND_ENABLE_STUB_SLOT
  return g_initialized ? 1 : 0;
#else
  return 0;
#endif
}

const char* label(size_t index) {
  if (index == 0) return "WIND_SPEED";
  return "UNKNOWN";
}

const char* type(size_t index) {
  if (index == 0) return "WIND";
  return "UNKNOWN";
}

bool read(size_t index, float& outValue) {
  if (index != 0) return false;
  if (!g_haveSample) return false;
  outValue = g_lastWindMps;
  return true;
}

void ingestWindMps(float windMps) {
  g_lastWindMps = windMps;
  g_haveSample = true;
}

} // namespace ultrasonic_wind_backend
