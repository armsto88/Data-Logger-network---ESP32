#include <Arduino.h>

#ifndef TX_PWM_PIN
#define TX_PWM_PIN 25
#endif

#ifndef TX_PWM_ACTIVE_HIGH
#define TX_PWM_ACTIVE_HIGH 1
#endif

#ifndef TX_PWM_ON_MS
#define TX_PWM_ON_MS 1000
#endif

#ifndef TX_PWM_OFF_MS
#define TX_PWM_OFF_MS 2000
#endif

static const uint8_t kOnLevel = TX_PWM_ACTIVE_HIGH ? HIGH : LOW;
static const uint8_t kOffLevel = TX_PWM_ACTIVE_HIGH ? LOW : HIGH;

void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(TX_PWM_PIN, OUTPUT);
  digitalWrite(TX_PWM_PIN, kOffLevel);

  Serial.println();
  Serial.println("ESP32-WROOM TX_PWM gate bring-up");
  Serial.printf("TX_PWM pin=%d active=%s\n", TX_PWM_PIN, TX_PWM_ACTIVE_HIGH ? "HIGH" : "LOW");
  Serial.printf("Pattern: ON %d ms, OFF %d ms\n", TX_PWM_ON_MS, TX_PWM_OFF_MS);
  Serial.println("Use this to verify 22V rail follows TX_PWM gate timing.");
}

void loop() {
  digitalWrite(TX_PWM_PIN, kOnLevel);
  Serial.printf("t=%lu ms | TX_PWM=ON\n", millis());
  delay(TX_PWM_ON_MS);

  digitalWrite(TX_PWM_PIN, kOffLevel);
  Serial.printf("t=%lu ms | TX_PWM=OFF\n", millis());
  delay(TX_PWM_OFF_MS);
}
