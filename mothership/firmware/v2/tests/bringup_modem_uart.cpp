// Mothership V1 bringup: Modem UART link check
// Power-on the A7670G and verify UART2 responds to AT commands.
// PASS = "OK" received from AT. Rail is left ON at the end (user resets board).

#include <Arduino.h>
#include "modem_at_helper.h"

void setup() {
  // CRITICAL: assert PWR_HOLD immediately so the board stays powered.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem UART Bring-up ===");
  Serial.printf("PWR_HOLD = GPIO%d (HIGH)\n", PIN_PWR_HOLD);
  Serial.printf("4V_EN    = GPIO%d\n", PIN_4V_EN);
  Serial.printf("ESP_PG   = GPIO%d\n", PIN_MODEM_PG);
  Serial.printf("PWRKEY   = GPIO%d\n", PIN_MODEM_PWRKEY);
  Serial.printf("STATUS   = GPIO%d\n", PIN_MODEM_STATUS);
  Serial.printf("UART     = TX GPIO%d / RX GPIO%d @ 115200\n", PIN_MODEM_TX, PIN_MODEM_RX);

  // --- Stage 1: Enable 4V rail (PWM soft-start) ---
  Serial.println();
  Serial.println("--- Stage 1: Enable 4V rail (soft-start) ---");
  bool pg = modemRailOn();
  Serial.printf("ESP_PG = %s | 4V_EN = HIGH\n",
                digitalRead(PIN_MODEM_PG) == HIGH ? "HIGH" : "LOW");
  if (!pg) {
    Serial.println("FAIL: Power-good did not assert within timeout.");
    Serial.println("      Check TPS63020, inductor, and 4V rail voltage.");
    Serial.println("=== Test ABORTED (rail did not come up) ===");
    return;  // loop() will just heartbeat
  }
  Serial.println("PASS: 4V rail is up (ESP_PG HIGH).");

  // --- Stage 2: Pulse PWRKEY to boot the modem ---
  Serial.println();
  Serial.println("--- Stage 2: Pulse PWRKEY ---");
  Serial.printf("STATUS before pulse = %s\n", modemReadStatus() == HIGH ? "HIGH" : "LOW");
  modemPulsePwrkey();
  Serial.printf("PWRKEY pulsed %d ms. STATUS = %s\n",
                MODEM_PWRKEY_ON_MS, modemReadStatus() == HIGH ? "HIGH" : "LOW");

  // --- Stage 3: Wait for boot (STATUS HIGH + AT handshake) ---
  Serial.println();
  Serial.println("--- Stage 3: Wait for modem boot ---");
  Serial2.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  bool booted = modemWaitBoot(15000);
  Serial.printf("STATUS = %s\n", modemReadStatus() == HIGH ? "HIGH" : "LOW");
  if (!booted) {
    Serial.println("FAIL: Modem did not respond to AT within 15 s.");
    Serial.println("      Check PWRKEY wiring, STATUS level shifter, and UART connections.");
    Serial.println("=== Test ABORTED (no UART response) ===");
    return;
  }
  Serial.println("PASS: Modem booted and responded to AT.");

  // --- Stage 4: Send AT, ATE0, AT ---
  Serial.println();
  Serial.println("--- Stage 4: AT command sequence ---");
  String resp;

  Serial.println("[CMD] AT");
  if (modemSendAT("AT", resp, 2000)) {
    Serial.println("PASS: AT -> OK");
  } else {
    Serial.print("FAIL: AT -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println("[CMD] ATE0 (echo off)");
  if (modemSendAT("ATE0", resp, 2000)) {
    Serial.println("PASS: ATE0 -> OK");
  } else {
    Serial.print("FAIL: ATE0 -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println("[CMD] AT (echo off, expect clean OK)");
  if (modemSendAT("AT", resp, 2000)) {
    Serial.println("PASS: AT -> OK (echo off confirmed)");
  } else {
    Serial.print("FAIL: AT -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println();
  Serial.println("=== Modem UART bring-up complete ===");
  Serial.println("Rail is left ON. Reset the board to re-run, or load the next test.");
}

void loop() {
  // Heartbeat — rail stays on via PWR_HOLD.
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[HB] t=%lu ms | STATUS=%d PG=%d 4V_EN=%d\n",
                  millis(), modemReadStatus(),
                  digitalRead(PIN_MODEM_PG), digitalRead(PIN_4V_EN));
  }
}