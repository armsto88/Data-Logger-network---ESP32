#pragma once

#include <Arduino.h>

// Stub backend for future ultrasonic wind integration.
// Default behavior is non-intrusive (no exported sensor slots) until enabled.
namespace ultrasonic_wind_backend {

bool init();
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

// Optional ingestion hook for future ISR/driver integration.
void ingestWindMps(float windMps);

} // namespace ultrasonic_wind_backend
