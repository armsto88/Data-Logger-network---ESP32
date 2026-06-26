// bringup_ads1015_soil.cpp
//
// Validates ADS1015IDGSR ADC and CWT TH-A soil moisture+temperature sensor.
// Prints raw mV for all 4 channels, plus interpreted soil values for the
// two probes on the current wiring:
//   SOIL1: A0 = temperature, A1 = moisture
//   SOIL2: A2 = moisture,   A3 = temperature
//
// Hardware:
//   I2C:    SDA=18, SCL=19
//   ADS1015 at 0x48 on main I2C bus (no mux)
//   CWT TH-A sensors connected:
//     SOIL1 temperature output → A0
//     SOIL1 moisture output    → A1
//     SOIL2 moisture output    → A2
//     SOIL2 temperature output → A3
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

static constexpr uint8_t CH_SOIL1_TEMP  = 0;
static constexpr uint8_t CH_SOIL1_MOIST = 1;
static constexpr uint8_t CH_SOIL2_MOIST = 2;
static constexpr uint8_t CH_SOIL2_TEMP  = 3;

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
    Serial.println(F("Wiring: SOIL1 A0=temp A1=moisture | SOIL2 A2=moisture A3=temp"));
    g_ads_ok = true;
}

void loop() {
    if (!g_ads_ok) { delay(2000); return; }

    float v0 = read_avg_volts(0);
    float v1 = read_avg_volts(1);
    float v2 = read_avg_volts(2);
    float v3 = read_avg_volts(3);

    float soil1TempC    = cwt_temp(v0);
    float soil1Moisture = cwt_moisture(v1);
    float soil2Moisture = cwt_moisture(v2);
    float soil2TempC    = cwt_temp(v3);

    Serial.println(F("---- sample ----"));
    Serial.printf("[ADS] A0 = %6.0f mV   A1 = %6.0f mV   A2 = %6.0f mV   A3 = %6.0f mV\n",
                  v0 * 1000.0f, v1 * 1000.0f, v2 * 1000.0f, v3 * 1000.0f);
    Serial.printf("[SOIL1] MOISTURE = %5.1f %%    TEMP = %5.2f C\n", soil1Moisture, soil1TempC);
    Serial.printf("[SOIL2] MOISTURE = %5.1f %%    TEMP = %5.2f C\n", soil2Moisture, soil2TempC);

    // Sanity flags
    if (v1 < 0.05f) {
        Serial.println(F("  Warning: A1 near zero — SOIL1 moisture may be unpowered or disconnected."));
    }
    if (v2 < 0.05f) {
        Serial.println(F("  Warning: A2 near zero — SOIL2 moisture may be unpowered or disconnected."));
    }
    if (v1 > CWT_VOLT_FS * 1.05f || v2 > CWT_VOLT_FS * 1.05f) {
        Serial.println(F("  Warning: moisture channel above 5V — check gain/divider."));
    }

    delay(2000);
}
