#include <Arduino.h>

#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 35
#endif
#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES 32
#endif
#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF 3.3000f
#endif
#ifndef BAT_DIVIDER_SCALE
#define BAT_DIVIDER_SCALE 2.0000f
#endif
#ifndef BAT_PRINT_MS
#define BAT_PRINT_MS 1000
#endif

namespace {

uint16_t readAdcAvg() {
  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; ++i) {
    sum += static_cast<uint16_t>(analogRead(BAT_ADC_PIN));
    delay(2);
  }
  return static_cast<uint16_t>(sum / BAT_ADC_SAMPLES);
}

float adcToPinVolts(uint16_t raw) {
  return (static_cast<float>(raw) / 4095.0f) * BAT_ADC_VREF;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Battery Voltage Bring-up (IO35) ===");

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  Serial.printf("[CFG] ADC_PIN=%d SAMPLES=%d VREF=%1.4f DIV_SCALE=%1.4f PRINT_MS=%d\n",
                BAT_ADC_PIN,
                BAT_ADC_SAMPLES,
                BAT_ADC_VREF,
                BAT_DIVIDER_SCALE,
                BAT_PRINT_MS);
  Serial.println("[NOTE] battery_V = pin_V * DIV_SCALE; tune DIV_SCALE from DMM reference if needed.");
}

void loop() {
  const uint16_t raw = readAdcAvg();
  const float pinV = adcToPinVolts(raw);
  const float batV = pinV * BAT_DIVIDER_SCALE;

  Serial.printf("BAT raw=%4u pin_V=%1.4f batt_V=%1.4f\n", raw, pinV, batV);
  delay(BAT_PRINT_MS);
}
