#include <Arduino.h>

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif

#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

#ifndef PWR_HOLD_ON_MS
#define PWR_HOLD_ON_MS 15000
#endif

#ifndef PWR_HOLD_OFF_MS
#define PWR_HOLD_OFF_MS 15000
#endif

static const uint8_t kOnLevel = PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW;
static const uint8_t kOffLevel = PWR_HOLD_ACTIVE_HIGH ? LOW : HIGH;

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, kOffLevel);

  Serial.println();
  Serial.println("ESP32-WROOM PWR_HOLD gate bring-up");
  Serial.printf("PWR_HOLD pin=%d active=%s\n", PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? "HIGH" : "LOW");
  Serial.printf("Pattern: ON %d ms, OFF %d ms\n", PWR_HOLD_ON_MS, PWR_HOLD_OFF_MS);
  Serial.println("Probe VSYS and related power rails while states toggle.");
}

void loop() {
  digitalWrite(PWR_HOLD_PIN, kOnLevel);
  Serial.printf("t=%lu ms | PWR_HOLD=ON\n", millis());
  delay(PWR_HOLD_ON_MS);

  digitalWrite(PWR_HOLD_PIN, kOffLevel);
  Serial.printf("t=%lu ms | PWR_HOLD=OFF\n", millis());
  delay(PWR_HOLD_OFF_MS);
}
