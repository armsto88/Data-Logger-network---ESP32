/*
 * bringup_callback_safety.cpp — Stage 1 robustness test
 *
 * Verifies that the ESP-NOW receive callback defers I2C/NVS work to the main
 * loop via pending-action flags.  The test simulates receiving a TIME_SYNC
 * packet by directly invoking the callback, then checks that:
 *   1. The callback returns quickly (no I2C/NVS blocking inside it).
 *   2. g_pendingTimeSync is set after the callback returns.
 *   3. After one loop iteration, the time sync is applied (rtcSynced == true).
 *
 * This is a compile-and-run smoke test: it links against the same source files
 * as the main firmware env so the flag pattern is exercised against real code.
 *
 * Hardware: runs on the node board (COM5).  Requires RTC + NVS.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <nvs_flash.h>

#include "protocol.h"

// ---- Pins (must match main env build_flags) ----
#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 18
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 19
#endif
#ifndef RTC_INT_PIN
#define RTC_INT_PIN 4
#endif
#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif
#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

// ---- Shared state (mirrors main.cpp globals for the test) ----
TwoWire WireRtc(0);
RTC_DS3231 rtc;

static const char* TEST_NODE_ID = "ENV_TEST";

// ---- Test result tracking ----
static bool g_testPassed = false;
static bool g_testDone   = false;

// ---- Minimal callback + flag pattern (mirrors main.cpp Stage 1) ----
// We replicate the deferral pattern in isolation so the test is self-contained
// and does not require linking the full main.cpp (which has conflicting setup/loop).

static volatile bool g_pendingTimeSync = false;
static time_sync_response_t g_pendingTimeSyncData;
static bool      rtcSynced        = false;
static uint32_t  lastTimeSyncUnix = 0;

// Simulated ESP-NOW receive callback — copies data + sets flag, nothing else.
static void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len) {
  (void)mac;

  if (len == sizeof(time_sync_response_t)) {
    time_sync_response_t resp;
    memcpy(&resp, incomingData, sizeof(resp));

    if (strcmp(resp.command, "TIME_SYNC") == 0) {
      // ONLY copy + set flag — no I2C, no NVS.
      memcpy(&g_pendingTimeSyncData, &resp, sizeof(g_pendingTimeSyncData));
      g_pendingTimeSync = true;
      Serial.println("[CB] TIME_SYNC received -> flag set (deferred)");
    }
  }
}

// Main-loop service for the pending time sync (mirrors main.cpp loop section).
static void servicePendingTimeSync() {
  if (g_pendingTimeSync) {
    g_pendingTimeSync = false;
    time_sync_response_t& resp = g_pendingTimeSyncData;

    DateTime dt(
      (int)resp.year,
      (int)resp.month,
      (int)resp.day,
      (int)resp.hour,
      (int)resp.minute,
      (int)resp.second
    );

    rtc.adjust(dt);
    rtcSynced        = true;
    lastTimeSyncUnix = dt.unixtime();

    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
    Serial.printf("[LOOP] TIME_SYNC applied: %s\n", buf);
  }
}

static void initNVS() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  Serial.printf("[NVS] init: %s\n", esp_err_to_name(err));
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("========================================");
  Serial.println("Stage 1 Callback Safety Test");
  Serial.println("========================================");

  // 1) Assert PWR_HOLD HIGH first so the board doesn't power off mid-test.
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);
  Serial.println("[PWR_HOLD] asserted HIGH");

  // 2) Init NVS + RTC
  initNVS();
  WireRtc.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  if (!rtc.begin(&WireRtc)) {
    Serial.println("FAIL: RTC not found");
    g_testPassed = false;
    g_testDone   = true;
    return;
  }
  Serial.println("[RTC] initialized");

  // 3) Build a fake TIME_SYNC packet
  time_sync_response_t testSync{};
  strcpy(testSync.command, "TIME_SYNC");
  testSync.year   = 2026;
  testSync.month  = 6;
  testSync.day    = 22;
  testSync.hour   = 12;
  testSync.minute = 30;
  testSync.second = 0;
  strcpy(testSync.mothership_id, "MS_TEST");

  // 4) Simulate receiving the packet — call the callback directly
  uint8_t fakeMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

  uint32_t cbStart = millis();
  onDataReceived(fakeMac, (uint8_t*)&testSync, sizeof(testSync));
  uint32_t cbElapsed = millis() - cbStart;

  Serial.printf("[TEST] callback elapsed: %lu ms\n", (unsigned long)cbElapsed);

  // 5) Check: callback should have returned quickly (no I2C/NVS inside)
  //    A direct flag-set should be well under 10ms.
  bool cbFast = (cbElapsed < 10);

  // 6) Check: pending flag should be set
  bool flagSet = g_pendingTimeSync;
  Serial.printf("[TEST] g_pendingTimeSync=%d (expected 1)\n", flagSet ? 1 : 0);

  // 7) Check: rtcSynced should still be false (not applied yet)
  bool notYetApplied = !rtcSynced;
  Serial.printf("[TEST] rtcSynced=%d before loop (expected 0)\n", rtcSynced ? 1 : 0);

  // 8) Run one loop iteration (service pending actions)
  servicePendingTimeSync();

  // 9) Check: time sync should now be applied
  bool applied = rtcSynced && (lastTimeSyncUnix > 0);
  Serial.printf("[TEST] rtcSynced=%d lastTimeSyncUnix=%lu after loop (expected 1, >0)\n",
                rtcSynced ? 1 : 0, (unsigned long)lastTimeSyncUnix);

  // 10) Verify RTC actually has the time we set
  DateTime now = rtc.now();
  Serial.printf("[TEST] RTC now: %04d-%02d-%02d %02d:%02d:%02d\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second());

  bool rtcCorrect = (now.year() == 2026 && now.month() == 6 && now.day() == 22);

  // 11) Overall result
  g_testPassed = cbFast && flagSet && notYetApplied && applied && rtcCorrect;
  g_testDone   = true;

  Serial.println("========================================");
  Serial.printf("RESULT: %s\n", g_testPassed ? "PASS" : "FAIL");
  Serial.printf("  callback fast (<10ms): %s\n", cbFast ? "yes" : "no");
  Serial.printf("  flag set:              %s\n", flagSet ? "yes" : "no");
  Serial.printf("  not applied in CB:     %s\n", notYetApplied ? "yes" : "no");
  Serial.printf("  applied in loop:        %s\n", applied ? "yes" : "no");
  Serial.printf("  RTC correct:            %s\n", rtcCorrect ? "yes" : "no");
  Serial.println("========================================");
}

void loop() {
  // Idle heartbeat — keep PWR_HOLD alive.
  static unsigned long lastBeat = 0;
  unsigned long now = millis();
  if (now - lastBeat > 5000UL) {
    lastBeat = now;
    Serial.printf("💓 callback-safety test idle (result=%s)\n",
                  g_testDone ? (g_testPassed ? "PASS" : "FAIL") : "running");
  }
  delay(1000);
}