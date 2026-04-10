// sensors.h
#pragma once
#include <Arduino.h>

// One logical sensor "slot" on the node.
// label      = human-readable name for CSV, e.g. "DS18B20_TEMP_1"
// sensorType = generic type/category, e.g. "DS18B20", "SOIL_MOISTURE", "AIR_TEMP"
struct SensorSlot {
    const char* label;
    const char* sensorType;
};

// Max number of sensors handled by this node firmware
constexpr size_t MAX_SENSORS = 8;

// Global registry (defined in sensors.cpp)
extern SensorSlot g_sensors[MAX_SENSORS];
extern size_t     g_numSensors;

// Called once in setup()
bool initSensors();

// Read a single sensor by index; returns true on success
bool readSensor(size_t index, float &outValue);
