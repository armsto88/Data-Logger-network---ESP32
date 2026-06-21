// Mothership V1 bringup: Network status query (no antenna required)
// Power-on the A7670G and query AT+CSQ, AT+CREG?, AT+CEREG?, AT+CGATT?, AT+COPS?.
// Without an antenna, CSQ=99,99 and CREG=0,2 are EXPECTED — PASS = command
// responded, not actual registration. Graceful power-off at end.

#include <Arduino.h>
#include "modem_at_helper.h"

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem Network Status Bring-up ===");
  Serial.println("NOTE: No antenna connected. CSQ=99,99 and CREG=0,2 are EXPECTED.");
  Serial.println("      PASS = command responded, NOT actual registration.");

  // --- Power on ---
  Serial.println();
  Serial.println("--- Power on: 4V rail + PWRKEY + boot ---");
  bool pg = modemRailOn();
  if (!pg) {
    Serial.println("FAIL: Power-good did not assert. Aborting.");
    Serial.println("=== Test ABORTED ===");
    return;
  }
  Serial.println("PASS: 4V rail up (ESP_PG HIGH).");

  modemInitUart();
  modemPulsePwrkey();
  if (!modemWaitBoot(15000)) {
    Serial.println("FAIL: Modem did not boot / respond to AT. Aborting.");
    modemGracefulOff();
    Serial.println("=== Test ABORTED ===");
    return;
  }
  Serial.println("PASS: Modem booted.");

  String resp;
  modemSendAT("ATE0", resp, 2000);

  bool allResponded = true;

  // --- AT+CSQ: signal quality ---
  Serial.println();
  Serial.println("--- AT+CSQ (signal quality) ---");
  if (modemSendAT("AT+CSQ", resp, 2000)) {
    Serial.println("PASS: AT+CSQ responded:");
    Serial.print(resp);
    if (resp.indexOf("+CSQ: 99,99") >= 0) {
      Serial.println("      (99,99 = no signal — expected without antenna)");
    }
  } else {
    Serial.print("FAIL: AT+CSQ -> "); Serial.println(resp.length() ? resp : "(no response)");
    allResponded = false;
  }

  // --- AT+CREG?: GSM registration status ---
  Serial.println();
  Serial.println("--- AT+CREG? (GSM registration) ---");
  if (modemSendAT("AT+CREG?", resp, 2000)) {
    Serial.println("PASS: AT+CREG? responded:");
    Serial.print(resp);
    if (resp.indexOf("+CREG: 0,0") >= 0 || resp.indexOf("+CREG: 0,2") >= 0) {
      Serial.println("      (not registered — expected without antenna)");
    }
  } else {
    Serial.print("FAIL: AT+CREG? -> "); Serial.println(resp.length() ? resp : "(no response)");
    allResponded = false;
  }

  // --- AT+CEREG?: EPS registration status ---
  Serial.println();
  Serial.println("--- AT+CEREG? (EPS registration) ---");
  if (modemSendAT("AT+CEREG?", resp, 2000)) {
    Serial.println("PASS: AT+CEREG? responded:");
    Serial.print(resp);
    if (resp.indexOf("+CEREG: 0,0") >= 0 || resp.indexOf("+CEREG: 0,2") >= 0) {
      Serial.println("      (not registered — expected without antenna)");
    }
  } else {
    Serial.print("FAIL: AT+CEREG? -> "); Serial.println(resp.length() ? resp : "(no response)");
    allResponded = false;
  }

  // --- AT+CGATT?: GPRS attach status ---
  Serial.println();
  Serial.println("--- AT+CGATT? (GPRS attach) ---");
  if (modemSendAT("AT+CGATT?", resp, 2000)) {
    Serial.println("PASS: AT+CGATT? responded:");
    Serial.print(resp);
    if (resp.indexOf("+CGATT: 0") >= 0) {
      Serial.println("      (not attached — expected without antenna)");
    }
  } else {
    Serial.print("FAIL: AT+CGATT? -> "); Serial.println(resp.length() ? resp : "(no response)");
    allResponded = false;
  }

  // --- AT+COPS?: operator selection ---
  Serial.println();
  Serial.println("--- AT+COPS? (operator) ---");
  if (modemSendAT("AT+COPS?", resp, 2000)) {
    Serial.println("PASS: AT+COPS? responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+COPS? -> "); Serial.println(resp.length() ? resp : "(no response)");
    allResponded = false;
  }

  // --- Verdict ---
  Serial.println();
  Serial.println("=== Modem network status bring-up complete ===");
  if (allResponded) {
    Serial.println("OVERALL PASS: All network-status commands responded.");
    Serial.println("             (Registration is NOT expected without an antenna.)");
  } else {
    Serial.println("OVERALL FAIL: One or more commands did not respond.");
  }

  // --- Graceful power off ---
  Serial.println();
  Serial.println("--- Graceful power off ---");
  modemGracefulOff();
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void loop() {
  // Idle — test runs once in setup().
  delay(5000);
}