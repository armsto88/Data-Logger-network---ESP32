// sensors_ds18b20.h
#pragma once
#include <Arduino.h>

namespace ds18b20_backend {

    // Discover sensors, set up OneWire/DallasTemperature, etc.
    bool   init();

    // How many DS18B20 sensors are available?
    size_t count();

    // Human-readable label (for CSV "label" column)
    const char* label(size_t index);   // e.g. "DS18B20_TEMP_1"

    // Generic type string (for CSV "sensor_type" column)
    const char* type(size_t index);    // e.g. "DS18B20"

    // Read a single DS18B20 by index; returns true on success.
    bool   read(size_t index, float &outValue);
}
