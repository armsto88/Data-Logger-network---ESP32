// sensors_ds18b20.cpp
#include "sensors_ds18b20.h"

#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef DS18B20_PIN
#define DS18B20_PIN 10   // ESP32-C3 DS18B20 data pin – adjust if needed
#endif

// ---- OneWire / Dallas objects ----
static OneWire oneWire(DS18B20_PIN);
static DallasTemperature dallas(&oneWire);

// We keep DS18B20-specific state here
static const size_t MAX_DS_SENSORS = 8;
static size_t       s_count = 0;
static String       s_labels[MAX_DS_SENSORS];  // own label storage

namespace ds18b20_backend {

bool init() {
    Serial.println(F("[DS18B20] init() – bus scan"));
    dallas.begin();

    int deviceCount = dallas.getDeviceCount();
    Serial.printf("[DS18B20] GPIO %d → %d device(s)\n",
                  DS18B20_PIN, deviceCount);

    s_count = 0;

    for (int i = 0; i < deviceCount && s_count < MAX_DS_SENSORS; ++i) {
        // DS18B20_TEMP_1, DS18B20_TEMP_2, ...
        String lbl = String("DS18B20_TEMP_") + String(s_count + 1);
        s_labels[s_count] = lbl;
        s_count++;
    }

    if (s_count == 0) {
        Serial.println(F("[DS18B20] ⚠️ No DS18B20 devices found"));
        return false;
    }

    Serial.printf("[DS18B20] ✅ Registered %u DS18B20 sensor(s)\n",
                  (unsigned)s_count);
    return true;
}

size_t count() {
    return s_count;
}

const char* label(size_t index) {
    if (index >= s_count) return "DS18B20_UNKNOWN";
    return s_labels[index].c_str();
}

const char* type(size_t /*index*/) {
    // You can generalise this later if needed
    return "DS18B20";
}

bool read(size_t index, float &outValue) {
    if (index >= s_count) {
        Serial.printf("[DS18B20] read: index %u out of range (count=%u)\n",
                      (unsigned)index, (unsigned)s_count);
        return false;
    }

    dallas.requestTemperatures();
    float tC = dallas.getTempCByIndex(index);

    if (tC == DEVICE_DISCONNECTED_C) {
        Serial.printf("[DS18B20] index %u → DEVICE_DISCONNECTED_C\n",
                      (unsigned)index);
        return false;
    }

    outValue = tC;

    Serial.printf("[DS18B20] %s = %.2f °C (slot %u)\n",
                  label(index),
                  outValue,
                  (unsigned)index);
    return true;
}

} // namespace ds18b20_backend
