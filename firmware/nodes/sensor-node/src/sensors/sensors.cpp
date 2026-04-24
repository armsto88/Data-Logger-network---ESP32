// src/sensors.cpp
#include <Arduino.h>
#include "sensors.h"
#include "protocol.h"
#include "sensors_sht41.h"
#include "sensors_par_as7343.h"
#include "soil_moist_temp.h"
#include "sensors_ultrasonic_wind.h"
#include "sensors_aux_i2c.h"

namespace {

uint16_t resolveSensorId(const char* label, const char* type) {
  if (label) {
    if (strcmp(label, "AIR_TEMP") == 0) return SENSOR_ID_AIR_TEMP;
    if (strcmp(label, "AIR_RH") == 0) return SENSOR_ID_AIR_RH;
    if (strcmp(label, "PAR") == 0) return SENSOR_ID_UNKNOWN;
    if (strcmp(label, "WIND_SPEED") == 0) return SENSOR_ID_WIND_SPEED;
    if (strcmp(label, "SOIL1_VWC") == 0) return SENSOR_ID_SOIL1_VWC;
    if (strcmp(label, "SOIL2_VWC") == 0) return SENSOR_ID_SOIL2_VWC;
    if (strcmp(label, "SOIL1_TEMP") == 0) return SENSOR_ID_SOIL1_TEMP;
    if (strcmp(label, "SOIL2_TEMP") == 0) return SENSOR_ID_SOIL2_TEMP;
    if (strcmp(label, "AUX1_INPUT") == 0) return SENSOR_ID_AUX1;
    if (strcmp(label, "AUX2_INPUT") == 0) return SENSOR_ID_AUX2;
  }

  if (type) {
    if (strcmp(type, SENSOR_TYPE_AIR_TEMP) == 0) return SENSOR_ID_AIR_TEMP;
    if (strcmp(type, SENSOR_TYPE_HUMIDITY) == 0) return SENSOR_ID_AIR_RH;
    if (strcmp(type, SENSOR_TYPE_PAR) == 0) return SENSOR_ID_UNKNOWN;
    if (strcmp(type, SENSOR_TYPE_WIND) == 0) return SENSOR_ID_WIND_SPEED;
    if (strcmp(type, SENSOR_TYPE_SOIL_VWC) == 0) return SENSOR_ID_SOIL1_VWC;
    if (strcmp(type, SENSOR_TYPE_SOIL_TEMP) == 0) return SENSOR_ID_SOIL1_TEMP;
    if (strcmp(type, SENSOR_TYPE_AUX) == 0) return SENSOR_ID_AUX1;
  }

  return SENSOR_ID_UNKNOWN;
}

} // namespace

// Define the global registry
SensorSlot g_sensors[MAX_SENSORS];
size_t     g_numSensors = 0;

bool initSensors() {
  Serial.println(F("[SENS] initSensors()"));

  g_numSensors = 0;

  // ----- SHT41 backend -----
  if (sht41_backend::init()) {
    size_t shtCount = sht41_backend::count();
    Serial.printf("[SENS] SHT41 backend reports %u sensor(s)\n",
                  (unsigned)shtCount);

    for (size_t i = 0; i < shtCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = sht41_backend::label(i);
      g_sensors[g_numSensors].sensorType = sht41_backend::type(i);
      g_sensors[g_numSensors].sensorId   = resolveSensorId(g_sensors[g_numSensors].label,
                           g_sensors[g_numSensors].sensorType);
      Serial.printf("[SENS] Slot %u -> id=%u label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
            (unsigned)g_sensors[g_numSensors].sensorId,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] SHT41 backend init FAILED"));
  }

  // ----- PAR backend (AS7343) -----
  if (par_as7343_backend::init()) {
    size_t parCount = par_as7343_backend::count();
    Serial.printf("[SENS] PAR backend reports %u sensor(s)\n",
                  (unsigned)parCount);

    for (size_t i = 0; i < parCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = par_as7343_backend::label(i);
      g_sensors[g_numSensors].sensorType = par_as7343_backend::type(i);
      g_sensors[g_numSensors].sensorId   = resolveSensorId(g_sensors[g_numSensors].label,
                           g_sensors[g_numSensors].sensorType);
      Serial.printf("[SENS] Slot %u -> id=%u label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
            (unsigned)g_sensors[g_numSensors].sensorId,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] PAR backend init FAILED"));
  }

  // ----- Soil moisture + temp backend (ADS1115) -----
  if (soil_moist_temp_backend::init()) {
    size_t soilCount = soil_moist_temp_backend::count();
    Serial.printf("[SENS] soil_moist_temp backend reports %u sensor(s)\n",
                  (unsigned)soilCount);

    for (size_t i = 0; i < soilCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = soil_moist_temp_backend::label(i);
      g_sensors[g_numSensors].sensorType = soil_moist_temp_backend::type(i);
      g_sensors[g_numSensors].sensorId   = resolveSensorId(g_sensors[g_numSensors].label,
                           g_sensors[g_numSensors].sensorType);
      Serial.printf("[SENS] Slot %u -> id=%u label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
            (unsigned)g_sensors[g_numSensors].sensorId,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] soil_moist_temp backend init FAILED"));
  }

  // ----- Ultrasonic wind backend (stub) -----
  if (ultrasonic_wind_backend::init()) {
    size_t windCount = ultrasonic_wind_backend::count();
    Serial.printf("[SENS] ultrasonic_wind backend reports %u sensor(s)\n",
                  (unsigned)windCount);

    for (size_t i = 0; i < windCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = ultrasonic_wind_backend::label(i);
      g_sensors[g_numSensors].sensorType = ultrasonic_wind_backend::type(i);
      g_sensors[g_numSensors].sensorId   = resolveSensorId(g_sensors[g_numSensors].label,
                           g_sensors[g_numSensors].sensorType);
      Serial.printf("[SENS] Slot %u -> label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] ultrasonic_wind backend init FAILED"));
  }

  // ----- AUX I2C backend (stub, 2 ports) -----
  if (aux_i2c_backend::init()) {
    size_t auxCount = aux_i2c_backend::count();
    Serial.printf("[SENS] aux_i2c backend reports %u sensor(s)\n",
                  (unsigned)auxCount);

    for (size_t i = 0; i < auxCount && g_numSensors < MAX_SENSORS; ++i) {
      g_sensors[g_numSensors].label      = aux_i2c_backend::label(i);
      g_sensors[g_numSensors].sensorType = aux_i2c_backend::type(i);
      g_sensors[g_numSensors].sensorId   = resolveSensorId(g_sensors[g_numSensors].label,
                           g_sensors[g_numSensors].sensorType);
      Serial.printf("[SENS] Slot %u -> label='%s', type='%s'\n",
                    (unsigned)g_numSensors,
                    g_sensors[g_numSensors].label,
                    g_sensors[g_numSensors].sensorType);
      ++g_numSensors;
    }
  } else {
    Serial.println(F("[SENS] aux_i2c backend init FAILED"));
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

  // SHT41 slots are first
  size_t shtCount  = sht41_backend::count();
  size_t parCount  = par_as7343_backend::count();
  size_t soilCount = soil_moist_temp_backend::count();
  size_t windCount = ultrasonic_wind_backend::count();
  size_t auxCount  = aux_i2c_backend::count();

  if (index < shtCount) {
    return sht41_backend::read(index, outValue);
  }

  // PAR backend after SHT41
  size_t parIndex = index - shtCount;
  if (parIndex < parCount) {
    return par_as7343_backend::read(parIndex, outValue);
  }

  // Soil backend comes after SHT41 and PAR
  size_t soilIndex = index - shtCount - parCount;
  if (soilIndex < soilCount) {
    return soil_moist_temp_backend::read(soilIndex, outValue);
  }

  // Ultrasonic backend after SHT41, PAR, and soil
  size_t windIndex = index - shtCount - parCount - soilCount;
  if (windIndex < windCount) {
    return ultrasonic_wind_backend::read(windIndex, outValue);
  }

  // AUX backend after SHT41, PAR, soil, and wind
  size_t auxIndex = index - shtCount - parCount - soilCount - windCount;
  if (auxIndex < auxCount) {
    return aux_i2c_backend::read(auxIndex, outValue);
  }

  // Shouldn’t really happen, but guard anyway
  Serial.printf("[SENS] readSensor: index %u does not map to any backend\n",
                (unsigned)index);
  return false;
}
