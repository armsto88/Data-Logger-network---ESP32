#pragma once

#include <Arduino.h>

namespace sht41_backend {

bool init();
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

} // namespace sht41_backend
