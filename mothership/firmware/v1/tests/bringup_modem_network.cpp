// Mothership V1 bringup: Network status query (antenna connected)
// Power-on the A7670G and poll AT+CSQ, AT+CREG?, AT+CEREG? every 5s for up
// to 90s, waiting for network registration. Then print a final summary of
// AT+CSQ, AT+CREG?, AT+CEREG?, AT+CGATT?, AT+COPS?. Graceful power-off at end.

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
  Serial.println("Antenna connected. Polling for network registration (up to 180 seconds).");

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

  // --- Force operator auto-select ---
  Serial.println();
  Serial.println("--- Setting operator auto-select (AT+COPS=0) ---");
  modemSendAT("AT+COPS=0", resp, 10000);
  Serial.println(resp);
  delay(2000);

  // --- Polling loop: query registration every 5s for up to 180s ---
  Serial.println();
  Serial.println("--- Polling for network registration ---");
  const unsigned long pollIntervalMs = 5000;
  const unsigned long pollTimeoutMs = 180000;
  const unsigned long pollStart = millis();
  bool registered = false;
  bool gsmRegistered = false;

  while (millis() - pollStart < pollTimeoutMs) {
    unsigned long elapsed = (millis() - pollStart) / 1000;

    // AT+CSQ
    Serial.printf("[%lus] AT+CSQ: ", elapsed);
    if (modemSendAT("AT+CSQ", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // AT+CREG?
    Serial.printf("[%lus] AT+CREG?: ", elapsed);
    if (modemSendAT("AT+CREG?", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // AT+CEREG?
    Serial.printf("[%lus] AT+CEREG?: ", elapsed);
    if (modemSendAT("AT+CEREG?", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // Check registration success criteria.
    // CSQ not 99,99 AND CREG shows 0,1 (home) or 0,5 (roaming) AND
    // CEREG shows 0,1 (home) or 0,5 (roaming).
    String csqResp, cregResp, ceregResp;
    bool csqOk = modemSendAT("AT+CSQ", csqResp, 2000) &&
                 csqResp.indexOf("+CSQ: 99,99") < 0;
    bool cregOk = modemSendAT("AT+CREG?", cregResp, 2000) &&
                  (cregResp.indexOf("+CREG: 0,1") >= 0 ||
                   cregResp.indexOf("+CREG: 0,5") >= 0);
    bool ceregOk = modemSendAT("AT+CEREG?", ceregResp, 2000) &&
                   (ceregResp.indexOf("+CEREG: 0,1") >= 0 ||
                    ceregResp.indexOf("+CEREG: 0,5") >= 0);

    if (cregOk) {
      gsmRegistered = true;
    }

    if (csqOk && cregOk && ceregOk) {
      Serial.println();
      Serial.println("NETWORK REGISTERED");
      registered = true;
      break;
    } else if (cregOk && !ceregOk) {
      Serial.println();
      Serial.println("GSM registered, waiting for EPS...");
    }

    Serial.println();
    delay(pollIntervalMs);
  }

  // --- Final summary ---
  Serial.println();
  Serial.println("=== Final network status summary ===");

  Serial.println();
  Serial.println("--- AT+CSQ (signal quality) ---");
  if (modemSendAT("AT+CSQ", resp, 2000)) {
    Serial.println("PASS: AT+CSQ responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+CSQ -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println();
  Serial.println("--- AT+CREG? (GSM registration) ---");
  if (modemSendAT("AT+CREG?", resp, 2000)) {
    Serial.println("PASS: AT+CREG? responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+CREG? -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println();
  Serial.println("--- AT+CEREG? (EPS registration) ---");
  if (modemSendAT("AT+CEREG?", resp, 2000)) {
    Serial.println("PASS: AT+CEREG? responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+CEREG? -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println();
  Serial.println("--- AT+CGATT? (GPRS attach) ---");
  if (modemSendAT("AT+CGATT?", resp, 2000)) {
    Serial.println("PASS: AT+CGATT? responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+CGATT? -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  Serial.println();
  Serial.println("--- AT+COPS? (operator) ---");
  if (modemSendAT("AT+COPS?", resp, 2000)) {
    Serial.println("PASS: AT+COPS? responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+COPS? -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  // --- Verdict ---
  Serial.println();
  Serial.println("=== Modem network status bring-up complete ===");
  if (registered) {
    Serial.println("OVERALL PASS: Modem registered on network (GSM + EPS).");
  } else if (gsmRegistered) {
    Serial.println("PARTIAL: GSM registered but EPS not registered. May need more time or data provisioning.");
  } else {
    Serial.println("OVERALL: Commands responded but modem did not register within 180s. Check antenna and SIM.");
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