// Simple SIM test — powers on the modem, checks AT, reads SIM status,
// waits for network registration, then does a test HTTPS POST to the
// Supabase ingest endpoint using the production ModemDriver::httpsPost().
// Uses the production code path so the APN, NETOPEN, CCH* SSL, and
// chunked send are all exercised exactly as in the real firmware.

#include <Arduino.h>
#include "comms/modem_driver.h"

ModemDriver modem;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== SIM + DATA TEST (Think Mobile) ===\n");

  Serial.println("[1/7] Initialising modem pins...");
  modem.init();
  Serial.println("  OK");

  Serial.println("[2/7] Powering on modem (rail + PWRKEY + boot)...");
  if (!modem.powerOn()) {
    Serial.println("  FAIL — modem did not respond to AT");
    while (true) delay(1000);
  }
  Serial.println("  OK — modem is responsive");

  Serial.println("[3/7] Reading IMEI...");
  String imei = modem.getImei();
  Serial.printf("  IMEI: %s\n", imei.length() ? imei.c_str() : "(not available)");

  Serial.println("[4/7] Checking SIM card (AT+CPIN?)...");
  Serial2.println("AT+CPIN?");
  delay(3000);
  String cpinResp = "";
  while (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      cpinResp += line + "\n";
      Serial.printf("  %s\n", line.c_str());
    }
  }
  if (cpinResp.indexOf("READY") >= 0) {
    Serial.println("  SIM is READY");
  } else if (cpinResp.indexOf("NOT INSERTED") >= 0) {
    Serial.println("  FAIL — SIM not detected");
    while (true) delay(1000);
  } else {
    Serial.println("  SIM status unclear — see output above");
  }

  Serial.println("[5/7] Waiting for network registration (60s timeout)...");
  if (!modem.waitForNetwork(60000)) {
    Serial.println("  FAIL — network registration timeout");
    int rssi = modem.getSignalQuality();
    Serial.printf("  Signal quality: %d (99=no signal)\n", rssi);
    Serial.println("[7/7] Shutting down modem...");
    modem.gracefulShutdown();
    Serial.println("  Done. Test complete.");
    return;
  }
  Serial.println("  OK — registered on network");
  int rssi = modem.getSignalQuality();
  Serial.printf("  Signal quality: %d (0=worst, 31=best, 99=no signal)\n", rssi);

  // [6/7] Data test — use the production httpsPost() to send a test payload
  // to the Supabase ingest endpoint with ?dry_run=true so nothing is stored.
  Serial.println("\n[6/7] Data test: HTTPS POST to Supabase (dry_run)...");
  Serial.println("  Using production ModemDriver::httpsPost() — same path as real firmware");

  const char* testUrl =
    "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh?dry_run=true";
  const char* testPayload =
    "{\"readings\":[],\"meta\":{\"firmwareVersion\":\"sim-test\"},\"status\":{\"batVoltage\":0}}";
  const char* testAuth = "fm_bkyd_001_b3d189ae-8b1a-4c2a-8dc8-2b6730d90567";

  Serial.printf("  URL: %s\n", testUrl);
  Serial.printf("  Payload: %u bytes\n", (unsigned)strlen(testPayload));

  HttpsPostResult result = modem.httpsPost(
    String(testUrl),
    String(testPayload),
    "application/json",
    String(testAuth)
  );

  Serial.printf("  Result: success=%d HTTP=%d\n", result.success ? 1 : 0, result.httpStatus);
  if (result.responseBody.length() > 0) {
    Serial.printf("  Response body: %s\n", result.responseBody.c_str());
  }
  if (result.errorDetail.length() > 0) {
    Serial.printf("  Error detail: %s\n", result.errorDetail.c_str());
  }

  if (result.success && result.httpStatus == 200) {
    Serial.println("  ✅ DATA WORKS — HTTPS POST succeeded!");
  } else {
    Serial.println("  ⚠️ POST did not return 200 — check error detail above");
    Serial.println("  (This could be auth/payload issues, not data connectivity)");
  }

  Serial.println("[7/7] Shutting down modem...");
  modem.gracefulShutdown();
  Serial.println("  Done. Test complete.");
}

void loop() {
  delay(1000);
}