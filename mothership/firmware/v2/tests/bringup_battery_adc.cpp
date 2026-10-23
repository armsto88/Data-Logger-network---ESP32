// Mothership/FieldHub bringup: Battery ADC and modem power-good
// Reads VOLT_ESP (GPIO34, ADC1) and ESP_PG (GPIO35, input-only).
// Voltage divider: R157=220kΩ, R158=100kΩ → V_bat = V_adc × 3.2
// V_adc comes from analogReadMilliVolts() (eFuse-calibrated), the same read
// production power.cpp uses since 08bc191. The old raw × VREF / 4095 maths
// read ~14% low at ADC_11db and is printed alongside only for comparison.

#include <Arduino.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef PIN_BATTERY_ADC
#define PIN_BATTERY_ADC 34
#endif
#ifndef PIN_MODEM_PG
#define PIN_MODEM_PG 35
#endif
#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES 16
#endif
#ifndef BAT_DIVIDER_R1
#define BAT_DIVIDER_R1 220000.0f
#endif
#ifndef BAT_DIVIDER_R2
#define BAT_DIVIDER_R2 100000.0f
#endif
#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF 3.6f  // ADC_11db attenuation range is 0–3.6 V
#endif
#ifndef BAT_ADC_MAX
#define BAT_ADC_MAX 4095.0f
#endif
#ifndef BAT_READ_INTERVAL_MS
#define BAT_READ_INTERVAL_MS 2000
#endif

// V_bat = raw × VREF × (R1 + R2) / R2 / ADC_MAX
static constexpr float kBatScale = BAT_ADC_VREF * (BAT_DIVIDER_R1 + BAT_DIVIDER_R2) / (BAT_DIVIDER_R2 * BAT_ADC_MAX);
// Uncalibrated comparison only: 3.6 × 3.2 / 4095 ≈ 0.00281 V/LSB

static float gLastRawAvg = 0.0f;

float readBatteryVoltage() {
  uint32_t sumRaw = 0;
  uint32_t sumMv = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sumRaw += analogRead(PIN_BATTERY_ADC);
    sumMv += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  gLastRawAvg = static_cast<float>(sumRaw) / BAT_ADC_SAMPLES;
  const float pinV = static_cast<float>(sumMv) / BAT_ADC_SAMPLES / 1000.0f;
  return pinV * (BAT_DIVIDER_R1 + BAT_DIVIDER_R2) / BAT_DIVIDER_R2;
}

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Battery ADC (input-only, no pull-up needed)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // 0–3.6 V range

  // Modem power-good (input-only, external pull-up on PCB)
  pinMode(PIN_MODEM_PG, INPUT);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== FieldHub Battery ADC Bring-up ===");
  Serial.printf("BATTERY_ADC = GPIO%d (ADC1, 12-bit, 0–3.6 V)\n", PIN_BATTERY_ADC);
  Serial.printf("MODEM_PG    = GPIO%d (input-only, external pull-up)\n", PIN_MODEM_PG);
  Serial.printf("Divider: R1=%.0f kΩ, R2=%.0f kΩ, scale=%.5f V/LSB\n",
                BAT_DIVIDER_R1 / 1000.0f, BAT_DIVIDER_R2 / 1000.0f, kBatScale);
  Serial.printf("Samples per reading: %d\n", BAT_ADC_SAMPLES);
  Serial.println("Reading battery voltage every 2 seconds...");
}

void loop() {
  float vBat = readBatteryVoltage();
  int pgState = digitalRead(PIN_MODEM_PG);

  Serial.printf("t=%lu ms | V_bat=%.3f V (calibrated) | raw_avg=%.1f (uncal %.3f V) | ESP_PG=%s\n",
                millis(), vBat, gLastRawAvg, gLastRawAvg * kBatScale,
                pgState == HIGH ? "HIGH (power-good)" : "LOW (power-bad or rail off)");

  delay(BAT_READ_INTERVAL_MS);
}