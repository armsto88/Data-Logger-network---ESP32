// Modem + SIM + Network + HTTPS test using the production ModemDriver.
// Tests the full upload path with the current SIM card (Think Mobile).
//
// Sequence:
//   1. Power on modem (rail → PWRKEY → boot → AT ready)
//   2. Identify (IMEI, signal quality)
//   3. SIM check (AT+CPIN? → READY)
//   4. Network registration (AT+CREG? / AT+CEREG?)
//   5. HTTPS POST to Supabase ingest endpoint (dry_run=true)
//   6. Graceful shutdown
//
// Upload to the mothership, open serial monitor at 115200.

#include <Arduino.h>
#include "comms/modem_driver.h"

ModemDriver modem;

// Helper: send a raw AT command to Serial2 and print the response.
// The modem driver starts UART2 during powerOn(), so we can use it directly.
void sendRawAT(const char* cmd, uint32_t timeoutMs = 3000) {
  Serial2.print(cmd);
  Serial2.print("\r\n");
  Serial.printf("  >> %s\n", cmd);
  uint32_t start = millis();
  String resp;
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = Serial2.read();
      resp += c;
    }
  }
  // Print response line by line
  int lineStart = 0;
  while (lineStart < (int)resp.length()) {
    int nl = resp.indexOf('\n', lineStart);
    if (nl < 0) nl = resp.length();
    String line = resp.substring(lineStart, nl);
    line.trim();
    if (line.length() > 0) Serial.printf("  << %s\n", line.c_str());
    lineStart = nl + 1;
  }
}

// Supabase ingest endpoint (dry_run mode — validates without storing)
static constexpr const char* TEST_URL =
    "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh?dry_run=true";

// Test API key — replace with the real key if you want the POST to succeed.
// With a dummy key the Edge Function should return 401, which still proves
// the modem → Supabase HTTPS path works end-to-end.
static constexpr const char* TEST_API_KEY =
    "fm_bkyd_001_b3d189ae-8b1a-4c2a-8dc8-2b6730d90567";

// Minimal test payload — one reading + status
static constexpr const char* TEST_PAYLOAD =
    "{\"readings\":[{\"nodeId\":\"TEST\",\"datetime\":\"2026-07-01T00:00:00Z\","
    "\"seqNum\":1,\"sensorPresent\":0,\"qualityFlags\":0,\"configVersion\":1,"
    "\"batVoltage\":3.80}],"
    "\"meta\":{\"firmwareVersion\":\"test\",\"uploadReason\":\"test\"},"
    "\"status\":{\"batVoltage\":3.80,\"flashUsagePct\":0,"
    "\"wakeIntervalMinutes\":5,\"syncIntervalMinutes\":90,"
    "\"syncMode\":\"interval\",\"fleetTotal\":0,\"fleetDeployed\":0,"
    "\"fleetPaired\":0,\"fleetUnpaired\":0,\"pendingRows\":0,"
    "\"rowsUploaded\":0,\"rtcUnix\":0,\"deviceId\":\"TEST\"}}";

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n========================================");
  Serial.println("  MODEM + SIM + NETWORK + HTTPS TEST");
  Serial.println("  Using production ModemDriver class");
  Serial.println("========================================\n");

  // --- Step 1: Init + Power On ---
  Serial.println("[1/6] Initialising modem driver...");
  modem.init();
  delay(500);

  Serial.println("[2/6] Powering on modem (rail → PWRKEY → boot → AT)...");
  if (!modem.powerOn()) {
    Serial.println("FAIL: Modem did not power on / AT not responding");
    Serial.println("Check: 4V rail, PWRKEY pulse, antenna, SIM seated");
    while (true) delay(1000);
  }
  Serial.println("OK: Modem powered on, AT responding\n");

  // --- Step 2: Identify ---
  Serial.println("[3/6] Modem identification:");
  String imei = modem.getImei();
  Serial.printf("  IMEI: %s\n", imei.length() ? imei.c_str() : "(unknown)");
  int rssi = modem.getSignalQuality();
  Serial.printf("  Signal: %d dBm (%s)\n", rssi,
                rssi == 99 ? "no signal" :
                rssi == 0 ? "-113 dBm or less" :
                rssi == 31 ? "-51 dBm or greater" : "good");
  Serial.println();

  // --- Step 3: SIM check ---
  Serial.println("[4/6] SIM card check...");
  {
    sendRawAT("AT+CPIN?", 5000);
    sendRawAT("AT+CICCID", 3000);
    sendRawAT("AT+COPS?", 3000);
  }
  Serial.println();

  // --- Step 4: Network registration ---
  Serial.println("[5/6] Waiting for network registration (60s timeout)...");
  if (!modem.waitForNetwork(60000)) {
    Serial.println("FAIL: Network registration timeout");
    Serial.println("Check: antenna connected, SIM activated, coverage in area");
    // Continue to shutdown anyway
  } else {
    Serial.println("OK: Registered on network\n");

    // Check registration status detail
    sendRawAT("AT+CREG?", 3000);
    sendRawAT("AT+CEREG?", 3000);
    sendRawAT("AT+COPS?", 3000);
  }
  Serial.println();

  // --- Step 5: HTTPS POST test ---
  Serial.println("[6/6] HTTPS POST test to Supabase (dry_run=true)...");
  Serial.printf("  URL: %s\n", TEST_URL);
  Serial.printf("  Payload: %u bytes\n", (unsigned)strlen(TEST_PAYLOAD));

  HttpsPostResult result = modem.httpsPost(
      String(TEST_URL),
      String(TEST_PAYLOAD),
      "application/json",
      String(TEST_API_KEY));

  Serial.printf("\n  Result: %s\n", result.success ? "SUCCESS" : "FAIL");
  Serial.printf("  HTTP status: %d\n", result.httpStatus);
  if (result.responseBody.length() > 0) {
    Serial.printf("  Response body: %s\n", result.responseBody.c_str());
  }
  if (result.errorDetail.length() > 0) {
    Serial.printf("  Error detail: %s\n", result.errorDetail.c_str());
  }

  if (result.success && result.httpStatus == 200) {
    Serial.println("\n  ✅ Supabase accepted the POST — modem + SIM + HTTPS all working!");
  } else if (result.httpStatus == 401) {
    Serial.println("\n  ⚠️  HTTPS path works (got 401) — API key may be invalid or revoked");
    Serial.println("  But the modem → Supabase connectivity is confirmed.");
  } else if (result.httpStatus == 400) {
    Serial.println("\n  ⚠️  HTTPS path works (got 400) — payload format issue");
  } else {
    Serial.println("\n  ❌ HTTPS POST failed — check signal, antenna, SSL/TLS");
  }
  Serial.println();

  // --- Step 6: Graceful shutdown ---
  Serial.println("Shutting down modem...");
  modem.gracefulShutdown();
  Serial.println("Modem powered off.\n");

  Serial.println("========================================");
  Serial.println("  TEST COMPLETE");
  Serial.println("========================================");
  while (true) delay(1000);
}

void loop() {
  // Nothing — test runs in setup()
}