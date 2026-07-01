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
#include <Wire.h>
#include <esp_now.h>
#include <Preferences.h>
#include <WiFi.h>

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
#include "config/transmission_settings.h"
#include "storage/upload_queue.h"
#include "storage/json_payload.h"
#include "comms/modem_driver.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#ifndef DEFAULT_SYNC_INTERVAL_MIN
#define DEFAULT_SYNC_INTERVAL_MIN 60
#endif
static constexpr uint32_t kSyncSessionLimitMs = 180000UL;

// ---------------------------------------------------------------------------
// Upload subsystem globals
// ---------------------------------------------------------------------------
UploadQueue uploadQueue;

// Project started — first-ever boot timestamp (set once in NVS, never
// overwritten).  Populated in setup() for dashboard/status reporting.
uint32_t g_projectStartedUnix = 0;

// gSyncIntervalMin is now owned by config_server.cpp (loaded from NVS in
// config mode).  In sync-wake mode it falls back to the compile-time default
// when config mode has not run yet (e.g. first boot).

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void handleSyncWake();
void handleConfigWake();
void handleServiceWake();
void performModemUpload(const TransmissionSettings& txSettings, uint32_t sessionStartMs);

static void boundedRetryAndShutdown(const char* context) {
  constexpr int kMaxAttempts = 3;

  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    Serial.printf("[RETRY] %s: attempt %d/%d\n", context, attempt, kMaxAttempts);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    delay(100);

    if (initRTC() != RTC_ABSENT && armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN)) {
      Serial.printf("[RETRY] %s: rescue alarm armed on attempt %d\n", context, attempt);
      Serial.println("[RETRY] Releasing PWR_HOLD - board will wake on rescue alarm");
      releasePwrHold();
      return;
    }

    delay(500);
  }

  Serial.printf("[RETRY] %s: all attempts failed - trying one final best-effort rescue alarm\n",
                context);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(100);
  const RtcInitStatus rtcStatus = initRTC();
  const bool rescueArmed = armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN);
  Serial.printf("[RETRY] %s: final RTC init=%s rescue alarm=%s\n",
                context, rtcStatus == RTC_OK ? "ok" :
                         (rtcStatus == RTC_PRESENT_TIME_INVALID ? "TIME_INVALID" : "ABSENT"),
                rescueArmed ? "armed" : "FAILED");
  Serial.printf("[RETRY] %s: releasing PWR_HOLD as last resort\n", context);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// ESP-NOW snapshot processing (main task only)
// ---------------------------------------------------------------------------
static bool ensureSnapshotAckPeer(const uint8_t* mac) {
  if (!mac) return false;

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  esp_err_t addResult = esp_now_add_peer(&peer);
  return addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST;
}

static void sendSnapshotAck(const uint8_t* mac, const DecodedSnapshot& decoded, bool persisted) {
  if (!mac) return;

  snapshot_ack_t ack{};
  strncpy(ack.command, "SNAPSHOT_ACK", sizeof(ack.command) - 1);
  strncpy(ack.nodeId, decoded.nodeId, sizeof(ack.nodeId) - 1);
  ack.seqNum = decoded.seqNum;
  ack.persisted = persisted ? 1 : 0;
  ack.protocolVersion = decoded.protocolVersion;

  if (!ensureSnapshotAckPeer(mac)) {
    Serial.printf("[SNAP-ACK] peer add failed for %.15s seq=%lu\n",
                  decoded.nodeId, static_cast<unsigned long>(decoded.seqNum));
    return;
  }

  esp_err_t sendResult = esp_now_send(mac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
  Serial.printf("[SNAP-ACK] %.15s seq=%lu persisted=%u proto=%u send=%s\n",
                ack.nodeId, static_cast<unsigned long>(ack.seqNum),
                static_cast<unsigned>(ack.persisted),
                (unsigned)ack.protocolVersion,
                sendResult == ESP_OK ? "OK" : esp_err_to_name(sendResult));
}

void processSnapshot(const DecodedSnapshot& decoded, const uint8_t* mac) {
  if (!mac) return;

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  const float* batV = decoded.find(SENSOR_ID_BAT_V);
  const float* airT = decoded.find(SENSOR_ID_AIR_TEMP);
  const float* airH = decoded.find(SENSOR_ID_AIR_RH);
  Serial.printf("[SNAP] RX=%s nodeId=%.15s seq=%lu present=0x%04X proto=%u bat=%.2fV airT=%.1f airH=%.1f\n",
                macStr, decoded.nodeId, static_cast<unsigned long>(decoded.seqNum),
                (unsigned)decoded.sensorPresent, (unsigned)decoded.protocolVersion,
                batV ? *batV : 0.0f, airT ? *airT : 0.0f, airH ? *airH : 0.0f);

  bool persisted = false;
  if (flashIsReady()) {
    persisted = logDecodedSnapshot(decoded);
    if (!persisted) {
      Serial.println("[SNAP] Flash logging failed");
    }
  } else {
    Serial.println("[SNAP] Flash unavailable; snapshot not durably logged");
  }
  sendSnapshotAck(mac, decoded, persisted);

  for (auto& n : registeredNodes) {
    if (strncmp(n.nodeId.c_str(), decoded.nodeId, 16) == 0 ||
        memcmp(n.mac, mac, 6) == 0) {
      n.lastSeen = millis();
      n.isActive = true;
      if (batV && !isnan(*batV)) {
        n.lastReportedBatV = *batV;
      }
      if (decoded.nodeTimestamp > 0 && decoded.nodeTimestamp > n.lastNodeTimestamp) {
        n.lastNodeTimestamp = decoded.nodeTimestamp;
      }
      if (decoded.configVersion > 0 && decoded.configVersion > n.configVersionApplied) {
        n.configVersionApplied = decoded.configVersion;
      }
      break;
    }
  }

  // Legacy sensor_data_message_t packets are intentionally ignored. All
  // deployed nodes send node_snapshot_t (V1) or node_snapshot_v2_t (V2).
}

// V1 compatibility wrapper — decodes a node_snapshot_t into the common
// DecodedSnapshot form and delegates to the canonical overload.
void processSnapshot(const node_snapshot_t* snap, const uint8_t* mac) {
  if (!snap || !mac) return;
  DecodedSnapshot decoded;
  decodeV1(*snap, decoded);
  processSnapshot(decoded, mac);
}

// ---------------------------------------------------------------------------
// Modem upload sequence (called from handleSyncWake when txSettings.enabled)
// ---------------------------------------------------------------------------
void performModemUpload(const TransmissionSettings& txSettings, uint32_t sessionStartMs) {
  Serial.println("[UPLOAD] === Starting modem upload sequence ===");

  const uint32_t retryNowUnix = getRTCTime();
  const uint32_t retryIntervalMin = txSettings.uploadIntervalMin > 0 ?
      txSettings.uploadIntervalMin :
      static_cast<uint32_t>(gSyncIntervalMin > 0 ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN);
  const uint32_t retryCooldownSec = retryIntervalMin * 60UL;

  ModemDriver modem;
  modem.init();

  auto sessionExpired = [&]() -> bool {
    return millis() - sessionStartMs > kSyncSessionLimitMs;
  };

  // 1. Power on modem
  if (!modem.powerOn()) {
    Serial.println("[UPLOAD] FAIL: Modem power-on failed");
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
    return;
  }
  Serial.println("[UPLOAD] Modem powered on");
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout after modem power-on - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // 2. Wait for network registration (60s timeout — will fail without antenna)
  Serial.println("[UPLOAD] Waiting for network registration (60s timeout)...");
  if (!modem.waitForNetwork(60000)) {
    Serial.println("[UPLOAD] Network registration failed/timeout — skipping upload");
    modem.gracefulShutdown();
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
    return;
  }
  Serial.println("[UPLOAD] Network registered");
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout after network registration - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // -----------------------------------------------------------------------
  // JSON upload path (multi-POST loop) — used when txSettings.useJsonUpload
  // is true.  Falls back to the single-POST CSV path below on build failure.
  // -----------------------------------------------------------------------
  if (txSettings.useJsonUpload) {
    constexpr uint16_t kMaxReadingsPerPost = 100;
    constexpr uint32_t kJsonChunkBytes     = 16384;  // ~100-130 rows of CSV

    // Supabase: header-only Bearer auth, JSON array body, no query params.
    // The legacy Google Apps Script path (apiKey empty) still appends action.
    const bool isSupabase = txSettings.apiKey.length() > 0;
    const String authHeader = txSettings.apiKey.length() > 0
        ? txSettings.apiKey : txSettings.authToken;

    const uint32_t totalPendingRows = uploadQueue.getPendingRows();
    Serial.printf("[UPLOAD] JSON path: %u pending rows, %u per POST\n",
                  (unsigned)totalPendingRows, (unsigned)kMaxReadingsPerPost);

    bool anyJsonSuccess = false;
    bool firstChunk = true;
    uint32_t nowUnix = getRTCTime();

    // Build the status context once per upload session — the backend stores
    // it in mothership_status on every POST, so the dashboard sees fresh
    // battery/flash/fleet/schedule data after each collection.
    const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
    const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
    const uint32_t flashPct = (fsTotal > 0)
      ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;
    const UploadCursor statusCursor = uploadQueue.getCursor();
    const auto allNodes = getRegisteredNodes();
    uint16_t fTotal = 0, fDeployed = 0, fPaired = 0, fUnpaired = 0, fPending = 0;
    for (const auto& n : allNodes) {
      fTotal++;
      if (n.state == DEPLOYED) fDeployed++;
      else if (n.state == PAIRED) fPaired++;
      else fUnpaired++;
      if (n.stateChangePending || n.deployPending) fPending++;
    }
    const StatusContext statusCtx = {
      readBatteryVoltage(), flashPct, fsTotal, fsUsed,
      "scheduled", computeNextSyncIsoLocal(),
      gWakeIntervalMin, gSyncIntervalMin,
      (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily",
      fTotal, fDeployed, fPaired, fUnpaired,
      uploadQueue.getPendingRows(), statusCursor.rowsUploaded,
      statusCursor.retryCount, statusCursor.lastUploadUnix,
      FW_VERSION, FW_BUILD, getRTCTime(),
      WiFi.macAddress(),
      fPending, txSettings.enabled,
      uploadQueue.getPendingRows(), (uint64_t)getCSVFileSize(), String("")
    };

    while (uploadQueue.getPendingRows() > 0 && !sessionExpired()) {
      UploadPayload payload = uploadQueue.getNewData(kJsonChunkBytes);
      if (payload.byteLength == 0) {
        Serial.println("[UPLOAD] JSON: no data returned from queue");
        break;
      }
      Serial.printf("[UPLOAD] JSON chunk: %u bytes, ~%u rows\n",
                    payload.byteLength, payload.rowEstimate);

      // Send the mothership status object only on the FIRST POST of the
      // session — it doesn't change between chunks, so repeating it on every
      // chunk just bloats the body and writes mothership_status N times.
      JsonPayload json = buildJsonUpload(payload.csvData, kMaxReadingsPerPost,
                                         FW_VERSION, firstChunk ? &statusCtx : nullptr,
                                         getRTCTime());
      if (json.ok && json.rowCount == 0 && json.csvBytesConsumed > 0) {
        // The row(s) in this chunk were malformed and skipped by the builder.
        // Advance past them (and purge) so the cursor doesn't stall — do NOT
        // POST or treat as an error.
        Serial.printf("[UPLOAD] JSON: skipped malformed row(s), advancing %u bytes\n",
                      (unsigned)json.csvBytesConsumed);
        nowUnix = getRTCTime();
        uploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, nowUnix);
        uploadQueue.purgeUploaded();
        continue;
      }
      if (!json.ok || json.rowCount == 0) {
        // Build failed (heap) — fall back to a CSV POST for this chunk.
        Serial.println("[UPLOAD] JSON build failed/empty — falling back to CSV POST");
        String url = buildUploadUrl(txSettings);
        if (!isSupabase) {
          url += (url.indexOf('?') >= 0) ? "&action=uploadSync" : "?action=uploadSync";
        }
        HttpsPostResult result = modem.httpsPost(url, payload.csvData,
                                                 "text/csv", authHeader);
        if (result.httpStatus == 200) {
          Serial.printf("[UPLOAD] CSV fallback SUCCESS: HTTP 200, %u bytes\n",
                        payload.byteLength);
          nowUnix = getRTCTime();
          uploadQueue.advanceCursor(payload.startOffset + payload.byteLength, nowUnix);
          uploadQueue.purgeUploaded();
          uploadQueue.resetRetryCount();
          anyJsonSuccess = true;
          firstChunk = false;
          continue;
        } else if (result.httpStatus == 400 || result.httpStatus == 401) {
          Serial.printf("[UPLOAD] CSV fallback non-retryable HTTP %d (%s) — stopping\n",
                        result.httpStatus,
                        result.httpStatus == 401 ? "credentials" : "bad payload");
          break;  // not transient — do not increment retry counter
        } else {
          Serial.printf("[UPLOAD] CSV fallback retryable HTTP %d, %s\n",
                        result.httpStatus, result.errorDetail.c_str());
          uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
          break;
        }
      }

      Serial.printf("[UPLOAD] JSON built: %u bytes, %u readings, consumed %u CSV bytes\n",
                    json.byteLength, (unsigned)json.rowCount, (unsigned)json.csvBytesConsumed);

      String url = buildUploadUrl(txSettings);
      if (!isSupabase) {
        url += (url.indexOf('?') >= 0)
            ? (firstChunk ? "&action=uploadSync" : "&action=uploadData")
            : (firstChunk ? "?action=uploadSync" : "?action=uploadData");
      }

      if (sessionExpired()) {
        Serial.println("[WATCHDOG] Session timeout before JSON POST - forcing shutdown");
        modem.gracefulShutdown();
        return;
      }

      Serial.printf("[UPLOAD] POSTing JSON to %s (%u bytes)\n",
                    url.c_str(), json.byteLength);
      HttpsPostResult result = modem.httpsPost(url, json.body,
                                               "application/json", authHeader);

      if (result.httpStatus == 200) {
        Serial.printf("[UPLOAD] JSON SUCCESS: HTTP 200, %u readings\n",
                      (unsigned)json.rowCount);
        nowUnix = getRTCTime();
        uploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, nowUnix);
        uploadQueue.purgeUploaded();
        uploadQueue.resetRetryCount();
        anyJsonSuccess = true;
        firstChunk = false;
        // Continue loop for next chunk if more rows remain.
      } else if (result.httpStatus == 400 || result.httpStatus == 401) {
        // Not transient: bad payload or bad credentials.  Do NOT advance the
        // cursor and do NOT increment the retry counter — retrying won't help.
        Serial.printf("[UPLOAD] JSON non-retryable HTTP %d (%s) — not advancing cursor, not retrying\n",
                      result.httpStatus,
                      result.httpStatus == 401 ? "auth/credential issue" : "bad payload");
        break;
      } else {
        // 429, 5xx, or transport error (-1): retry with backoff next window.
        Serial.printf("[UPLOAD] JSON retryable HTTP %d, %s\n",
                      result.httpStatus, result.errorDetail.c_str());
        uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
        break;
      }
    }

    // No status-only push for Supabase: an empty array carries no data, so we
    // simply skip uploading when there are no pending rows.
    if (firstChunk) {
      Serial.println("[UPLOAD] JSON: no pending rows — nothing to upload");
    }

    if (anyJsonSuccess) {
      uploadQueue.resetRetryCount();
    }

    // Emergency purge + graceful shutdown.
    uploadQueue.emergencyPurgeIfFull(80);
    modem.gracefulShutdown();
    Serial.println("[UPLOAD] Modem upload sequence complete (JSON path)");
    return;
  }

  // -----------------------------------------------------------------------
  // CSV fallback path (single POST) — existing behaviour, unchanged.
  // -----------------------------------------------------------------------
  // 3. Get new data from cursor
  UploadPayload payload = uploadQueue.getNewData(txSettings.maxBytesPerSession);
  if (payload.byteLength == 0) {
    Serial.println("[UPLOAD] No new data to upload");
    modem.gracefulShutdown();
    return;
  }
  Serial.printf("[UPLOAD] Payload: %u bytes, ~%u rows\n", payload.byteLength, payload.rowEstimate);

  // 4. Build URL with auth token
  String url = buildUploadUrl(txSettings);
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout before HTTPS upload - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // 5. HTTPS POST
  Serial.printf("[UPLOAD] POSTing to %s\n", url.c_str());
  HttpsPostResult result = modem.httpsPost(url, payload.csvData, "text/plain", txSettings.authToken);

  // 6. Handle result (302 is a valid success for Google Apps Script redirects)
  if (result.success) {
    Serial.printf("[UPLOAD] SUCCESS: HTTP %d, %u bytes uploaded\n", result.httpStatus, payload.byteLength);
    uint32_t nowUnix = getRTCTime();
    uploadQueue.advanceCursor(payload.startOffset + payload.byteLength, nowUnix);
    uploadQueue.purgeUploaded();
    uploadQueue.resetRetryCount();
  } else {
    Serial.printf("[UPLOAD] FAIL: HTTP %d, %s\n", result.httpStatus, result.errorDetail.c_str());
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
  }

  // 7. Emergency purge check (regardless of upload success)
  uploadQueue.emergencyPurgeIfFull(80);

  // 8. Graceful shutdown
  modem.gracefulShutdown();
  Serial.println("[UPLOAD] Modem upload sequence complete");
}

// ---------------------------------------------------------------------------
// Sync wake handler
// ---------------------------------------------------------------------------
void handleSyncWake() {
  Serial.println("=== SYNC WAKE ===");
  const uint32_t sessionStartMs = millis();
  bool sessionTimedOut = false;
  setLed(true);

  // Load sync interval from NVS (gSyncIntervalMin is only set during config mode)
  loadWakeIntervalFromNVS();
  loadSyncModeFromNVS();
  loadDailySyncTimeFromNVS();
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  Serial.printf("[SYNC] Loaded from NVS: wake=%d min sync=%d min mode=%s daily=%02d:%02d\n",
                gWakeIntervalMin, gSyncIntervalMin,
                gSyncMode == SYNC_MODE_DAILY ? "daily" : "interval",
                gSyncDailyHour, gSyncDailyMinute);

  // --- Schedule transition detection (interval mode) -----------------------
  // If the wake/sync interval changed during config mode, the persisted anchor
  // still holds the OLD sync interval+phase that the sleeping fleet's A2
  // alarms are aligned to, and config shutdown armed THIS mothership to that
  // OLD schedule so it meets the nodes now. The nodes are only awake during
  // this sync window, so we must hand them the NEW schedule here and then both
  // sides re-anchor to it (commit happens at the re-arm below). We detect the
  // transition BEFORE the window by comparing the desired sync interval (from
  // the NVS wake setting) against the interval stored in the anchor.
  const int newSyncIntervalMin = gSyncIntervalMin;  // desired (NEW) from NVS wake
  loadSyncRuntimeGuardsFromNVS();                   // loads OLD anchor into globals
  const int anchorSyncIntervalMin = gSyncIntervalMin;
  gSyncIntervalMin = newSyncIntervalMin;            // restore NEW for upload policy
  const bool scheduleTransitionPending =
      (gSyncMode == SYNC_MODE_INTERVAL) &&
      (newSyncIntervalMin > 0) &&
      (anchorSyncIntervalMin > 0) &&
      (newSyncIntervalMin != anchorSyncIntervalMin);
  uint32_t transitionNewPhase = 0;
  if (scheduleTransitionPending) {
    // Minute-align the current meeting slot. The SAME value is announced to
    // the fleet during the window and used for the mothership re-anchor, so
    // both sides arm to identical slot boundaries regardless of small drift.
    const uint32_t nowUnix = getRTCTime();
    transitionNewPhase = (nowUnix / 60UL) * 60UL;
    // Point the phase anchor at the new slot NOW so any status object built
    // during this session's upload reports the correct next-sync time. Without
    // this, nextSyncLocal is computed from the NEW interval but the OLD phase
    // (the anchor isn't committed until the re-arm below), giving a wrong
    // displayed time. The re-arm reloads + commits the same transitionNewPhase,
    // so this early assignment doesn't affect scheduling.
    gLastSyncBroadcastUnix = transitionNewPhase;
    Serial.printf("[SYNC] Transition pending: old sync=%d min -> new sync=%d min, new phase=%lu (announcing this window)\n",
                  anchorSyncIntervalMin, newSyncIntervalMin,
                  (unsigned long)transitionNewPhase);
  }

  // Init subsystems
  const RtcInitStatus rtcStatus = initRTC();
  if (rtcStatus != RTC_OK) {
    Serial.printf("[FATAL] RTC %s - sync scheduling is unsafe\n",
                  rtcStatus == RTC_PRESENT_TIME_INVALID ? "time invalid" : "absent");
    boundedRetryAndShutdown(rtcStatus == RTC_PRESENT_TIME_INVALID ?
                            "RTC time invalid" : "RTC init failed");
    return;
  }

  if (!initSD()) {
    Serial.println("[WARN] SD card init failed — continuing with flash if available");
  } else {
    Serial.println("[STORAGE] SD card mounted");
  }
  if (!initFlash()) {
    Serial.println("[WARN] Flash init failed — continuing without snapshot logging/upload queue");
  } else {
    Serial.println("[STORAGE] Active snapshot/upload storage: FLASH (LittleFS)");
  }

  // Init upload queue (after flash is ready) and emergency-purge before
  // logging new data so there is always space for incoming node snapshots.
  if (flashIsReady()) {
    uploadQueue.init();
    uploadQueue.emergencyPurgeIfFull(80);
  }

  // Load paired/deployed nodes from NVS so fleet counts and node metadata
  // are available for the JSON upload payload.
  loadPairedNodes();

  // Init ESP-NOW in sync-only mode
  if (!initEspNowSyncOnly(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW init failed — sync window will be empty");
  }
  initSnapQueue(8);

  // Listen for node data for SYNC_WINDOW_MS
  // Broadcast SYNC_WINDOW_OPEN repeatedly every 5 seconds so nodes that wake
  // later (e.g., 10 seconds after the mothership) can still catch the marker.
  // Intelligent early shutdown: exit once all deployed nodes have reported,
  // with a minimum listen time of 15 seconds.

  // Count deployed nodes for early shutdown tracking
  int deployedCount = 0;
  for (const auto& n : registeredNodes) {
    if (n.state == DEPLOYED) deployedCount++;
  }

  Serial.printf("[SYNC] Listening for %d ms (deployed nodes: %d)...\n", SYNC_WINDOW_MS, deployedCount);

  unsigned long startMs = millis();
  unsigned long lastBroadcastMs = 0;
  const unsigned long kMinListenMs = 45000;  // minimum 45s — allows node to flush full queue

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session timeout before ESP-NOW listen - skipping window");
    sessionTimedOut = true;
  }

  while (!sessionTimedOut && millis() - startMs < SYNC_WINDOW_MS) {
    if (millis() - sessionStartMs > kSyncSessionLimitMs) {
      Serial.println("[WATCHDOG] Session timeout during ESP-NOW listen");
      sessionTimedOut = true;
      break;
    }
    // Re-broadcast sync window marker every 5 seconds
    if (millis() - lastBroadcastMs >= 5000 || lastBroadcastMs == 0) {
      broadcastSyncWindowOpen();
      // Keep the fleet's RECORDING interval aligned. SET_SYNC_SCHED carries
      // only the sync schedule, so the wake/recording interval must be pushed
      // separately (SET_SCHEDULE). Broadcast it EVERY window (not just during a
      // transition) — nodes apply it only when it differs, so it's idempotent,
      // and a recording-interval change reaches the fleet on the next sync even
      // after the sync-schedule transition has already committed.
      if (gWakeIntervalMin > 0) {
        broadcastWakeIntervalNow(gWakeIntervalMin);
      }
      // During a schedule transition, hand the NEW sync schedule to the fleet
      // on the same cadence so every node that wakes within the window catches it.
      if (scheduleTransitionPending) {
        broadcastSyncScheduleNow(newSyncIntervalMin, transitionNewPhase);
      }
      lastBroadcastMs = millis();
    }
    EspNowSnapSlot slots[4];
    int drained = drainSnapQueue(slots, 4);
    for (int i = 0; i < drained; ++i) {
      processSnapshot(&slots[i].snap, slots[i].mac);
    }

    // Check if all deployed nodes have synced (after minimum listen time)
    if (deployedCount > 0 && (millis() - startMs) >= kMinListenMs) {
      int syncedCount = 0;
      for (const auto& n : registeredNodes) {
        // A node is considered synced if it was seen recently (within this sync window)
        if (n.state == DEPLOYED && n.isActive && (millis() - n.lastSeen) < kMinListenMs) {
          syncedCount++;
        }
      }
      if (syncedCount >= deployedCount) {
        Serial.printf("[SYNC] All %d deployed nodes synced — shutting down early (after %lu ms)\n",
                      syncedCount, millis() - startMs);
        break;
      }
    }

    delay(10);
  }

  Serial.println("[SYNC] Sync window closed");

  // Persist paired-node state (including freshly reported battery voltages)
  // to NVS so it survives power-off between sync cycles.
  savePairedNodes();

  // Drain packets already accepted before unregistering the producer.
  EspNowSnapSlot finalSlots[4];
  int finalDrained = 0;
  do {
    finalDrained = drainSnapQueue(finalSlots, 4);
    for (int i = 0; i < finalDrained; ++i) {
      processSnapshot(&finalSlots[i].snap, finalSlots[i].mac);
    }
  } while (finalDrained > 0);

  // Stop the WiFi-task producer before upload or purge code touches files.
  deinitEspNowSync();

  // --- LTE upload phase ---
  // Entirely conditional on txSettings.enabled — a complete no-op when
  // disabled, with no serial spam.  Upload failure never blocks the sync
  // wake from completing; local logging is always primary.
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session timeout before upload - skipping upload");
    sessionTimedOut = true;
  }

  if (!sessionTimedOut && txSettings.enabled && flashIsReady()) {
    uploadQueue.incrementWakeCounter();

    // Determine upload policy: uploadIntervalMin=0 means every wake.
    // Otherwise compute how many sync wakes to skip.
    uint8_t policyWakes = 1;  // default: every wake
    if (txSettings.uploadIntervalMin > 0 && gSyncIntervalMin > 0) {
      policyWakes = (uint8_t)(txSettings.uploadIntervalMin / gSyncIntervalMin);
      if (policyWakes < 1) policyWakes = 1;
    }

    if (uploadQueue.shouldUploadThisWake(policyWakes)) {
      float batV = readBatteryVoltage();
      uint16_t batMv = (uint16_t)(batV * 1000);

      if (batMv >= txSettings.minBatteryMv) {
        if (!uploadQueue.maxRetriesExceeded(txSettings.maxRetriesPerWindow, getRTCTime())) {
          if (uploadQueue.getPendingBytes() > 0) {
            performModemUpload(txSettings, sessionStartMs);
            if (millis() - sessionStartMs > kSyncSessionLimitMs) {
              Serial.println("[WATCHDOG] Session timeout during upload - proceeding to alarm re-arm");
              sessionTimedOut = true;
            }
          } else {
            Serial.println("[UPLOAD] No new data to upload");
          }
        } else {
          Serial.printf("[UPLOAD] Max retries (%u) exceeded — skipping\n", txSettings.maxRetriesPerWindow);
        }
      } else {
        Serial.printf("[UPLOAD] Battery %u mV < min %u mV — skipping\n", batMv, txSettings.minBatteryMv);
      }
    } else {
      Serial.println("[UPLOAD] Not scheduled this wake — skipping");
    }
  }

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session limit reached - forcing alarm re-arm and shutdown");
    sessionTimedOut = true;
  }

  // Re-arm according to the configured schedule mode.
  loadSyncRuntimeGuardsFromNVS();

  // Detect a schedule transition: if the wake interval was changed in
  // config mode, the anchor still holds the OLD sync interval (used to
  // wake the mothership at the time the nodes expected).  Now that the
  // sync window has run and nodes have applied the new desired config,
  // re-anchor to the NEW schedule so both mothership and nodes converge.
  const int desiredSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  if (gSyncMode == SYNC_MODE_INTERVAL &&
      desiredSyncIntervalMin > 0 &&
      gSyncIntervalMin > 0 &&
      desiredSyncIntervalMin != gSyncIntervalMin) {
    // Commit the transition. Re-anchor to the SAME minute-aligned phase that
    // was announced to the fleet (broadcastSyncScheduleNow above) so the
    // mothership and nodes arm to identical slot boundaries. The nodes applied
    // this phase via SET_SYNC_SCHED and re-armed A2 to the same boundary.
    uint32_t newPhase = transitionNewPhase;
    if (newPhase == 0) {
      // Defensive fallback if the transition wasn't detected pre-window
      // (e.g. anchor was missing then). Use the current minute slot.
      const uint32_t nowUnix = getRTCTime();
      newPhase = (nowUnix / 60UL) * 60UL;
    }
    Serial.printf("[SYNC] Schedule transition commit: %d->%d min, re-anchored phase=%lu\n",
                  gSyncIntervalMin, desiredSyncIntervalMin,
                  (unsigned long)newPhase);
    gLastSyncBroadcastUnix = newPhase;
    gSyncIntervalMin = desiredSyncIntervalMin;
    saveSyncRuntimeGuardsToNVS();
  }

  if (gSyncMode == SYNC_MODE_DAILY) {
    if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
      Serial.println("[FATAL] Failed to arm daily alarm - starting bounded recovery");
      boundedRetryAndShutdown("Daily alarm arm failed");
      return;
    }
  } else {
    const int syncInterval = (gSyncIntervalMin > 0) ?
                             gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN;
    const uint32_t phaseUnix = static_cast<uint32_t>(gLastSyncBroadcastUnix);
    if (!armNextSyncAlarmPhase(syncInterval, phaseUnix)) {
      Serial.println("[FATAL] Failed to arm next sync alarm - starting bounded recovery");
      boundedRetryAndShutdown("Sync alarm arm failed");
      return;
    }
  }

  // Verify alarm is properly set before power-down
  if (!verifyAlarmSet()) {
    Serial.println("[FATAL] Alarm verification failed - starting bounded recovery");
    boundedRetryAndShutdown("Alarm verification failed");
    return;
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

  // Keep the config request latched for the whole session. It is cleared only
  // during the final shutdown sequence, after the RTC alarm is armed.
  setLed(true);

  // Step 1: Init RTC for time display and alarm scheduling
  Serial.println("[CFG-DBG] Step 1: initRTC...");
  Serial.flush();
  const RtcInitStatus configRtcStatus = initRTC();
  if (configRtcStatus == RTC_ABSENT) {
    Serial.println("[WARN] RTC init failed in config mode — continuing without RTC");
  }
  if (configRtcStatus == RTC_PRESENT_TIME_INVALID) {
    Serial.println("[WARN] RTC time invalid in config mode - set the clock before shutdown");
  }
  Serial.println("[CFG-DBG] Step 1 done: initRTC");
  Serial.flush();

  // Step 2: Init storage: try SD first, fall back to flash (LittleFS)
  Serial.println("[CFG-DBG] Step 2: initSD / initFlash...");
  Serial.flush();
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
  Serial.println("[CFG-DBG] Step 2 done: initSD / initFlash");
  Serial.flush();

  // Step 3: Load paired nodes
  Serial.println("[CFG-DBG] Step 3: loading paired nodes...");
  Serial.flush();
  loadPairedNodes();
  Serial.println("[CFG-DBG] Step 3 done: loadPairedNodes OK");
  Serial.flush();

  // Step 4: Load wake interval from NVS
  Serial.println("[CFG-DBG] Step 4: loadWakeIntervalFromNVS...");
  Serial.flush();
  loadWakeIntervalFromNVS();
  Serial.println("[CFG-DBG] Step 4 done: loadWakeIntervalFromNVS");
  Serial.flush();

  // Step 5: Compute auto sync min
  Serial.println("[CFG-DBG] Step 5: computeAutoSyncMin...");
  Serial.flush();
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  Serial.println("[CFG-DBG] Step 5 done: computeAutoSyncMin");
  Serial.flush();

  // Step 6: Load sync mode from NVS
  Serial.println("[CFG-DBG] Step 6: loadSyncModeFromNVS...");
  Serial.flush();
  loadSyncModeFromNVS();
  Serial.println("[CFG-DBG] Step 6 done: loadSyncModeFromNVS");
  Serial.flush();

  // Step 7: Load daily sync time from NVS
  Serial.println("[CFG-DBG] Step 7: loadDailySyncTimeFromNVS...");
  Serial.flush();
  loadDailySyncTimeFromNVS();
  Serial.println("[CFG-DBG] Step 7 done: loadDailySyncTimeFromNVS");
  Serial.flush();

  // Step 8: Load sync runtime guards from NVS
  Serial.println("[CFG-DBG] Step 8: loadSyncRuntimeGuardsFromNVS...");
  Serial.flush();
  loadSyncRuntimeGuardsFromNVS();
  Serial.println("[CFG-DBG] Step 8 done: loadSyncRuntimeGuardsFromNVS");
  Serial.flush();

  Serial.printf("[CONFIG] wake=%d min sync=%d min mode=%s daily=%02d:%02d\n",
                gWakeIntervalMin, gSyncIntervalMin,
                (gSyncMode == 1) ? "interval" : "daily",
                gSyncDailyHour, gSyncDailyMinute);

  // Step 9: Init ESP-NOW in config mode (AP+STA on ESPNOW_CHANNEL)
  Serial.println("[CFG-DBG] Step 9: initEspNowConfig...");
  Serial.flush();
  if (!initEspNowConfig(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW config init failed — continuing without ESP-NOW");
  }
  Serial.println("[CFG-DBG] Step 9 done: initEspNowConfig");
  Serial.flush();

  // Step 10: Start WiFi AP + web server
  Serial.println("[CFG-DBG] Step 10: startConfigServer...");
  Serial.flush();
  startConfigServer();
  Serial.println("[CFG-DBG] Step 10 done: startConfigServer");
  Serial.flush();

  // Config server loop — exit from the web UI or on the 10-minute timeout.
  unsigned long configStartMs = millis();
  const unsigned long kConfigTimeoutMs = 10UL * 60UL * 1000UL;  // 10 min

  // Config button is for WAKING only — not for exiting.
  // The SN74LVC2G74 latch bounces unpredictably, so we don't poll it for exit.
  // Exit is via web UI /shutdown route or 10-minute timeout.
  Serial.println("[CONFIG] Config mode active. Use web UI Resume Sync button or wait 10 min.");

  while (true) {
    configServerLoop();

    // Exit condition 1: web UI shutdown button
    if (gShutdownRequested) {
      Serial.println("[CONFIG] Sync & Power Down requested via web UI — exiting config mode");
      gShutdownRequested = false;
      break;
    }

    // Exit condition 2: 10-minute timeout
    if (millis() - configStartMs > kConfigTimeoutMs) {
      Serial.println("[CONFIG] 10-min timeout reached — exiting config mode");
      break;
    }

    delay(10);
  }

  // 7. Save NVS + re-arm alarm before power-down
  // Load the anchor BEFORE saving so we can detect a schedule change.
  // If the wake interval was changed during config mode, the anchor still
  // holds the OLD sync interval that the nodes' A2 alarms are armed to.
  // We must wake at the OLD schedule to meet the nodes, push the new
  // desired config, then switch to the new schedule at the next sync wake.
  loadSyncRuntimeGuardsFromNVS();
  const int anchorSyncIntervalMin = gSyncIntervalMin;  // interval from the loaded anchor
  const uint32_t anchorPhaseUnix = (uint32_t)gLastSyncBroadcastUnix;
  const int desiredSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  const bool scheduleChangedDuringConfig =
      (gSyncMode == SYNC_MODE_INTERVAL) &&
      (desiredSyncIntervalMin != anchorSyncIntervalMin) &&
      (anchorSyncIntervalMin > 0);

  // Save the OLD interval+phase to the anchor so that:
  //  (a) this shutdown arms the alarm to the OLD schedule (meet nodes), and
  //  (b) handleSyncWake at the next wake detects the mismatch between
  //      the anchor's OLD interval and the NVS wake interval's NEW sync,
  //      triggering the schedule transition re-anchor.
  // Do NOT save the new interval here — the transition is completed in
  // handleSyncWake after the nodes have been contacted.
  if (scheduleChangedDuringConfig) {
    Serial.printf("[CONFIG] Schedule changed during config: arming OLD sync %d min phase=%lu to meet nodes (new sync=%d min pending)\n",
                  anchorSyncIntervalMin, (unsigned long)anchorPhaseUnix,
                  desiredSyncIntervalMin);
    // gSyncIntervalMin and gLastSyncBroadcastUnix already hold the OLD
    // values from loadSyncRuntimeGuardsFromNVS() — save them as-is.
    saveSyncRuntimeGuardsToNVS();
  } else {
    // No schedule change — save normally with the current values.
    gSyncIntervalMin = desiredSyncIntervalMin;
    saveSyncRuntimeGuardsToNVS();
  }

  const RtcInitStatus shutdownRtcStatus = initRTC();
  if (shutdownRtcStatus != RTC_ABSENT) {
    if (shutdownRtcStatus == RTC_PRESENT_TIME_INVALID) {
      Serial.println("[WARN] RTC time remains invalid - arming best-effort alarm");
    }
    if (gSyncMode == 1) {
      // Interval mode — phase-aligned with nodes
      uint32_t phaseUnix = (uint32_t)gLastSyncBroadcastUnix;
      if (!armNextSyncAlarmPhase(gSyncIntervalMin > 0 ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN, phaseUnix)) {
        Serial.println("[FATAL] Failed to arm next sync alarm - starting bounded recovery");
        boundedRetryAndShutdown("Config wake sync alarm arm failed");
        return;
      }
    } else {
      // Daily mode
      if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
        Serial.println("[FATAL] Failed to arm daily alarm - starting bounded recovery");
        boundedRetryAndShutdown("Config wake daily alarm arm failed");
        return;
      }
    }
    if (!verifyAlarmSet()) {
      Serial.println("[FATAL] Alarm verification failed - starting bounded recovery");
      boundedRetryAndShutdown("Config wake alarm verification failed");
      return;
    }
  } else {
    Serial.println("[FATAL] RTC not available - cannot arm alarm; starting bounded recovery");
    boundedRetryAndShutdown("Config wake RTC unavailable");
    return;
  }

  Serial.println("[CONFIG] Powering down.");
  setLed(false);
  clearConfigLatch();
  delay(100);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// Service wake handler (USB connected)
// ---------------------------------------------------------------------------
void handleServiceWake() {
  Serial.println("=== SERVICE WAKE (USB) ===");
  Serial.println("[SERVICE] No config button, no RTC alarm.");
  Serial.println("[SERVICE] Powering off. Press config button to enter config mode.");

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

  // Arm a conservative fallback before wake classification or long-running
  // sync/upload work. Preserve the flag that caused an RTC wake because the
  // rescue alarm's verified commit clears A1F by design.
  bool rtcAlarmPendingAtBoot = false;
  const RtcInitStatus bootRtcStatus = initRTC();
  if (bootRtcStatus != RTC_ABSENT) {
    rtcAlarmPendingAtBoot = readAlarmFlag();
    if (armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN)) {
      Serial.printf("[BOOT] Rescue alarm armed for %d minutes\n", DEFAULT_SYNC_INTERVAL_MIN);
    } else {
      Serial.println("[BOOT] Warning: failed to arm rescue alarm");
    }
  } else {
    Serial.println("[BOOT] Warning: RTC unavailable; rescue alarm not armed");
  }

  // Project started — first-ever boot timestamp.  Stored once in NVS
  // namespace "tx" under "first_boot" and never overwritten, so it
  // survives reboots and firmware updates.  Only written when the RTC
  // has a valid time (after 2020-01-01) to avoid storing a garbage
  // timestamp from an unsynced RTC.
  {
    Preferences prefs;
    if (prefs.begin("tx", false)) {
      uint32_t firstBoot = prefs.getUInt("first_boot", 0);
      if (firstBoot == 0) {
        // RTC not yet synced — defer writing until we have a valid time.
        // Check again on every boot; once the RTC is set (via config mode
        // "Set time" or NTP sync), the next boot will capture it.
        uint32_t nowUnix = getRTCTime();
        if (nowUnix > 1577836800UL) {  // after 2020-01-01 00:00:00 UTC
          firstBoot = nowUnix;
          prefs.putUInt("first_boot", firstBoot);
          Serial.printf("[BOOT] Project started at unix=%u\n", (unsigned)firstBoot);
        } else {
          Serial.println("[BOOT] RTC not synced — deferring projectStarted write");
        }
      }
      prefs.end();
      g_projectStartedUnix = firstBoot;
    } else {
      Serial.println("[BOOT] Warning: could not open NVS \"tx\" for project-start");
    }
  }

  // Print battery voltage at boot
  float vBat = readBatteryVoltage();
  Serial.printf("[PWR] Battery: %.3f V\n", vBat);

  // Capture wake inputs independently. Config wins if the button latch and
  // RTC alarm are active at the same time.
  WakeSources sources = detectWakeSources();
  sources.rtcAlarm = sources.rtcAlarm || rtcAlarmPendingAtBoot;
  WakeReason reason = selectWakeReason(sources);
  printWakeSources(sources);
  printWakeReason(reason);

  // PWR_HOLD is already secure. Clear a pending RTC flag only after both wake
  // inputs have been captured, including when config mode takes priority.
  if (sources.rtcAlarm && !clearAlarmFlag()) {
    Serial.println("[WAKE] Warning: failed to clear RTC alarm flag");
  }

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
    case WAKE_UNKNOWN:
      Serial.println("[WAKE] Unknown wake reason (RTC read failed) - arming rescue and shutting down");
      boundedRetryAndShutdown("WAKE_UNKNOWN: RTC status read failed");
      break;
    default:
      Serial.println("[WAKE] Unknown wake reason — defaulting to sync");
      handleSyncWake();
      break;
  }
}

void loop() {
  // If we reach here, releasePwrHold() didn't cut power.
  // This happens when USB/SW10 is still holding VSYS after flashing.
  // Just keep trying to release PWR_HOLD — the board will die when
  // the user flicks SW10 to remove USB power.
  static unsigned long lastRetry = 0;
  if (millis() - lastRetry > 2000) {
    Serial.println("[WAIT] Board still alive — flick SW10 to cut USB power.");
    digitalWrite(PIN_PWR_HOLD, LOW);
    lastRetry = millis();
  }
  delay(100);
}
