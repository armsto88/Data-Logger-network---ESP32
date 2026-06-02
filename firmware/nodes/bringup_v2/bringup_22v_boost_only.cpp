/*
 * Minimal 22V Boost ON Test
 * 
 * Purpose: Just turn on the MT3608 22V boost rail and hold it.
 * No deep sleep, no fancy logic. Probe 22V_SYS with DMM.
 * 
 * WARNING: May brownout on battery power due to MT3608 inrush.
 * If it reboots, it will try again on the next boot.
 */

#include <Arduino.h>

#define PIN_TX_22V_EN_N 5
#define PIN_PWR_HOLD 23

void setup() {
  Serial.begin(115200);
  delay(500);

  // Latch power
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Turn on 22V boost
  pinMode(PIN_TX_22V_EN_N, OUTPUT);
  digitalWrite(PIN_TX_22V_EN_N, LOW);  // LOW = boost ON

  Serial.println("=== 22V Boost ON ===");
  Serial.println("TX_22V_EN_N = LOW (boost enabled)");
  Serial.println("Probe 22V_SYS with DMM");
  Serial.println("Press 'd' to disable, 'e' to re-enable");
}

void loop() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == 'd') {
      digitalWrite(PIN_TX_22V_EN_N, HIGH);
      Serial.println("Boost OFF");
    } else if (cmd == 'e') {
      digitalWrite(PIN_TX_22V_EN_N, LOW);
      Serial.println("Boost ON");
    }
  }
  delay(100);
}