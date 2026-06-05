#pragma once

#include <Arduino.h>

// Stub backend for two AUX I2C ports.
// Default behavior is non-intrusive (no exported sensor slots) until enabled.
namespace aux_i2c_backend {

bool init();
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

// Optional ingestion hooks for future attached AUX sensors.
void ingestAux1(float value);
void ingestAux2(float value);

} // namespace aux_i2c_backend
