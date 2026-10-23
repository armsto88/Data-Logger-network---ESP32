// Mothership V1 bringup: Modem identification
// Power-on the A7670G and query ATI, AT+GMM, AT+CGSN, AT+CGMR.
// PASS = IMEI (AT+CGSN) is exactly 15 digits. Graceful power-off at end.

#include <Arduino.h>
#include "modem_at_helper.h"

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem Identification Bring-up ===");

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

  // Echo off for clean parsing.
  String resp;
  modemSendAT("ATE0", resp, 2000);

  // --- ATI: module identification ---
  Serial.println();
  Serial.println("--- ATI (module identification) ---");
  if (modemSendAT("ATI", resp, 2000)) {
    Serial.println("PASS: ATI responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: ATI -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  // --- AT+GMM: model name ---
  Serial.println();
  Serial.println("--- AT+GMM (model name) ---");
  if (modemSendAT("AT+GMM", resp, 2000)) {
    Serial.println("PASS: AT+GMM responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+GMM -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  // --- AT+CGSN: IMEI ---
  Serial.println();
  Serial.println("--- AT+CGSN (IMEI) ---");
  String imei;
  bool imeiOk = modemSendAT("AT+CGSN", imei, 2000);
  Serial.print("Response: ");
  Serial.println(imei.length() ? imei : "(no response)");

  // Extract the 15-digit IMEI from the response (first all-digit line).
  String imeiDigits = "";
  String line = "";
  imei += "\n";
  for (size_t i = 0; i < imei.length(); i++) {
    char c = imei[i];
    if (c == '\r' || c == '\n') {
      if (line.length() == 15) {
        bool allDigits = true;
        for (size_t j = 0; j < line.length(); j++) {
          if (!isdigit((unsigned char)line[j])) { allDigits = false; break; }
        }
        if (allDigits) { imeiDigits = line; break; }
      }
      line = "";
    } else {
      line += c;
    }
  }

  if (imeiOk && imeiDigits.length() == 15) {
    Serial.printf("PASS: IMEI = %s (15 digits)\n", imeiDigits.c_str());
  } else {
    Serial.println("FAIL: IMEI not 15 digits or no response.");
    Serial.println("      Check SIM/modem seating. A7670G should return a 15-digit IMEI.");
  }

  // --- AT+CGMR: firmware version ---
  Serial.println();
  Serial.println("--- AT+CGMR (firmware revision) ---");
  if (modemSendAT("AT+CGMR", resp, 2000)) {
    Serial.println("PASS: AT+CGMR responded:");
    Serial.print(resp);
  } else {
    Serial.print("FAIL: AT+CGMR -> "); Serial.println(resp.length() ? resp : "(no response)");
  }

  // --- Verdict ---
  Serial.println();
  Serial.println("=== Modem identification bring-up complete ===");
  if (imeiDigits.length() == 15) {
    Serial.println("OVERALL PASS: IMEI valid (15 digits).");
  } else {
    Serial.println("OVERALL FAIL: IMEI not valid.");
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