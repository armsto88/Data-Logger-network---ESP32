// Mothership V1 bringup: Battery ADC and modem power-good
// Reads VOLT_ESP (GPIO34, ADC1) and ESP_PG (GPIO35, input-only).
// Voltage divider: R1=220kΩ, R2=100kΩ → V_bat = V_adc × 3.2
// With ADC attenuation DB_11 (0–3.6 V range): V_adc = raw × 3.6 / 4095
// Combined: V_bat = raw × 3.6 × 3.2 / 4095 = raw × 0.00281
// Simplified: V_bat = raw × 0.00257 (using 3.3 V reference for more accuracy)

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
#define BAT_ADC_VREF 3.3f
#endif
#ifndef BAT_ADC_MAX
#define BAT_ADC_MAX 4095.0f
#endif
#ifndef BAT_READ_INTERVAL_MS
#define BAT_READ_INTERVAL_MS 2000
#endif

// V_bat = raw × VREF × (R1 + R2) / R2 / ADC_MAX
static constexpr float kBatScale = BAT_ADC_VREF * (BAT_DIVIDER_R1 + BAT_DIVIDER_R2) / (BAT_DIVIDER_R2 * BAT_ADC_MAX);
// = 3.3 × 3.2 / 4095 ≈ 0.00257

float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sum += analogRead(PIN_BATTERY_ADC);
  }
  float avg = static_cast<float>(sum) / BAT_ADC_SAMPLES;
  return avg * kBatScale;
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
  Serial.println("=== Mothership V1 Battery ADC Bring-up ===");
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

  Serial.printf("t=%lu ms | V_bat=%.3f V | raw_avg=%.1f | ESP_PG=%s\n",
                millis(), vBat, vBat / kBatScale,
                pgState == HIGH ? "HIGH (power-good)" : "LOW (power-bad or rail off)");

  delay(BAT_READ_INTERVAL_MS);
}