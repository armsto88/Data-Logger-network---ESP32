// Mothership V1 bringup: Modem power rail control
// Validates the TPS63020 buck-boost regulator via 4V_EN (GPIO33)
// and power-good feedback via ESP_PG (GPIO35).
// Does NOT attempt to boot the modem — only validates the power rail.

#include <Arduino.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef PIN_4V_EN
#define PIN_4V_EN 33
#endif
#ifndef PIN_MODEM_PG
#define PIN_MODEM_PG 35
#endif
#ifndef MODEM_PG_TIMEOUT_MS
#define MODEM_PG_TIMEOUT_MS 5000
#endif
#ifndef MODEM_RAIL_ON_MS
#define MODEM_RAIL_ON_MS 5000
#endif

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Modem power enable (active HIGH)
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);

  // Power-good input (input-only, external pull-up on PCB)
  pinMode(PIN_MODEM_PG, INPUT);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem Power Rail Bring-up ===");
  Serial.printf("4V_EN   = GPIO%d (OUTPUT, active HIGH)\n", PIN_4V_EN);
  Serial.printf("ESP_PG  = GPIO%d (INPUT, external pull-up)\n", PIN_MODEM_PG);
  Serial.printf("PG timeout = %d ms\n", MODEM_PG_TIMEOUT_MS);
  Serial.printf("Rail-on time = %d ms\n", MODEM_RAIL_ON_MS);

  // Phase 1: Check initial state (rail should be OFF)
  Serial.println();
  Serial.println("--- Phase 1: Initial state (4V_EN=LOW) ---");
  int pgInitial = digitalRead(PIN_MODEM_PG);
  Serial.printf("ESP_PG = %s (expected LOW when rail off)\n",
                 pgInitial == HIGH ? "HIGH" : "LOW");

  // Phase 2: Enable modem power rail
  Serial.println();
  Serial.println("--- Phase 2: Enabling 4V_EN (rail ON) ---");
  digitalWrite(PIN_4V_EN, HIGH);
  Serial.printf("4V_EN = HIGH at t=%lu ms\n", millis());

  // Wait for power-good with timeout
  unsigned long enableStart = millis();
  bool pgOk = false;
  while (millis() - enableStart < MODEM_PG_TIMEOUT_MS) {
    int pg = digitalRead(PIN_MODEM_PG);
    if (pg == HIGH) {
      unsigned long pgTime = millis() - enableStart;
      Serial.printf("ESP_PG = HIGH after %lu ms — power rail is GOOD\n", pgTime);
      pgOk = true;
      break;
    }
    delay(10);
  }

  if (!pgOk) {
    Serial.printf("ESP_PG did NOT go HIGH within %d ms\n", MODEM_PG_TIMEOUT_MS);
    Serial.println("Check TPS63020 enable, inductor, and output voltage.");
  }

  // Phase 3: Hold rail on for measurement
  Serial.println();
  Serial.printf("--- Phase 3: Holding rail ON for %d ms ---\n", MODEM_RAIL_ON_MS);
  Serial.println("Probe MODEM_VBAT_3V9 rail with multimeter. Expected ~3.9 V.");
  delay(MODEM_RAIL_ON_MS);

  // Phase 4: Disable modem power rail
  Serial.println();
  Serial.println("--- Phase 4: Disabling 4V_EN (rail OFF) ---");
  digitalWrite(PIN_4V_EN, LOW);
  Serial.printf("4V_EN = LOW at t=%lu ms\n", millis());

  delay(500);
  int pgAfter = digitalRead(PIN_MODEM_PG);
  Serial.printf("ESP_PG = %s (expected LOW after rail off)\n",
                 pgAfter == HIGH ? "HIGH" : "LOW");

  Serial.println();
  Serial.println("=== Modem power rail bring-up complete ===");
  Serial.println("Board stays powered via PWR_HOLD. Reset to re-test.");
}

void loop() {
  // Board stays on via PWR_HOLD; nothing more to do
  delay(5000);
  Serial.printf("t=%lu ms | 4V_EN=LOW, ESP_PG=%d\n", millis(), digitalRead(PIN_MODEM_PG));
}