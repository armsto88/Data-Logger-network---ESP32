// bringup_ads1015_soil.cpp
//
// Validates ADS1015IDGSR ADC and CWT TH-A soil moisture+temperature sensor.
// Prints raw mV for all 4 channels, plus interpreted soil values for the
// sensor connected on A0 (moisture) and A2 (temperature).
//
// Hardware:
//   I2C:    SDA=18, SCL=19
//   ADS1015 at 0x48 on main I2C bus (no mux)
//   CWT TH-A sensor ch 1 connected:
//     moisture output → A0
//     temperature output → A2
//   (A1, A3 printed as raw mV — may be floating/unconnected)
//
// Gain: GAIN_TWO_THIRDS (±6.144 V full scale) to handle 0–5 V sensor outputs.
// Important: ensure sensor supply voltage does not exceed ADS VDD + 0.3 V.
//
// CWT TH-A linear conversion (0–5 V range):
//   moisture (%) = (100 / 5) × sensorV = 20 × V
//   temperature (°C) = (120 / 5) × sensorV − 40 = 24 × V − 40
//
// Build: pio run -e esp32wroom-ads1015-soil -t upload

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif
#ifndef ADS_ADDR
#define ADS_ADDR 0x48
#endif
#ifndef SAMPLE_AVG
#define SAMPLE_AVG 8
#endif

// CWT TH-A 0–5 V linear ranges
static constexpr float CWT_VOLT_FS    = 5.0f;
static constexpr float CWT_MOIST_MAX  = 100.0f;  // %
static constexpr float CWT_TEMP_MIN_C = -40.0f;
static constexpr float CWT_TEMP_MAX_C =  80.0f;

static Adafruit_ADS1015 g_ads;
static bool g_ads_ok = false;

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float cwt_moisture(float sensorV) {
    return clampf((CWT_MOIST_MAX / CWT_VOLT_FS) * sensorV, 0.0f, CWT_MOIST_MAX);
}

static inline float cwt_temp(float sensorV) {
    float t = ((CWT_TEMP_MAX_C - CWT_TEMP_MIN_C) / CWT_VOLT_FS) * sensorV + CWT_TEMP_MIN_C;
    return clampf(t, CWT_TEMP_MIN_C, CWT_TEMP_MAX_C);
}

// Average N single-ended reads on a channel; return voltage via Adafruit computeVolts()
static float read_avg_volts(uint8_t ch) {
    long sum = 0;
    for (uint8_t i = 0; i < SAMPLE_AVG; i++) {
        sum += g_ads.readADC_SingleEnded(ch);
        delay(2);
    }
    int16_t avg = (int16_t)(sum / SAMPLE_AVG);
    return g_ads.computeVolts(avg);
}

void setup() {
    Serial.begin(115200);
    delay(800);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

    Serial.println(F("\n=== ADS1015 + CWT TH-A soil bringup ==="));
    Serial.printf("I2C SDA=%d SCL=%d  ADS1015=0x%02X  avg=%d samples\n",
                  I2C_SDA_PIN, I2C_SCL_PIN, ADS_ADDR, SAMPLE_AVG);

    if (!g_ads.begin(ADS_ADDR, &Wire)) {
        Serial.println(F("ADS1015 NOT FOUND — check wiring/address/power. Halting."));
        while (true) delay(1000);
    }

    // GAIN_TWO_THIRDS = ±6.144 V full scale → handles 0–5 V sensor outputs.
    // ADS1015 inputs must not exceed VDD+0.3V — ensure sensor VDD ≤ ADS VDD.
    g_ads.setGain(GAIN_TWOTHIRDS);

    Serial.println(F("ADS1015 OK  gain=TWOTHIRDS(±6.144V)"));
    Serial.println(F("Sensor on: A0=moisture, A2=temperature (CWT TH-A 0-5V)"));
    Serial.println(F("A1/A3 printed as raw mV only (unconnected channels expected ~0 V)"));
    g_ads_ok = true;
}

void loop() {
    if (!g_ads_ok) { delay(2000); return; }

    float v0 = read_avg_volts(0);
    float v1 = read_avg_volts(1);
    float v2 = read_avg_volts(2);
    float v3 = read_avg_volts(3);

    // CWT TH-A interpretation for sensor on A0/A2
    float moisture = cwt_moisture(v0);
    float tempC    = cwt_temp(v2);

    Serial.println(F("---- sample ----"));
    Serial.printf("[ADS] A0 = %6.0f mV   A1 = %6.0f mV   A2 = %6.0f mV   A3 = %6.0f mV\n",
                  v0 * 1000.0f, v1 * 1000.0f, v2 * 1000.0f, v3 * 1000.0f);
    Serial.printf("[SOIL1] MOISTURE = %5.1f %%    TEMP = %5.2f C\n", moisture, tempC);

    // Sanity flags
    if (v0 < 0.05f) {
        Serial.println(F("  ⚠️  A0 near zero — sensor not connected or not powered?"));
    }
    if (v0 > CWT_VOLT_FS * 1.05f) {
        Serial.println(F("  ⚠️  A0 above 5V — check gain/divider"));
    }

    delay(2000);
}
