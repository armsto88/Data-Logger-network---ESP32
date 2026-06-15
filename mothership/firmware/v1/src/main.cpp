// Mothership V1 main firmware
// Wake-reason branching architecture with power gating, RTC alarm scheduling,
// ESP-NOW sync, SD logging, and config/service modes.
//
// Boot sequence:
//   1. Assert PWR_HOLD (GPIO26) — CRITICAL, must be first
//   2. Detect wake reason (config latch, RTC alarm, or USB)
//   3. Branch to appropriate handler
//   4. Re-arm RTC alarm before power-down
//   5. Release PWR_HOLD to power off

#include <Arduino.h>

#include "system/pins.h"
#include "system/power.h"
#include "system/wake_reason.h"
#include "time/rtc_alarm.h"
#include "comms/espnow_sync.h"
#include "storage/sd_logger.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#ifndef DEFAULT_SYNC_INTERVAL_MIN
#define DEFAULT_SYNC_INTERVAL_MIN 60
#endif

static int gSyncIntervalMin = DEFAULT_SYNC_INTERVAL_MIN;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void handleSyncWake();
void handleConfigWake();
void handleServiceWake();

// ---------------------------------------------------------------------------
// ESP-NOW receive callback
// ---------------------------------------------------------------------------
void onEspNowData(const uint8_t* mac, const uint8_t* data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.printf("[ESP-NOW] RX from %s | len=%d\n", macStr, len);

  // Log to SD if available
  if (sdIsReady() && len >= sizeof(sensor_data_message_t)) {
    const sensor_data_message_t* msg = reinterpret_cast<const sensor_data_message_t*>(data);
    NodeSnapshot snap;
    strncpy(snap.nodeId, msg->nodeId, sizeof(snap.nodeId) - 1);
    strncpy(snap.sensorType, msg->sensorType, sizeof(snap.sensorType) - 1);
    strncpy(snap.sensorLabel, msg->sensorLabel, sizeof(snap.sensorLabel) - 1);
    snap.sensorId = msg->sensorId;
    snap.value = msg->value;
    snap.nodeTimestamp = msg->nodeTimestamp;
    snap.qualityFlags = msg->qualityFlags;
    logSnapshot(&snap);
  }
}

// ---------------------------------------------------------------------------
// Sync wake handler
// ---------------------------------------------------------------------------
void handleSyncWake() {
  Serial.println("=== SYNC WAKE ===");
  setLed(true);

  // Clear the RTC alarm flag that woke us
  clearAlarmFlag();

  // Init subsystems
  if (!initRTC()) {
    Serial.println("[FATAL] RTC init failed — cannot re-arm alarm. Staying on.");
    while (true) { delay(1000); }
  }

  if (!initSD()) {
    Serial.println("[WARN] SD card init failed — continuing without logging");
  }

  // Init ESP-NOW in sync-only mode
  if (!initEspNowSyncOnly(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW init failed — sync window will be empty");
  }
  registerReceiveCallback(onEspNowData);

  // Broadcast sync window open
  broadcastSyncWindowOpen();

  // Listen for node data for SYNC_WINDOW_MS
  Serial.printf("[SYNC] Listening for %d ms...\n", SYNC_WINDOW_MS);
  unsigned long startMs = millis();
  while (millis() - startMs < SYNC_WINDOW_MS) {
    espnowSyncLoop();
    delay(10);
  }

  Serial.println("[SYNC] Sync window closed");

  // Re-arm alarm for next sync
  if (!armNextSyncAlarm(gSyncIntervalMin)) {
    Serial.println("[FATAL] Failed to arm next sync alarm! Staying on.");
    while (true) { delay(1000); }
  }

  // Verify alarm is properly set before power-down
  if (!verifyAlarmSet()) {
    Serial.println("[FATAL] Alarm verification failed! Staying on.");
    while (true) { delay(1000); }
  }

  Serial.println("[SYNC] Alarm armed and verified. Powering down.");
  setLed(false);
  delay(100);
  releasePwrHold();  // Board powers off here
}

// ---------------------------------------------------------------------------
// Config wake handler
// ---------------------------------------------------------------------------
void handleConfigWake() {
  Serial.println("=== CONFIG WAKE ===");

  // Clear the config latch
  clearConfigLatch();
  Serial.printf("[CONFIG] Latch cleared, CONFIG_WAKE=%s\n",
                 readConfigWake() ? "STILL SET" : "cleared");

  // Init RTC for time display and alarm scheduling
  initRTC();

  // Init SD for config storage
  initSD();

  // TODO: Start WiFi AP + web server (same as current firmware)
  // For now, blink LED to indicate config mode
  Serial.println("[CONFIG] Config mode active. TODO: start WiFi AP + web server.");

  // Blink LED to show config mode
  for (int i = 0; i < 30; i++) {
    toggleLed();
    delay(200);
  }

  // Re-arm alarm for next sync before power-down
  if (initRTC()) {
    armNextSyncAlarm(gSyncIntervalMin);
    if (!verifyAlarmSet()) {
      Serial.println("[FATAL] Alarm verification failed! Staying on.");
      while (true) { delay(1000); }
    }
  }

  Serial.println("[CONFIG] Powering down.");
  setLed(false);
  delay(100);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// Service wake handler (USB connected)
// ---------------------------------------------------------------------------
void handleServiceWake() {
  Serial.println("=== SERVICE WAKE (USB) ===");
  Serial.println("[SERVICE] Full runtime mode — staying on while USB connected.");

  // Init all subsystems
  initRTC();
  initSD();

  // Init ESP-NOW for continuous receive
  initEspNowSyncOnly(ESPNOW_CHANNEL);
  registerReceiveCallback(onEspNowData);

  setLed(true);

  // TODO: Start WiFi AP + web server + BLE (same as current firmware)
  // For now, just stay on and log data
  Serial.println("[SERVICE] Running indefinitely. Disconnect USB and reset to test other wake modes.");

  // Main loop — stay on while USB is connected
  while (true) {
    espnowSyncLoop();
    delay(10);

    // Print battery voltage every 30 seconds
    static unsigned long lastBatMs = 0;
    if (millis() - lastBatMs > 30000) {
      float vBat = readBatteryVoltage();
      Serial.printf("[SERVICE] V_bat=%.3f V\n", vBat);
      lastBatMs = millis();
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino setup and loop
// ---------------------------------------------------------------------------
void setup() {
  // CRITICAL: Assert PWR_HOLD as the very first action
  powerInit();
  assertPwrHold();

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Firmware ===");
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);

  // Print battery voltage at boot
  float vBat = readBatteryVoltage();
  Serial.printf("[PWR] Battery: %.3f V\n", vBat);

  // Detect wake reason
  WakeReason reason = detectWakeReason();
  printWakeReason(reason);

  // Branch based on wake reason
  switch (reason) {
    case WAKE_RTC_ALARM:
      handleSyncWake();
      break;
    case WAKE_CONFIG_BUTTON:
      handleConfigWake();
      break;
    case WAKE_USB_SERVICE:
      handleServiceWake();
      break;
    default:
      Serial.println("[WAKE] Unknown wake reason — defaulting to sync");
      handleSyncWake();
      break;
  }
}

void loop() {
  // Should not reach here — all handlers either power down or loop internally.
  // If we somehow get here, stay on and blink LED to indicate unexpected state.
  Serial.println("[WARN] Unexpected return to main loop!");
  toggleLed();
  delay(500);
}