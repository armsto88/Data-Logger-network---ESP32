// Mothership V1 bringup: SIM card detection
// Power-on the A7670G and query AT+CPIN?, AT+CIMI, AT+CCID.
// PASS = +CPIN: READY (SIM present). If NOT INSERTED, report FAIL with guidance.
// Graceful power-off at end.

#include <Arduino.h>
#include "modem_at_helper.h"

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem SIM Detection Bring-up ===");

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

  // --- AT+CPIN?: SIM status ---
  Serial.println();
  Serial.println("--- AT+CPIN? (SIM status) ---");
  String cpinResp;
  bool cpinOk = modemSendAT("AT+CPIN?", cpinResp, 5000);
  Serial.print("Response: ");
  Serial.println(cpinResp.length() ? cpinResp : "(no response)");

  bool simReady = (cpinOk && cpinResp.indexOf("+CPIN: READY") >= 0);
  bool simNotInserted = (cpinResp.indexOf("+CPIN: NOT INSERTED") >= 0);

  if (simReady) {
    Serial.println("PASS: +CPIN: READY — SIM is present and ready.");
  } else if (simNotInserted) {
    Serial.println("FAIL: +CPIN: NOT INSERTED — no SIM detected.");
    Serial.println("      Guidance: Insert a SIM card into the A7670G tray and re-run.");
  } else {
    Serial.println("FAIL: Unexpected CPIN response (see above).");
    Serial.println("      Guidance: Check SIM seating and SIM tray wiring.");
  }

  // --- AT+CIMI: IMSI (only if SIM present) ---
  Serial.println();
  Serial.println("--- AT+CIMI (IMSI) ---");
  if (simReady) {
    String imsi;
    if (modemSendAT("AT+CIMI", imsi, 2000)) {
      Serial.println("PASS: AT+CIMI responded:");
      Serial.print(imsi);
    } else {
      Serial.print("FAIL: AT+CIMI -> "); Serial.println(imsi.length() ? imsi : "(no response)");
    }
  } else {
    Serial.println("SKIP: SIM not present — AT+CIMI requires a SIM.");
  }

  // --- AT+CCID: ICCID (only if SIM present) ---
  Serial.println();
  Serial.println("--- AT+CCID (ICCID) ---");
  if (simReady) {
    String ccid;
    if (modemSendAT("AT+CCID", ccid, 2000)) {
      Serial.println("PASS: AT+CCID responded:");
      Serial.print(ccid);
    } else {
      Serial.print("FAIL: AT+CCID -> "); Serial.println(ccid.length() ? ccid : "(no response)");
    }
  } else {
    Serial.println("SKIP: SIM not present — AT+CCID requires a SIM.");
  }

  // --- Verdict ---
  Serial.println();
  Serial.println("=== Modem SIM detection bring-up complete ===");
  if (simReady) {
    Serial.println("OVERALL PASS: SIM present (+CPIN: READY).");
  } else {
    Serial.println("OVERALL FAIL: SIM not detected.");
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