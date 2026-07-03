// sensors.h
#pragma once
#include <Arduino.h>

// Which backend produced a slot. Stored per-slot so readSensor() dispatches
// directly instead of inferring the backend from cumulative counts — that
// inference breaks the moment mask gating skips a slot, so slots are now
// self-describing.
enum SensorBackendId : uint8_t {
    SENSOR_BACKEND_SHT41 = 0,
    SENSOR_BACKEND_SPECTRAL,
    SENSOR_BACKEND_SOIL,
    SENSOR_BACKEND_ULTRASONIC_WIND,
    SENSOR_BACKEND_REED_WIND,
    SENSOR_BACKEND_AUX,
};

// One logical sensor "slot" on the node.
// label       = human-readable name for CSV, e.g. "DS18B20_TEMP_1"
// sensorType  = generic type/category, e.g. "DS18B20", "SOIL_MOISTURE", "AIR_TEMP"
// backend     = SensorBackendId that owns this slot
// backendIndex= index within that backend's own list
struct SensorSlot {
    uint16_t sensorId;
    const char* label;
    const char* sensorType;
    uint8_t backend;
    uint8_t backendIndex;
};

// Max number of sensors handled by this node firmware.
// Baseline standard profile uses 8 slots, with headroom for AUX expansion.
constexpr size_t MAX_SENSORS = 16;

// Global registry (defined in sensors.cpp)
extern SensorSlot g_sensors[MAX_SENSORS];
extern size_t     g_numSensors;

// Configured "expected" sensor mask (SNAP_PRESENT_* bits + NODE_SENSOR_MASK_VALID).
// Set from NVS before initSensors(). 0 = auto: register every detected backend
// (legacy behaviour). When NODE_SENSOR_MASK_VALID is set, passive sensors are
// only registered/read when their capability bit is present.
extern uint16_t g_expectedSensorMask;

// Called once in setup()
bool initSensors();

// Read a single sensor by index; returns true on success
bool readSensor(size_t index, float &outValue);

// V2 key-value snapshot support
#include "protocol.h"

// Build an array of V2 readings from the sensor registry.
// Iterates g_sensors[], reads each sensor via readSensor(), and emits
// {sensorId, value} pairs. Skips SENSOR_ID_UNKNOWN (0) and failed reads.
// Returns the number of readings written into `out`.
size_t buildReadingsArray(v2_reading_t* out, size_t maxCount);
