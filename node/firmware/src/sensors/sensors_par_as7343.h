#pragma once

#include <Arduino.h>

namespace par_as7343_backend {

bool init();
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

} // namespace par_as7343_backend
