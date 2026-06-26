#include <Arduino.h>

#include "sensors_aux_i2c.h"

#ifndef AUX_I2C_ENABLE_STUB_SLOTS
#define AUX_I2C_ENABLE_STUB_SLOTS 0
#endif

namespace {

bool g_initialized = false;
bool g_haveAux1 = false;
bool g_haveAux2 = false;
float g_aux1 = NAN;
float g_aux2 = NAN;

} // namespace

namespace aux_i2c_backend {

bool init() {
  if (g_initialized) return true;
  g_initialized = true;
  Serial.println(F("[AUX] AUX I2C backend stub ready (2 ports)"));
  return true;
}

size_t count() {
#if AUX_I2C_ENABLE_STUB_SLOTS
  return g_initialized ? 2 : 0;
#else
  return 0;
#endif
}

const char* label(size_t index) {
  switch (index) {
    case 0: return "AUX1_INPUT";
    case 1: return "AUX2_INPUT";
    default: return "UNKNOWN";
  }
}

const char* type(size_t index) {
  switch (index) {
    case 0:
    case 1:
      return "AUX";
    default:
      return "UNKNOWN";
  }
}

bool read(size_t index, float& outValue) {
  switch (index) {
    case 0:
      if (!g_haveAux1) return false;
      outValue = g_aux1;
      return true;
    case 1:
      if (!g_haveAux2) return false;
      outValue = g_aux2;
      return true;
    default:
      return false;
  }
}

void ingestAux1(float value) {
  g_aux1 = value;
  g_haveAux1 = true;
}

void ingestAux2(float value) {
  g_aux2 = value;
  g_haveAux2 = true;
}

} // namespace aux_i2c_backend
