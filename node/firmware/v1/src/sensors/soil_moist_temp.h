// include/soil_moist_temp.h
#pragma once
#include <Arduino.h>

namespace soil_moist_temp_backend {

    // Called once in initSensors()
    bool   init();

    // How many logical soil sensors we expose
    // (2x VWC + 2x temp = 4)
    size_t count();

    // Label for CSV / debug, e.g. "SOIL1_VWC"
    const char* label(size_t index);

    // Generic type, e.g. "SOIL_VWC" or "SOIL_TEMP"
    const char* type(size_t index);

    // Read sensor by index; returns true on success
    bool   read(size_t index, float &outValue);
}
