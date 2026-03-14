// src/sensors.cpp
#include <Arduino.h>
#include "sensors.h"
#include "sensors_ds18b20.h"
#include "soil_moist_temp.h"

// Define the global registry
SensorSlot g_sensors[MAX_SENSORS];
size_t     g_numSensors = 0;

bool initSensors() {
  Serial.println(F("[SENS] initSensors()"));

  g_numSensors = 0;

  // ----- DS18B20 backend -----
  if (ds18b20_backend::init()) {
    size_t dsCount = ds18b20_backend::count();
    Serial.printf("[SENS] DS18B20 backend reports %u sensor(s)\n",
                  (unsigned)dsCount);

    for (size_t i = 0; i < dsCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = ds18b20_backend::label(i);
      g_sensors[g_numSensors].sensorType = ds18b20_backend::type(i);
      Serial.printf("[SENS] Slot %u → label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] DS18B20 backend init FAILED or 0 devices"));
  }

  // ----- Soil moisture + temp backend (ADS1115) -----
  if (soil_moist_temp_backend::init()) {
    size_t soilCount = soil_moist_temp_backend::count();
    Serial.printf("[SENS] soil_moist_temp backend reports %u sensor(s)\n",
                  (unsigned)soilCount);

    for (size_t i = 0; i < soilCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = soil_moist_temp_backend::label(i);
      g_sensors[g_numSensors].sensorType = soil_moist_temp_backend::type(i);
      Serial.printf("[SENS] Slot %u → label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] soil_moist_temp backend init FAILED"));
  }

  Serial.printf("[SENS] ✅ Total registered sensors: %u\n",
                (unsigned)g_numSensors);

  return (g_numSensors > 0);
}

bool readSensor(size_t index, float &outValue) {
  if (index >= g_numSensors) {
    Serial.printf("[SENS] readSensor: index %u out of range (g_numSensors=%u)\n",
                  (unsigned)index, (unsigned)g_numSensors);
    return false;
  }

  // DS18B20 sensors are first
  size_t dsCount   = ds18b20_backend::count();
  size_t soilCount = soil_moist_temp_backend::count();

  if (index < dsCount) {
    // Directly slot → DS18B20 index
    return ds18b20_backend::read(index, outValue);
  }

  // Soil backend comes after DS18B20
  size_t soilIndex = index - dsCount;
  if (soilIndex < soilCount) {
    return soil_moist_temp_backend::read(soilIndex, outValue);
  }

  // Shouldn’t really happen, but guard anyway
  Serial.printf("[SENS] readSensor: index %u does not map to any backend\n",
                (unsigned)index);
  return false;
}
