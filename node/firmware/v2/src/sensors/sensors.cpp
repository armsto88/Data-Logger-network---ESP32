// src/sensors.cpp
#include <Arduino.h>
#include <Wire.h>
#include "sensors.h"
#include "protocol.h"
#include "sensors_sht41.h"
#include "sensors_par_as7343.h"
#include "soil_moist_temp.h"
#include "sensors_ultrasonic_wind.h"
#include "sensors_reed_wind.h"
#include "sensors_aux_i2c.h"

extern TwoWire WireRtc;

namespace {
// Per-backend I2C read budgets (ms). If a backend exceeds its budget, a
// warning is logged so a hung sensor is visible without blocking the wake
// cycle. These are post-hoc safety nets — WireRtc.setTimeOut() handles the
// per-transaction blocking case.
constexpr uint32_t SHT41_READ_BUDGET_MS   = 2000UL;
constexpr uint32_t SPECTRAL_READ_BUDGET_MS = 5000UL;
constexpr uint32_t SOIL_READ_BUDGET_MS    = 3000UL;
} // namespace

namespace {

uint16_t resolveSensorId(const char* label, const char* type) {
  if (label) {
    if (strcmp(label, "AIR_TEMP") == 0) return SENSOR_ID_AIR_TEMP;
    if (strcmp(label, "AIR_RH") == 0) return SENSOR_ID_AIR_RH;
    if (strcmp(label, "SPECTRAL_415") == 0) return SENSOR_ID_SPECTRAL_415;
    if (strcmp(label, "SPECTRAL_445") == 0) return SENSOR_ID_SPECTRAL_445;
    if (strcmp(label, "SPECTRAL_480") == 0) return SENSOR_ID_SPECTRAL_480;
    if (strcmp(label, "SPECTRAL_515") == 0) return SENSOR_ID_SPECTRAL_515;
    if (strcmp(label, "SPECTRAL_555") == 0) return SENSOR_ID_SPECTRAL_555;
    if (strcmp(label, "SPECTRAL_590") == 0) return SENSOR_ID_SPECTRAL_590;
    if (strcmp(label, "SPECTRAL_630") == 0) return SENSOR_ID_SPECTRAL_630;
    if (strcmp(label, "SPECTRAL_680") == 0) return SENSOR_ID_SPECTRAL_680;
    if (strcmp(label, "SPECTRAL_CLEAR") == 0) return SENSOR_ID_SPECTRAL_CLEAR;
    if (strcmp(label, "SPECTRAL_NIR") == 0) return SENSOR_ID_SPECTRAL_NIR;
    if (strcmp(label, "SPECTRAL_GAIN") == 0) return SENSOR_ID_SPECTRAL_GAIN;
    if (strcmp(label, "SPECTRAL_ATIME_MS") == 0) return SENSOR_ID_SPECTRAL_ATIME;
    if (strcmp(label, "SPECTRAL_SAT") == 0) return SENSOR_ID_SPECTRAL_SAT;
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
uint16_t   g_expectedSensorMask = 0;

namespace {

// Is this slot permitted by the configured mask? Auto mode (mask not valid)
// registers everything, preserving legacy behaviour. Self-identifying sensors
// are never gated — only the passive ones the node cannot probe for on the bus.
// The two wind backends share the WIND capability bit but are selected
// independently: reed cup on SNAP_PRESENT_WIND, ultrasonic on its own selector.
bool sensorAllowedByMask(uint8_t backend, uint16_t sensorId) {
  if (!(g_expectedSensorMask & NODE_SENSOR_MASK_VALID)) return true;
  if (backend == SENSOR_BACKEND_REED_WIND)
    return (g_expectedSensorMask & SNAP_PRESENT_WIND) != 0;
  if (backend == SENSOR_BACKEND_ULTRASONIC_WIND)
    return (g_expectedSensorMask & NODE_SENSOR_CFG_WIND_ULTRASONIC) != 0;
  const uint16_t bit = snapPresentBitForSensorId(sensorId);
  if ((bit & NODE_SENSOR_PASSIVE_BITS) == 0) return true;
  return (g_expectedSensorMask & bit) != 0;
}

// Register one backend slot, honouring the configured mask. Records the owning
// backend + backend-local index so readSensor() can dispatch directly (mask
// gating means g_sensors[] no longer mirrors each backend's full count).
bool commitSensorSlot(uint8_t backend, size_t backendIndex,
                      const char* label, const char* type) {
  if (g_numSensors >= MAX_SENSORS) return false;
  const uint16_t id = resolveSensorId(label, type);
  if (!sensorAllowedByMask(backend, id)) {
    Serial.printf("[SENS] gated id=%u label='%s' (mask 0x%04X)\n",
                  (unsigned)id, label ? label : "?", (unsigned)g_expectedSensorMask);
    return false;
  }
  SensorSlot& slot  = g_sensors[g_numSensors];
  slot.sensorId     = id;
  slot.label        = label;
  slot.sensorType   = type;
  slot.backend      = backend;
  slot.backendIndex = (uint8_t)backendIndex;
  Serial.printf("[SENS] Slot %u -> id=%u label='%s' type='%s' backend=%u\n",
                (unsigned)g_numSensors, (unsigned)id,
                label ? label : "?", type ? type : "?", (unsigned)backend);
  ++g_numSensors;
  return true;
}

} // namespace

bool initSensors() {
  Serial.println(F("[SENS] initSensors()"));

  // Set a 2-second per-I2C-transaction timeout on the shared bus so a hung
  // sensor returns an I2C error instead of blocking the wake cycle forever.
  // Individual sensor backends already check endTransmission()/requestFrom()
  // return values, so a timeout surfaces as a normal read failure (NaN).
  WireRtc.setTimeOut(2000);
  Serial.println(F("[SENS] WireRtc I2C timeout set to 2000ms"));

  if (g_expectedSensorMask & NODE_SENSOR_MASK_VALID) {
    Serial.printf("[SENS] configured sensor mask = 0x%04X (passive sensors gated)\n",
                  (unsigned)g_expectedSensorMask);
  } else {
    Serial.println(F("[SENS] no configured sensor mask -> auto-detect all backends"));
  }

  g_numSensors = 0;

  // ----- SHT41 backend (self-identifying I2C: always auto-detected) -----
  if (sht41_backend::init()) {
    size_t shtCount = sht41_backend::count();
    Serial.printf("[SENS] SHT41 backend reports %u sensor(s)\n", (unsigned)shtCount);
    for (size_t i = 0; i < shtCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_SHT41, i,
                       sht41_backend::label(i), sht41_backend::type(i));
  } else {
    Serial.println(F("[SENS] SHT41 backend init FAILED"));
  }

  // ----- Spectral backend (AS734x family, self-identifying I2C) -----
  if (par_as7343_backend::init()) {
    size_t parCount = par_as7343_backend::count();
    Serial.printf("[SENS] spectral backend reports %u sensor(s)\n", (unsigned)parCount);
    for (size_t i = 0; i < parCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_SPECTRAL, i,
                       par_as7343_backend::label(i), par_as7343_backend::type(i));
  } else {
    Serial.println(F("[SENS] PAR backend init FAILED"));
  }

  // ----- Soil moisture + temp backend (ADS1115; passive, mask-gated) -----
  if (soil_moist_temp_backend::init()) {
    size_t soilCount = soil_moist_temp_backend::count();
    Serial.printf("[SENS] soil_moist_temp backend reports %u sensor(s)\n", (unsigned)soilCount);
    for (size_t i = 0; i < soilCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_SOIL, i,
                       soil_moist_temp_backend::label(i), soil_moist_temp_backend::type(i));
  } else {
    Serial.println(F("[SENS] soil_moist_temp backend init FAILED"));
  }

  // ----- Ultrasonic wind backend (passive, mask-gated) -----
  if (ultrasonic_wind_backend::init()) {
    size_t windCount = ultrasonic_wind_backend::count();
    Serial.printf("[SENS] ultrasonic_wind backend reports %u sensor(s)\n", (unsigned)windCount);
    for (size_t i = 0; i < windCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_ULTRASONIC_WIND, i,
                       ultrasonic_wind_backend::label(i), ultrasonic_wind_backend::type(i));
  } else {
    Serial.println(F("[SENS] ultrasonic_wind backend init FAILED"));
  }

  // ----- Reed cup anemometer backend (WH-SP-WS01; passive, mask-gated).
  //        Selection is now runtime (the WIND capability bit) rather than the
  //        old compile-time NODE_HAS_REED_WIND flag. -----
  if (reed_wind_backend::init()) {
    size_t reedCount = reed_wind_backend::count();
    if (reedCount > 0)
      Serial.printf("[SENS] reed_wind backend reports %u sensor(s)\n", (unsigned)reedCount);
    for (size_t i = 0; i < reedCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_REED_WIND, i,
                       reed_wind_backend::label(i), reed_wind_backend::type(i));
  }

  // ----- AUX I2C backend (passive, mask-gated) -----
  if (aux_i2c_backend::init()) {
    size_t auxCount = aux_i2c_backend::count();
    Serial.printf("[SENS] aux_i2c backend reports %u sensor(s)\n", (unsigned)auxCount);
    for (size_t i = 0; i < auxCount; ++i)
      commitSensorSlot(SENSOR_BACKEND_AUX, i,
                       aux_i2c_backend::label(i), aux_i2c_backend::type(i));
  } else {
    Serial.println(F("[SENS] aux_i2c backend init FAILED"));
  }

  Serial.printf("[SENS] ✅ Total registered sensors: %u\n", (unsigned)g_numSensors);

  return (g_numSensors > 0);
}

bool readSensor(size_t index, float &outValue) {
  if (index >= g_numSensors) {
    Serial.printf("[SENS] readSensor: index %u out of range (g_numSensors=%u)\n",
                  (unsigned)index, (unsigned)g_numSensors);
    return false;
  }

  const SensorSlot& s = g_sensors[index];
  switch (s.backend) {
    case SENSOR_BACKEND_SHT41: {
      uint32_t t0 = millis();
      bool ok = sht41_backend::read(s.backendIndex, outValue);
      uint32_t dt = millis() - t0;
      if (dt > SHT41_READ_BUDGET_MS) {
        Serial.printf("[SENS] WARNING: SHT41 read took %lums (budget %lums)\n",
                      (unsigned long)dt, (unsigned long)SHT41_READ_BUDGET_MS);
      }
      return ok;
    }
    case SENSOR_BACKEND_SPECTRAL: {
      uint32_t t0 = millis();
      bool ok = par_as7343_backend::read(s.backendIndex, outValue);
      uint32_t dt = millis() - t0;
      if (dt > SPECTRAL_READ_BUDGET_MS) {
        Serial.printf("[SENS] WARNING: Spectral read took %lums (budget %lums)\n",
                      (unsigned long)dt, (unsigned long)SPECTRAL_READ_BUDGET_MS);
      }
      return ok;
    }
    case SENSOR_BACKEND_SOIL: {
      uint32_t t0 = millis();
      bool ok = soil_moist_temp_backend::read(s.backendIndex, outValue);
      uint32_t dt = millis() - t0;
      if (dt > SOIL_READ_BUDGET_MS) {
        Serial.printf("[SENS] WARNING: Soil read took %lums (budget %lums)\n",
                      (unsigned long)dt, (unsigned long)SOIL_READ_BUDGET_MS);
      }
      return ok;
    }
    case SENSOR_BACKEND_ULTRASONIC_WIND:
      return ultrasonic_wind_backend::read(s.backendIndex, outValue);
    case SENSOR_BACKEND_REED_WIND:
      return reed_wind_backend::read(s.backendIndex, outValue);
    case SENSOR_BACKEND_AUX:
      return aux_i2c_backend::read(s.backendIndex, outValue);
    default:
      Serial.printf("[SENS] readSensor: slot %u has unknown backend %u\n",
                    (unsigned)index, (unsigned)s.backend);
      return false;
  }
}

// V2 key-value readings from sensor registry.
// Reads each registered sensor via readSensor() and emits {sensorId, value}
// pairs. Skips SENSOR_ID_UNKNOWN (0) and failed reads (value left as NaN).
size_t buildReadingsArray(v2_reading_t* out, size_t maxCount) {
  size_t count = 0;
  for (size_t i = 0; i < g_numSensors && count < maxCount; ++i) {
    if (g_sensors[i].sensorId == SENSOR_ID_UNKNOWN) continue;

    float value = NAN;
    // Emit a reading only on a successful, finite read so the mothership's
    // sensorPresent bit means "reported successfully this cycle". A failed read
    // is omitted — the mothership CSV still renders "nan" for the absent channel,
    // and a configured-but-absent sensor surfaces as a fault instead of a silent
    // NaN. (Skipping also keeps a broken sensor's channel out of the snapshot.)
    if (!readSensor(i, value) || isnan(value)) {
      Serial.printf("[SENS] buildReadingsArray: read failed/NaN for slot %u (id=%u) — omitted\n",
                    (unsigned)i, (unsigned)g_sensors[i].sensorId);
      continue;
    }

    out[count].sensorId = g_sensors[i].sensorId;
    out[count].value    = value;
    ++count;
  }

  // Spectral housekeeping. The AS7341 is a single physical sensor; its Clear,
  // NIR, applied gain, integration time, and saturation flag describe that one
  // spectral measurement rather than being independent channels. They are
  // appended here as extra readings instead of occupying registry slots (which
  // would let one sensor consume 13 of MAX_SENSORS). Emitted only when a
  // spectral sample succeeded this cycle; getMetadata() reads that exact cached
  // exposure and never starts a second acquisition.
  if (par_as7343_backend::metadataAvailable()) {
    par_as7343_backend::SpectralMetadata md = par_as7343_backend::getMetadata();
    const bool finite = isfinite(md.clear) && isfinite(md.nir) &&
                        isfinite(md.gain) && isfinite(md.integrationMs);
    if (!md.valid) {
      Serial.println(F("[SENS-SPEC] metadata unavailable after spectral read"));
    } else if (!finite) {
      Serial.printf("[SENS-SPEC] metadata non-finite: clear=%.3f nir=%.3f gain=%.3f tint=%.3f\n",
                    md.clear, md.nir, md.gain, md.integrationMs);
    } else if ((maxCount - count) < SPECTRAL_METADATA_READING_COUNT) {
      // Append all five or none. A partial metadata set cannot safely qualify
      // the exposure and would turn capacity pressure into misleading rows.
      Serial.printf("[SENS-SPEC] no snapshot capacity: used=%u max=%u need=%u; metadata omitted\n",
                    static_cast<unsigned>(count), static_cast<unsigned>(maxCount),
                    static_cast<unsigned>(SPECTRAL_METADATA_READING_COUNT));
    } else {
      const struct { uint16_t id; float val; } extras[] = {
        { SENSOR_ID_SPECTRAL_CLEAR, md.clear },
        { SENSOR_ID_SPECTRAL_NIR,   md.nir },
        { SENSOR_ID_SPECTRAL_GAIN,  md.gain },
        { SENSOR_ID_SPECTRAL_ATIME, md.integrationMs },
        { SENSOR_ID_SPECTRAL_SAT,   (float)md.saturated },
      };
      for (const auto& e : extras) {
        out[count].sensorId = e.id;
        out[count].value    = e.val;
        ++count;
      }
      Serial.printf("[SENS-SPEC] appended ids=1109-1113 clear=%.3f nir=%.3f gain=%.1f "
                    "integration_ms=%.3f saturated=%u total=%u/%u\n",
                    md.clear, md.nir, md.gain, md.integrationMs,
                    static_cast<unsigned>(md.saturated),
                    static_cast<unsigned>(count), static_cast<unsigned>(maxCount));
    }
  }

  return count;
}
