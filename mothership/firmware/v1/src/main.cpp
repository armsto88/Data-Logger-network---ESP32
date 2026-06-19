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
#include "storage/flash_logger.h"
#include "config/node_registry.h"
#include "comms/espnow_config.h"
#include "config/config_server.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#ifndef DEFAULT_SYNC_INTERVAL_MIN
#define DEFAULT_SYNC_INTERVAL_MIN 60
#endif

// gSyncIntervalMin is now owned by config_server.cpp (loaded from NVS in
// config mode).  In sync-wake mode it falls back to the compile-time default
// when config mode has not run yet (e.g. first boot).

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

  // --- node_snapshot_t handling (primary path, 124 bytes) ---
  // Nodes send a node_snapshot_t with command="NODE_SNAPSHOT".
  if (len >= (int)sizeof(node_snapshot_t)) {
    const node_snapshot_t* snap = reinterpret_cast<const node_snapshot_t*>(data);
    if (strncmp(snap->command, "NODE_SNAPSHOT", 15) == 0) {
      Serial.printf("[SNAP] nodeId=%.15s seq=%lu present=0x%04X bat=%.2fV airT=%.1f airH=%.1f\n",
                    snap->nodeId,
                    (unsigned long)snap->seqNum,
                    snap->sensorPresent,
                    snap->batVoltage,
                    snap->airTemp,
                    snap->airHumidity);

      // Log to flash if ready (SD fallback), otherwise try SD.
      if (flashIsReady()) {
        logSnapshotRow(snap);
      } else if (sdIsReady() && len >= (int)sizeof(sensor_data_message_t)) {
        // SD-only legacy fallback (rare; SD usually broken on V1).
      }
      return;
    }
  }

  // --- Legacy sensor_data_message_t fallback (68 bytes) ---
  if (sdIsReady() && len >= (int)sizeof(sensor_data_message_t)) {
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
  } else if (flashIsReady() && len >= (int)sizeof(sensor_data_message_t)) {
    // Legacy message but only flash available — log a minimal CSV row.
    const sensor_data_message_t* msg = reinterpret_cast<const sensor_data_message_t*>(data);
    char row[256];
    snprintf(row, sizeof(row), "%lu,%.15s,,,,,%.4f,%.4f,%.4f",
             (unsigned long)millis(),
             msg->nodeId,
             msg->value,
             0.0f, 0.0f);
    flashLogCSVRow(String(row));
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
    Serial.println("[WARN] SD card init failed — falling back to flash (LittleFS)");
    if (!initFlash()) {
      Serial.println("[WARN] Flash init also failed — continuing without logging");
    } else {
      Serial.println("[STORAGE] Active storage: FLASH (LittleFS)");
    }
  } else {
    Serial.println("[STORAGE] Active storage: SD card");
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
  int syncInterval = (gSyncIntervalMin > 0) ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN;
  if (!armNextSyncAlarm(syncInterval)) {
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

  setLed(true);

  // 1. Init RTC for time display and alarm scheduling
  if (!initRTC()) {
    Serial.println("[WARN] RTC init failed in config mode — continuing without RTC");
  }

  // 2. Init storage: try SD first, fall back to flash (LittleFS)
  if (!initSD()) {
    Serial.println("[WARN] SD card init failed — falling back to flash (LittleFS)");
    if (!initFlash()) {
      Serial.println("[WARN] Flash init also failed — continuing without logging");
    } else {
      Serial.println("[STORAGE] Active storage: FLASH (LittleFS)");
    }
  } else {
    Serial.println("[STORAGE] Active storage: SD card");
    // Also init flash as a secondary store for the config server's CSV view.
    initFlash();
  }

  // 3. Load paired nodes + NVS globals
  loadPairedNodes();
  loadWakeIntervalFromNVS();
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  loadSyncModeFromNVS();
  loadDailySyncTimeFromNVS();
  loadSyncRuntimeGuardsFromNVS();

  Serial.printf("[CONFIG] wake=%d min sync=%d min mode=%s daily=%02d:%02d\n",
                gWakeIntervalMin, gSyncIntervalMin,
                (gSyncMode == 1) ? "interval" : "daily",
                gSyncDailyHour, gSyncDailyMinute);

  // 4. Init ESP-NOW in config mode (AP+STA on ESPNOW_CHANNEL)
  if (!initEspNowConfig(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW config init failed — continuing without ESP-NOW");
  }

  // 5. Start WiFi AP + web server
  startConfigServer();

  // 6. Config server loop — exit on second button press or 30-min timeout
  Serial.println("[CONFIG] Config mode active. Press config button again to exit.");
  unsigned long configStartMs = millis();
  const unsigned long kConfigTimeoutMs = 30UL * 60UL * 1000UL;  // 30 min

  // Long grace period after latch clear — the SN74LVC2G74 latch can re-trigger
  // for several seconds after being cleared due to button bounce.
  Serial.println("[CONFIG] Config mode active. Hold button for 2 seconds to exit.");
  delay(5000);  // 5-second grace period
  clearConfigLatch();  // clear any residual latch re-trigger

  while (true) {
    configServerLoop();

    // Exit condition 1: config button held for 2 seconds (sustained press)
    if (isConfigButtonPressed()) {
      // Start sustained-press timer
      unsigned long pressStart = millis();
      bool sustained = true;
      while (isConfigButtonPressed() && (millis() - pressStart < 2000)) {
        configServerLoop();
        delay(50);
      }
      if (isConfigButtonPressed()) {
        // Held for 2+ seconds — confirmed exit
        Serial.println("[CONFIG] Button held 2s — exiting config mode");
        clearConfigLatch();
        delay(200);
        break;
      } else {
        // Released before 2 seconds — not a sustained press, clear and continue
        Serial.println("[CONFIG] Brief button press — ignoring (need 2s hold to exit)");
        clearConfigLatch();
        delay(500);
      }
    }

    // Exit condition 2: 30-minute timeout
    if (millis() - configStartMs > kConfigTimeoutMs) {
      Serial.println("[CONFIG] 30-min timeout reached — exiting config mode");
      break;
    }

    delay(5);
  }

  // 7. Save NVS + re-arm alarm before power-down
  saveSyncRuntimeGuardsToNVS();

  if (initRTC()) {
    if (gSyncMode == 1) {
      // Interval mode
      if (!armNextSyncAlarm(gSyncIntervalMin > 0 ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN)) {
        Serial.println("[FATAL] Failed to arm next sync alarm! Staying on.");
        while (true) { delay(1000); }
      }
    } else {
      // Daily mode
      if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
        Serial.println("[FATAL] Failed to arm daily alarm! Staying on.");
        while (true) { delay(1000); }
      }
    }
    if (!verifyAlarmSet()) {
      Serial.println("[FATAL] Alarm verification failed! Staying on.");
      while (true) { delay(1000); }
    }
  } else {
    Serial.println("[FATAL] RTC not available — cannot arm alarm. Staying on.");
    while (true) { delay(1000); }
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
  Serial.println("[SERVICE] No config button, no RTC alarm — arming alarm and powering down.");

  // Init RTC
  if (!initRTC()) {
    Serial.println("[WARN] RTC init failed — cannot arm alarm. Staying on for diagnostics.");
    setLed(true);
    while (true) { delay(1000); }
  }

  // Arm alarm for next sync interval
  int syncInterval = (gSyncIntervalMin > 0) ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN;
  if (!armNextSyncAlarm(syncInterval)) {
    Serial.println("[FATAL] Failed to arm sync alarm! Staying on.");
    while (true) { delay(1000); }
  }
  if (!verifyAlarmSet()) {
    Serial.println("[FATAL] Alarm verification failed! Staying on.");
    while (true) { delay(1000); }
  }

  Serial.println("[SERVICE] Alarm armed. Powering down.");
  Serial.println("[SERVICE] Press config button to enter config mode.");
  setLed(false);
  delay(100);
  releasePwrHold();
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