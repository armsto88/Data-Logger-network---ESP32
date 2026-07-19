// Config-mode WiFi AP + web server for Mothership V1.
// Ported from production main.cpp — adapted to use V1's rtc_alarm.h and
// flash_logger.h instead of production's rtc_manager.h / sd_manager.h.

#include "config/config_server.h"
#include "config/node_registry.h"
#include "config/transmission_settings.h"
#include "storage/upload_queue.h"
#include "storage/json_payload.h"
#include "comms/modem_driver.h"
#include "comms/espnow_config.h"
#include "time/rtc_alarm.h"
#include "storage/flash_logger.h"
#include "system/power.h"
#include "system/pins.h"
#include "system/hardware_identity.h"
#include "protocol.h"
#include "firmware_identity.h"
#include "ota/mothership_selfupdate.h"
#include "command_dispatcher.h"
#include "control/backend_command_ingest.h"
#include "control/node_config_control.h"
#include "esp_ota_ops.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <RTClib.h>
#include <qrcode.h>
#include <vector>

// ---------------------------------------------------------------------------
// Device identification and WiFi
// ---------------------------------------------------------------------------
const char* DEVICE_ID = "001";
// Config-mode AP SSID — reassigned to hwApSsid() ("FieldHub(<MAC>)") in
// startConfigServer() once eFuse/WiFi are ready. The AP is open (no password).
static String ssid = "FieldHub";
const char* FW_VERSION = "v1.0.0";
// Git build id injected by scripts/fw_version.py; __DATE__/__TIME__ freeze
// across cached reflashes (see memory: flash-recovery-bootloader).
#ifndef FW_GIT
#define FW_GIT "nogit"
#endif
const char* FW_BUILD   = FW_GIT;

// ---------------------------------------------------------------------------
// Sync globals + NVS
// ---------------------------------------------------------------------------
Preferences gPrefs;

int gWakeIntervalMin = 0;
static constexpr int kSyncFillK = 18;
int computeAutoSyncMin(int wakeMin) {
  return (wakeMin > 0) ? (wakeMin * kSyncFillK) : 0;
}
int gSyncIntervalMin = 0;
int gSyncDailyHour = 6;
int gSyncDailyMinute = 0;
long gLastSyncBroadcastEpochDay = -1;
int gSyncMode = SYNC_MODE_INTERVAL;
unsigned long gLastSyncBroadcastMs = 0;
unsigned long gLastSyncBroadcastUnix = 0;
long long gLastSyncIntervalSlot = -1;
const int kAllowedIntervals[] = {1, 5, 10, 20, 30, 60};
const size_t kAllowedCount = sizeof(kAllowedIntervals) / sizeof(kAllowedIntervals[0]);

struct __attribute__((packed)) SyncAnchorRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;
  uint32_t phaseUnix;
  uint16_t intervalMin;
  uint8_t mode;
  uint8_t reserved;
  uint32_t crc;
};

static constexpr uint32_t kSyncAnchorMagic = 0x53594E43UL;
static constexpr uint16_t kSyncAnchorVersion = 1;
static constexpr uint32_t kMinValidPhaseUnix = 1704067200UL;  // 2024-01-01 UTC
static constexpr const char* kSyncAnchorA = "sync_anchor_a";
static constexpr const char* kSyncAnchorB = "sync_anchor_b";

struct RecordingIntervalMember {
  char nodeId[CMD_NODEID_LEN];
  uint16_t wireConfigVersion;
  uint16_t sensorMask;
  uint8_t targetState;
  uint8_t converged;
};

struct RecordingIntervalPlan {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  char commandId[CMD_ID_LEN];
  uint32_t dispatcherRevision;
  uint32_t syncPhaseUnix;
  uint16_t syncIntervalMin;
  uint8_t wakeIntervalMin;
  uint8_t memberCount;
  RecordingIntervalMember members[CMD_MAX_NODES];
  uint32_t checksum;
};

static constexpr uint32_t kRecordingPlanMagic = 0x464D5249UL;  // FMRI
static constexpr uint16_t kRecordingPlanVersion = 1;
static constexpr const char* kRecordingPlanNs = "rec_interval";
static constexpr const char* kRecordingPlanA = "plan_a";
static constexpr const char* kRecordingPlanB = "plan_b";
static RecordingIntervalPlan gRecordingPlan{};

static uint32_t recordingPlanChecksum(RecordingIntervalPlan plan) {
  plan.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&plan);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(plan); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

static bool recordingPlanValid(const RecordingIntervalPlan& plan) {
  return plan.magic == kRecordingPlanMagic &&
         plan.version == kRecordingPlanVersion &&
         plan.size == sizeof(plan) && plan.memberCount <= CMD_MAX_NODES &&
         plan.checksum == recordingPlanChecksum(plan);
}

static bool readRecordingPlan(Preferences& prefs, const char* key,
                              RecordingIntervalPlan& plan) {
  return prefs.getBytesLength(key) == sizeof(plan) &&
         prefs.getBytes(key, &plan, sizeof(plan)) == sizeof(plan) &&
         recordingPlanValid(plan);
}

static bool persistRecordingPlan() {
  RecordingIntervalPlan candidate = gRecordingPlan;
  candidate.magic = kRecordingPlanMagic;
  candidate.version = kRecordingPlanVersion;
  candidate.size = sizeof(candidate);
  candidate.generation = gRecordingPlan.generation + 1U;
  candidate.checksum = recordingPlanChecksum(candidate);
  const char* key = (candidate.generation & 1U)
      ? kRecordingPlanA : kRecordingPlanB;
  Preferences prefs;
  if (!prefs.begin(kRecordingPlanNs, false)) return false;
  if (prefs.isKey(key)) prefs.remove(key);
  const bool wrote = prefs.putBytes(key, &candidate, sizeof(candidate)) ==
                     sizeof(candidate);
  prefs.end();
  if (!wrote || !prefs.begin(kRecordingPlanNs, true)) return false;
  RecordingIntervalPlan verify{};
  const bool verified = readRecordingPlan(prefs, key, verify) &&
                        memcmp(&candidate, &verify, sizeof(candidate)) == 0;
  prefs.end();
  if (verified) gRecordingPlan = candidate;
  return verified;
}

static uint32_t syncAnchorCrc(const SyncAnchorRecord& record) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&record);
  const size_t length = offsetof(SyncAnchorRecord, crc);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

static bool syncAnchorIntervalValid(uint16_t interval, uint8_t mode) {
  if (mode == SYNC_MODE_DAILY && interval == 0) return true;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == static_cast<uint16_t>(kAllowedIntervals[i] * kSyncFillK)) return true;
  }
  return interval == 60;  // conservative first-boot/default rescue interval
}

static bool syncAnchorValid(const SyncAnchorRecord& record) {
  return record.magic == kSyncAnchorMagic &&
         record.version == kSyncAnchorVersion &&
         record.phaseUnix >= kMinValidPhaseUnix &&
         (record.mode == SYNC_MODE_DAILY || record.mode == SYNC_MODE_INTERVAL) &&
         syncAnchorIntervalValid(record.intervalMin, record.mode) &&
         record.crc == syncAnchorCrc(record);
}

static bool readSyncAnchor(Preferences& prefs, const char* key,
                           SyncAnchorRecord& record) {
  if (prefs.getBytesLength(key) != sizeof(record)) return false;
  return prefs.getBytes(key, &record, sizeof(record)) == sizeof(record);
}

static SyncAnchorRecord makeSyncAnchor(uint16_t generation) {
  SyncAnchorRecord record{};
  record.magic = kSyncAnchorMagic;
  record.version = kSyncAnchorVersion;
  record.generation = generation;
  record.phaseUnix = static_cast<uint32_t>(gLastSyncBroadcastUnix);
  record.intervalMin = static_cast<uint16_t>(max(gSyncIntervalMin, 0));
  record.mode = static_cast<uint8_t>(gSyncMode);
  record.crc = syncAnchorCrc(record);
  return record;
}

// ---------------------------------------------------------------------------
// Web server + DNS
// ---------------------------------------------------------------------------
WebServer server(80);
DNSServer dnsServer;

volatile bool gShutdownRequested = false;

// Upload queue instance for config-mode UI (separate from the sync-wake
// global in main.cpp, which is only active during a sync wake).
UploadQueue gUploadQueue;

// ---------------------------------------------------------------------------
// Non-blocking node-discovery state machine
// ---------------------------------------------------------------------------
// Driven from configServerLoop() (the main loop) — NOT the ESP-NOW receive
// callback. The POST handler only flips gDiscovery.active and returns
// immediately; broadcasts are scheduled with millis() so the web server, DNS
// and ESP-NOW callback keep running. Node responses are registered into
// registeredNodes by the existing onEspNowRecv callback; the browser learns
// about them by polling /api/live.
struct DiscoveryUiState {
  bool     active = false;
  uint32_t startedMs = 0;
  uint32_t nextBroadcastMs = 0;
  uint8_t  broadcastsSent = 0;
  uint16_t foundAtStart = 0;   // registeredNodes.size() when the scan began
  uint32_t generation = 0;     // bumps each scan so the UI can detect new scans
  uint8_t  lastResult = 0;     // 0 idle/running, 1 found, 2 none, 3 failed, 4 timeout
};
static DiscoveryUiState gDiscovery;
static constexpr uint32_t kDiscoveryTimeoutMs      = 8000;
static constexpr uint32_t kDiscoveryBroadcastGapMs = 1200;
static constexpr uint8_t  kDiscoveryMaxBroadcasts  = 7;

// Wrap-safe "has now reached/passed deadline?" for millis() timestamps.
static inline bool timeReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

static void startDiscovery() {
  if (gDiscovery.active) return;  // already scanning — ignore re-entry (no overlap)
  const uint32_t now = millis();
  gDiscovery.active = true;
  gDiscovery.startedMs = now;
  gDiscovery.nextBroadcastMs = now;  // first burst on the next loop tick
  gDiscovery.broadcastsSent = 0;
  gDiscovery.foundAtStart = (uint16_t)registeredNodes.size();
  gDiscovery.generation++;
  gDiscovery.lastResult = 0;
  Serial.printf("[DISCOVERY] scan %lu started (nodes=%u)\n",
                (unsigned long)gDiscovery.generation, (unsigned)gDiscovery.foundAtStart);
}

static void discoveryTick() {
  if (!gDiscovery.active) return;
  const uint32_t now = millis();

  if (gDiscovery.broadcastsSent < kDiscoveryMaxBroadcasts &&
      timeReached(now, gDiscovery.nextBroadcastMs)) {
    const bool ok = sendDiscoveryBroadcast();   // single non-blocking esp_now_send (+1 retry)
    gDiscovery.broadcastsSent++;
    gDiscovery.nextBroadcastMs = now + kDiscoveryBroadcastGapMs;
    if (!ok && gDiscovery.broadcastsSent >= kDiscoveryMaxBroadcasts &&
        registeredNodes.size() == gDiscovery.foundAtStart) {
      gDiscovery.active = false;
      gDiscovery.lastResult = 3;  // every broadcast failed and nothing was found
      Serial.println("[DISCOVERY] failed: broadcasts did not send");
      return;
    }
  }

  if (timeReached(now, gDiscovery.startedMs + kDiscoveryTimeoutMs)) {
    const int found = (int)registeredNodes.size() - (int)gDiscovery.foundAtStart;
    gDiscovery.active = false;
    gDiscovery.lastResult = (found > 0) ? 1 : 2;  // found / none
    Serial.printf("[DISCOVERY] scan %lu done: %d new node(s)\n",
                  (unsigned long)gDiscovery.generation, found > 0 ? found : 0);
  }
}

// ---------------------------------------------------------------------------
// RTC helpers (adapt production's rtc_manager API to V1's rtc_alarm.h)
// ---------------------------------------------------------------------------
static uint32_t getRTCTimeUnix() {
  return getRTCTime();
}

static BackendCommandApplyResult executeBackendNodeConfigFromUi(
    const Command& command) {
  const NodeConfigApplyResult applied = controlApplyNodeConfig(command);
  BackendCommandApplyResult result{};
  result.command = applied.command;
  result.durable = applied.durable;
  result.applied = applied.registryApplied;
  result.wireConfigVersion = applied.wireConfigVersion;
  return result;
}

static bool resolveBackendNodeConfigFromUi(const Command& requested,
                                           Command& resolved,
                                           CmdOutcome& rejection) {
  return controlResolveBackendNodeConfig(requested, resolved, rejection);
}

static BackendCommandApplyResult executeBackendRecordingIntervalFromUi(
    const Command& command) {
  return configApplyBackendRecordingInterval(command);
}

static void ingestBackendResponseFromUi(const String& responseBody) {
  const uint32_t rtcBefore = getRTCTimeUnix();
  Serial.printf("[CONTROL] manual HTTP response body bytes=%u\n",
                static_cast<unsigned>(responseBody.length()));
  const BackendIngestResult result = backendIngestUploadResponse(
      responseBody, rtcBefore, rtcBefore >= kMinValidPhaseUnix,
      resolveBackendNodeConfigFromUi,
      executeBackendNodeConfigFromUi,
      executeBackendRecordingIntervalFromUi);
  const uint32_t diagnosticUnix = result.serverTimeUnix >= kMinValidPhaseUnix
      ? result.serverTimeUnix : rtcBefore;
  const bool diagnosticsDurable = backendControlRecordDiagnostics(
      result, responseBody.length(), diagnosticUnix);
  Serial.printf("[CONTROL] manual response=%s rejection=%s bytes=%u commands=%u processed=%u rejected=%u replayed=%u nextCursor=%lu cursor=%lu diagnostics=%s\n",
                backendIngestStatusStr(result.status),
                backendIngestRejectionStr(result.rejection),
                static_cast<unsigned>(responseBody.length()),
                static_cast<unsigned>(result.commandCount),
                static_cast<unsigned>(result.processedCount),
                static_cast<unsigned>(result.rejectedCount),
                static_cast<unsigned>(result.replayedCount),
                static_cast<unsigned long>(result.responseNextCursor),
                static_cast<unsigned long>(result.persistedCursor),
                diagnosticsDurable ? "durable" : "FAILED");
  // Do NOT let the backend response clock override the RTC (see the matching
  // note in main.cpp ingestBackendResponse): the DS3231 set from browser-UTC is
  // the authority. The backend's serverTimeUnix was observed to be LOCAL wall
  // time and clobbered a correct UTC RTC, desyncing the fleet. Diagnostics above
  // still record serverTimeUnix; the RTC is left untouched here.
}

static NodeConfigApplyResult applyLocalDesiredConfig(
    const String& nodeId, const NodeDesiredConfig& desired,
    bool overrideSyncSchedule = false, bool allowUnpair = false) {
  NodeConfigApplyOptions options{};
  options.allowUnpair = allowUnpair;
  options.overrideSyncSchedule = overrideSyncSchedule;
  options.syncIntervalMin = desired.syncIntervalMin;
  options.syncPhaseUnix = desired.syncPhaseUnix;
  // Compare-and-set against the shared dispatcher revision. ESP-NOW RX runs on
  // the WiFi task, so a node's NODE_HELLO/CONFIG_ACK can bump the revision
  // between the read and our submit, yielding OUT_REVISION_CONFLICT — a normal,
  // retryable outcome, not a real failure. Retry once; controlApplyLocalNodeConfig
  // re-reads the live dispatcherRevision() each call, so the second attempt uses
  // the post-conflict revision. Capped at 2 attempts so a pathological ESP-NOW
  // hot-loop can't stall the HTTP handler.
  NodeConfigApplyResult result{};
  for (int attempt = 0; attempt < 2; ++attempt) {
    result = controlApplyLocalNodeConfig(
        nodeId.c_str(), desired.wakeIntervalMin ? desired.wakeIntervalMin : 1,
        desired.targetState, desired.sensorMask, options);
    if (result.command.outcome != OUT_REVISION_CONFLICT) break;
    Serial.printf("[CTRL] %s revision conflict on attempt %d — retrying\n",
                  nodeId.c_str(), attempt + 1);
  }
  Serial.printf("[CTRL] %s local config -> %s rev=%lu wireV=%u durable=%d applied=%d\n",
                nodeId.c_str(), cmdOutcomeStr(result.command.outcome),
                static_cast<unsigned long>(
                    result.command.assignedRevision
                        ? result.command.assignedRevision
                        : result.command.currentRevision),
                static_cast<unsigned>(result.wireConfigVersion),
                result.durable ? 1 : 0, result.registryApplied ? 1 : 0);
  return result;
}

static bool setRTCTime(int year, int month, int day, int hour, int minute, int second) {
  DateTime dt(year, month, day, hour, minute, second);
  setRTCTime(dt.unixtime());
  return true;
}

static const char* kMonthShort[] = {
  "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void getRTCTimeString(char* buf, size_t bufLen) {
  uint32_t u = getRTCTime();
  if (u == 0) {
    snprintf(buf, bufLen, "RTC unset");
    return;
  }
  DateTime dt(u);
  uint8_t m = dt.month();
  if (m < 1) m = 1; if (m > 12) m = 12;
  // Format: "14:32 · 26 Jun 2026"
  snprintf(buf, bufLen, "%02d:%02d \xc2\xb7 %02d %s %04d",
           dt.hour(), dt.minute(), dt.day(), kMonthShort[m], dt.year());
}

static String formatDateTimeDisplay(const DateTime& dt) {
  char b[24];
  uint8_t m = dt.month();
  if (m < 1) m = 1; if (m > 12) m = 12;
  // Format: "14:32 · 26 Jun 2026"
  snprintf(b, sizeof(b), "%02d:%02d \xc2\xb7 %02d %s %04d",
           dt.hour(), dt.minute(), dt.day(), kMonthShort[m], dt.year());
  return String(b);
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
void loadWakeIntervalFromNVS() {
  bool migrated = false;
  if (gPrefs.begin("ui", false)) {
    if (!gPrefs.getBool("wake_v2_init", false)) {
      gPrefs.putInt("wake_min", 5);
      gPrefs.putBool("wake_v2_init", true);
      migrated = true;
    }
    int v = gPrefs.getInt("wake_min", gWakeIntervalMin);
    gPrefs.end();

    bool ok = (v == 0);
    for (size_t i = 0; i < kAllowedCount; i++) {
      if (v == kAllowedIntervals[i]) { ok = true; break; }
    }
    gWakeIntervalMin = ok ? v : 0;
  }
  if (migrated) {
    Serial.println("[UI] wake interval migration applied: defaulted global trigger to 5 min");
  }
}

static bool saveWakeIntervalToNVS(int mins) {
  if (!gPrefs.begin("ui", false)) return false;
  const bool wrote = gPrefs.putInt("wake_min", mins) == sizeof(int32_t);
  const int verify = gPrefs.getInt("wake_min", -1);
  gPrefs.end();
  return wrote && verify == mins;
}

void loadSyncModeFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int m = gPrefs.getInt("sync_mode", SYNC_MODE_INTERVAL);
    gPrefs.end();
    gSyncMode = (m == SYNC_MODE_INTERVAL) ? SYNC_MODE_INTERVAL : SYNC_MODE_DAILY;
  }
}

static void saveSyncModeToNVS(int mode) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("sync_mode", mode);
    gPrefs.end();
  }
}

static const char* syncModeLabel() {
  return (gSyncMode == SYNC_MODE_INTERVAL) ? "Interval" : "Daily";
}

void loadDailySyncTimeFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int hh = gPrefs.getInt("sync_hh", gSyncDailyHour);
    int mm = gPrefs.getInt("sync_mm", gSyncDailyMinute);
    gPrefs.end();
    if (hh < 0 || hh > 23) hh = 6;
    if (mm < 0 || mm > 59) mm = 0;
    gSyncDailyHour = hh;
    gSyncDailyMinute = mm;
  }
}

static void saveDailySyncTimeToNVS(int hh, int mm) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("sync_hh", hh);
    gPrefs.putInt("sync_mm", mm);
    gPrefs.end();
  }
}

void loadSyncRuntimeGuardsFromNVS() {
  if (!gPrefs.begin("ui", false)) return;

  gLastSyncBroadcastEpochDay = gPrefs.getLong("sync_day", -1);
  gLastSyncIntervalSlot = gPrefs.getLong64("sync_slot", -1);

  SyncAnchorRecord anchorA{};
  SyncAnchorRecord anchorB{};
  const bool hasA = readSyncAnchor(gPrefs, kSyncAnchorA, anchorA);
  const bool hasB = readSyncAnchor(gPrefs, kSyncAnchorB, anchorB);
  const bool validA = hasA && syncAnchorValid(anchorA);
  const bool validB = hasB && syncAnchorValid(anchorB);

  const SyncAnchorRecord* selected = nullptr;
  const char* selectedKey = nullptr;
  if (validA && validB) {
    selected = static_cast<int16_t>(anchorB.generation - anchorA.generation) > 0 ?
               &anchorB : &anchorA;
    selectedKey = selected == &anchorB ? kSyncAnchorB : kSyncAnchorA;
  } else if (validA) {
    selected = &anchorA;
    selectedKey = kSyncAnchorA;
  } else if (validB) {
    selected = &anchorB;
    selectedKey = kSyncAnchorB;
  }

  if (selected) {
    gLastSyncBroadcastUnix = selected->phaseUnix;
    gSyncIntervalMin = selected->intervalMin;
    gSyncMode = selected->mode;
    Serial.printf("[SYNC] Loaded anchor %s generation=%u phase=%lu interval=%u mode=%u\n",
                  selectedKey, selected->generation,
                  static_cast<unsigned long>(selected->phaseUnix),
                  selected->intervalMin, selected->mode);
    gPrefs.end();
    return;
  }

  const uint32_t legacyPhase = gPrefs.getULong("sync_last_unix", 0);
  gLastSyncBroadcastUnix = legacyPhase;
  if (legacyPhase >= kMinValidPhaseUnix) {
    Serial.println("[SYNC] Falling back to legacy phase anchor");
    if (!hasA && !hasB) {
      SyncAnchorRecord migrated = makeSyncAnchor(0);
      const size_t written = gPrefs.putBytes(kSyncAnchorA, &migrated, sizeof(migrated));
      Serial.printf("[SYNC] Legacy anchor migration to A: %s\n",
                    written == sizeof(migrated) ? "OK" : "FAILED");
    }
  } else {
    gLastSyncBroadcastUnix = 0;
    // Can't tell from here whether this is a fresh device that has never
    // completed a sync broadcast (expected — e.g. no nodes paired yet) or an
    // established fleet that genuinely lost its anchor, so this stays
    // informational rather than an alarming WARN on every unpaired boot.
    Serial.println("[SYNC] No valid phase anchor in NVS (fresh device, or no sync broadcast yet)");
  }
  gPrefs.end();
}

void saveSyncRuntimeGuardsToNVS() {
  // gLastSyncBroadcastUnix only gets set by an actual SET_SYNC_SCHED broadcast
  // to a paired node — with zero nodes paired (e.g. right after onboarding,
  // before any node has joined) it's still 0. Persisting a record with that
  // phase would fail syncAnchorValid() by design, so skip the write instead of
  // attempting it and logging a misleading "FAILED": there is genuinely no
  // anchor to save yet, not an NVS/write problem.
  if (gLastSyncBroadcastUnix < kMinValidPhaseUnix) {
    Serial.println("[SYNC] No anchor to save yet (no sync broadcast has reached a paired node)");
    return;
  }

  if (!gPrefs.begin("ui", false)) {
    Serial.println("[SYNC] Anchor save failed: NVS unavailable");
    return;
  }

  SyncAnchorRecord anchorA{};
  SyncAnchorRecord anchorB{};
  const bool validA = readSyncAnchor(gPrefs, kSyncAnchorA, anchorA) && syncAnchorValid(anchorA);
  const bool validB = readSyncAnchor(gPrefs, kSyncAnchorB, anchorB) && syncAnchorValid(anchorB);

  uint16_t nextGeneration = 0;
  if (validA && validB) {
    const uint16_t newest = static_cast<int16_t>(anchorB.generation - anchorA.generation) > 0 ?
                            anchorB.generation : anchorA.generation;
    nextGeneration = static_cast<uint16_t>(newest + 1U);
  } else if (validA) {
    nextGeneration = static_cast<uint16_t>(anchorA.generation + 1U);
  } else if (validB) {
    nextGeneration = static_cast<uint16_t>(anchorB.generation + 1U);
  }

  SyncAnchorRecord record = makeSyncAnchor(nextGeneration);
  const char* key = (nextGeneration & 1U) == 0 ? kSyncAnchorA : kSyncAnchorB;
  bool verified = false;
  for (int attempt = 1; attempt <= 2 && !verified; ++attempt) {
    const size_t written = gPrefs.putBytes(key, &record, sizeof(record));
    SyncAnchorRecord readBack{};
    verified = written == sizeof(record) && readSyncAnchor(gPrefs, key, readBack) &&
               memcmp(&record, &readBack, sizeof(record)) == 0 && syncAnchorValid(readBack);
    Serial.printf("[SYNC] Anchor save %s generation=%u attempt=%d: %s\n",
                  key, nextGeneration, attempt, verified ? "OK" : "FAILED");
  }

  // Keep legacy guard fields current for downgrade compatibility.
  gPrefs.putLong("sync_day", gLastSyncBroadcastEpochDay);
  gPrefs.putULong("sync_last_unix", gLastSyncBroadcastUnix);
  gPrefs.putLong64("sync_slot", gLastSyncIntervalSlot);
  gPrefs.end();
}

// ---------------------------------------------------------------------------
// Sync schedule computation
// ---------------------------------------------------------------------------
String formatSyncTimeHHMM(int hh, int mm) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
  return String(b);
}

static bool parseHHMM(const String& hhmm, int& outHour, int& outMinute) {
  if (hhmm.length() != 5 || hhmm[2] != ':') return false;
  if (!isDigit(hhmm[0]) || !isDigit(hhmm[1]) || !isDigit(hhmm[3]) || !isDigit(hhmm[4])) return false;
  int hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  int mm = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  outHour = hh;
  outMinute = mm;
  return true;
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix);

String computeNextSyncIsoLocal() {
  const uint32_t nowUnix = getRTCTimeUnix();
  const uint32_t nextUnix = computeNextSyncUnix(nowUnix);
  DateTime next(nextUnix > 0 ? nextUnix : nowUnix);
  return formatDateTimeDisplay(next);
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix) {
  if (nowUnix <= 946684800UL) return 0;

  if (gSyncMode == SYNC_MODE_INTERVAL) {
    const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
    const uint32_t anchorUnix = (gLastSyncBroadcastUnix > 0)
      ? gLastSyncBroadcastUnix
      : (nowUnix - (nowUnix % 60UL));
    if (nowUnix < anchorUnix) return anchorUnix;
    const uint32_t elapsedSec = nowUnix - anchorUnix;
    const uint32_t slotsElapsed = elapsedSec / periodSec;
    uint32_t next = anchorUnix + (slotsElapsed + 1UL) * periodSec;
    // Ensure the result is at least one full interval in the future
    // (during upload phase, the next slot boundary may be very close)
    while (next <= nowUnix) {
      next += periodSec;
    }
    return next;
  }

  DateTime now(nowUnix);
  DateTime next(now.year(), now.month(), now.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  if (now.unixtime() >= next.unixtime()) {
    DateTime tomorrow(now.unixtime() + 24UL * 60UL * 60UL);
    next = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  }
  return next.unixtime();
}

static String computeNextWakeIsoLocal(int intervalMin, uint32_t nodeLastSeenMs, bool nodeIsActive) {
  if (intervalMin <= 0) return String("n/a");
  const uint32_t nowUnix = getRTCTimeUnix();
  if (nowUnix <= 946684800UL) return String("n/a");

  const uint32_t periodSec = (uint32_t)intervalMin * 60UL;
  uint32_t nextUnix = nowUnix + periodSec;
  bool estimated = true;

  if (nodeLastSeenMs > 0) {
    const uint32_t nowMs = millis();
    const uint32_t ageMs = (nowMs >= nodeLastSeenMs) ? (nowMs - nodeLastSeenMs) : 0;
    const uint32_t ageSec = ageMs / 1000UL;
    const uint32_t anchorUnix = (nowUnix > ageSec) ? (nowUnix - ageSec) : nowUnix;
    nextUnix = anchorUnix + periodSec;
    if (nextUnix <= nowUnix) {
      const uint32_t behind = nowUnix - nextUnix;
      const uint32_t jumps = (behind / periodSec) + 1UL;
      nextUnix += jumps * periodSec;
    }
    estimated = !nodeIsActive;
  }

  DateTime now(nowUnix);
  DateTime next(nextUnix);
  const uint32_t deltaSec = (next.unixtime() > now.unixtime()) ? (next.unixtime() - now.unixtime()) : 0;
  const uint32_t deltaMin = (deltaSec + 59UL) / 60UL;

  String out = formatDateTimeDisplay(next);
  out += " (in ";
  out += String((unsigned long)deltaMin);
  out += " min";
  if (estimated) out += ", est";
  out += ")";
  return out;
}

static bool isAllowedInterval(int interval) {
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) return true;
  }
  return false;
}

// Non-destructively read the persisted sync anchor's interval — the schedule
// the sleeping fleet's A2 alarms are currently aligned to. Returns -1 when no
// valid anchor exists. Does NOT touch sync globals (unlike
// loadSyncRuntimeGuardsFromNVS), so it is safe to call from page handlers.
static int readAnchorSyncIntervalMin() {
  if (!gPrefs.begin("ui", true)) return -1;
  SyncAnchorRecord a{};
  SyncAnchorRecord b{};
  const bool validA = readSyncAnchor(gPrefs, kSyncAnchorA, a) && syncAnchorValid(a);
  const bool validB = readSyncAnchor(gPrefs, kSyncAnchorB, b) && syncAnchorValid(b);
  gPrefs.end();
  if (validA && validB) {
    return (static_cast<int16_t>(b.generation - a.generation) > 0)
      ? (int)b.intervalMin : (int)a.intervalMin;
  }
  if (validA) return (int)a.intervalMin;
  if (validB) return (int)b.intervalMin;
  return -1;
}

static uint32_t recordingTransitionPhaseUnix(uint16_t newSyncMin) {
  const uint32_t oldPhaseUnix = static_cast<uint32_t>(gLastSyncBroadcastUnix);
  const int oldAnchorInterval = readAnchorSyncIntervalMin();
  const uint32_t nowUnix = getRTCTimeUnix();
  uint32_t transition = 0;
  if (gSyncMode == SYNC_MODE_INTERVAL && oldPhaseUnix > 0 &&
      oldAnchorInterval > 0 && nowUnix > 946684800UL) {
    const uint32_t oldPeriodSec = static_cast<uint32_t>(oldAnchorInterval) * 60UL;
    if (nowUnix < oldPhaseUnix) {
      transition = oldPhaseUnix;
    } else {
      const uint32_t slots = (nowUnix - oldPhaseUnix) / oldPeriodSec;
      transition = oldPhaseUnix + (slots + 1UL) * oldPeriodSec;
    }
    transition -= transition % 60UL;
  }
  if (transition == 0) {
    const int savedSync = gSyncIntervalMin;
    gSyncIntervalMin = newSyncMin;
    transition = computeNextSyncUnix(nowUnix);
    gSyncIntervalMin = savedSync;
  }
  return transition;
}

void configInitRecordingIntervalControl() {
  gRecordingPlan = {};
  Preferences prefs;
  if (!prefs.begin(kRecordingPlanNs, true)) return;
  RecordingIntervalPlan a{}, b{};
  const bool aValid = readRecordingPlan(prefs, kRecordingPlanA, a);
  const bool bValid = readRecordingPlan(prefs, kRecordingPlanB, b);
  prefs.end();
  if (aValid && bValid) {
    gRecordingPlan = static_cast<int32_t>(b.generation - a.generation) > 0
        ? b : a;
  } else if (aValid) {
    gRecordingPlan = a;
  } else if (bValid) {
    gRecordingPlan = b;
  }
  // Repair a reset after the registry stored an applied version but before the
  // aggregate command result was advanced.
  for (const auto& node : registeredNodes) {
    if (node.configVersionApplied > 0)
      configMarkRecordingIntervalNodeConverged(
          node.nodeId.c_str(), node.configVersionApplied);
  }
}

BackendCommandApplyResult configApplyBackendRecordingInterval(
    const Command& command) {
  BackendCommandApplyResult out{};
  if (command.type != CMD_SET_RECORDING_INTERVAL ||
      (command.recordingIntervalMin != 1 &&
       command.recordingIntervalMin != 5 &&
       command.recordingIntervalMin != 10 &&
       command.recordingIntervalMin != 20 &&
       command.recordingIntervalMin != 30 &&
       command.recordingIntervalMin != 60)) return out;

  const bool known = dispatcherKnownCmd(command.cmdId);
  if (!known && strncmp(gRecordingPlan.commandId, command.cmdId,
                        CMD_ID_LEN) != 0) {
    RecordingIntervalPlan plan{};
    plan.magic = kRecordingPlanMagic;
    plan.version = kRecordingPlanVersion;
    plan.size = sizeof(plan);
    strlcpy(plan.commandId, command.cmdId, CMD_ID_LEN);
    plan.wakeIntervalMin = command.recordingIntervalMin;
    plan.syncIntervalMin = gSyncMode == SYNC_MODE_INTERVAL
        ? static_cast<uint16_t>(computeAutoSyncMin(command.recordingIntervalMin))
        : 0;
    plan.syncPhaseUnix = recordingTransitionPhaseUnix(plan.syncIntervalMin);

    for (const auto& node : registeredNodes) {
      if (node.state != PAIRED && node.state != DEPLOYED) continue;
      if (plan.memberCount >= CMD_MAX_NODES) return out;
      const NodeDesiredConfig desired = getDesiredConfig(node.nodeId.c_str());
      if (desired.configVersion == UINT16_MAX) return out;
      RecordingIntervalMember& member = plan.members[plan.memberCount++];
      strlcpy(member.nodeId, node.nodeId.c_str(), CMD_NODEID_LEN);
      member.wireConfigVersion = desired.configVersion == 0
          ? 1 : static_cast<uint16_t>(desired.configVersion + 1U);
      member.sensorMask = desired.sensorMask;
      member.targetState = desired.targetState;
    }
    gRecordingPlan = plan;
    if (!persistRecordingPlan()) return out;
  }

  if (strncmp(gRecordingPlan.commandId, command.cmdId, CMD_ID_LEN) != 0 ||
      gRecordingPlan.wakeIntervalMin != command.recordingIntervalMin) return out;

  // The old rendezvous anchor is intentionally untouched. Only the desired
  // schedule changes; both sides meet at the next old slot and transition from
  // the same syncPhaseUnix.
  gWakeIntervalMin = gRecordingPlan.wakeIntervalMin;
  gSyncIntervalMin = gRecordingPlan.syncIntervalMin;
  if (!saveWakeIntervalToNVS(gWakeIntervalMin)) return out;

  for (uint8_t i = 0; i < gRecordingPlan.memberCount; ++i) {
    const RecordingIntervalMember& member = gRecordingPlan.members[i];
    NodeDesiredConfig desired = getDesiredConfig(member.nodeId);
    desired.configVersion = member.wireConfigVersion;
    desired.wakeIntervalMin = gRecordingPlan.wakeIntervalMin;
    desired.syncIntervalMin = gRecordingPlan.syncIntervalMin;
    desired.syncPhaseUnix = gRecordingPlan.syncPhaseUnix;
    desired.targetState = member.targetState;
    desired.sensorMask = member.sensorMask;
    if (!setDesiredConfig(member.nodeId, desired)) return out;
    const NodeDesiredConfig verify = getDesiredConfig(member.nodeId);
    if (verify.configVersion != desired.configVersion ||
        verify.wakeIntervalMin != desired.wakeIntervalMin ||
        verify.syncIntervalMin != desired.syncIntervalMin ||
        verify.syncPhaseUnix != desired.syncPhaseUnix ||
        verify.targetState != desired.targetState ||
        verify.sensorMask != desired.sensorMask) return out;
    for (auto& node : registeredNodes) {
      if (node.nodeId != member.nodeId) continue;
      node.stateChangePending = true;
      node.pendingTargetState = PENDING_TO_DEPLOYED;
      node.pendingSinceMs = millis();
      node.pendingLastAttemptMs = 0;
      break;
    }
  }
  savePairedNodes();

  // ACCEPTED becomes visible only after every desired config above has been
  // written and read back. Redelivery calls dispatcherSubmit idempotently.
  out.command = dispatcherSubmit(command);
  CommandResult stored{};
  if (!dispatcherResultFor(command.cmdId, &stored) ||
      (stored.outcome != OUT_ACCEPTED && stored.outcome != OUT_CONVERGED))
    return out;
  uint32_t revision = 0;
  if (!dispatcherCurrentGlobalCommand(command.cmdId, &revision)) return out;
  if (gRecordingPlan.dispatcherRevision != revision) {
    gRecordingPlan.dispatcherRevision = revision;
    if (!persistRecordingPlan()) return out;
  }

  out.durable = dispatcherEnsureDurable();
  out.applied = out.durable;
  if (gRecordingPlan.memberCount == 0 && stored.outcome == OUT_ACCEPTED) {
    dispatcherMarkGlobalConverged(revision);
    out.durable = dispatcherEnsureDurable();
  }
  Serial.printf("[CONTROL] recording interval desired durable id=%s wake=%u sync=%u phase=%lu nodes=%u revision=%lu replay=%d\n",
                command.cmdId,
                static_cast<unsigned>(gRecordingPlan.wakeIntervalMin),
                static_cast<unsigned>(gRecordingPlan.syncIntervalMin),
                static_cast<unsigned long>(gRecordingPlan.syncPhaseUnix),
                static_cast<unsigned>(gRecordingPlan.memberCount),
                static_cast<unsigned long>(revision), known ? 1 : 0);
  return out;
}

bool configMarkRecordingIntervalNodeConverged(const char* nodeId,
                                              uint16_t configVersion) {
  if (!nodeId || !nodeId[0] || !gRecordingPlan.commandId[0] ||
      gRecordingPlan.dispatcherRevision == 0) return false;
  bool changed = false;
  for (uint8_t i = 0; i < gRecordingPlan.memberCount; ++i) {
    RecordingIntervalMember& member = gRecordingPlan.members[i];
    if (strncmp(member.nodeId, nodeId, CMD_NODEID_LEN) == 0 &&
        configVersion >= member.wireConfigVersion && !member.converged) {
      member.converged = 1;
      changed = true;
    }
  }
  if (changed && !persistRecordingPlan()) return false;
  bool applicabilityChanged = false;
  for (uint8_t i = 0; i < gRecordingPlan.memberCount; ++i) {
    RecordingIntervalMember& member = gRecordingPlan.members[i];
    if (member.converged) continue;
    bool stillAssigned = false;
    for (const auto& node : registeredNodes) {
      if (node.nodeId == member.nodeId &&
          (node.state == PAIRED || node.state == DEPLOYED)) {
        stillAssigned = true;
        break;
      }
    }
    if (!stillAssigned) {
      member.converged = 1;
      applicabilityChanged = true;
    }
  }
  if (applicabilityChanged && !persistRecordingPlan()) return false;
  for (uint8_t i = 0; i < gRecordingPlan.memberCount; ++i) {
    if (!gRecordingPlan.members[i].converged) return changed;
  }
  const bool converged = dispatcherMarkGlobalConverged(
      gRecordingPlan.dispatcherRevision);
  if (converged) {
    Serial.printf("[CONTROL] recording interval CONVERGED id=%s revision=%lu nodes=%u\n",
                  gRecordingPlan.commandId,
                  static_cast<unsigned long>(gRecordingPlan.dispatcherRevision),
                  static_cast<unsigned>(gRecordingPlan.memberCount));
  }
  return changed || applicabilityChanged || converged;
}

// Next time the mothership wakes on the OLD (current-fleet) schedule — i.e.
// the moment it will next meet the sleeping nodes and hand over the new
// schedule. Uses the preserved old phase anchor (gLastSyncBroadcastUnix).
static uint32_t computeNextOldSlotUnix(int oldIntervalMin) {
  const uint32_t nowUnix = getRTCTimeUnix();
  const uint32_t phase = (uint32_t)gLastSyncBroadcastUnix;
  if (oldIntervalMin <= 0 || phase == 0 || nowUnix <= 946684800UL) return 0;
  const uint32_t period = (uint32_t)oldIntervalMin * 60UL;
  if (nowUnix < phase) return phase;
  const uint32_t slots = (nowUnix - phase) / period;
  return phase + (slots + 1UL) * period;
}

// "Next upload" as the user will actually observe it. While a schedule change
// is pending, the fleet is still asleep on the OLD schedule and keeps
// collecting on it until the next old-schedule slot (the handover), when both
// mothership and nodes switch to the new interval together. During that window
// computeNextSyncIsoLocal() is misleading — it projects the not-yet-active new
// interval forward from the preserved old phase, showing a slot that will not
// actually occur. So when a change is pending we report the handover time
// instead. The pending-detection here MUST match buildScheduleTransitionBanner()
// so the banner's "Handover at" and this "Next upload" never disagree.
static String computeNextCollectionIsoLocal() {
  if (gSyncMode == SYNC_MODE_INTERVAL) {
    const int oldSync = readAnchorSyncIntervalMin();
    const int newSync = computeAutoSyncMin(gWakeIntervalMin);
    if (oldSync > 0 && newSync > 0 && oldSync != newSync) {
      const uint32_t handoverUnix = computeNextOldSlotUnix(oldSync);
      if (handoverUnix > 0) return formatDateTimeDisplay(DateTime(handoverUnix));
    }
  }
  return computeNextSyncIsoLocal();
}

// Build a "schedule change pending" banner shown only when the desired sync
// interval (derived from the wake setting) differs from the anchor the fleet
// is currently aligned to. The new schedule is delivered to nodes at the next
// collection on the OLD schedule (see handleSyncWake in main.cpp).
static String buildScheduleTransitionBanner() {
  if (gSyncMode != SYNC_MODE_INTERVAL) return String();
  const int oldSync = readAnchorSyncIntervalMin();
  const int newSync = computeAutoSyncMin(gWakeIntervalMin);
  if (oldSync <= 0 || newSync <= 0 || oldSync == newSync) return String();

  const uint32_t handoverUnix = computeNextOldSlotUnix(oldSync);

  String out;
  out.reserve(700);
  out += F("<div class='section' style='border-color:#E1B17A;background:#fff8e1'>"
           "<h3 style='color:#8a4b00'>Schedule change pending</h3>"
           "<p class='muted'>Active nodes are asleep on the current schedule. They keep "
           "collecting on it until the next scheduled collection, then switch to the new "
           "schedule automatically — no node desync.</p>"
           "<div class='stats' style='margin:0'>"
           "<div class='stat'><strong>Current (fleet)</strong><span class='num' style='font-size:16px'>");
  out += String(oldSync);
  out += F(" min</span></div>"
           "<div class='stat'><strong>After handover</strong><span class='num' style='font-size:16px'>");
  out += String(newSync);
  out += F(" min</span></div>"
           "<div class='stat'><strong>Handover at</strong><span class='num' style='font-size:14px'>");
  if (handoverUnix > 0) {
    out += formatDateTimeDisplay(DateTime(handoverUnix));
  } else {
    out += F("next collection");
  }
  out += F("</span></div></div></div>");
  return out;
}

static String formatBytesUi(uint64_t bytes) {
  char b[24];
  if (bytes < 1024ULL) {
    snprintf(b, sizeof(b), "%lu B", (unsigned long)bytes);
  } else if (bytes < (1024ULL * 1024ULL)) {
    snprintf(b, sizeof(b), "%.1f KB", (double)bytes / 1024.0);
  } else if (bytes < (1024ULL * 1024ULL * 1024ULL)) {
    snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else {
    snprintf(b, sizeof(b), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  }
  return String(b);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static String jsonEscapeLocal(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') { out += "\\n"; }
    else if (c == '\r') { out += "\\r"; }
    else if (c == '\t') { out += "\\t"; }
    else { out += c; }
  }
  return out;
}

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    switch (c) {
      case '&':  out += F("&amp;"); break;
      case '<':  out += F("&lt;"); break;
      case '>':  out += F("&gt;"); break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default:   out += c;
    }
  }
  return out;
}

static const char* nodeStateToString(int s) {
  switch (s) {
    case UNPAIRED: return "UNPAIRED";
    case PAIRED:   return "PAIRED";
    case DEPLOYED: return "DEPLOYED";
    default:       return "UNKNOWN";
  }
}

// Render `text` as a self-contained, offline QR code SVG (no external assets).
// Dark modules are drawn as a single SVG path over a white quiet-zone border,
// so it scans reliably from a laptop webcam. Version 6 / ECC-M comfortably fits
// the ~60-byte hardware-registration deep link with margin to spare.
static String renderQrSvg(const String& text, int modulepx = 6) {
  QRCode qrcode;
  const uint8_t version = 6;                 // 41x41 — ~130B byte-mode at ECC-M
  uint8_t data[256];                         // qrcode_getBufferSize(6) == 211
  if (qrcode_initText(&qrcode, data, version, ECC_MEDIUM, text.c_str()) < 0) {
    return String(F("<p class='muted'>QR unavailable</p>"));
  }
  const int n = qrcode.size;
  const int quiet = 4;                       // required 4-module quiet zone
  const int dim = (n + quiet * 2) * modulepx;

  String svg;
  svg.reserve((size_t)n * n * 8 + 256);
  svg += F("<svg xmlns='http://www.w3.org/2000/svg' width='");
  svg += String(dim);
  svg += F("' height='");
  svg += String(dim);
  svg += F("' viewBox='0 0 ");
  svg += String(dim); svg += F(" "); svg += String(dim);
  svg += F("' role='img' aria-label='FieldHub hardware QR' shape-rendering='crispEdges'>");
  svg += F("<rect width='100%' height='100%' fill='#ffffff'/><path fill='#000000' d='");
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      if (qrcode_getModule(&qrcode, x, y)) {
        const int px = (x + quiet) * modulepx;
        const int py = (y + quiet) * modulepx;
        svg += 'M'; svg += String(px); svg += ' '; svg += String(py);
        svg += 'h'; svg += String(modulepx);
        svg += 'v'; svg += String(modulepx);
        svg += 'h'; svg += String(-modulepx);
        svg += 'z';
      }
    }
  }
  svg += F("'/></svg>");
  return svg;
}

// ---------------------------------------------------------------------------
// CSS / JS (copied verbatim from production main.cpp)
// ---------------------------------------------------------------------------
const char COMMON_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#faf8f4; --panel:#ffffff; --text:#4a4640; --sub:#6b665e; --border:#e8e4dc;
  --primary:#5b7553; --success:#7a9b70; --warn:#c47a5a; --danger:#c45a4a;
  --btn-solid:#5b7553;
  --input-bg:#f5f3ee;
  --input-bg-active:#ede9e2;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(74,74,72,.12);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{scroll-behavior:smooth}
html,body{margin:0;padding:0;background:linear-gradient(180deg,#faf8f4 0%, #e8e4dc 100%);color:var(--text);
  font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif}
a{color:var(--primary);text-decoration:none}
:focus-visible{outline:3px solid rgba(33,150,243,.35);outline-offset:2px}

/* Layout */
.container{max-width:600px;margin:0 auto;padding:var(--sp-3)}
.header{padding:var(--sp-3) 0;text-align:center}
.header-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.header-actions{display:flex;align-items:center;gap:8px}
.h1{font-size:24px;font-weight:700;margin:0}
.top-time{display:flex;align-items:center;justify-content:space-between;gap:10px;background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:14px 16px;margin-bottom:12px;min-height:62px}
.top-time__label{color:var(--sub);font-size:.95rem;font-weight:600}
.top-time__value{font-weight:700;color:#566246;font-size:1.25rem}
.section{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:var(--sp-3);box-shadow:var(--shadow);margin:var(--sp-3) 0}
.section h3{margin:0 0 var(--sp-2);font-size:18px}
.section-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:0 0 var(--sp-2)}
.section-head h3{margin:0}
.muted{color:var(--sub);font-size:.95rem}

/* Stats */
.stats{display:grid;grid-template-columns:1fr 1fr 1fr;gap:var(--sp-1);text-align:center}
.stat{background:#fafafa;border:1px solid var(--border);border-radius:8px;padding:10px}
.stat strong{display:block;font-size:13px;color:var(--sub);margin-bottom:2px}
.stat .num{font-size:18px;font-weight:700}
.stats--kpi .stat{padding:14px}
.stats--kpi .stat strong{font-size:12px;letter-spacing:.02em;text-transform:uppercase}
.stats--kpi .stat .num{font-size:24px}
.stats--kpi .stat--deployed-active{background:rgba(122,155,112,.25);border-color:#7a9b70}
.stats--kpi .stat--paired-active{background:rgba(196,122,90,.25);border-color:#c47a5a}
.stats--kpi .stat--unpaired-active{background:rgba(196,90,74,.20);border-color:#c45a4a}

/* Lists/cards */
.list{display:grid;gap:var(--sp-1)}
.item{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px;display:block;color:inherit}
.item-row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.item--node{
  padding:14px 12px;
  min-height:86px;
  cursor:pointer;
  border-width:2px;
  border-color:#c9d0c3;
  background:linear-gradient(180deg,#ffffff 0%, #f4f7ef 100%);
  box-shadow:0 4px 12px rgba(74,74,72,.12);
  transition:transform .12s ease, box-shadow .12s ease, border-color .12s ease, background .12s ease;
}
.item--node:hover{
  border-color:#9faf97;
  box-shadow:0 8px 18px rgba(74,74,72,.18);
  background:linear-gradient(180deg,#ffffff 0%, #eef3e5 100%);
}
.item--node:focus-visible{
  outline:3px solid rgba(79,109,122,.35);
  outline-offset:2px;
}
.item--node:active{
  transform:translateY(1px) scale(.995);
  box-shadow:0 3px 9px rgba(74,74,72,.14);
}
.node-row{display:grid;grid-template-columns:minmax(0,1fr) auto auto;align-items:center;gap:12px}
.node-main{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.node-name{margin-left:10px}
.node-timing{display:grid;grid-template-columns:auto auto;gap:10px}
.node-timing-cell{display:flex;flex-direction:column;align-items:flex-start;gap:3px;min-width:86px}
.node-timing-label{font-size:.72rem;color:var(--sub);font-weight:700;letter-spacing:.02em;text-transform:uppercase}
.node-timing-value{font-size:.86rem}
.node-status{display:grid;grid-template-columns:auto auto;gap:10px}
.node-status-cell{display:flex;flex-direction:column;align-items:flex-start;gap:3px;min-width:86px}
.item--node .chip{font-weight:600}
.item--node .chip{white-space:nowrap}
.node-select-wrap{display:grid;grid-template-columns:44px minmax(0,1fr);gap:8px;align-items:stretch}
.node-select-control{display:flex;align-items:center;justify-content:center;border:2px solid #c9d0c3;
  border-radius:8px;background:#f7f8f4;cursor:pointer;min-height:86px}
.node-select-control input{width:22px;height:22px;margin:0;accent-color:var(--primary)}
.node-select-wrap.is-selected .node-select-control{border-color:var(--primary);background:rgba(122,155,112,.20)}
.node-select-wrap.is-selected .item--node{border-color:var(--primary)}
.batch-actions{position:sticky;top:74px;z-index:8;margin:0 0 12px;padding:12px;border:1px solid var(--border);
  border-radius:10px;background:rgba(255,255,255,.96);box-shadow:var(--shadow);backdrop-filter:blur(5px)}
.batch-actions__head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}
.batch-actions__buttons{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
.batch-actions__buttons .btn{margin:0;padding:10px 8px;min-height:44px}
.batch-actions__buttons .btn--remove{background:#fff;color:#7a2a20;border-color:#c45a4a}

/* Chips */
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}
.chip--state-deployed{border-color:#7a9b70;background:rgba(122,155,112,.25);color:#3d5e35}
.chip--state-paused{border-color:#5a7fc4;background:rgba(90,127,196,.20);color:#233a6b}
.chip--state-paired{border-color:#c47a5a;background:rgba(196,122,90,.25);color:#8a4a2e}
.chip--state-unpaired{border-color:#c45a4a;background:rgba(196,90,74,.20);color:#7a2a20}
.chip--link-awake{border-color:#b3e5fc;background:#e1f5fe;color:#01579b}
.chip--link-asleep{border-color:#e1bee7;background:#f3e5f5;color:#4a148c}
.chip--link-offline{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}
.chip--cfg-pending{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00}
.chip--cfg-ok{border-color:#c8e6c9;background:#f1f8e9;color:#256029}
.chip--bat-ok{border-color:#7a9b70;background:rgba(122,155,112,.25);color:#3d5e35}
.chip--bat-med{border-color:#c47a5a;background:rgba(196,122,90,.25);color:#8a4a2e}
.chip--bat-low{border-color:#c45a4a;background:rgba(196,90,74,.20);color:#7a2a20}

/* Forms */
.label{display:block;margin:8px 0 6px;color:var(--sub);font-size:.95rem}
.input, input[type="text"], input[type="number"], select{
  width:100%;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--input-bg)
}
.input:focus, input[type="text"]:focus, input[type="number"]:focus, select:focus{
  background:var(--input-bg-active)
}
.help{color:var(--sub);font-size:.85rem;margin-top:6px}
.row{display:flex;gap:var(--sp-1);flex-wrap:wrap}
.col{flex:1 1 220px;min-width:0}
.subpanel{display:none;margin-top:12px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa}
.action-stack .btn{padding:14px 16px;min-height:52px;font-size:1rem}

/* Buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;
  padding:12px 16px;border-radius:8px;border:1px solid var(--border);background:#fff;color:var(--text);
  cursor:pointer;width:100%;margin-top:8px;text-decoration:none}
.btn--primary,.btn--success,.btn--warn{background:var(--btn-solid);color:#fff;border-color:transparent}
.btn--sm{width:auto;min-height:36px;padding:8px 12px;margin-top:0;font-size:.9rem}
.btn--action{padding:14px 16px;min-height:52px;font-size:1rem}
.btn:disabled{opacity:.6;cursor:not-allowed}
.action-choices{display:grid;gap:8px;margin-top:10px}
.action-choice input{position:absolute;opacity:0;pointer-events:none}
.action-choice span{display:flex;align-items:center;justify-content:center;min-height:46px;padding:10px 12px;border-radius:8px;border:1px solid var(--border);font-weight:700;cursor:pointer;transition:filter .12s ease, transform .12s ease, box-shadow .12s ease}
.action-choice--start span{border-color:#7a9b70;background:#fff;color:#3d5e35}
.action-choice--stop span{border-color:#c47a5a;background:#fff;color:#8a4a2e}
.action-choice--unpair span{border-color:#c45a4a;background:#fff;color:#7a2a20}
.action-choice span:hover{filter:brightness(.98)}
.action-choice input:checked + span{box-shadow:0 0 0 2px rgba(79,109,122,.35) inset;transform:translateY(1px)}
.action-choice--start input:checked + span{background:rgba(122,155,112,.25)}
.action-choice--stop input:checked + span{background:rgba(196,122,90,.25)}
.action-choice--unpair input:checked + span{background:rgba(196,90,74,.20)}
.icon{width:1.2em;height:1.2em;display:inline-block;vertical-align:-0.12em;fill:currentColor}
.quick-row{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:0 0 12px}
.quick-row .btn{margin-top:0;min-height:52px}
.quick-row .subpanel{grid-column:2 / 3;margin-top:0}
.quick-row form{margin:0}
.quick-row form .btn{width:100%}

/* Utility */
.center{text-align:center}
.badge{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;color:var(--sub);font-size:.85rem}

/* Time health pills in Node Manager */
.time-health{
  display:flex;
  flex-direction:column;
  align-items:flex-start;
  gap:2px;
  font-size:.85rem;
}
.health-pill{
  display:inline-flex;
  align-items:center;
  justify-content:center;
  padding:2px 8px;
  border-radius:999px;
  font-size:.75rem;
  font-weight:600;
  color:#ffffff;
  white-space:nowrap;
}
.health-fresh{background:#16a34a;}
.health-ok{background:#f97316;}
.health-stale{background:#dc2626;}
.health-unknown{background:#6b7280;}
.health-subtext{
  font-size:.7rem;
  color:#6b7280;
}

@media(max-width:480px){
  /* Prevent horizontal scroll from any wide content */
  body{overflow-x:hidden}

  /* Quick-action grid: 2 columns on phones instead of 3 */
  .quick-row{grid-template-columns:1fr 1fr}
  .quick-row .subpanel{grid-column:1 / -1}

  /* Node cards: stack name above timing/status */
  .node-row{grid-template-columns:1fr;gap:8px}
  .node-timing,.node-status{grid-template-columns:1fr 1fr;gap:6px}
  .node-timing-cell,.node-status-cell{min-width:0}
  .node-select-wrap{grid-template-columns:42px minmax(0,1fr);gap:6px}
  .batch-actions{top:66px}
  .batch-actions__buttons{grid-template-columns:1fr}

  /* Small buttons: meet 44px touch target minimum */
  .btn--sm{min-height:44px;padding:10px 14px}

  /* Reduce section spacing to reduce vertical scrolling */
  .section{margin:10px 0;padding:12px}

  /* Sticky header so navigation stays visible while scrolling */
  .header{position:sticky;top:0;z-index:10;background:var(--bg);padding:10px 0}

  /* KPI stat numbers: prevent text overflow on narrow screens */
  .stats--kpi .stat .num{overflow:hidden;text-overflow:ellipsis}

  /* Top time bar: slightly smaller on phones */
  .top-time{padding:10px 12px;min-height:52px}
  .top-time__value{font-size:1.1rem}
}

@media(min-width:768px){.container{max-width:720px}}

/* --- Responsiveness layer: spinner, loading, discovery, connection --- */
/* Kept tiny and CSS-only — no images, no external assets (ESP32 flash). */
.spinner{display:inline-block;width:1em;height:1em;border:2px solid rgba(120,120,120,.3);
  border-top-color:currentColor;border-radius:50%;animation:fmspin .7s linear infinite;
  vertical-align:-.15em;margin-right:6px}
@keyframes fmspin{to{transform:rotate(360deg)}}
.btn.is-loading{opacity:.9;cursor:progress}
.btn.is-ok{box-shadow:0 0 0 2px rgba(122,155,112,.6) inset}
.btn.is-err{box-shadow:0 0 0 2px rgba(196,90,74,.6) inset}

#ui-status.s-ok{border-color:#7a9b70;color:#3d5e35;background:rgba(122,155,112,.12)}
#ui-status.s-warn{border-color:#c47a5a;color:#8a4b00;background:#fff8e1}
#ui-status.s-err{border-color:#c45a4a;color:#7a2a20;background:rgba(196,90,74,.10)}
#ui-status.s-progress{border-color:#4f6d7a;color:#33525c;background:#eef5f8}

.discovery-panel{margin:10px 0;padding:12px;border:1px solid #b3e5fc;border-radius:8px;background:#e8f6fe}
.discovery-panel .dp-row{display:flex;align-items:center;justify-content:space-between;gap:10px;font-size:.9rem;margin:2px 0}
.discovery-panel strong{display:inline-flex;align-items:center;color:#01579b}
.discovery-panel .dp-msg{color:var(--sub);font-size:.82rem;margin-top:6px}

.node-new{animation:fmflash 1.6s ease-out 1}
@keyframes fmflash{0%{box-shadow:0 0 0 3px #f6d365;background:#fffbe6}70%{background:#fffef6}100%{box-shadow:none}}

.conn-dot{display:inline-flex;align-items:center;gap:5px;font-size:.76rem;color:var(--sub)}
.conn-dot::before{content:'';width:8px;height:8px;border-radius:50%;background:#7a9b70;flex:none}
.conn-dot.c-updating::before{background:#4f9bd6}
.conn-dot.c-warn::before{background:#c47a5a}
.conn-dot.c-err::before{background:#c45a4a}

/* Full-screen loading overlay — shown during page navigation + form submits */
.fm-load-overlay{position:fixed;inset:0;background:rgba(250,249,246,.6);display:none;
  align-items:center;justify-content:center;z-index:9999}
.fm-load-overlay.show{display:flex}
.fm-load-box{display:flex;flex-direction:column;align-items:center;gap:12px;padding:20px 26px;
  background:#fff;border:1px solid var(--border);border-radius:14px;box-shadow:0 10px 34px rgba(0,0,0,.16);
  color:var(--sub);font-size:.92rem}
.fm-load-box .spinner{width:2em;height:2em;border-width:3px;margin:0;color:#7a9b70}

/* --- Persistent navigation: fixed bottom bar (phone) / static row (desktop) --- */
/* Content clears the fixed bar; desktop override below zeroes this. */
body{padding-bottom:calc(70px + env(safe-area-inset-bottom))}
.tabbar{position:fixed;left:0;right:0;bottom:0;z-index:20;display:grid;grid-template-columns:repeat(4,1fr);
  background:var(--panel);border-top:1px solid var(--border);box-shadow:0 -2px 10px rgba(74,74,72,.10);
  padding-bottom:env(safe-area-inset-bottom)}
.tabbar a{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;
  min-height:56px;padding:6px 4px;color:var(--sub);font-size:.72rem;font-weight:600;
  border-top:3px solid transparent;text-decoration:none}
.tabbar a .icon{width:22px;height:22px}
.tabbar a[aria-current="page"]{color:var(--primary);border-top-color:var(--primary);background:rgba(91,117,83,.08)}
@media(hover:hover){.tabbar a:hover{color:var(--text);background:rgba(91,117,83,.06)}}

/* Flat secondary section — border only. Shadow is reserved for primary surfaces. */
.section--flat{box-shadow:none}

/* Primary action card at the end of the page flow (shadowed to stand out from
   the flat secondary sections). Not floating — the fixed tab bar owns the bottom. */
.sticky-action{margin:16px 0;padding:12px;background:var(--panel);border:1px solid var(--border);
  border-radius:var(--radius);box-shadow:var(--shadow)}
.sticky-action .btn{margin-top:0}

/* Tabular numerals so readings/voltage/time/percent don't jitter as they update. */
.num,.top-time__value,.chip--bat-ok,.chip--bat-med,.chip--bat-low{font-variant-numeric:tabular-nums}

@media(min-width:768px){
  .tabbar{position:static;box-shadow:none;border-top:none;border-bottom:1px solid var(--border);
    max-width:720px;margin:0 auto 8px}
  .tabbar a{flex-direction:row;gap:8px;min-height:48px;font-size:.9rem;
    border-top:none;border-bottom:3px solid transparent}
  .tabbar a[aria-current="page"]{border-top-color:transparent;border-bottom-color:var(--primary)}
  body{padding-bottom:0}
}

@media(prefers-reduced-motion:reduce){
  html{scroll-behavior:auto}
  .spinner{animation:none}
  .node-new{animation:none}
  .item--node,.action-choice span,.btn{transition:none}
}
)CSS";

const char COMMON_JS[] PROGMEM = R"JS(
// Full-screen loading overlay for page navigations + (non-async) form submits.
// Makes the ESP32's page-render/transfer gap feel responsive instead of relying
// on the browser's tiny built-in spinner. Async forms manage their own UI.
(function(){
  function ov(){ return document.getElementById('fm-load'); }
  function show(msg){ var o=ov(); if(!o) return; var m=document.getElementById('fm-load-msg');
    if(m && msg) m.textContent=msg; o.classList.add('show'); }
  function hide(){ var o=ov(); if(o) o.classList.remove('show'); }
  window.fmShowLoading=show; window.fmHideLoading=hide;
  document.addEventListener('click', function(e){
    var t=e.target; while(t && t.nodeType!==1) t=t.parentNode;
    var a = (t && t.closest) ? t.closest('a') : null;
    if(!a) return;
    if(a.target==='_blank' || a.hasAttribute('download')) return;
    if(e.metaKey||e.ctrlKey||e.shiftKey||e.button) return;
    var href=a.getAttribute('href')||'';
    if(!href || href.charAt(0)==='#' || href.lastIndexOf('javascript:',0)===0) return;
    show('Loading…');
  }, true);
  document.addEventListener('submit', function(e){
    var f=e.target;
    if(f && f.classList && f.classList.contains('async-form')) return;  // handles own UI
    show('Working…');
  }, true);
  window.addEventListener('pageshow', hide);   // incl. bfcache back/forward
  document.addEventListener('DOMContentLoaded', hide);
})();

// Status messages: kind = 'ok' | 'warn' | 'err' | 'progress'. ARIA-live region.
// Transient (ok/progress) auto-clear; errors persist until replaced.
function showUiStatus(message, kind){
  var box = document.getElementById('ui-status');
  if (!box) return;
  box.className = 'help s-' + (kind || 'ok');
  box.style.display = 'block';
  box.setAttribute('role','status');
  box.textContent = message;
  if (box._t){ clearTimeout(box._t); box._t = null; }
  if (kind === 'ok' || kind === 'progress'){
    box._t = setTimeout(function(){ box.style.display = 'none'; }, 4000);
  }
}

function asFormBody(form){
  var data = new FormData(form);
  data.append('ajax', '1');
  return new URLSearchParams(data);
}

// AbortController may be absent in very old captive-portal webviews — guard it
// so polling/fetch still works (just without a hard timeout) on those.
function fmAbort(){ return (typeof AbortController !== 'undefined') ? new AbortController() : null; }

// Restrained connection indicator.
function setConnection(state){
  var el = document.getElementById('conn-status');
  if (!el) return;
  el.className = 'conn-dot' + (state==='updating' ? ' c-updating'
                            : state==='warn' ? ' c-warn'
                            : state==='err' ? ' c-err' : '');
  el.textContent = state==='updating' ? 'Updating…'
                 : state==='warn' ? 'Connection interrupted'
                 : state==='err' ? 'Reconnecting…' : 'Connected';
}

// --- Async button loading states (contextual labels + inline spinner) ---
function btnLabelFor(form, btn){
  if (btn && btn.getAttribute('data-loading-label')) return btn.getAttribute('data-loading-label');
  var a = (form && form.getAttribute('action')) || '';
  if (a.indexOf('find-stations')>=0 || a.indexOf('discover')>=0) return 'Searching…';
  if (a.indexOf('save')>=0 || a.indexOf('transmission')>=0) return 'Saving…';
  if (a.indexOf('recording-interval')>=0 || a.indexOf('wake-interval')>=0) return 'Applying…';
  if (a.indexOf('sync')>=0) return 'Updating…';
  if (a.indexOf('manual-upload')>=0) return 'Uploading…';
  if (a.indexOf('start')>=0 || a.indexOf('shutdown')>=0) return 'Starting…';
  if (a.indexOf('set-time')>=0) return 'Setting time…';
  return 'Working…';
}
function setBtnLoading(btn, label){
  if (!btn) return;
  if (btn._orig == null) btn._orig = (btn.tagName==='INPUT') ? btn.value : btn.innerHTML;
  btn.disabled = true;
  btn.setAttribute('aria-busy','true');
  btn.classList.add('is-loading');
  if (btn.tagName==='INPUT') btn.value = label || 'Working…';
  else btn.innerHTML = '<span class="spinner" aria-hidden="true"></span>' + (label || 'Working…');
}
function clearBtnLoading(btn){
  if (!btn) return;
  btn.disabled = false;
  btn.removeAttribute('aria-busy');
  btn.classList.remove('is-loading','is-ok','is-err');
  if (btn._orig != null){
    if (btn.tagName==='INPUT') btn.value = btn._orig; else btn.innerHTML = btn._orig;
    btn._orig = null;
  }
}
function flashBtn(btn, ok){
  if (!btn) return;
  btn.classList.remove('is-loading');
  btn.classList.add(ok ? 'is-ok' : 'is-err');
}

// --- Node cards: incremental, XSS-safe (textContent for device strings) ---
function chipState(state, paused, pending, desiredTarget){
  if (pending && Number(desiredTarget)===0) return ['chip chip--state-unpaired','Remove queued'];
  if (state==='DEPLOYED' && pending && Number(desiredTarget)===3) return ['chip chip--state-paused','Pause queued'];
  if (state==='DEPLOYED' && pending && Number(desiredTarget)===2) return ['chip chip--state-deployed','Resume queued'];
  if (state==='DEPLOYED') return paused ? ['chip chip--state-paused','Paused']
                                        : ['chip chip--state-deployed','Active'];
  if (state==='PAIRED')   return ['chip chip--state-paired','Connected'];
  return ['chip chip--state-unpaired','New'];
}
function chipBatt(v){
  if (v===null || v===undefined) return ['chip','n/a'];
  var c = v>=3.9 ? 'chip chip--bat-ok' : v>=3.5 ? 'chip chip--bat-med' : 'chip chip--bat-low';
  return [c, v.toFixed(2)+'V'];
}
function lastSeenTxt(sec){
  if (sec===null || sec===undefined || sec<0) return 'n/a';
  return Math.floor(sec/60) + ' min ago';
}
function nodeCell(parentClass, labelText, fieldName){
  var d=document.createElement('div'); d.className=parentClass;
  var l=document.createElement('span'); l.className='node-timing-label'; l.textContent=labelText;
  var v=document.createElement('span'); v.setAttribute('data-f', fieldName);
  d.appendChild(l); d.appendChild(v); return d;
}
function applyNodeFields(card, n){
  if (card && card.dataset){ card.dataset.state=n.state||''; card.dataset.paused=n.paused?'1':'0'; }
  var st=chipState(n.state, n.paused, n.pending, n.desiredTarget), s=card.querySelector('[data-f="status"]');
  if (s){ s.className=st[0]; s.textContent=st[1]; }
  var bt=chipBatt(n.batV), b=card.querySelector('[data-f="batt"]');
  if (b){ b.className=bt[0]; b.textContent=bt[1]; }
  var r=card.querySelector('[data-f="rec"]'); if (r){ r.className='chip node-timing-value'; r.textContent=(n.recMin||0)+' min'; }
  var ls=card.querySelector('[data-f="lastseen"]'); if (ls){ ls.className='chip node-timing-value'; ls.textContent=lastSeenTxt(n.lastSeenSec); }
  var lbl=card.querySelector('[data-f="label"]'); if (lbl) lbl.textContent = n.label || n.id;
}
function nodeCardEl(n){
  var wrap=document.createElement('div'); wrap.className='node-select-wrap node-new';
  wrap.setAttribute('data-node-id', n.id);
  var pick=document.createElement('label'); pick.className='node-select-control'; pick.title='Select node';
  var cb=document.createElement('input'); cb.type='checkbox'; cb.name='node_id'; cb.value=n.id;
  cb.className='node-select'; cb.setAttribute('form','batch-node-actions'); cb.setAttribute('aria-label','Select '+(n.label||n.id));
  cb.addEventListener('change',function(){ if(window.updateBatchSelection) window.updateBatchSelection(cb); });
  pick.appendChild(cb); wrap.appendChild(pick);
  var a=document.createElement('a');
  a.className='item item--node';
  a.href='/station?id=' + encodeURIComponent(n.id);
  var row=document.createElement('div'); row.className='node-row';
  var main=document.createElement('div'); main.className='node-main';
  var strong=document.createElement('strong'); strong.setAttribute('data-f','label'); main.appendChild(strong);
  var status=document.createElement('div'); status.className='node-status';
  status.appendChild(nodeCell('node-status-cell','Status','status'));
  status.appendChild(nodeCell('node-status-cell','Battery','batt'));
  var timing=document.createElement('div'); timing.className='node-timing';
  timing.appendChild(nodeCell('node-timing-cell','Recording','rec'));
  timing.appendChild(nodeCell('node-timing-cell','Last seen','lastseen'));
  row.appendChild(main); row.appendChild(status); row.appendChild(timing);
  a.appendChild(row); wrap.appendChild(a); applyNodeFields(wrap, n); return wrap;
}
function findCard(list, id){
  var k=list.children;
  for (var i=0;i<k.length;i++){ if (k[i].getAttribute && k[i].getAttribute('data-node-id')===id) return k[i]; }
  return null;
}
// Update existing cards in place; append new ones (highlighted). We do NOT
// remove cards that drop out of a poll — a node going briefly silent should not
// make its card vanish; explicit unpair reloads the page.
function reconcileNodes(nodes){
  var list=document.getElementById('node-list');
  if (!list || !nodes) return;
  for (var i=0;i<nodes.length;i++){
    var n=nodes[i], card=findCard(list, n.id);
    if (!card) list.appendChild(nodeCardEl(n));
    else applyNodeFields(card, n);
  }
  var empty=document.getElementById('node-empty');
  if (empty) empty.style.display = nodes.length ? 'none' : '';
}
function updateKpis(f){
  var set=function(id,v){ var e=document.getElementById(id); if (e && v!=null) e.textContent=String(v); };
  set('kpi-deployed-num', f.active);
  set('kpi-paired-num', f.connected);
  set('kpi-unpaired-num', f.new);
}
function setText(id,v){ var e=document.getElementById(id); if (e) e.textContent=v; }

// --- Live poller: one timer, visibility-aware, failure backoff, no overlap ---
// Polls the RAM-only /api/live endpoint. Cadence adapts: fast during discovery
// or just after an action, slow when idle, paused when the tab is hidden. This
// keeps steady-state load on the ESP32 low.
var FM = {
  timer:null, inFlight:false, fails:0, lastVersion:-1, discActive:false, fastUntil:0, started:false,
  IDLE:4000, FAST:650, BUSY_WINDOW:4000,
  start:function(){
    if (this.started) return;
    // Only poll on pages that actually display live data — keeps idle load off
    // the ESP32 on static form-result pages.
    if (!document.getElementById('conn-status') && !document.getElementById('node-list') &&
        !document.getElementById('discovery-panel') && !document.getElementById('kpi-deployed-num')) return;
    this.started=true;
    document.addEventListener('visibilitychange', function(){
      if (document.hidden) FM.clear(); else FM.bump();
    });
    this.tick();
  },
  clear:function(){ if (this.timer){ clearTimeout(this.timer); this.timer=null; } },
  delay:function(){
    if (document.hidden) return 0;
    if (this.fails>0) return Math.min(8000, 1000*Math.pow(2, this.fails-1));
    return (this.discActive || Date.now()<this.fastUntil) ? this.FAST : this.IDLE;
  },
  schedule:function(){ this.clear(); var d=this.delay(); if (d>0) this.timer=setTimeout(function(){FM.tick();}, d); },
  bump:function(){ this.fastUntil=Date.now()+this.BUSY_WINDOW; this.clear(); this.tick(); },
  startDiscoveryUi:function(){ this.discActive=true; var p=document.getElementById('discovery-panel'); if(p) p.hidden=false; this.bump(); },
  tick:function(){
    if (this.inFlight || document.hidden){ this.schedule(); return; }
    this.inFlight=true;
    if (this.fails===0) setConnection('updating');
    var ctrl=fmAbort(); var to=ctrl?setTimeout(function(){ctrl.abort();},4000):null;
    fetch('/api/live', {cache:'no-store', signal:ctrl?ctrl.signal:undefined})
      .then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })
      .then(function(d){ if(to)clearTimeout(to); FM.fails=0; setConnection('ok'); FM.apply(d); })
      .catch(function(_){ if(to)clearTimeout(to); FM.fails++; setConnection(FM.fails>2?'err':'warn'); })
      .then(function(){ FM.inFlight=false; FM.schedule(); });
  },
  apply:function(d){
    var disc = d.discovery || {};
    if (disc.active){
      this.discActive=true;
      var p=document.getElementById('discovery-panel'); if(p){ p.hidden=false; if(p._h){clearTimeout(p._h);p._h=null;} }
      setText('dp-state','Searching');
      setText('dp-elapsed', Math.floor((disc.elapsedMs||0)/1000)+'s');
      setText('dp-found', String(disc.foundThisScan||0));
      var fb=document.getElementById('find-btn');
      if (fb && !fb.classList.contains('is-loading')) setBtnLoading(fb,'Searching for nodes…');
    } else if (this.discActive){
      this.discActive=false;
      this.onDiscoveryDone(disc);
    }
    if (d.version !== this.lastVersion){
      this.lastVersion = d.version;
      if (d.fleet) updateKpis(d.fleet);
      if (d.nodes) reconcileNodes(d.nodes);
    }
  },
  onDiscoveryDone:function(disc){
    var found = disc ? (disc.foundThisScan||0) : 0, res = disc ? disc.result : 0;
    var msg = res===1 ? (found+' new node'+(found===1?'':'s')+' found')
            : res===2 ? 'No new nodes found'
            : res===3 ? 'Discovery failed'
            : res===4 ? 'Discovery timed out' : 'Discovery complete';
    showUiStatus(msg, res===1?'ok':'warn');
    setText('dp-state', msg);
    var fb=document.getElementById('find-btn'); if (fb) clearBtnLoading(fb);
    var p=document.getElementById('discovery-panel');
    if (p){ if (p._h) clearTimeout(p._h); p._h=setTimeout(function(){p.hidden=true;p._h=null;}, 4500); }
    this.lastVersion = -1;  // force one more KPI/node refresh after the scan
  }
};

function wireAsyncForms(){
  var forms = document.querySelectorAll('form.async-form');
  for (var i=0;i<forms.length;i++){
    (function(form){
      if (form._wired) return; form._wired = true;
      form.addEventListener('submit', function(e){
        e.preventDefault();
        var btn = form.querySelector('button[type="submit"],input[type="submit"]');
        if (btn && btn.disabled) return;  // prevent duplicate submission
        var action = form.getAttribute('action') || '';
        var isFind = action.indexOf('find-stations')>=0 || action.indexOf('discover')>=0;
        setBtnLoading(btn, btnLabelFor(form, btn));
        if (isFind) FM.startDiscoveryUi();  // immediate panel + fast polling

        var ctrl = fmAbort();
        var to = ctrl ? setTimeout(function(){ ctrl.abort(); }, 15000) : null;
        fetch(form.action, {method:'POST', body:asFormBody(form),
              headers:{'Content-Type':'application/x-www-form-urlencoded'},
              signal: ctrl?ctrl.signal:undefined})
          .then(function(r){ return r.text().then(function(t){ return {ok:r.ok, t:t}; }); })
          .then(function(res){
            if (to) clearTimeout(to);
            var ok = res.ok, msg = res.t;
            try { var j = JSON.parse(res.t); ok = !!j.ok; msg = j.message || (ok?'Done':'Request failed'); } catch(_){}
            if (isFind){
              // Discovery now runs server-side; the live poller owns the button,
              // panel and node list. No full-page reload.
              showUiStatus(ok ? 'Searching for nodes…' : ('Discovery failed: '+msg), ok?'progress':'err');
              if (!ok) clearBtnLoading(btn);
              FM.bump();
            } else {
              showUiStatus(msg, ok?'ok':'err');
              flashBtn(btn, ok);
              setTimeout(function(){ clearBtnLoading(btn); }, ok?700:1200);
              FM.bump();  // promptly refresh KPIs/nodes after the action
            }
          })
          .catch(function(err){
            if (to) clearTimeout(to);
            var aborted = err && err.name==='AbortError';
            showUiStatus(aborted ? 'Request timed out — check connection' : ('Request failed: '+err), 'err');
            if (btn && !isFind){ flashBtn(btn,false); setTimeout(function(){clearBtnLoading(btn);},1200); }
            else if (isFind) clearBtnLoading(btn);
          });
      });
    })(forms[i]);
  }
}

function setCurrentTime(){
  const n=new Date();
  const z=n=>String(n).padStart(2,'0');
  const s=`${z(n.getUTCHours())}:${z(n.getUTCMinutes())}:${z(n.getUTCSeconds())} ${z(n.getUTCDate())}-${z(n.getUTCMonth()+1)}-${n.getUTCFullYear()}`;
  const el=document.getElementById('datetime'); if(el) el.value=s;
}
const MONTH_SHORT=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
function formatHubClock(ms){
  const dt=new Date(ms);
  return `${String(dt.getUTCHours()).padStart(2,'0')}:${String(dt.getUTCMinutes()).padStart(2,'0')} \u00b7 ${String(dt.getUTCDate()).padStart(2,'0')} ${MONTH_SHORT[dt.getUTCMonth()]} ${dt.getUTCFullYear()}`;
}
function toggleSettings(){
  const panel=document.getElementById('settings-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
function toggleGlobalInterval(){
  const panel=document.getElementById('global-interval-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
function toggleInfoPanel(){
  const panel=document.getElementById('info-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
window.addEventListener('DOMContentLoaded', () => {
  setCurrentTime();
  wireAsyncForms();
  FM.start();   // begin adaptive /api/live polling (drives KPIs, nodes, discovery, connection)
});

(function(){
  function parseHubClock(str){
    if (!str) return NaN;
    // Accept "HH:MM · DD Mon YYYY" or legacy "HH:MM:SS DD-MM-YYYY"
    const m = str.match(/^(\d{2}):(\d{2})(?::(\d{2}))?\s*[\u00b7\-]\s*(\d{1,2})\s+([A-Za-z]{3})\s+(\d{4})$/);
    if (m){
      const mon = MONTH_SHORT.indexOf(m[5]);
      if (mon < 0) return NaN;
      const value = Date.UTC(+m[6], mon, +m[4], +m[1], +m[2], m[3] ? +m[3] : 0);
      return isNaN(value) ? NaN : value;
    }
    if (str.length >= 19){
      const H = +str.slice(0,2), M = +str.slice(3,5), S = +str.slice(6,8);
      const d = +str.slice(9,11), mo = +str.slice(12,14), y = +str.slice(15,19);
      const value = Date.UTC(y, mo-1, d, H, M, S);
      return isNaN(value) ? NaN : value;
    }
    return NaN;
  }
  function startClock(){
    const el = document.getElementById('rtc-now');
    if (!el) return;
    const initial = (el.textContent || '').trim();
    const rtcMs   = parseHubClock(initial);
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatHubClock(nowMs);
    }
    draw();
    setInterval(draw, 1000);
  }
  if (document.readyState === 'loading'){
    document.addEventListener('DOMContentLoaded', startClock);
  } else {
    startClock();
  }
})();
)JS";

// ---------------------------------------------------------------------------
// Page frame helpers
// ---------------------------------------------------------------------------
static bool isAjaxRequest() {
  return server.hasArg("ajax") && server.arg("ajax") == "1";
}

static void sendAjaxResult(bool ok, const String& message) {
  String body;
  body.reserve(message.length() + 40);
  body += "{\"ok\":";
  body += ok ? "true" : "false";
  body += ",\"message\":\"";
  body += message;
  body += "\"}";
  server.send(ok ? 200 : 400, "application/json", body);
}

static void sendCaptivePortalLanding() {
  if (server.method() == HTTP_OPTIONS || server.method() == HTTP_HEAD) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(204);
    return;
  }
  server.send(200, "text/html",
              "<!doctype html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Logger Portal</title></head><body>"
              "<h2>Logger Portal</h2><p>Open local dashboard:</p>"
              "<p><a href='http://192.168.4.1/'>http://192.168.4.1/</a></p>"
              "</body></html>");
}

// OS connectivity-check ("captive portal probe") responders. Rather than
// serving a portal page (which makes iOS/Android classify the AP as a
// no-internet captive network and — on Android especially — aggressively drop
// the association mid-setup), each probe gets the exact "you have internet,
// no portal here" answer its OS expects. This keeps the phone stably attached
// to the FieldHub AP while provisioning. The trade is that the OS no longer
// auto-launches the portal, so the setup flow tells the user to open
// http://192.168.4.1 explicitly. (Ref: github.com/tzapu/WiFiManager#114.)
static void sendProbeNoContent() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(204);  // Android generate_204 success + generic "nothing here"
}
static void sendProbeAppleSuccess() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html",
              "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
              "<BODY>Success</BODY></HTML>");
}
static void sendProbeNcsi() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "Microsoft NCSI");
}
static void sendProbeMsConnect() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "Microsoft Connect Test");
}

// Persistent primary navigation. navActive selects the current destination:
// 0=Overview, 1=Stations, 2=Export, 3=Settings; <0 emits nothing (child/result
// pages keep their header Back button). Export is a download route, so its tab is
// a plain link with no aria-current state. Icons reuse the inline SVGs already in
// the page bodies — no external assets.
static String navBarHtml(int navActive) {
  if (navActive < 0) return String();
  auto tab = [](const char* href, const char* label, const char* pathD, bool active) {
    String s = F("<a href='");
    s += href;
    s += F("'");
    if (active) s += F(" aria-current='page'");
    s += F("><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='");
    s += pathD;
    s += F("'/></svg><span>");
    s += label;
    s += F("</span></a>");
    return s;
  };
  String h = F("<nav class='tabbar' aria-label='Primary'>");
  h += tab("/", "Overview",
           "M12 3l9 8h-3v9h-5v-6h-2v6H4v-9H1z", navActive == 0);
  h += tab("/stations", "Nodes",
           "M12 2a4 4 0 110 8 4 4 0 010-8zm-7 12a3 3 0 110 6 3 3 0 010-6zm14 0a3 3 0 110 6 3 3 0 010-6zM9.3 9.8l-3 3.9 1.6 1.2 3-3.9-1.6-1.2zm5.4 0l-1.6 1.2 3 3.9 1.6-1.2-3-3.9z",
           navActive == 1);
  h += tab("/export", "Export",
           "M12 3a1 1 0 011 1v8.59l2.3-2.3 1.4 1.42-4.7 4.7-4.7-4.7 1.4-1.42 2.3 2.3V4a1 1 0 011-1zm-7 14h14v2H5v-2z",
           navActive == 2);
  h += tab("/settings", "Settings",
           "M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 00.12-.61l-1.92-3.32a.488.488 0 00-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.484.484 0 00-.48-.41h-3.84a.485.485 0 00-.48.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 00-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z",
           navActive == 3);
  h += F("</nav>");
  return h;
}

static String headCommon(const String& title, const String& actionsHtml = String(), int navActive = -1) {
  // No-store on every portal page: phones/captive-portal browsers cache the AP
  // pages aggressively, which repeatedly showed stale UI after a reflash. This
  // header is queued for this request's send(); the belt-and-braces meta tags
  // below cover browsers that ignore the HTTP header.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  String h;
  h.reserve(4500);
  h += F("<!DOCTYPE html><html><head>");
  h += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  h += F("<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>"
         "<meta http-equiv='Pragma' content='no-cache'><meta http-equiv='Expires' content='0'>");
  h += F("<style>"); h += FPSTR(COMMON_CSS); h += F("</style>");
  h += F("<script>"); h += FPSTR(COMMON_JS);  h += F("</script>");
  h += F("</head><body>");
  h += F("<div id='fm-load' class='fm-load-overlay'><div class='fm-load-box'>"
         "<span class='spinner'></span><span id='fm-load-msg'>Loading&hellip;</span></div></div>");
  h += F("<div class='container'>");
  h += F("<div class='header'>");
  h += F("<div class='header-top'>");
  h += F("<div class='h1'>"); h += title; h += F("</div>");
  h += F("<div class='header-actions'>");
  if (actionsHtml.length()) {
    h += actionsHtml;
  } else {
    h += F("<a href='/' class='btn btn--sm'>Refresh</a>");
  }
  h += F("</div>");
  h += F("</div>");
  h += F("</div>");
  h += navBarHtml(navActive);
  return h;
}

static inline String footCommon() { return String(F("</div></body></html>")); }

// ---------------------------------------------------------------------------
// Status JSON (renamed from buildBleStatusDataJson)
// ---------------------------------------------------------------------------
static String buildStatusJson() {
  auto allNodes = getRegisteredNodes();

  size_t deployedCount = 0;
  size_t pendingCount = 0;
  size_t pendingToPairedCount = 0;
  size_t pendingToUnpairedCount = 0;
  for (const auto& n : allNodes) {
    if (n.state == DEPLOYED) deployedCount++;
    if (n.stateChangePending) {
      pendingCount++;
      if (n.pendingTargetState == PENDING_TO_PAIRED) pendingToPairedCount++;
      if (n.pendingTargetState == PENDING_TO_UNPAIRED) pendingToUnpairedCount++;
    }
  }

  const size_t pairedCount = getPairedNodes().size();
  const size_t unpairedCount = getUnpairedNodes().size();

  String json;
  json.reserve(420);
  json += "{";
  json += "\"deviceId\":\"";
  json += DEVICE_ID;
  json += "\",";
  json += "\"firmwareVersion\":\"";
  json += FW_VERSION;
  json += "\",";
  json += "\"firmwareBuild\":\"";
  json += FW_BUILD;
  json += "\",";
  json += "\"rtcUnix\":";
  json += String(getRTCTimeUnix());
  json += ",";
  json += "\"apEnabled\":";
  const wifi_mode_t mode = WiFi.getMode();
  const bool apEnabled = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  json += apEnabled ? "true" : "false";
  json += ",";
  json += "\"espnowChannel\":";
  json += String(ESPNOW_CHANNEL);
  json += ",";
  json += "\"wakeIntervalMinutes\":";
  json += String(gWakeIntervalMin);
  json += ",";
  json += "\"syncIntervalMinutes\":";
  json += String(gSyncIntervalMin);
  json += ",";
  json += "\"syncDailyTime\":\"";
  json += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
  json += "\",";
  json += "\"syncMode\":\"";
  json += (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily";
  json += "\",";
  json += "\"nextSyncLocal\":\"";
  json += computeNextCollectionIsoLocal();
  json += "\",";
  json += "\"fleet\":{";
  json += "\"total\":";
  json += String((unsigned)allNodes.size());
  json += ",";
  json += "\"unpaired\":";
  json += String((unsigned)unpairedCount);
  json += ",";
  json += "\"paired\":";
  json += String((unsigned)pairedCount);
  json += ",";
  json += "\"deployed\":";
  json += String((unsigned)deployedCount);
  json += ",";
  json += "\"pending\":";
  json += String((unsigned)pendingCount);
  json += ",";
  json += "\"pendingToPaired\":";
  json += String((unsigned)pendingToPairedCount);
  json += ",";
  json += "\"pendingToUnpaired\":";
  json += String((unsigned)pendingToUnpairedCount);
  json += "}";

  // Upload subsystem status
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);
  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t flashUsagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  json += ",\"upload\":{";
  json += "\"enabled\":";
  json += txSettings.enabled ? "true" : "false";
  json += ",\"cursorOffset\":";
  json += String(cursor.byteOffset);
  json += ",\"pendingBytes\":";
  json += String(pendingBytes);
  json += ",\"pendingRows\":";
  json += String(pendingRows);
  json += ",\"rowsUploaded\":";
  json += String(cursor.rowsUploaded);
  json += ",\"lastUploadUnix\":";
  json += String(cursor.lastUploadUnix);
  json += ",\"retryCount\":";
  json += String(cursor.retryCount);
  json += ",\"flashUsagePct\":";
  json += String(flashUsagePct);
  json += ",\"flashTotalBytes\":";
  json += String((unsigned long)fsTotal);
  json += ",\"flashUsedBytes\":";
  json += String((unsigned long)fsUsed);
  json += "}";

  json += "}";
  return json;
}

// ---------------------------------------------------------------------------
// Lightweight live-status endpoint (/api/live)
// ---------------------------------------------------------------------------
// RAM-only: serialises the node-registry snapshot + discovery state. Unlike
// buildStatusJson() it must NOT touch LittleFS, NVS or the upload queue,
// because the browser polls it every ~0.65-4 s. A fingerprint-derived
// `version` lets the client skip DOM work when nothing changed.
static uint32_t gLiveVersion = 1;
static uint32_t gLiveFingerprint = 0;

static String buildLiveJson() {
  const std::vector<NodeInfo> nodes = getRegisteredNodes();  // snapshot copy (safe read)
  const uint32_t now = millis();

  uint16_t nNew = 0, nConn = 0, nActive = 0, nPending = 0;
  uint32_t fp = 2166136261UL ^ (uint32_t)nodes.size();
  for (const auto& n : nodes) {
    if (n.state == DEPLOYED) nActive++;
    else if (n.state == PAIRED) nConn++;
    else nNew++;
    if (n.stateChangePending || n.deployPending) nPending++;
    const uint32_t battCenti = isnan(n.lastReportedBatV) ? 0u : (uint32_t)(n.lastReportedBatV * 100.0f);
    const DispatchNodeConfig* mirrored = dispatcherNodeConfig(n.nodeId.c_str());
    const bool mirroredPaused = mirrored
        ? mirrored->targetState == 3 : n.recordingPaused;
    const uint8_t desiredTarget = mirrored ? mirrored->targetState
        : (n.state == DEPLOYED ? (n.recordingPaused ? 3 : 2)
                              : static_cast<uint8_t>(n.state));
    fp ^= (uint32_t)n.state + ((uint32_t)n.configVersionApplied << 4) +
          (n.lastSeen / 1000UL) + battCenti + (mirroredPaused ? 97U : 0U) +
          (uint32_t)(n.nodeId.length() * 131U) + (uint32_t)(n.name.length() * 17U) +
          ((uint32_t)n.expectedSensorMask << 9) + ((uint32_t)n.sensorFaultMask << 20) +
          (mirrored ? ((uint32_t)mirrored->wakeIntervalMin << 24) : 0U) +
          ((uint32_t)desiredTarget * 2654435761UL);
    fp *= 16777619UL;  // FNV-style fold — cheap change detector, not a hash guarantee
  }
  fp ^= (gDiscovery.generation * 2654435761UL) + (gDiscovery.active ? 1UL : 0UL) +
        ((uint32_t)gDiscovery.broadcastsSent << 1) + (uint32_t)gDiscovery.lastResult;
  if (fp != gLiveFingerprint) { gLiveFingerprint = fp; gLiveVersion++; }

  const int found = (int)nodes.size() - (int)gDiscovery.foundAtStart;
  String j;
  j.reserve(220 + nodes.size() * 170);
  j += "{\"version\":";   j += String(gLiveVersion);
  j += ",\"uptimeMs\":";  j += String(now);
  j += ",\"discovery\":{\"active\":"; j += gDiscovery.active ? "true" : "false";
  j += ",\"generation\":";     j += String(gDiscovery.generation);
  j += ",\"elapsedMs\":";      j += String(gDiscovery.startedMs ? (now - gDiscovery.startedMs) : 0UL);
  j += ",\"timeoutMs\":";      j += String((uint32_t)kDiscoveryTimeoutMs);
  j += ",\"broadcastsSent\":"; j += String((unsigned)gDiscovery.broadcastsSent);
  j += ",\"foundThisScan\":";  j += String(found > 0 ? found : 0);
  j += ",\"result\":";         j += String((unsigned)gDiscovery.lastResult);
  j += "}";
  j += ",\"fleet\":{\"total\":"; j += String((unsigned)nodes.size());
  j += ",\"new\":";       j += String((unsigned)nNew);
  j += ",\"connected\":"; j += String((unsigned)nConn);
  j += ",\"active\":";    j += String((unsigned)nActive);
  j += ",\"pending\":";   j += String((unsigned)nPending);
  j += "}";
  j += ",\"nodes\":[";
  bool first = true;
  for (const auto& n : nodes) {
    if (!first) j += ",";
    first = false;
    const String disp = n.name.length() ? n.name : (n.userId.length() ? n.userId : n.nodeId);
    const DispatchNodeConfig* mirrored = dispatcherNodeConfig(n.nodeId.c_str());
    const uint8_t obs = (n.wakeIntervalMin > 0) ? n.wakeIntervalMin : n.inferredWakeIntervalMin;
    const int recMin = mirrored && mirrored->wakeIntervalMin > 0
        ? mirrored->wakeIntervalMin
        : ((obs > 0) ? (int)obs : (gWakeIntervalMin > 0 ? gWakeIntervalMin : 5));
    const bool mirroredPaused = mirrored
        ? mirrored->targetState == 3 : n.recordingPaused;
    const uint8_t desiredTarget = mirrored ? mirrored->targetState
        : (n.state == DEPLOYED ? (n.recordingPaused ? 3 : 2)
                              : static_cast<uint8_t>(n.state));
    j += "{\"id\":\"";     j += jsonEscapeLocal(n.nodeId); j += "\"";
    j += ",\"label\":\"";  j += jsonEscapeLocal(disp);     j += "\"";
    j += ",\"userId\":\""; j += jsonEscapeLocal(n.userId); j += "\"";
    j += ",\"state\":\"";  j += nodeStateToString(n.state); j += "\"";
    if (n.lastSeen > 0 && now >= n.lastSeen) { j += ",\"lastSeenSec\":"; j += String((now - n.lastSeen) / 1000UL); }
    else { j += ",\"lastSeenSec\":-1"; }
    if (isnan(n.lastReportedBatV)) { j += ",\"batV\":null"; }
    else { char b[12]; snprintf(b, sizeof(b), "%.2f", n.lastReportedBatV); j += ",\"batV\":"; j += b; }
    j += ",\"recMin\":";  j += String(recMin);
    j += ",\"paused\":";  j += mirroredPaused ? "true" : "false";
    j += ",\"pending\":"; j += (n.stateChangePending || n.deployPending) ? "true" : "false";
    j += ",\"desiredTarget\":"; j += String((unsigned)desiredTarget);
    // Configured-sensor state for the deployment UI: which sensors are marked
    // installed, what reported last cycle, and which configured ones are faulted.
    j += ",\"expectedSensorMask\":"; j += String((unsigned)n.expectedSensorMask);
    j += ",\"sensorPresentMask\":";  j += String((unsigned)n.lastSensorPresent);
    j += ",\"sensorFaultMask\":";    j += String((unsigned)n.sensorFaultMask);
    j += "}";
  }
  j += "]}";
  return j;
}

// ---------------------------------------------------------------------------
// Data status section (adapted to use flash_logger instead of SD)
// ---------------------------------------------------------------------------
static String buildDataStatusSectionHtml() {
  bool hasFile = flashIsReady() && LittleFS.exists("/datalog.csv");
  uint32_t records = 0;
  uint64_t fileBytes = 0;
  String lastConfirmedSync = "n/a";

  if (hasFile) {
    File file = LittleFS.open("/datalog.csv", "r");
    if (file) {
      fileBytes = (uint64_t)file.size();
      uint32_t lineNo = 0;
      String lastDataLine;
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        lineNo++;
        if (lineNo == 1) continue;
        records++;
        lastDataLine = line;
      }
      file.close();
      if (lastDataLine.length() > 0) {
        const int comma = lastDataLine.indexOf(',');
        if (comma > 0) {
          lastConfirmedSync = lastDataLine.substring(0, comma);
        }
      }
    } else {
      hasFile = false;
    }
  }

  const uint64_t totalBytes = (uint64_t)LittleFS.totalBytes();
  const uint64_t usedBytes  = (uint64_t)LittleFS.usedBytes();
  const uint64_t freeBytes  = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;

  String out;
  out.reserve(900);
  out += F("<div class='section'><h3>Data status</h3>");

  out += F("<div class='stats' style='margin:0 0 10px 0'>"
           "<div class='stat'><strong>Records</strong><span class='num'>");
  out += String(records);
  out += F("</span></div>"
           "<div class='stat'><strong>CSV size</strong><span class='num' style='font-size:16px'>");
  out += hasFile ? formatBytesUi(fileBytes) : String("n/a");
  out += F("</span></div>"
           "<div class='stat'><strong>Storage free</strong><span class='num' style='font-size:16px'>");
  out += (totalBytes > 0) ? formatBytesUi(freeBytes) : String("n/a");
  out += F("</span></div></div>");

  if (!hasFile) {
    out += F("<p class='muted'>No data recorded yet.</p>");
  }

  if (totalBytes > 0) {
    const uint32_t usedPct = (uint32_t)((usedBytes * 100ULL) / totalBytes);
    out += F("<p class='muted'><strong>Storage used:</strong> ");
    out += String(usedPct);
    out += F("% used (");
    out += formatBytesUi(usedBytes);
    out += F(" of ");
    out += formatBytesUi(totalBytes);
    out += F(")</p>");
  }

  out += F("<p class='muted'><strong>Last collection:</strong> ");
  out += lastConfirmedSync;
  out += F("</p>");

  // Upload status summary
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);
  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingRows = gUploadQueue.getPendingRows();
  }
  out += F("<p class='muted'><strong>Cloud upload:</strong> ");
  out += txSettings.enabled ? String("enabled") : String("disabled");
  out += F(" &nbsp;|&nbsp; <strong>Readings waiting:</strong> ");
  out += String(pendingRows);
  out += F(" &nbsp;|&nbsp; <strong>Readings sent:</strong> ");
  out += String(cursor.rowsUploaded);
  if (cursor.lastUploadUnix > 0) {
    DateTime lastUp(cursor.lastUploadUnix);
    out += F(" &nbsp;|&nbsp; <strong>Last upload:</strong> ");
    out += formatDateTimeDisplay(lastUp);
  }
  out += F("</p>");

  out += F("<p class='muted'>Use the Export Data page to download a CSV, or <a href='/settings'>Settings</a> to configure cloud upload.</p></div>");
  return out;
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
  // Unprovisioned (no connection key saved) → send the user straight into the
  // first-run setup wizard instead of the normal home page. Once provisioned,
  // "/" behaves exactly as before. The wizard itself stays reachable manually
  // (Settings → "Run setup wizard") regardless of this gate.
  {
    TransmissionSettings txProbe;
    loadTransmissionSettings(txProbe);
    if (txProbe.apiKey.length() == 0) {
      server.sendHeader("Location", "/setup", true);
      server.send(302, "text/plain", "");
      return;
    }
  }

  String html = headCommon("fieldMesh",
    F("<span id='conn-status' class='conn-dot'>Connected</span>"), 0);

  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes) {
    if (node.state == DEPLOYED) deployedNodes++;
  }

  // --- Mothership time bar ---
  html += F("<div class='top-time'><span class='top-time__label'>Mothership time (UTC)</span><span id='rtc-now' class='top-time__value'>");
  html += currentTime;
  html += F("</span></div>");

  // --- Set time (collapsed, near the time display) ---
  html += F("<details style='margin:4px 0 12px 0'>"
            "<summary style='font-size:14px;color:var(--sub);cursor:pointer;padding:4px 8px'>Set time</summary>"
            "<div style='margin-top:8px;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--panel)'>"
            "<p class='muted' style='margin:0 0 8px 0'>Only needed for initial setup or clock correction. Stored time is UTC.</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set new time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='HH:MM:SS DD-MM-YYYY' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Use browser UTC</button>"
            "<button type='submit' class='btn btn--success'>Set Time</button>"
            "</div>"
            "<div class='help'>Example: 21:05:00 14-11-2026</div>"
            "</form>"
            "</div>"
            "</details>");

  // --- Fleet KPI tiles (flat) ---
  html += F("<div class='section section--flat' aria-live='polite'>"
            "<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>"
            "<h3>Nodes</h3>"
            "<div class='stats stats--kpi' style='margin:0'>"
              "<div class='stat");
  if (deployedNodes > 0) html += F(" stat--deployed-active");
  html += F("'><strong>Active</strong><span id='kpi-deployed-num' class='num'>");
  html += String(deployedNodes);
  html += F("</span></div>"
              "<div class='stat");
  if (pairedNodes.size() > 0) html += F(" stat--paired-active");
  html += F("'><strong>Connected</strong><span id='kpi-paired-num' class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div>"
              "<div class='stat");
  if (unpairedNodes.size() > 0) html += F(" stat--unpaired-active");
  html += F("'><strong>New</strong><span id='kpi-unpaired-num' class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div>"
            "</div>");
  // Primary entry to the node manager (also reachable via the Stations tab).
  html += F("<a href='/stations' class='btn btn--success' style='margin-top:12px'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a4 4 0 110 8 4 4 0 010-8zm-7 12a3 3 0 110 6 3 3 0 010-6zm14 0a3 3 0 110 6 3 3 0 010-6zM9.3 9.8l-3 3.9 1.6 1.2 3-3.9-1.6-1.2zm5.4 0l-1.6 1.2 3 3.9 1.6-1.2-3-3.9z'/></svg> Manage nodes</a>");
  html += F("</div>");

  // --- Add stations: Find New Nodes + live discovery progress (flat) ---
  html += F("<div class='section section--flat'>"
            "<h3>Add nodes</h3>"
            "<form class='async-form' action='/find-stations' method='POST' style='margin:0 0 8px'>"
            "<button type='submit' id='find-btn' data-loading-label='Searching for nodes…' class='btn'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3c-3.9 0-7.4 1.6-9.9 4.1l1.4 1.4C5.6 6.4 8.7 5 12 5s6.4 1.4 8.5 3.5l1.4-1.4C19.4 4.6 15.9 3 12 3zm0 5c-2.6 0-5 .9-6.9 2.6l1.4 1.4C8 10.5 9.9 10 12 10s4 .5 5.5 2l1.4-1.4C17 8.9 14.6 8 12 8zm0 5c-1.3 0-2.5.5-3.5 1.5l1.4 1.4c.6-.6 1.3-.9 2.1-.9s1.5.3 2.1.9l1.4-1.4c-1-1-2.2-1.5-3.5-1.5zm0 4a2 2 0 100 4 2 2 0 000-4z'/></svg> Find New Nodes</button>"
            "</form>"
            "<div id='discovery-panel' class='discovery-panel' hidden>"
            "<div class='dp-row'><strong><span class='spinner' aria-hidden='true'></span>"
            "<span id='dp-state'>Searching</span></strong>"
            "<span>Elapsed <span id='dp-elapsed'>0s</span></span></div>"
            "<div class='dp-row'><span>New nodes this scan: <strong id='dp-found'>0</strong></span></div>"
            "<div class='dp-msg'>Keep new nodes powered on and in pairing mode.</div></div>"
            "</div>");

  // --- Hub health: battery + storage + last upload (flat, border-only) ---
  {
    const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
    const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
    const uint32_t storagePct = (fsTotal > 0) ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;
    const float hubBatV = readBatteryVoltage();
    const char* batClass = (hubBatV >= 3.9f) ? "chip--bat-ok"
                         : (hubBatV >= 3.5f) ? "chip--bat-med"
                         : "chip--bat-low";
    const char* batLabel = (hubBatV >= 3.9f) ? "OK"
                         : (hubBatV >= 3.5f) ? "Medium"
                         : "Low";
    char batBuf[10];
    snprintf(batBuf, sizeof(batBuf), "%.2fV", hubBatV);

    html += F("<div class='section section--flat'><h3>Mothership</h3>");
    html += F("<div class='stats' style='margin:0;text-align:left'>");
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Battery</strong><br>");
    html += F("<span class='chip ");
    html += batClass;
    html += F("' style='font-size:16px;font-weight:600'>");
    html += batBuf;
    html += F("</span> <span class='muted'>");
    html += batLabel;
    html += F("</span></div>");
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Storage</strong><br><span class='num' style='font-size:18px'>");
    html += String(storagePct);
    html += F("% used</span><br><span class='muted'>");
    html += (fsTotal > 0) ? formatBytesUi(fsUsed) : String("n/a");
    html += F("</span></div>");
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Last upload</strong><br><span class='num' style='font-size:14px'>");
    {
      UploadCursor cursor = {0,0,0,0,0};
      if (flashIsReady()) { gUploadQueue.init(); cursor = gUploadQueue.getCursor(); }
      if (cursor.lastUploadUnix > 0) {
        DateTime lastUp(cursor.lastUploadUnix);
        html += formatDateTimeDisplay(lastUp);
      } else {
        html += F("Never");
      }
    }
    html += F("</span></div>");
    html += F("</div></div>");
  }

  // --- Schedule change pending (only rendered during a transition) ---
  html += buildScheduleTransitionBanner();

  // --- Collection schedule (flat) ---
  {
    html += F("<div class='section section--flat'>"
              "<h3>Collection schedule</h3>"
              "<div class='stats' style='margin:0'>"
              "<div class='stat'><strong>Recording every</strong><span class='num' style='font-size:16px'>");
    if (gWakeIntervalMin > 0) {
      html += String(gWakeIntervalMin);
      html += F(" min");
    } else {
      html += F("Off");
    }
    html += F("</span></div>"
              "<div class='stat'><strong>Auto upload</strong><span class='num' style='font-size:16px'>");
    html += String(gSyncIntervalMin);
    html += F(" min</span></div>"
              "<div class='stat'><strong>Next upload</strong><span class='num' style='font-size:16px'><span id='kpi-next-sync'>");
    html += computeNextCollectionIsoLocal();
    html += F("</span></span></div>"
              "</div>"
              "</div>");
  }

  // --- Cloud upload status (flat; status shown as a chip, not a bare dot) ---
  {
    TransmissionSettings txSettings;
    loadTransmissionSettings(txSettings);
    UploadCursor cursor = {0, 0, 0, 0, 0};
    if (flashIsReady()) {
      gUploadQueue.init();
      cursor = gUploadQueue.getCursor();
    }

    const char* chipCls = "chip";
    const char* statusLabel = "Upload off";
    if (txSettings.enabled) {
      if (cursor.retryCount > 0) {
        chipCls = "chip chip--bat-med";
        statusLabel = "Last upload failed";
      } else {
        chipCls = "chip chip--cfg-ok";
        statusLabel = "Connected";
      }
    }

    html += F("<div class='section section--flat'>"
              "<h3>Cloud upload</h3>"
              "<p style='margin:4px 0'><span class='");
    html += chipCls;
    html += F("' style='font-weight:600'>");
    html += statusLabel;
    html += F("</span></p>");
    if (cursor.lastUploadUnix > 0) {
      DateTime lastUp(cursor.lastUploadUnix);
      html += F("<p class='muted'>Last upload ");
      html += formatDateTimeDisplay(lastUp);
      html += F(" &middot; ");
      html += String(cursor.rowsUploaded);
      html += F(" readings sent</p>");
    } else {
      html += F("<p class='muted'>No uploads yet &middot; ");
      html += String(cursor.rowsUploaded);
      html += F(" readings sent</p>");
    }
    html += F("</div>");
  }

  // --- Primary action: sticky Finish & Start Recording (pinned above tab bar) ---
  html += F("<div class='sticky-action'>"
            "<form class='async-form' action='/start' method='POST'>"
            "<button type='submit' class='btn btn--primary' style='width:100%;min-height:56px;font-size:18px;font-weight:700'>"
            "▶ Finish &amp; Start Recording"
            "</button>"
            "</form>"
            "<p class='muted' style='text-align:center;margin:6px 0 0'>Saves settings, closes the setup network, and begins recording</p>"
            "</div>");

  // --- About / Advanced collapsed panel ---
  html += F("<details style='margin:16px 0'>"
            "<summary style='font-weight:bold;cursor:pointer;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--panel)'>About / Advanced</summary>"
            "<div class='section' style='margin-top:8px'>"
            "<div class='help'><strong>Hardware code:</strong> ");
  html += htmlEscape(hwCode());
  // Internal radio node ID retained for diagnostics/compatibility only — this is
  // NOT the customer-facing FieldHub identity (that is the hardware code/MAC).
  html += F("<br><strong>Radio node ID (internal):</strong> ");
  html += DEVICE_ID;
  html += F("<br><strong>Firmware:</strong> ");
  html += FW_VERSION;
  html += F("<br><strong>Build:</strong> ");
  html += FW_BUILD;
  html += F("</div>"
            "<div class='help'><strong>WiFi network:</strong> ");
  html += ssid;
  html += F("<br><strong>Portal URL:</strong> http://192.168.4.1/"
            "<br><strong>Mothership address:</strong> ");
  html += getMothershipsMAC();
  html += F("<br><strong>Radio channel:</strong> ");
  html += String(ESPNOW_CHANNEL);
  html += F("</div>"
            "</div>"
            "</details>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetTime() {
  String dt = server.arg("datetime");
  int year, mm, dd, hh, mi, ss;
  const bool parsed =
      sscanf(dt.c_str(), "%d:%d:%d %d-%d-%d", &hh, &mi, &ss, &dd, &mm, &year) == 6 && year >= 2000;
  const bool ok = parsed && setRTCTime(year, mm, dd, hh, mi, ss);

  // AJAX branch (setup wizard / async forms): stay on the page with a JSON
  // result instead of navigating to a standalone confirmation page.
  if (isAjaxRequest()) {
    if (ok)          sendAjaxResult(true, String("Time set to ") + dt);
    else if (!parsed) sendAjaxResult(false, "Invalid time format (HH:MM:SS DD-MM-YYYY)");
    else              sendAjaxResult(false, "Failed to set RTC time");
    return;
  }

  if (ok) {
    String html = headCommon("fieldMesh");
    html += F("<div class='section center'><h3>SUCCESS: RTC Time Updated</h3><p>New time:<br><strong>");
    html += dt;
    html += F("</strong></p><a href='/' class='btn btn--primary'>Back to Main Page</a></div>");
    html += footCommon();
    server.send(200, "text/html", html);
  } else if (!parsed) {
    String html = headCommon("fieldMesh");
    html += F("<div class='section center'><h3>WARNING: Invalid Time Format</h3>"
              "<p>Please use the format: HH:MM:SS DD-MM-YYYY</p><p>You entered: <em>");
    html += dt;
    html += F("</em></p><a href='/' class='btn btn--primary'>Try Again</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
  } else {
    String html = headCommon("fieldMesh");
    html += F("<div class='section center'><h3>ERROR: Failed to Set RTC Time</h3><p>Please try again.</p>"
              "<a href='/' class='btn btn--primary'>Try Again</a></div>");
    html += footCommon();
    server.send(500, "text/html", html);
  }
}

// POST /set-remote-management (ajax) — lightweight standalone toggle used by the
// setup wizard. arg remote_management=1 enables, anything else disables. The
// full Settings form also sets this, but the wizard wants a one-field call that
// doesn't touch every other transmission setting.
static void handleSetRemoteManagement() {
  const bool want = server.hasArg("remote_management") &&
                    server.arg("remote_management") == "1";
  const bool ok = backendControlSetRemoteManagementEnabled(want);
  if (isAjaxRequest()) {
    if (ok) sendAjaxResult(true, want ? "Dashboard changes allowed" : "Dashboard changes off");
    else    sendAjaxResult(false, "Could not save setting");
    return;
  }
  server.sendHeader("Location", "/settings", true);
  server.send(302, "text/plain", "");
}

static void handleDownloadCSV() {
  if (!flashIsReady() || !LittleFS.exists("/datalog.csv")) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  File file = LittleFS.open("/datalog.csv", "r");
  if (!file) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "inline; filename=datalog.csv");
  server.sendHeader("Connection", "close");
  server.streamFile(file, "text/csv");
  file.close();
  Serial.println("[CSV] file downloaded by client");
}

static void handleDiscoverNodes() {
  // Non-blocking: flip the discovery state machine on and return immediately.
  // configServerLoop() schedules the broadcasts with millis(); the browser
  // tracks progress + results via /api/live. A second request while a scan is
  // already running is a no-op (startDiscovery ignores re-entry) and just
  // reports the current scan — no overlapping scans.
  const bool wasActive = gDiscovery.active;
  startDiscovery();

  if (isAjaxRequest()) {
    String b;
    b.reserve(112);
    b += "{\"ok\":true,\"active\":true,\"generation\":";
    b += String(gDiscovery.generation);
    b += wasActive ? ",\"message\":\"Scan already running\"}"
                   : ",\"message\":\"Searching for nodes\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", b);
    return;
  }

  // JS-disabled fallback: a meta-refresh to the Nodes page after the discovery
  // window so non-script browsers still see the results.
  String html = headCommon("fieldMesh");
  html += F("<meta http-equiv='refresh' content='9;url=/stations'>");
  html += F("<div class='section center'><h3>Searching for new nodes…</h3>"
            "<div class='muted'>Keep new nodes powered on and in pairing mode.</div>"
            "<div class='spinner' style='width:36px;height:36px;border-width:4px;margin:16px auto;color:#5b7553'></div>"
            "<p class='muted'><small>Redirecting to Nodes shortly…</small></p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetWakeInterval() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = (interval == 0);
  for (size_t i = 0; i < kAllowedCount; ++i)
    if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 0;

  gWakeIntervalMin = interval;
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  saveWakeIntervalToNVS(interval);

  // CRITICAL: Do NOT reset gLastSyncBroadcastUnix here.
  //
  // The nodes are asleep with their A2 alarms armed to the OLD sync
  // schedule.  If we reset the phase anchor now, the mothership will
  // wake at a different time than the nodes expect and the fleet
  // desyncs — the nodes never hear the SYNC_WINDOW_OPEN marker and
  // their queued data carries forward indefinitely.
  //
  // Strategy:
  //  1. Keep the OLD anchor in gLastSyncBroadcastUnix so the mothership
  //     wakes at the time the nodes expect (config shutdown arms with
  //     the old interval+phase — see main.cpp handleConfigWake).
  //  2. Compute the next OLD-schedule sync slot — that's the moment
  //     the mothership and nodes will next meet.  Use that timestamp
  //     as the NEW schedule's phase anchor.
  //  3. Push the new wakeIntervalMin, syncIntervalMin, and the new
  //     syncPhaseUnix to each node's desired config.  When the node
  //     wakes at the old time and receives CONFIG_SNAPSHOT, it applies
  //     the new schedule and re-arms A2 to newInterval + newPhase.
  //  4. The mothership does the same at handleSyncWake re-arm time
  //     (schedule transition detection in main.cpp).
  //
  // Both mothership and nodes converge on the new schedule starting
  // from the same anchor point.
  const uint32_t oldPhaseUnix = (uint32_t)gLastSyncBroadcastUnix;
  const uint16_t newSyncMin = (gSyncMode == SYNC_MODE_INTERVAL)
    ? (uint16_t)gSyncIntervalMin : 0u;

  // Compute the next sync slot on the OLD schedule — this is when the
  // mothership and nodes will next meet.  Use it as the new anchor.
  uint32_t transitionPhaseUnix = 0;
  if (gSyncMode == SYNC_MODE_INTERVAL && oldPhaseUnix > 0) {
    // Temporarily compute using the old interval (still in the anchor)
    // to find the next old-schedule slot. gSyncIntervalMin was already
    // overwritten with the NEW value above, so read the OLD interval straight
    // from the persisted anchor (this opens/closes gPrefs itself — the earlier
    // code read gPrefs while it was closed, silently yielding 0).
    const int oldAnchorInterval = readAnchorSyncIntervalMin();
    if (oldAnchorInterval > 0) {
      const uint32_t oldPeriodSec = (uint32_t)oldAnchorInterval * 60UL;
      const uint32_t nowUnix = getRTCTimeUnix();
      if (nowUnix < oldPhaseUnix) {
        transitionPhaseUnix = oldPhaseUnix;
      } else {
        const uint32_t elapsed = nowUnix - oldPhaseUnix;
        const uint32_t slots = elapsed / oldPeriodSec;
        transitionPhaseUnix = oldPhaseUnix + (slots + 1UL) * oldPeriodSec;
      }
      // Minute-align for DS3231 Alarm2
      transitionPhaseUnix -= (transitionPhaseUnix % 60UL);
    }
  }
  if (transitionPhaseUnix == 0) {
    // Fallback: use the next sync slot computed with current globals.
    // This is less precise but safe if the anchor was missing.
    transitionPhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
  }

  bool sent = false;
  if (interval > 0) {
    sent = broadcastWakeInterval(interval);
    for (const auto& node : registeredNodes) {
      if (node.state == PAIRED || node.state == DEPLOYED) {
        NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
        bool cfgChanged = false;
        if (dc.wakeIntervalMin != (uint8_t)interval) {
          dc.wakeIntervalMin = (uint8_t)interval;
          cfgChanged = true;
        }
        if (dc.syncIntervalMin != newSyncMin) {
          dc.syncIntervalMin = newSyncMin;
          cfgChanged = true;
        }
        // Push the transition phase anchor so the node re-arms A2 to
        // the new schedule at the same moment the mothership does.
        if (dc.syncPhaseUnix != transitionPhaseUnix) {
          dc.syncPhaseUnix = transitionPhaseUnix;
          cfgChanged = true;
        }
        if (dc.configVersion == 0) cfgChanged = true;
        if (cfgChanged) applyLocalDesiredConfig(node.nodeId, dc, true);
      }
    }
  }

  Serial.printf("[UI] Global wake interval set to %d min sync=%d min transitionPhase=%lu (old phase=%lu preserved, broadcast=%s)\n",
                interval, gSyncIntervalMin,
                (unsigned long)transitionPhaseUnix,
                (unsigned long)oldPhaseUnix, sent ? "yes" : "no");

  if (isAjaxRequest()) {
    if (interval > 0) {
      sendAjaxResult(true, sent ? "Recording interval applied to all nodes" : "No connected or active nodes");
    } else {
      sendAjaxResult(true, "Recording interval is OFF");
    }
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Recording Interval</h3><p>Selected: ");
  if (interval > 0) {
    html += String(interval);
    html += F(" min.</p><p style='color:#666'>");
    html += sent ? F("Applied to all connected and active nodes.") : F("No connected or active nodes.");
    html += F("</p>");
  } else {
    html += F("OFF.</p><p style='color:#666'>Recording interval disabled.</p>");
  }
  html += F("<a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleSetSyncMode() {
  // The Settings page upload-interval form sends preset="mode:value" (e.g.
  // "interval:18" or "daily:0"). Parse it first; fall back to plain "mode".
  String preset = server.arg("preset");
  String mode = server.arg("mode");
  if (preset.length() > 0) {
    int colonIdx = preset.indexOf(':');
    if (colonIdx > 0) {
      String presetMode = preset.substring(0, colonIdx);
      String presetVal  = preset.substring(colonIdx + 1);
      if (presetMode == "daily") {
        gSyncMode = SYNC_MODE_DAILY;
      } else {
        gSyncMode = SYNC_MODE_INTERVAL;
        int minVal = presetVal.toInt();
        if (minVal > 0) gSyncIntervalMin = minVal;
      }
      mode = (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily";
    }
  }
  int newMode = (mode == "interval") ? SYNC_MODE_INTERVAL : SYNC_MODE_DAILY;
  gSyncMode = newMode;
  saveSyncModeToNVS(gSyncMode);

  // Handover pattern (see handleSetWakeInterval): do NOT wipe
  // gLastSyncBroadcastUnix or overwrite the anchor. The deployed fleet is
  // asleep on the OLD schedule; preserving the old anchor in NVS is what makes
  // the mothership wake at the time the nodes expect. It meets them at the next
  // OLD-schedule slot and hands over the new schedule via the SET_SYNC_SCHED
  // broadcast during that sync window (handleSyncWake). Wiping the anchor here
  // would arm the mothership to a fresh slot the sleeping fleet never hears —
  // desyncing them even in interval mode.
  const int oldAnchorInterval = readAnchorSyncIntervalMin();
  uint32_t transitionPhaseUnix = (oldAnchorInterval > 0)
      ? computeNextOldSlotUnix(oldAnchorInterval) : 0;
  if (transitionPhaseUnix == 0) transitionPhaseUnix = computeNextSyncUnix(getRTCTimeUnix());

  const uint16_t modeSyncMin = (gSyncMode == SYNC_MODE_INTERVAL) ? (uint16_t)gSyncIntervalMin : 0u;
  // Best-effort desired-config push (authoritative delivery is the
  // SET_SYNC_SCHED broadcast during the next sync window).
  for (const auto& node : registeredNodes) {
    NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
    bool changed = false;
    if (dc.syncIntervalMin != modeSyncMin) { dc.syncIntervalMin = modeSyncMin; changed = true; }
    if (dc.syncPhaseUnix != transitionPhaseUnix) { dc.syncPhaseUnix = transitionPhaseUnix; changed = true; }
    if (dc.configVersion == 0) changed = true;
    if (changed) applyLocalDesiredConfig(node.nodeId, dc, true);
  }

  Serial.printf("[UI] Sync mode set to %s (anchor preserved, transitionPhase=%lu)\n",
                syncModeLabel(), (unsigned long)transitionPhaseUnix);

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Upload schedule set to ") + syncModeLabel());
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Upload Schedule Updated</h3><p>Mode: ");
  html += syncModeLabel();
  html += F("</p><p style='color:#666'>Next collection: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleSetSyncTime() {
  String hhmm = server.arg("sync_time");
  int hh = 0;
  int mm = 0;
  if (!parseHHMM(hhmm, hh, mm)) {
    if (isAjaxRequest()) {
      sendAjaxResult(false, "Invalid sync time. Use HH:MM");
      return;
    }
    String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                    "<h3>⚠️ Invalid upload time</h3><p>Please provide HH:MM (24h).</p>"
                    "<a href='/' style='display:inline-block;padding:10px 16px;"
                    "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
    server.send(400, "text/html", html);
    return;
  }

  gSyncDailyHour = hh;
  gSyncDailyMinute = mm;
  saveDailySyncTimeToNVS(hh, mm);

  // Handover pattern: preserve the old anchor (do NOT wipe
  // gLastSyncBroadcastUnix). The previous code wiped it unconditionally, which
  // would desync an interval-mode fleet even though this handler only changes
  // the daily time. Only push desired config in daily mode.
  if (gSyncMode == SYNC_MODE_DAILY) {
    const int oldAnchorInterval = readAnchorSyncIntervalMin();
    uint32_t transitionPhaseUnix = (oldAnchorInterval > 0)
        ? computeNextOldSlotUnix(oldAnchorInterval) : 0;
    if (transitionPhaseUnix == 0) transitionPhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
    for (const auto& node : registeredNodes) {
      NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
      bool changed = false;
      if (dc.syncIntervalMin != 0u) { dc.syncIntervalMin = 0u; changed = true; }
      if (dc.syncPhaseUnix != transitionPhaseUnix) { dc.syncPhaseUnix = transitionPhaseUnix; changed = true; }
      if (dc.configVersion == 0) changed = true;
      if (changed) applyLocalDesiredConfig(node.nodeId, dc, true);
    }
  }

  Serial.printf("[UI] Daily sync time set to %02d:%02d\n", gSyncDailyHour, gSyncDailyMinute);

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Daily upload time set to ") + formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute));
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Daily Upload Time Updated</h3><p>New daily upload time: ");
  html += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
  html += F("</p><p style='color:#666'>Next collection: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleRevertNode() {
  String nodeId = server.arg("node_id");
  bool found   = false;
  bool sentCmd = false;

  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId && node.state == DEPLOYED) {
      node.state = PAIRED;
      savePairedNodes();
      found = true;
      sentCmd = pairNode(nodeId);
      Serial.printf("[REVERT] %s -> PAIRED (cmd=%s)\n", nodeId.c_str(), sentCmd ? "OK" : "FAIL");
      break;
    }
  }

  String html = headCommon("fieldMesh");
  html += F("<div class='section center'>");
  if (found) {
    html += F("<h3>Node stopped</h3><p>Node <strong>");
    html += nodeId;
    html += F("</strong> is now connected but not active.</p>");
    if (!sentCmd) {
      html += F("<p class='muted'>Warning: could not send stop command to the node.</p>");
    }
  } else {
    html += F("<h3>Node not found or not active</h3><p>No action taken.</p>");
  }
  html += F("<a href='/stations' class='btn btn--primary'>Back to Nodes</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleStationsPage() {
  String html = headCommon("Nodes",
    "<span id='conn-status' class='conn-dot'>Connected</span><a href='/stations' class='btn btn--sm'>Refresh</a>", 1);
  auto allNodes = getRegisteredNodes();

  html += F("<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>");
  html += F("<div id='discovery-panel' class='discovery-panel' hidden>"
            "<div class='dp-row'><strong><span class='spinner' aria-hidden='true'></span>"
            "<span id='dp-state'>Searching</span></strong>"
            "<span>Elapsed <span id='dp-elapsed'>0s</span></span></div>"
            "<div class='dp-row'><span>New nodes this scan: <strong id='dp-found'>0</strong></span></div>"
            "<div class='dp-msg'>Keep new nodes powered on and in pairing mode.</div></div>");
  html += F("<div class='section'>");
  html += F("<h3>Nodes</h3>");

  // Bulk actions are opt-in: a single "Select multiple" button by default,
  // revealing the full action bar only when the user wants to act on several
  // nodes. Keeps the list header uncluttered for the common single-node case.
  html += F("<button id='batch-toggle' type='button' class='btn btn--sm' style='margin-bottom:10px' onclick='showBatchBar(true)'>Select multiple</button>");
  html += F("<form id='batch-node-actions' class='batch-actions' action='/batch-node-action' method='POST' onsubmit='return confirmBatchNodeAction()' hidden>"
            "<input id='batch-action' type='hidden' name='action' value=''>"
            "<div class='batch-actions__head'><div><strong id='batch-count'>0 selected</strong>"
            "<div class='help' style='margin:0'>Select up to 8 nodes</div></div>"
            "<div style='display:flex;gap:6px'>"
            "<button id='batch-select-all' type='button' class='btn btn--sm' onclick='toggleBatchSelection()'>Select all</button>"
            "<button id='batch-done' type='button' class='btn btn--sm' onclick='showBatchBar(false)'>Done</button></div></div>"
            "<div class='batch-actions__buttons'>"
            "<button id='batch-pause' type='submit' class='btn btn--warn' disabled onclick=\"document.getElementById('batch-action').value='pause'\">Pause selected</button>"
            "<button id='batch-resume' type='submit' class='btn btn--success' disabled onclick=\"document.getElementById('batch-action').value='resume'\">Resume selected</button>"
            "<button id='batch-remove' type='submit' class='btn btn--remove' disabled onclick=\"document.getElementById('batch-action').value='remove'\">Remove selected</button>"
            "</div></form>");

  // Empty-state hint (kept in the DOM, hidden by JS once nodes appear live).
  html += F("<p class='muted' id='node-empty'");
  if (!allNodes.empty()) html += F(" style='display:none'");
  html += F(">No nodes yet. Tap “Add New node” below, then “Find New Nodes”.</p>");

  // Node list. data-node-id + data-f hooks let the live poller update cards in
  // place and append newly discovered ones without a full-page reload.
  html += F("<div class='list' id='node-list'>");
  for (auto& node : allNodes) {
    String userId = node.userId;
    String name   = node.name;
    if (userId.isEmpty()) userId = getNodeUserId(node.nodeId);
    if (name.isEmpty())   name   = getNodeName(node.nodeId);
    NodeDesiredConfig nodeDesired = getDesiredConfig(node.nodeId.c_str());
    uint8_t observedWakeMin = (node.wakeIntervalMin > 0)
      ? node.wakeIntervalMin
      : node.inferredWakeIntervalMin;
    int nodeIntervalCurrentMin = (nodeDesired.wakeIntervalMin > 0)
      ? (int)nodeDesired.wakeIntervalMin
      : ((isAllowedInterval(gWakeIntervalMin) && gWakeIntervalMin > 0)
          ? gWakeIntervalMin
          : ((observedWakeMin > 0) ? (int)observedWakeMin : 5));

    String displayId = name.length() ? name : (userId.length() ? userId : node.nodeId);

    const bool desiredPending = node.stateChangePending &&
        nodeDesired.configVersion > node.configVersionApplied;
    const bool nodePaused = (node.state == DEPLOYED) &&
        (desiredPending ? nodeDesired.targetState == 3 : node.recordingPaused);
    const bool removePending = desiredPending && nodeDesired.targetState == 0;
    const char* stCls = removePending ? "chip chip--state-unpaired"
                       : nodePaused ? "chip chip--state-paused"
                       : (node.state == DEPLOYED) ? "chip chip--state-deployed"
                       : (node.state == PAIRED)   ? "chip chip--state-paired"
                       : "chip chip--state-unpaired";
    const char* stTxt = removePending ? "Remove queued"
                       : (desiredPending && nodeDesired.targetState == 3) ? "Pause queued"
                       : (desiredPending && nodeDesired.targetState == 2) ? "Resume queued"
                       : nodePaused ? "Paused"
                       : (node.state == DEPLOYED) ? "Active"
                       : (node.state == PAIRED)   ? "Connected" : "New";

    String battCls = F("chip");
    String battTxt = F("n/a");
    if (!isnan(node.lastReportedBatV)) {
      char bb[10];
      snprintf(bb, sizeof(bb), "%.2fV", node.lastReportedBatV);
      battTxt = bb;
      battCls = (node.lastReportedBatV >= 3.9f) ? F("chip chip--bat-ok")
              : (node.lastReportedBatV >= 3.5f) ? F("chip chip--bat-med")
              : F("chip chip--bat-low");
    }

    String seenTxt = F("n/a");
    if (node.lastSeen > 0) {
      const uint32_t nowMs = millis();
      const uint32_t ageMin = ((nowMs >= node.lastSeen) ? (nowMs - node.lastSeen) : 0) / 60000UL;
      seenTxt = String(ageMin) + F(" min ago");
    }

    html += F("<div class='node-select-wrap' data-node-id='");
    html += htmlEscape(node.nodeId);
    html += F("' data-state='");
    html += nodeStateToString(node.state);
    html += F("' data-paused='");
    html += nodePaused ? F("1") : F("0");
    html += F("'><label class='node-select-control' title='Select node'>"
              "<input class='node-select' type='checkbox' name='node_id' form='batch-node-actions' value='");
    html += htmlEscape(node.nodeId);
    html += F("' aria-label='Select ");
    html += htmlEscape(displayId);
    html += F("' onchange='updateBatchSelection(this)'></label>"
              "<a href='/station?id=");
    html += node.nodeId;
    html += F("' class='item item--node'><div class='node-row'>"
              "<div class='node-main'><strong data-f='label'>");
    html += htmlEscape(displayId);
    html += F("</strong>");
    if (name.length() && userId.length()) {
      html += F("<span class='muted node-name'>");
      html += htmlEscape(userId);
      html += F("</span>");
    }
    html += F("</div><div class='node-status'>"
              "<div class='node-status-cell'><span class='node-timing-label'>Status</span>"
              "<span class='");
    html += stCls; html += F("' data-f='status'>"); html += stTxt; html += F("</span></div>"
              "<div class='node-status-cell'><span class='node-timing-label'>Battery</span>"
              "<span class='");
    html += battCls; html += F("' data-f='batt'>"); html += battTxt; html += F("</span></div>"
              "</div><div class='node-timing'>"
              "<div class='node-timing-cell'><span class='node-timing-label'>Recording</span>"
              "<span class='chip node-timing-value' data-f='rec'>");
    html += String(nodeIntervalCurrentMin); html += F(" min</span></div>"
              "<div class='node-timing-cell'><span class='node-timing-label'>Last seen</span>"
              "<span class='chip node-timing-value' data-f='lastseen'>");
    html += seenTxt;
    html += F("</span></div></div></div></a></div>");
  }
  html += F("</div>");
  html += F(R"JS(<script>
(function(){
  var MAX_SELECTED=8;
  function boxes(){ return Array.prototype.slice.call(document.querySelectorAll('.node-select')); }
  // Bulk-action bar is opt-in. Show it via the "Select multiple" button, or
  // auto-reveal it the moment a node checkbox is ticked (so nothing is
  // unreachable); hiding it clears the current selection.
  window.showBatchBar=function(show){
    var bar=document.getElementById('batch-node-actions');
    var tog=document.getElementById('batch-toggle');
    if(bar)bar.hidden=!show;
    if(tog)tog.style.display=show?'none':'';
    if(!show){ boxes().forEach(function(b){b.checked=false;}); updateBatchSelection(); }
  };
  window.updateBatchSelection=function(changed){
    var all=boxes();
    var selected=all.filter(function(b){return b.checked;});
    if(changed && changed.checked && selected.length>MAX_SELECTED){
      changed.checked=false;
      alert('Select up to 8 nodes per action.');
      selected=all.filter(function(b){return b.checked;});
    }
    if(selected.length>0){
      var bar=document.getElementById('batch-node-actions');
      var tog=document.getElementById('batch-toggle');
      if(bar&&bar.hidden){bar.hidden=false;if(tog)tog.style.display='none';}
    }
    all.forEach(function(b){var w=b.closest('.node-select-wrap');if(w)w.classList.toggle('is-selected',b.checked);});
    var count=document.getElementById('batch-count');
    if(count)count.textContent=selected.length+' selected';
    ['batch-pause','batch-resume','batch-remove'].forEach(function(id){var e=document.getElementById(id);if(e)e.disabled=selected.length===0;});
    var selectAll=document.getElementById('batch-select-all');
    if(selectAll)selectAll.textContent=selected.length ? 'Clear' : 'Select all';
  };
  window.toggleBatchSelection=function(){
    var all=boxes(), selected=all.filter(function(b){return b.checked;});
    all.forEach(function(b,i){b.checked=selected.length ? false : i<MAX_SELECTED;});
    updateBatchSelection();
  };
  window.confirmBatchNodeAction=function(){
    var selected=boxes().filter(function(b){return b.checked;});
    if(!selected.length){alert('Select at least one node.');return false;}
    var action=(document.getElementById('batch-action')||{}).value||'';
    if(action==='remove'){
      return confirm('Remove '+selected.length+' selected node'+(selected.length===1?'':'s')+'? Deployed nodes will be removed after they confirm at their next sync. They must be paired again before reuse.');
    }
    if(action==='pause'||action==='resume'){
      var label=action==='pause'?'Pause':'Resume';
      return confirm(label+' recording on '+selected.length+' selected node'+(selected.length===1?'':'s')+'? Ineligible nodes will be left unchanged.');
    }
    return false;
  };
  updateBatchSelection();
})();
</script>)JS");
  html += F("</div>");

  // --- Add New node guided panel ---
  html += F("<div class='section'>"
            "<details>"
            "<summary style='font-weight:bold;cursor:pointer'>+ Add New node</summary>"
            "<div style='margin-top:12px'>"
            "<p class='muted'>1. Press the pair button on the node (hold 3 seconds).<br>"
            "2. Wait for the green light.<br>"
            "3. Tap “Find New Nodes” below.</p>"
            "<p class='muted'>The node will appear in the list as “New”. Tap it to activate and give it a name.</p>"
            "<form class='async-form' action='/find-stations' method='POST'>"
            "<button type='submit' id='find-btn' data-loading-label='Searching for nodes…' class='btn btn--primary'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3c-3.9 0-7.4 1.6-9.9 4.1l1.4 1.4C5.6 6.4 8.7 5 12 5s6.4 1.4 8.5 3.5l1.4-1.4C19.4 4.6 15.9 3 12 3zm0 5c-2.6 0-5 .9-6.9 2.6l1.4 1.4C8 10.5 9.9 10 12 10s4 .5 5.5 2l1.4-1.4C17 8.9 14.6 8 12 8zm0 5c-1.3 0-2.5.5-3.5 1.5l1.4 1.4c.6-.6 1.3-.9 2.1-.9s1.5.3 2.1.9l1.4-1.4c-1-1-2.2-1.5-3.5-1.5zm0 4a2 2 0 100 4 2 2 0 000-4z'/></svg> Find New Nodes</button>"
            "</form>"
            "</div>"
            "</details>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static NodeInfo* registeredNodeForUi(const String& nodeId) {
  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId) return &node;
  }
  return nullptr;
}

static bool selectedNodeAlreadyAdded(const std::vector<String>& selected,
                                     const String& nodeId) {
  for (const String& existing : selected) {
    if (existing == nodeId) return true;
  }
  return false;
}

static void handleBatchNodeAction() {
  static constexpr size_t kMaxBatchNodes = 8;
  const String action = server.arg("action");
  std::vector<String> selected;
  selected.reserve(kMaxBatchNodes + 1);
  for (uint8_t i = 0; i < server.args(); ++i) {
    if (server.argName(i) != "node_id") continue;
    String nodeId = server.arg(i);
    nodeId.trim();
    if (!nodeId.length() || nodeId.length() >= CMD_NODEID_LEN ||
        selectedNodeAlreadyAdded(selected, nodeId)) continue;
    selected.push_back(nodeId);
  }

  const bool actionValid = action == "pause" || action == "resume" ||
                           action == "remove";
  if (!actionValid || selected.empty() || selected.size() > kMaxBatchNodes) {
    String html = headCommon("Node action", "<a href='/stations' class='btn btn--sm'>Back</a>", 1);
    html += F("<div class='section center'><h3>Nothing changed</h3><p class='muted'>");
    if (!actionValid) html += F("Choose Pause, Resume, or Remove.");
    else if (selected.empty()) html += F("Select at least one node.");
    else html += F("Select no more than 8 nodes per action.");
    html += F("</p><a href='/stations' class='btn btn--primary'>Back to Nodes</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
    return;
  }

  uint8_t changed = 0;
  uint8_t unchanged = 0;
  uint8_t skipped = 0;
  uint8_t failed = 0;
  String rows;
  rows.reserve(selected.size() * 150);

  for (const String& nodeId : selected) {
    NodeInfo* node = registeredNodeForUi(nodeId);
    String label = getNodeName(nodeId);
    if (!label.length()) label = getNodeUserId(nodeId);
    if (!label.length()) label = nodeId;
    const char* tone = "chip";
    String outcome;

    if (!node) {
      ++skipped;
      tone = "chip chip--state-unpaired";
      outcome = F("Not registered - unchanged");
    } else if (action == "pause" || action == "resume") {
      if (node->state != DEPLOYED) {
        ++skipped;
        tone = "chip chip--state-paired";
        outcome = F("Not deployed - unchanged");
      } else {
        const uint8_t requestedTarget = action == "pause" ? 3 : 2;
        NodeDesiredConfig desired = getDesiredConfig(nodeId.c_str());
        const bool reportsRequested = requestedTarget == 3
            ? node->recordingPaused : !node->recordingPaused;
        if (desired.targetState == requestedTarget &&
            (node->stateChangePending || reportsRequested)) {
          ++unchanged;
          tone = requestedTarget == 3
              ? "chip chip--state-paused" : "chip chip--state-deployed";
          outcome = requestedTarget == 3
              ? F("Already paused or queued") : F("Already active or queued");
        } else {
          desired.targetState = requestedTarget;
          const NodeConfigApplyResult applied =
              applyLocalDesiredConfig(nodeId, desired);
          const bool ok = applied.durable && applied.registryApplied &&
              (applied.command.outcome == OUT_ACCEPTED ||
               applied.command.outcome == OUT_REPLAY);
          if (ok) {
            ++changed;
            tone = requestedTarget == 3
                ? "chip chip--state-paused" : "chip chip--state-deployed";
            outcome = requestedTarget == 3
                ? F("Pause queued for next sync")
                : F("Resume queued for next sync");
          } else {
            ++failed;
            tone = "chip chip--state-unpaired";
            outcome = F("Could not save desired state");
          }
        }
      }
    } else if (node->state == DEPLOYED) {
      NodeDesiredConfig desired = getDesiredConfig(nodeId.c_str());
      if (desired.targetState == 0 && node->stateChangePending &&
          node->pendingTargetState == PENDING_TO_UNPAIRED) {
        ++unchanged;
        tone = "chip chip--state-unpaired";
        outcome = F("Removal already queued");
      } else {
        desired.targetState = 0;
        const NodeConfigApplyResult applied =
            applyLocalDesiredConfig(nodeId, desired, false, true);
        const bool ok = applied.durable && applied.registryApplied &&
            (applied.command.outcome == OUT_ACCEPTED ||
             applied.command.outcome == OUT_REPLAY);
        if (ok) {
          ++changed;
          tone = "chip chip--state-unpaired";
          outcome = F("Removal queued - awaiting next-sync confirmation");
        } else {
          ++failed;
          tone = "chip chip--state-unpaired";
          outcome = F("Could not queue removal");
        }
      }
    } else {
      // New/connected nodes are awake in config mode, so preserve the existing
      // immediate unpair path. A deployed node never uses this branch.
      const bool sent = sendUnpairToNode(nodeId);
      const bool local = unpairNode(nodeId);
      if (local) {
        setNodeUserId(nodeId, "");
        setNodeName(nodeId, "");
        setNodeNotes(nodeId, "");
        NodeInfo* updated = registeredNodeForUi(nodeId);
        if (updated) {
          updated->userId = "";
          updated->name = "";
        }
      }
      if (sent && local) {
        ++changed;
        tone = "chip chip--state-unpaired";
        outcome = F("Removed");
      } else {
        ++failed;
        tone = "chip chip--state-unpaired";
        outcome = local ? F("Removed locally; node did not confirm")
                        : F("Removal failed");
      }
    }

    Serial.printf("[BATCH] action=%s node=%s outcome=%s\n",
                  action.c_str(), nodeId.c_str(), outcome.c_str());
    rows += F("<div class='item item-row'><strong>");
    rows += htmlEscape(label);
    rows += F("</strong><span class='");
    rows += tone;
    rows += F("'>");
    rows += htmlEscape(outcome);
    rows += F("</span></div>");
  }

  const char* actionLabel = action == "pause" ? "Pause"
      : action == "resume" ? "Resume" : "Remove";
  Serial.printf("[BATCH] %s complete selected=%u changed=%u unchanged=%u skipped=%u failed=%u\n",
                actionLabel, static_cast<unsigned>(selected.size()), changed,
                unchanged, skipped, failed);

  String html = headCommon("Node action", "<a href='/stations' class='btn btn--sm'>Back</a>", 1);
  html += F("<div class='section'><h3>");
  html += actionLabel;
  html += F(" selected nodes</h3><p><strong>");
  html += String(changed);
  html += F(" queued/applied</strong> &middot; ");
  html += String(unchanged);
  html += F(" already set &middot; ");
  html += String(skipped);
  html += F(" skipped &middot; ");
  html += String(failed);
  html += F(" failed</p><div class='list'>");
  html += rows;
  html += F("</div><p class='muted'>Pause, resume, and deployed-node removal complete independently at each node&rsquo;s next normal sync.</p>"
            "<a href='/stations' class='btn btn--primary'>Back to Nodes</a></div>");
  html += footCommon();
  server.send(failed ? 207 : 200, "text/html", html);
}

static void handleStationDetail() {
  String nodeId = server.arg("id");
  if (nodeId.length() == 0) nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  // A node that hasn't been deployed yet goes through the guided deploy wizard
  // instead of this dense management form. Once DEPLOYED it renders here as
  // normal (edit/pause/resume/remove). deploySelectedNodes() sets DEPLOYED
  // synchronously, so the wizard's post-deploy redirect lands on this view, not
  // back in the wizard.
  if (target && target->state != DEPLOYED) {
    server.sendHeader("Location", String("/station-setup?id=") + nodeId, true);
    server.send(302, "text/plain", "");
    return;
  }

  String actionsHtml = String("<a href='/stations' class='btn btn--sm'>Back</a><a href='/station?id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Node", actionsHtml);
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3>"
              "<p class='muted'>No node with that ID is currently registered.</p>"
              "<a href='/stations' class='btn btn--primary'>Back to Nodes</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  String userId  = getNodeUserId(target->nodeId);
  String name    = getNodeName(target->nodeId);
  String notes   = getNodeNotes(target->nodeId);
  const bool isDeployed = (target->state == DEPLOYED);
  const NodeDesiredConfig displayedDesired =
      getDesiredConfig(target->nodeId.c_str());
  const bool desiredPending = target->stateChangePending &&
      displayedDesired.configVersion > target->configVersionApplied;
  const bool displayedPaused = desiredPending
      ? displayedDesired.targetState == 3
      : target->recordingPaused;
  uint8_t observedWakeMin = (target->wakeIntervalMin > 0)
    ? target->wakeIntervalMin
    : target->inferredWakeIntervalMin;
  int activeIntervalMin = desiredPending && displayedDesired.wakeIntervalMin > 0
    ? displayedDesired.wakeIntervalMin
    : ((observedWakeMin > 0)
        ? observedWakeMin
        : (isAllowedInterval(gWakeIntervalMin) ? gWakeIntervalMin : 5));

  // --- Status header ---
  html += F("<div class='muted' style='margin:0 0 10px 0'>");
  if (target->state == DEPLOYED) {
    if (displayedPaused) {
      html += desiredPending
          ? F("<span class='chip chip--state-paused'>Pause queued</span>")
          : F("<span class='chip chip--state-paused'>Paused</span>");
    } else {
      html += desiredPending
          ? F("<span class='chip chip--state-deployed'>Resume/change queued</span>")
          : F("<span class='chip chip--state-deployed'>Active</span>");
    }
  }
  else if (target->state == PAIRED) html += F("<span class='chip chip--state-paired'>Connected</span>");
  else html += F("<span class='chip chip--state-unpaired'>New</span>");
  {
    float batV = target->lastReportedBatV;
    if (!isnan(batV)) {
      char batBuf[10];
      snprintf(batBuf, sizeof(batBuf), "%.2fV", batV);
      const char* batClass = (batV >= 3.9f) ? "chip--bat-ok"
                           : (batV >= 3.5f) ? "chip--bat-med"
                           : "chip--bat-low";
      html += F(" <span class='chip ");
      html += batClass;
      html += F("'>");
      html += batBuf;
      html += F("</span>");
    }
  }
  html += F("<br><strong>Recording every</strong> ");
  html += String(activeIntervalMin);
  html += F(" min");
  if (target->lastSeen > 0) {
    const uint32_t nowMs = millis();
    const uint32_t ageMs = (nowMs >= target->lastSeen) ? (nowMs - target->lastSeen) : 0;
    const uint32_t ageMin = ageMs / 60000UL;
    html += F("<br><strong>Last seen</strong> ");
    html += String(ageMin);
    html += F(" min ago");
  }
  html += F("</div>");

  // Sensor configuration — opens the toggle-button picker, returns here on save.
  html += F("<div style='margin-bottom:12px'>"
            "<a href='/node-sensors?id=");
  html += target->nodeId;
  html += F("' class='btn btn--sm' style='display:block;text-align:center;padding:12px'>"
            "&#9881; Configure Sensors</a></div>");

  html += F("<form action='/station' method='POST' onsubmit='return confirmRemove()'>"
            "<input type='hidden' name='node_id' value='");
  html += target->nodeId;
  html += F("'>");

  // --- Location section (manual entry with phone GPS guidance) ---
  html += F("<div class='section'>"
            "<h3>Location</h3>"
            "<p class='muted'>Find this node's coordinates using your phone's Maps app, then enter them below.</p>"
            "<div style='margin:8px 0;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--panel)'>"
            "<strong>How to get coordinates:</strong><br>"
            "<span class='muted'>1. Open Google Maps or Apple Maps on your phone<br>"
            "2. Tap the location dot to centre on your position<br>"
            "3. Long-press the blue dot → coordinates appear at the top<br>"
            "4. Copy the latitude and longitude values</span>"
            "</div>"
            "<label class='label'>Latitude</label>"
            "<input class='input' type='text' id='lat' name='lat' placeholder='-27.469771' value='");
  if (!isnan(target->latitude)) {
    char latBuf[16];
    snprintf(latBuf, sizeof(latBuf), "%.6f", target->latitude);
    html += latBuf;
  }
  html += F("'>"
            "<label class='label'>Longitude</label>"
            "<input class='input' type='text' id='lon' name='lon' placeholder='153.025124' value='");
  if (!isnan(target->longitude)) {
    char lonBuf[16];
    snprintf(lonBuf, sizeof(lonBuf), "%.6f", target->longitude);
    html += lonBuf;
  }
  html += F("'>"
            "</div>");

  html += F("<div style='margin-bottom:10px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa'>"
            "<label class='label' style='margin-top:0'>Notes</label>"
            "<input class='input' type='text' name='notes' maxlength='180' placeholder='Notes for this node' value='");
  html += htmlEscape(notes);
  html += F("'></div>");

  if (!isDeployed) {
    html += F("<label class='label'>Node ID (numeric, e.g. 001)</label>"
              "<input class='input' type='text' name='user_id' maxlength='3' "
              "placeholder='001' value='");
    html += userId;
    html += F("'>"
              "<label class='label'>Name</label>"
              "<input class='input' type='text' name='name' "
              "placeholder='e.g. North Hedge 01' value='");
    html += name;
    html += F("'>");
  } else {
    html += F("<div style='margin-top:10px;padding:10px;border:1px solid #E1B17A;border-radius:8px;background:#fff8e1'>"
              "<div style='font-weight:700;color:#8a4b00;margin-bottom:6px'>Are you sure?</div>"
              "<div class='muted' style='margin-bottom:8px'>Changing Node ID/Name on an active node can break tracking if used incorrectly.</div>"
              "<label style='display:flex;gap:8px;align-items:flex-start;margin-bottom:8px'>"
              "<input type='checkbox' name='edit_identity_confirm' value='yes' onchange=\"var en=this.checked;document.getElementById('dep-user-id').disabled=!en;document.getElementById('dep-name').disabled=!en;\">"
              "<span>I understand and want to edit Node ID/Name for this active node.</span>"
              "</label>"
              "<label class='label'>Node ID (numeric, e.g. 001)</label>"
              "<input id='dep-user-id' class='input' type='text' name='user_id' maxlength='3' placeholder='001' disabled value='");
    html += userId;
    html += F("'>"
              "<label class='label'>Name</label>"
              "<input id='dep-name' class='input' type='text' name='name' placeholder='e.g. North Hedge 01' disabled value='");
    html += name;
    html += F("'>"
              "</div>");
  }

  html += F("<div style='margin-bottom:12px'>"
            "<label class='label'>Recording interval (minutes)</label>"
            "<div style='font-size:14px;padding:6px 0'><strong>");
  html += String(gWakeIntervalMin > 0 ? gWakeIntervalMin : 0);
  html += F(" min</strong> &nbsp;<span style='font-size:12px;color:#888'>"
            "(applies to all nodes &mdash; change in Settings)</span></div>");
  if (gSyncIntervalMin > 0) {
    html += F("<div style='font-size:12px;color:#555'>Upload every ");
    html += String(gSyncIntervalMin);
    html += F(" min</div>");
  }
  html += F("</div>");

  html += F("<label class='label'>Node action (optional)</label>"
            "<div class='muted' style='font-size:12px'>Leave these unselected when only saving the node details above.</div>"
            "<div class='action-choices'>");
  if (!isDeployed) {
    // New / Connected node — the in-hand deploy path.
    html += F("<label class='action-choice action-choice--start'><input type='radio' name='action' value='start'><span>Deploy (start recording)</span></label>");
  } else if (displayedPaused) {
    // Paused deployed node — resume remotely at the next sync.
    html += F("<label class='action-choice action-choice--start'><input type='radio' name='action' value='resume'><span>Resume recording</span></label>");
  } else {
    // Active deployed node — pause (standby) remotely at the next sync.
    html += F("<label class='action-choice action-choice--stop'><input type='radio' name='action' value='pause'><span>Pause recording</span></label>");
  }
  html += F("<label class='action-choice action-choice--unpair'><input type='radio' name='action' value='unpair'><span>Remove node</span></label>"
            "</div>");
  if (isDeployed) {
    html += F("<div class='muted' style='font-size:12px;margin-top:6px'>Pause/Resume/Remove are delivered at the node&rsquo;s next sync check-in.</div>");
  }
  html += F("<button type='submit' class='btn btn--success' style='margin-top:12px'>"
            "Save Changes</button>"
            "</form>");

  html += F("<script>function confirmRemove(){var r=document.querySelector('input[name=action]:checked');if(r&&r.value==='unpair'){return confirm('Remove this node? You will need to re-add it with the pair button.');}return true;}</script>");
  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleNodeConfigSave() {
  String nodeId   = server.arg("node_id");
  String userId   = server.arg("user_id");
  String name     = server.arg("name");
  String notes    = server.arg("notes");
  bool editIdentityConfirmed = server.hasArg("edit_identity_confirm") && server.arg("edit_identity_confirm") == "yes";
  String action   = server.arg("action");
  int interval = (gWakeIntervalMin > 0) ? gWakeIntervalMin : 5;

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  const bool isCurrentlyDeployed = (target && target->state == DEPLOYED);
  const bool allowIdentityEdit = (!isCurrentlyDeployed) || editIdentityConfirmed;
  if (allowIdentityEdit) {
    if (server.hasArg("user_id")) setNodeUserId(nodeId, userId);
    if (server.hasArg("name")) setNodeName(nodeId, name);
  }
  if (server.hasArg("notes")) setNodeNotes(nodeId, notes);

  // --- GPS coordinates (optional) ---
  if (target) {
    String latStr = server.arg("lat");
    String lonStr = server.arg("lon");
    if (latStr.length() > 0 && lonStr.length() > 0) {
      float lat = latStr.toFloat();
      float lon = lonStr.toFloat();
      if (lat != 0.0f || lon != 0.0f) {
        target->latitude  = lat;
        target->longitude = lon;
      }
    }
  }

  NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  bool cfgChanged = false;
  const uint16_t desiredSyncMin = (gSyncMode == SYNC_MODE_DAILY)
      ? 0u : (uint16_t)computeAutoSyncMin(interval);
  const int anchorSyncIntervalMin = readAnchorSyncIntervalMin();
  const bool scheduleHandoverPending =
      gSyncMode == SYNC_MODE_INTERVAL &&
      anchorSyncIntervalMin > 0 &&
      anchorSyncIntervalMin != desiredSyncMin;

  uint32_t bootstrapPhaseUnix = scheduleHandoverPending
      ? computeNextOldSlotUnix(anchorSyncIntervalMin)
      : (uint32_t)gLastSyncBroadcastUnix;
  if (bootstrapPhaseUnix == 0) bootstrapPhaseUnix = getRTCTimeUnix();
  if (gSyncMode == SYNC_MODE_DAILY) {
    const uint32_t dayStartUnix = (bootstrapPhaseUnix / 86400UL) * 86400UL;
    const uint32_t targetOffset = (uint32_t)gSyncDailyHour * 3600UL + (uint32_t)gSyncDailyMinute * 60UL;
    uint32_t nextDailyUnix = dayStartUnix + targetOffset;
    if (nextDailyUnix <= getRTCTimeUnix()) nextDailyUnix += 86400UL;
    bootstrapPhaseUnix = nextDailyUnix;
  }
  bootstrapPhaseUnix -= (bootstrapPhaseUnix % 60UL);

  // A per-node metadata/lifecycle save must not rewrite the fleet anchor or
  // replace a transition phase prepared by handleSetWakeInterval(). Doing so
  // can make a pause/resume action erase the OLD-vs-NEW schedule marker before
  // sleeping nodes have received the handover. Only fill schedule fields for a
  // new/legacy desired-config record; fleet schedule changes are owned by the
  // global schedule handlers.
  const bool newDesiredConfig = dc.configVersion == 0;
  if (!scheduleHandoverPending || newDesiredConfig ||
      (dc.wakeIntervalMin == 0 && interval > 0)) {
    if (dc.wakeIntervalMin != (uint8_t)interval) {
      dc.wakeIntervalMin = (uint8_t)interval;
      cfgChanged = true;
    }
  }
  if (!scheduleHandoverPending || newDesiredConfig ||
      (gSyncMode == SYNC_MODE_INTERVAL && dc.syncIntervalMin == 0)) {
    if (dc.syncIntervalMin != desiredSyncMin) {
      dc.syncIntervalMin = desiredSyncMin;
      cfgChanged = true;
    }
  }
  if (!scheduleHandoverPending || newDesiredConfig || dc.syncPhaseUnix == 0) {
    if (dc.syncPhaseUnix != bootstrapPhaseUnix) {
      dc.syncPhaseUnix = bootstrapPhaseUnix;
      cfgChanged = true;
    }
  }
  if (newDesiredConfig) cfgChanged = true;
  bool configSaveOk = true;  // metadata/schedule save outcome (for AJAX callers)
  if (cfgChanged) {
    const NodeConfigApplyResult applied = applyLocalDesiredConfig(nodeId, dc, true);
    configSaveOk = applied.durable && applied.registryApplied &&
                   (applied.command.outcome == OUT_ACCEPTED ||
                    applied.command.outcome == OUT_REPLAY);
    dc = getDesiredConfig(nodeId.c_str());
  }
  if (target) target->wakeIntervalMin = (uint8_t)interval;
  // Persist any updated node fields (name/notes/lat/lon) to NVS.
  if (target) savePairedNodes();

  Serial.printf("[CONFIG] %s interval=%d min desired v%u changed=%d\n",
                nodeId.c_str(), interval, (unsigned)dc.configVersion, cfgChanged ? 1 : 0);

  bool deployOk = false;
  bool revertOk = false;
  bool pairOk   = false;
  bool unpairOk = false;
  bool pauseOk  = false;
  bool resumeOk = false;

  if (action == "none" || action.length() == 0) {
    // No lifecycle change requested — only name/notes/schedule saved.
  } else if (action == "start") {
    if (target && target->state == UNPAIRED) {
      pairOk = pairNode(nodeId);
      if (pairOk) {
        target->state = PAIRED;
        savePairedNodes();
      }
    }
    std::vector<String> ids;
    ids.push_back(nodeId);
    deployOk = deploySelectedNodes(ids);
    Serial.printf("[CONFIG] %s start -> deploy: %s\n", nodeId.c_str(), deployOk ? "OK" : "FAIL");
  } else if (action == "stop") {
    if (target) {
      target->state = PAIRED;
      target->deployPending = false;
      savePairedNodes();
      revertOk = pairNode(nodeId);
      Serial.printf("[CONFIG] %s stop -> PAIRED: %s\n", nodeId.c_str(), revertOk ? "OK" : "FAIL");
    }
  } else if (action == "pause") {
    // Standby: stop recording but keep the node deployed + syncing (remotely
    // resumable). Deferred via the sync-window NODE_CONFIG reconcile, same as
    // unpair — a deployed node is asleep now.
    if (target && target->state == DEPLOYED) {
      NodeDesiredConfig du = getDesiredConfig(nodeId.c_str());
      du.targetState = 3;  // STANDBY
      const NodeConfigApplyResult applied = applyLocalDesiredConfig(nodeId, du);
      pauseOk = applied.durable && applied.registryApplied &&
                (applied.command.outcome == OUT_ACCEPTED ||
                 applied.command.outcome == OUT_REPLAY);
      du = getDesiredConfig(nodeId.c_str());
      Serial.printf("[CONFIG] %s pause SCHEDULED: desired v%u STANDBY at next sync\n",
                    nodeId.c_str(), (unsigned)du.configVersion);
    }
  } else if (action == "resume") {
    if (target && target->state == DEPLOYED) {
      NodeDesiredConfig du = getDesiredConfig(nodeId.c_str());
      du.targetState = 2;  // DEPLOYED / ACTIVE
      const NodeConfigApplyResult applied = applyLocalDesiredConfig(nodeId, du);
      resumeOk = applied.durable && applied.registryApplied &&
                 (applied.command.outcome == OUT_ACCEPTED ||
                  applied.command.outcome == OUT_REPLAY);
      du = getDesiredConfig(nodeId.c_str());
      Serial.printf("[CONFIG] %s resume SCHEDULED: desired v%u ACTIVE at next sync\n",
                    nodeId.c_str(), (unsigned)du.configVersion);
    }
  } else if (action == "unpair") {
    if (target) {
      if (target->state == DEPLOYED) {
        // Deferred unpair: a deployed node is asleep now and only reachable in
        // its sync window. Record the desired UNPAIRED state (durable, version
        // bumped) and KEEP the node in the registry. The sync-window reconcile
        // broadcasts NODE_CONFIG(UNPAIRED); the node ACKs and is removed then.
        // Do NOT send imperatively or delete locally (that orphaned the node).
        NodeDesiredConfig du = getDesiredConfig(nodeId.c_str());
        du.targetState = 0;  // UNPAIRED
        const NodeConfigApplyResult applied =
            applyLocalDesiredConfig(nodeId, du, false, true);
        unpairOk = applied.durable && applied.registryApplied &&
                   (applied.command.outcome == OUT_ACCEPTED ||
                    applied.command.outcome == OUT_REPLAY);
        du = getDesiredConfig(nodeId.c_str());
        Serial.printf("[CONFIG] %s unpair SCHEDULED (deployed): desired v%u UNPAIRED at next sync\n",
                      nodeId.c_str(), (unsigned)du.configVersion);
      } else {
        // Awake node (unpaired/paired, still listening in config mode): the
        // immediate command works because the node is not asleep.
        bool sent  = sendUnpairToNode(nodeId);
        bool local = unpairNode(nodeId);
        unpairOk = sent && local;
        setNodeUserId(nodeId, "");
        setNodeName(nodeId, "");
        setNodeNotes(nodeId, "");
        target->userId = "";
        target->name   = "";
        Serial.printf("[CONFIG] %s unpair -> send=%s local=%s\n",
                      nodeId.c_str(), sent ? "OK" : "FAIL", local ? "OK" : "FAIL");
      }
    }
  }

  String finalUserId = getNodeUserId(nodeId);
  String finalName   = getNodeName(nodeId);
  String finalNotes  = getNodeNotes(nodeId);
  if (finalUserId.isEmpty()) finalUserId = userId;
  if (finalName.isEmpty())   finalName   = name;
  if (target) {
    target->userId = finalUserId;
    target->name   = finalName;
  }

  // AJAX branch (node-deploy wizard / async callers): stay on the page with a
  // JSON result instead of the standalone confirmation page. Metadata-only saves
  // (action=none) report the config-persist outcome; lifecycle actions report
  // their own. (Reporting a blind true here previously masked a failed metadata
  // save so the wizard sailed past a broken step.)
  if (isAjaxRequest()) {
    bool ajaxOk = configSaveOk;
    if      (action == "start")  ajaxOk = deployOk;
    else if (action == "pause")  ajaxOk = pauseOk;
    else if (action == "resume") ajaxOk = resumeOk;
    else if (action == "unpair") ajaxOk = unpairOk;
    String j = String("{\"ok\":") + (ajaxOk ? "true" : "false") +
               ",\"deploy\":" + (deployOk ? "true" : "false") +
               ",\"pair\":"   + (pairOk ? "true" : "false") + "}";
    server.send(ajaxOk ? 200 : 500, "application/json", j);
    return;
  }

  String actionsHtml = String("<a href='/stations' class='btn btn--sm'>Back</a><a href='/station?id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Node", actionsHtml);
  html += F("<div class='section center'>"
            "<h3>Node settings applied</h3>");

  if (!target) {
    html += F("<p class='muted'>Warning: this node ID is not currently in the registered list.</p>");
  }

  html += F("<p><strong>Node ID:</strong> ");
  html += (finalUserId.length() ? htmlEscape(finalUserId) : String("-"));
  html += F("<br><strong>Name:</strong> ");
  html += (finalName.length() ? htmlEscape(finalName) : String("-"));
  html += F("<br><strong>Notes:</strong> ");
  html += (finalNotes.length() ? htmlEscape(finalNotes) : String("-"));
  html += F("<br><strong>Recording interval:</strong> ");
  html += String(interval);
  html += F(" min<br><strong>Action:</strong> ");
  html += action;
  html += F("</p>");

  html += F("<p class='muted'>"
            "Schedule stored for this node (applies on next wake)");
  if (pairOk)   html += F("<br>Connect: OK");
  if (deployOk) html += F("<br>Deploy: REQUESTED (awaiting node confirmation)");
  if (revertOk) html += F("<br>Stop monitoring: OK");
  if (pauseOk)  html += F("<br>Pause: SCHEDULED &mdash; the node stops recording at its next sync check-in (stays deployed &amp; resumable)");
  if (resumeOk) html += F("<br>Resume: SCHEDULED &mdash; the node resumes recording at its next sync check-in");
  if (unpairOk) html += F("<br>Remove node: SCHEDULED &mdash; completes when the node confirms at its next sync");
  if (!pairOk && !deployOk && !revertOk && !pauseOk && !resumeOk && !unpairOk) {
    html += F("<br>No lifecycle change requested.");
  }
  html += F("</p>"
            "<a href='/stations' class='btn btn--primary'>Back to Nodes</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

// GET /station-setup?id=X — guided node deployment wizard. Entered by clicking a
// not-yet-deployed node in the node manager (handleStationDetail 302s here).
// Structurally identical to the hub /setup wizard (client-side .wz-step show/
// hide, data-wz controller). Every step POSTs to an endpoint that already
// exists — /station (handleNodeConfigSave, action=none for metadata, action=
// start to deploy) and /set-node-sensors — so there is no new persistence logic.
//   1 Identify (ID + name)  2 Sensors  3 Location  4 Deploy.
static void handleStationSetupWizard() {
  String nodeId = server.arg("id");
  if (nodeId.length() == 0) nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  String html = headCommon("Set up node", F("<a href='/stations' class='btn btn--sm'>Exit</a>"));
  if (!target) {
    html += F("<div class='section'><h3>Node not found</h3>"
              "<a href='/stations' class='btn btn--primary'>Back to Nodes</a></div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }
  // Already deployed → nothing to onboard; send to the management view.
  if (target->state == DEPLOYED) {
    server.sendHeader("Location", String("/station?id=") + nodeId, true);
    server.send(302, "text/plain", "");
    return;
  }

  const String userId = getNodeUserId(nodeId);
  const String name   = getNodeName(nodeId);
  const NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  const uint16_t sensorMask = (dc.sensorMask & NODE_SENSOR_MASK_VALID)
      ? (uint16_t)(dc.sensorMask & ~NODE_SENSOR_MASK_VALID) : 0;
  const bool hasIdentity = userId.length() > 0 || name.length() > 0;
  const int startStep = hasIdentity ? 2 : 1;  // resume past identity if already named

  html += F("<div class='section'>"
            "<p class='muted' style='margin:0 0 10px'>Node setup &middot; step <span id='wz-cur'>1</span> of 4</p>");

  // Step 1 — Identify
  html += F("<div class='wz-step' data-step='1'>"
            "<h3>1. Name this node</h3>"
            "<p class='muted'>Give the node a short ID and a name so you can recognise it.</p>"
            "<label class='label'>Node ID (numeric, e.g. 001)</label>"
            "<input class='input' id='wz-uid' type='text' maxlength='3' inputmode='numeric' placeholder='001' value='");
  html += htmlEscape(userId);
  html += F("'><label class='label'>Name</label>"
            "<input class='input' id='wz-name' type='text' placeholder='e.g. North Hedge 01' value='");
  html += htmlEscape(name);
  html += F("'><div id='wz-id-result' class='help' style='margin-top:8px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--primary' id='wz-id-next'>Save &amp; continue</button></div>"
            "</div>");

  // Step 2 — Sensors (reuses the same toggle picker as /node-sensors)
  html += F("<div class='wz-step' data-step='2' style='display:none'>"
            "<h3>2. Sensors installed</h3>"
            "<p class='muted'>Tap the sensors fitted to this node.</p>"
            "<div class='sensor-grid' id='wz-sensor-grid'>");
  struct SensorOption { uint16_t bit; const char* label; const char* grp; };
  static const SensorOption kOpts[] = {
    { (uint16_t)(SNAP_PRESENT_AIR_TEMP | SNAP_PRESENT_AIR_RH), "Air (SHT41)",        "" },
    { SNAP_PRESENT_SPECTRAL,                                   "Spectral (AS7343)",  "" },
    { SNAP_PRESENT_SOIL1,                                      "Soil Probe 1",       "" },
    { SNAP_PRESENT_SOIL2,                                      "Soil Probe 2",       "" },
    { SNAP_PRESENT_WIND,                                       "Wind (Reed cup)",    "wind" },
    { NODE_SENSOR_CFG_WIND_ULTRASONIC,                         "Wind (Ultrasonic)",  "wind" },
    { SNAP_PRESENT_AUX1,                                       "Aux 1",              "" },
    { SNAP_PRESENT_AUX2,                                       "Aux 2",              "" },
  };
  for (const auto& opt : kOpts) {
    const bool sel = (sensorMask & opt.bit) != 0;
    html += F("<button type='button' class='sbtn");
    if (sel) html += F(" sbtn--on");
    html += F("' data-bit='"); html += String((unsigned)opt.bit); html += F("'");
    if (opt.grp[0]) { html += F(" data-grp='"); html += opt.grp; html += F("'"); }
    html += F(">"); html += opt.label; html += F("</button>");
  }
  html += F("</div><div id='wz-sensor-result' class='help' style='margin-top:8px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn' data-wz='skip'>Skip</button> "
            "<button type='button' class='btn btn--primary' id='wz-sensor-next'>Save &amp; continue</button></div>"
            "</div>");

  // Step 3 — Location
  html += F("<div class='wz-step' data-step='3' style='display:none'>"
            "<h3>3. Location</h3>"
            "<p class='muted'>Find this node's coordinates in your phone's Maps app "
            "(long-press your location, copy the numbers), then enter them.</p>"
            "<label class='label'>Latitude</label>"
            "<input class='input' id='wz-lat' type='text' inputmode='decimal' placeholder='-27.469771' value='");
  if (!isnan(target->latitude)) { char b[16]; snprintf(b, sizeof(b), "%.6f", target->latitude); html += b; }
  html += F("'><label class='label'>Longitude</label>"
            "<input class='input' id='wz-lon' type='text' inputmode='decimal' placeholder='153.025124' value='");
  if (!isnan(target->longitude)) { char b[16]; snprintf(b, sizeof(b), "%.6f", target->longitude); html += b; }
  html += F("'><div id='wz-loc-result' class='help' style='margin-top:8px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn' data-wz='skip'>Skip</button> "
            "<button type='button' class='btn btn--primary' id='wz-loc-next'>Save &amp; continue</button></div>"
            "</div>");

  // Step 4 — Deploy
  html += F("<div class='wz-step' data-step='4' style='display:none'>"
            "<h3>4. Deploy</h3>"
            "<p class='muted'>Start recording on this node. It joins the fleet on the current recording schedule.</p>"
            "<div id='wz-deploy-result' class='help' style='margin-top:8px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn btn--success' id='wz-deploy-go' style='min-width:160px'>Deploy node</button></div>"
            "</div>");

  html += F("</div>");  // .section

  // Sensor picker styling (same as /node-sensors).
  html += F("<style>"
    ".sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:8px}"
    ".sbtn{padding:16px 10px;border:2px solid #cbd5e1;border-radius:12px;background:#fff;"
    "font-size:15px;font-weight:600;color:#333;cursor:pointer;text-align:center;min-height:60px;line-height:1.25}"
    ".sbtn--on{background:#1f9d55;border-color:#1a8548;color:#fff}"
    "</style>");

  // Controller — mirrors the hub /setup wizard.
  html += F("<script>(function(){");
  html += F("var NODE=\""); html += htmlEscape(nodeId); html += F("\";");
  html += F("var startStep="); html += String(startStep); html += F(";");
  html += F(
    "var TOTAL=4,cur=startStep||1;"
    "var steps=[].slice.call(document.querySelectorAll('.wz-step'));"
    "function post(u,d){var b=Object.keys(d).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(d[k]);}).join('&')+'&ajax=1';"
    "return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(function(r){return r.json();});}"
    "function chip(ok,txt){return \"<span class='chip \"+(ok?'chip--cfg-ok':'chip--bat-low')+\"' style='font-weight:600'>\"+txt+\"</span>\";}"
    "function show(n){cur=Math.max(1,Math.min(TOTAL,n));"
    "steps.forEach(function(s){s.style.display=(parseInt(s.dataset.step,10)===cur)?'block':'none';});"
    "document.getElementById('wz-cur').textContent=cur;window.scrollTo(0,0);}"
    "window.wzGo=show;"
    "document.addEventListener('click',function(e){var t=e.target;while(t&&t!==document&&!t.getAttribute('data-wz'))t=t.parentNode;"
    "if(!t||t===document)return;var a=t.getAttribute('data-wz');"
    "if(a==='next'||a==='skip')show(cur+1);else if(a==='back')show(cur-1);});"
    // Step 1 identify
    "document.getElementById('wz-id-next').addEventListener('click',function(){"
    "var uid=document.getElementById('wz-uid').value,nm=document.getElementById('wz-name').value;"
    "post('/station',{node_id:NODE,user_id:uid,name:nm,action:'none'}).then(function(j){"
    "document.getElementById('wz-id-result').innerHTML=chip(j.ok,j.ok?'Saved':'Save failed');"
    "if(j.ok)show(2);});});"
    // Step 2 sensors
    "var sbtns=[].slice.call(document.querySelectorAll('#wz-sensor-grid .sbtn'));"
    "sbtns.forEach(function(b){b.addEventListener('click',function(){"
    "var on=!b.classList.contains('sbtn--on');"
    "if(on&&b.dataset.grp){sbtns.forEach(function(o){if(o!==b&&o.dataset.grp===b.dataset.grp)o.classList.remove('sbtn--on');});}"
    "b.classList.toggle('sbtn--on',on);});});"
    "function sensorMask(){var v=0;sbtns.forEach(function(b){if(b.classList.contains('sbtn--on'))v|=parseInt(b.dataset.bit,10);});return v;}"
    "document.getElementById('wz-sensor-next').addEventListener('click',function(){"
    "post('/set-node-sensors',{node_id:NODE,mask:sensorMask()}).then(function(j){"
    "document.getElementById('wz-sensor-result').innerHTML=chip(j.ok,j.ok?'Sensors saved':(j.error||'Save failed'));"
    "if(j.ok)show(3);});});"
    // Step 3 location
    "document.getElementById('wz-loc-next').addEventListener('click',function(){"
    "var lat=document.getElementById('wz-lat').value,lon=document.getElementById('wz-lon').value;"
    "post('/station',{node_id:NODE,lat:lat,lon:lon,action:'none'}).then(function(j){"
    "document.getElementById('wz-loc-result').innerHTML=chip(j.ok,j.ok?'Location saved':'Save failed');"
    "if(j.ok)show(4);});});"
    // Step 4 deploy
    "document.getElementById('wz-deploy-go').addEventListener('click',function(){"
    "var btn=this;btn.disabled=true;btn.textContent='Deploying...';"
    "post('/station',{node_id:NODE,action:'start'}).then(function(j){"
    "if(j.ok){document.getElementById('wz-deploy-result').innerHTML=chip(true,'Deployed');"
    "setTimeout(function(){location.href='/station?id='+encodeURIComponent(NODE);},600);}"
    "else{btn.disabled=false;btn.textContent='Deploy node';"
    "document.getElementById('wz-deploy-result').innerHTML=chip(false,'Deploy failed — try again');}"
    "}).catch(function(){btn.disabled=false;btn.textContent='Deploy node';"
    "document.getElementById('wz-deploy-result').innerHTML=chip(false,'Network error — try again');});});"
    "show(cur);"
    "})();</script>");
  html += footCommon();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// LTE upload settings + manual upload handlers
// ---------------------------------------------------------------------------

static String formatUploadTime(uint32_t unixSec) {
  if (unixSec == 0) return String("never");
  DateTime dt(unixSec);
  return formatDateTimeDisplay(dt);
}

static void handleSettings() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t storagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  String actionsHtml = String("<a href='/settings' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Settings", actionsHtml, 3);

  html += F("<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>");

  // Re-run the guided setup wizard (pre-filled with current values) for anyone
  // who'd rather step through onboarding again than hunt through this page.
  html += F("<div class='section section--flat' style='margin-bottom:10px'>"
            "<a href='/setup' class='btn btn--sm'>Run setup wizard</a></div>");

  // --- Schedule change pending (only rendered during a transition) ---
  html += buildScheduleTransitionBanner();

  html += F("<div class='section'>");

  // --- Recording interval presets ---
  // Uses the canonical kAllowedIntervals (same set the setup wizard offers), so
  // an interval chosen in the wizard is always representable/shown selected here.
  html += F("<h3>Recording interval</h3>"
            "<p class='muted'>How often each node measures.</p>"
            "<form class='async-form' action='/set-recording-interval' method='POST'>"
            "<div class='action-choices'>");
  for (size_t i = 0; i < kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<label class='action-choice action-choice--start'><input type='radio' name='interval' value='");
    html += String(v);
    html += F("'");
    if (v == gWakeIntervalMin) html += F(" checked");
    html += F("><span>");
    html += String(v);
    html += F(" min</span></label>");
  }
  html += F("</div>"
            "<button type='submit' class='btn btn--primary' style='margin-top:8px'>Apply</button>"
            "</form>");
  html += F("<p class='muted' style='margin-top:6px'>Selected: ");
  if (gWakeIntervalMin > 0) {
    html += String(gWakeIntervalMin);
    html += F(" min");
  } else {
    html += F("Off");
  }
  html += F("</p>");
  html += F("</div>");

  // --- Cloud connection ---
  html += F("<div class='section'>");
  html += F("<h3>Cloud connection</h3>");

  html += F("<form class='async-form' action='/save-settings' method='POST'>");

  html += F("<label class='label'><input type='checkbox' name='enabled' value='1'");
  if (tx.enabled) html += F(" checked");
  html += F("> <strong>Enable cloud upload</strong></label>");
  html += F("<div class='help'>When enabled, the Mothership uploads new data during collection rounds (subject to schedule and battery checks).</div>");

  const bool remoteManagementEnabled = backendControlRemoteManagementEnabled();
  html += F("<label class='label' style='margin-top:12px'><input type='checkbox' name='remote_management' value='1'");
  if (remoteManagementEnabled) html += F(" checked");
  html += F("> <strong>Allow dashboard changes to nodes</strong></label>");
  html += F("<div class='help'>Lets changes made in the dashboard reach nodes during the next sync. Turning it off just stops that — logging, syncing and uploads keep working.</div>");

  html += F("<label class='label' for='api_key'>API Key</label>");
  if (tx.apiKey.length() > 0) {
    html += F("<p class='muted'>API key configured (last 4: ");
    html += tx.apiKey.substring(tx.apiKey.length() - 4);
    html += F(")</p>");
  }
  html += F("<input class='input' id='api_key' name='api_key' type='password' placeholder='Enter new key to replace' value=''>");
  html += F("<div class='help'>Leave blank to keep the current key, or enter a new <code>fm_xxxxxxxx</code> API key from the fieldMesh dashboard.</div>");

  html += F("<label class='label' for='qr_string' style='margin-top:10px'>QR code string (optional)</label>");
  html += F("<input class='input' id='qr_string' name='qr_string' type='text' placeholder='Paste url|key here'>");
  html += F("<div class='help'>If you have a QR code string of the form <code>url|key</code>, paste it here to set both endpoint and API key at once.</div>");

  html += F("<label class='label' style='margin-top:10px'>Endpoint (read-only)</label>");
  html += F("<p class='muted' style='word-break:break-all;font-size:13px'>");
  html += htmlEscape(tx.endpointUrl);
  html += F("</p>");

  // FieldHub hardware identity — the customer-facing code + canonical MAC used
  // to register this hub in the dashboard. No customer-facing "Device ID".
  html += F("<label class='label' style='margin-top:10px'>FieldHub hardware code</label>");
  html += F("<p class='muted' style='font-size:14px;font-weight:600'>");
  html += htmlEscape(hwCode());
  html += F("</p>");
  html += F("<label class='label'>Hardware MAC</label>");
  html += F("<p class='muted' style='font-family:monospace;font-size:13px'>");
  html += htmlEscape(hwMacString());
  html += F("</p>");
  html += F("<div class='help'>Use the code on the home screen to register this FieldHub in the FieldMesh dashboard, then scan the provisioning code it gives you.</div>");

  // Cloud status line
  {
    const char* dotColour = "#6b7280";
    const char* statusLabel = "Upload off";
    if (tx.enabled) {
      if (tx.apiKey.length() > 0) {
        if (cursor.retryCount > 0) {
          dotColour = "#c47a5a";
          statusLabel = "Last upload failed";
        } else {
          dotColour = "#7a9b70";
          statusLabel = "Connected";
        }
      } else {
        dotColour = "#c47a5a";
        statusLabel = "Not set up";
      }
    }
    html += F("<p style='margin:8px 0'><span style='display:inline-block;width:12px;height:12px;border-radius:50%;background:");
    html += dotColour;
    html += F(";margin-right:8px;vertical-align:middle'></span><strong>");
    html += statusLabel;
    html += F("</strong>");
    if (cursor.lastUploadUnix > 0) {
      html += F(" &middot; last upload ");
      html += formatUploadTime(cursor.lastUploadUnix);
    }
    html += F("</p>");
  }

  // --- Advanced settings (collapsed) ---
  html += F("<details style='margin-top:14px'>");
  html += F("<summary style='font-weight:bold;cursor:pointer'>Advanced settings</summary>");

  // Upload interval, min-battery, max-bytes and max-retries are no longer exposed
  // here — they're fixed at sensible defaults in transmission_settings (upload
  // every sync, 3500 mV 1S Li-ion guard, 96 KB cap, 3 retries).

  html += F("<label class='label'><input type='checkbox' name='allow_manual' value='1'");
  if (tx.allowManualUpload) html += F(" checked");
  html += F("> <strong>Allow manual upload from this page</strong></label>");
  html += F("<div class='help'>Manual upload powers on the modem and transmits now. This takes 30-60s and draws extra power.</div>");

  // Legacy fields (Auth token / Site ID / Deployment ID) hidden for now — their
  // stored values are preserved untouched on save (see handleSetTransmission).

  html += F("</details>");

  html += F("<button type='submit' class='btn btn--primary' style='margin-top:12px'>Save</button>");
  html += F("</form>");
  html += F("</div>");

  // --- Storage + manual upload status ---
  html += F("<div class='section'><h3>Storage</h3>");
  html += F("<div class='stats' style='margin:0 0 10px 0'>");
  html += F("<div class='stat'><strong>Readings waiting</strong><span class='num' style='font-size:16px'>");
  html += String(pendingRows);
  html += F("</span></div>");
  html += F("<div class='stat'><strong>Last upload</strong><span class='num' style='font-size:14px'>");
  html += formatUploadTime(cursor.lastUploadUnix);
  html += F("</span></div>");
  html += F("<div class='stat'><strong>Storage used</strong><span class='num' style='font-size:16px'>");
  html += String(storagePct);
  html += F("%</span></div></div>");

  if (tx.allowManualUpload) {
    html += F("<form action='/manual-upload' method='POST'>");
    html += F("<input type='hidden' name='ajax' value='1'>");
    html += F("<button type='submit' class='btn btn--warn'>Upload now (30-60s)</button>");
    html += F("</form>");
  } else {
    html += F("<p class='muted'>Manual upload is disabled in advanced settings.</p>");
  }

  html += F("</div>");

  // --- Save & Start Monitoring ---
  html += F("<div class='section action-stack'>"
            "<form class='async-form' action='/start' method='POST'>"
            "<button type='submit' class='btn btn--primary'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M8 5v14l11-7z'/></svg> Save &amp; Start Recording</button>"
            "</form>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetTransmission() {
  TransmissionSettings tx;
  tx.enabled          = server.hasArg("enabled") && server.arg("enabled") == "1";
  tx.endpointUrl      = server.arg("url");
  tx.apiKey           = server.arg("api_key");  // may be blank; preserved below
  // authToken / siteId / deploymentId inputs are hidden — carried from prev below.

  // QR code string: if present and contains '|', split into url|key.
  String qrString = server.arg("qr_string");
  qrString.trim();
  if (qrString.length() > 0) {
    int pipeIdx = qrString.indexOf('|');
    if (pipeIdx > 0) {
      tx.endpointUrl = qrString.substring(0, pipeIdx);
      tx.apiKey      = qrString.substring(pipeIdx + 1);
      Serial.printf("[UI] QR string parsed: endpoint set, key length=%u\n",
                    (unsigned)tx.apiKey.length());
    } else {
      // No pipe — treat the whole string as an API key.
      tx.apiKey = qrString;
    }
  }
  tx.allowManualUpload = server.hasArg("allow_manual") && server.arg("allow_manual") == "1";

  // Remote dashboard control is a plain on/off — enabling it isn't a big deal
  // (it only lets dashboard-queued changes reach nodes at the next sync), so
  // there is no separate "I understand" confirmation gate.
  const bool remoteRequested = server.hasArg("remote_management") &&
                               server.arg("remote_management") == "1";

  // uploadIntervalMin / minBatteryMv / maxBytesPerSession / maxRetriesPerWindow
  // are no longer user-editable — carry the fixed defaults straight from load
  // (which forces them), along with the phase anchor this form doesn't edit.
  TransmissionSettings prev;
  loadTransmissionSettings(prev);
  tx.uploadPhaseUnix     = prev.uploadPhaseUnix;
  tx.uploadIntervalMin   = prev.uploadIntervalMin;
  tx.minBatteryMv        = prev.minBatteryMv;
  tx.maxBytesPerSession  = prev.maxBytesPerSession;
  tx.maxRetriesPerWindow = prev.maxRetriesPerWindow;
  tx.useJsonUpload = prev.useJsonUpload;
  tx.mothershipId = prev.mothershipId;  // not editable via this form yet
  tx.projectId = prev.projectId;        // not editable via this form yet
  tx.authToken    = prev.authToken;     // legacy fields hidden — preserve stored values
  tx.siteId       = prev.siteId;
  tx.deploymentId = prev.deploymentId;
  // Keep the previous API key if the form field was left blank (no QR string).
  if (tx.apiKey.length() == 0 || tx.apiKey.indexOf("\u2022") >= 0) {
    tx.apiKey = prev.apiKey;
  }
  // The Settings page renders the endpoint read-only (no <input name='url'>),
  // so server.arg("url") is always "" except via the legacy qr_string pipe
  // format. Without this, every plain Settings save would silently wipe the
  // endpoint set by /provision-apply back to blank (masked today only because
  // the load-time fallback happens to match the sole allow-listed endpoint).
  if (tx.endpointUrl.length() == 0) {
    tx.endpointUrl = prev.endpointUrl;
  }

  if (!backendControlSetRemoteManagementEnabled(remoteRequested)) {
    if (isAjaxRequest()) sendAjaxResult(false, "Could not persist remote management setting");
    else server.send(500, "text/plain", "Could not persist remote management setting");
    return;
  }

  saveTransmissionSettings(tx);
  Serial.printf("[UI] Transmission settings saved: enabled=%d url=%s site=%s\n",
                tx.enabled ? 1 : 0, tx.endpointUrl.c_str(), tx.siteId.c_str());

  if (isAjaxRequest()) {
    sendAjaxResult(true, "Settings saved");
    return;
  }

  String html = headCommon("Settings", "<a href='/' class='btn btn--sm'>Back</a>");
  html += F("<div class='section center'><h3>Settings saved</h3>"
            "<a href='/settings' class='btn btn--primary'>Back to Settings</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleManualUpload() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  if (!tx.allowManualUpload) {
    if (isAjaxRequest()) {
      sendAjaxResult(false, "Manual upload is disabled in settings");
      return;
    }
    String html = headCommon("Settings", "<a href='/settings' class='btn btn--sm'>Back</a>");
    html += F("<div class='section center'><h3>Manual upload disabled</h3>"
              "<p class='muted'>Enable manual upload in advanced settings first.</p>"
              "<a href='/settings' class='btn btn--primary'>Back</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
    return;
  }

  // NOTE: This is a blocking handler — the web request hangs for 30-60s
  // while the modem powers on, registers, and uploads.  This could be
  // improved later with a background task + status polling, but for V1
  // field use the blocking approach is acceptable.
  Serial.println("[UI] Manual upload requested — starting blocking sequence");

  String resultMsg;
  bool ok = false;

  if (!flashIsReady()) {
    resultMsg = "Storage not ready — cannot read data";
  } else {
    gUploadQueue.init();
    if (gUploadQueue.getPendingBytes() == 0) {
      resultMsg = "No new data to upload";
      ok = true;
    } else {
      ModemDriver modem;
      modem.init();

      // Resting battery reading, before the modem rail loads it down.
      const float manRestingBatV = readBatteryVoltage();
      const uint32_t manSessionStartMs = millis();
      if (!modem.powerOn()) {
        resultMsg = "Modem power-on failed";
      } else if (!modem.waitForNetwork(60000)) {
        resultMsg = "Network registration failed/timeout (antenna connected?)";
        modem.gracefulShutdown();
      } else {
        // Batch upload: up to 100 readings per POST. Loop a bounded number of
        // POSTs per click so a manual upload drains a useful amount without
        // tying up config mode indefinitely.
        const String authHeader = tx.apiKey.length() > 0 ? tx.apiKey : tx.authToken;
        const String url = buildUploadUrl(tx);
        const int kMaxManualPosts = 6;   // up to ~600 readings per click
        int posts = 0;
        int sent = 0;
        bool stop = false;
        String lastErr;

        // Build status context for manual uploads.
        const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
        const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
        const uint32_t flashPct = (fsTotal > 0)
          ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;
        const UploadCursor manCursor = gUploadQueue.getCursor();
        const auto allNodes = getRegisteredNodes();
        uint16_t mTotal = 0, mDeployed = 0, mPaired = 0, mUnpaired = 0, mPending = 0, mPaused = 0;
        for (const auto& n : allNodes) {
          mTotal++;
          if (n.state == DEPLOYED) mDeployed++;
          else if (n.state == PAIRED) mPaired++;
          else mUnpaired++;
          if (n.stateChangePending || n.deployPending) mPending++;
          if (n.state == DEPLOYED && n.recordingPaused) mPaused++;
        }
        extern uint32_t g_projectStartedUnix;  // defined in main.cpp
        extern String   g_resetReasonStr;      // defined in main.cpp
        extern uint32_t g_bootCount;           // defined in main.cpp
        const uint32_t manNowUnix = getRTCTimeUnix();
        const char* manLastResult =
            (manCursor.lastUploadUnix > 0 && manCursor.retryCount == 0) ? "success"
            : (manCursor.retryCount > 0) ? "failed" : "pending";

        // Radio link quality + modem identity (queried live while registered).
        ModemDiagnostics manDiag;
        modem.getDiagnostics(manDiag);
        // 0xFFFFFFFF = "not tracked" -> emitted as null (manual path doesn't
        // time registration).
        const String manModemJson = modemDiagnosticsToJson(manDiag, 0xFFFFFFFFUL);
        const float manLoadedBatV = readBatteryVoltage();  // under modem load
        const String manDiagJson =
            String("{\"resetReason\":\"") + g_resetReasonStr +
            "\",\"bootCount\":" + String(g_bootCount) +
            ",\"freeHeap\":" + String((unsigned)ESP.getFreeHeap()) +
            ",\"minFreeHeap\":" + String((unsigned)ESP.getMinFreeHeap()) +
            ",\"snapQueueDropped\":0" +
            ",\"batLoadedV\":" +
            (isnan(manLoadedBatV) ? String("null") : String(manLoadedBatV, 2)) +
            ",\"sessionMs\":" + String((unsigned)(millis() - manSessionStartMs)) + "}";

        const StatusContext manStatusCtx = {
          manRestingBatV, flashPct, fsTotal, fsUsed,
          "manual", computeNextSyncIsoLocal(),
          gWakeIntervalMin, gSyncIntervalMin,
          (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily",
          mTotal, mDeployed, mPaired, mUnpaired,
          gUploadQueue.getPendingRows(), manCursor.rowsUploaded,
          manCursor.retryCount, manCursor.lastUploadUnix,
          FW_SEMVER, FW_BUILD, manNowUnix,
          WiFi.macAddress(),
          mPending, tx.enabled,
          gUploadQueue.getPendingRows(), (uint64_t)getCSVFileSize(), String(""),
          buildNodesStatusJson(manNowUnix),
          buildTransmissionStatusJson(tx),
          (gSyncMode == SYNC_MODE_DAILY)
              ? formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute) : String(""),
          gUploadQueue.getPendingBytes(),
          g_projectStartedUnix,
          manLastResult,
          manModemJson,
          manDiagJson,
          mPaused,
          mothershipFirmwareStatusJson(),
          backendControlStatusJson()
        };

        while (posts < kMaxManualPosts && gUploadQueue.getPendingRows() > 0 && !stop) {
          UploadPayload payload = gUploadQueue.getNewData(16384);
          if (payload.byteLength == 0) break;

          // Status object only on the first POST of this click (it doesn't
          // change between chunks). posts increments after a POST, so it stays
          // 0 until the first real POST; malformed-skip iterations don't POST.
          JsonPayload json = buildJsonUpload(payload.csvData, 100, FW_SEMVER,
                                             (posts == 0) ? &manStatusCtx : nullptr,
                                             getRTCTimeUnix());
          if (json.ok && json.rowCount == 0 && json.csvBytesConsumed > 0) {
            // Malformed row(s) skipped by the builder — advance past them and
            // keep going, don't POST or error out.
            gUploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, getRTCTimeUnix());
            gUploadQueue.purgeUploaded();
            continue;
          }
          if (!json.ok || json.rowCount == 0) { lastErr = "JSON build failed (heap?)"; break; }

          HttpsPostResult result = modem.httpsPost(url, json.body, "application/json", authHeader);
          posts++;
          if (result.httpStatus == 200) {
            gUploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, getRTCTimeUnix(),
                                       json.rowCount);
            gUploadQueue.purgeUploaded();
            gUploadQueue.resetRetryCount();
            sent += json.rowCount;
            ingestBackendResponseFromUi(result.responseBody);
          } else if (result.httpStatus == 401 || result.httpStatus == 400) {
            char buf[80];
            snprintf(buf, sizeof(buf), "HTTP %d (%s)",
                     result.httpStatus, result.httpStatus == 401 ? "unauthorized" : "bad request");
            lastErr = String(buf);
            stop = true;  // not transient — don't increment retry counter
          } else {
            char buf[80];
            snprintf(buf, sizeof(buf), "HTTP %d, %s", result.httpStatus, result.errorDetail.c_str());
            lastErr = String(buf);
            gUploadQueue.incrementRetryCount();
            stop = true;
          }
        }
        modem.gracefulShutdown();

        if (sent > 0 && lastErr.length() == 0) {
          char buf[64];
          snprintf(buf, sizeof(buf), "Upload OK: %d reading%s sent", sent, sent == 1 ? "" : "s");
          resultMsg = String(buf); ok = true;
        } else if (sent > 0) {
          char buf[96];
          snprintf(buf, sizeof(buf), "Sent %d then stopped: %s", sent, lastErr.c_str());
          resultMsg = String(buf); ok = true;
        } else {
          resultMsg = String("Upload failed: ") + (lastErr.length() ? lastErr : String("no readings sent"));
        }
      }
    }
  }

  Serial.printf("[UI] Manual upload result: %s\n", resultMsg.c_str());

  if (isAjaxRequest()) {
    sendAjaxResult(ok, resultMsg);
    return;
  }

  String html = headCommon("Settings", "<a href='/settings' class='btn btn--sm'>Back</a>");
  html += F("<div class='section center'><h3>Manual upload</h3><p>");
  html += resultMsg;
  html += F("</p><a href='/settings' class='btn btn--primary'>Back to Settings</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleUploadStatus() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t flashUsagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  String json;
  json.reserve(320);
  json += "{";
  json += "\"enabled\":";
  json += tx.enabled ? "true" : "false";
  json += ",\"cursorOffset\":";
  json += String(cursor.byteOffset);
  json += ",\"pendingBytes\":";
  json += String(pendingBytes);
  json += ",\"pendingRows\":";
  json += String(pendingRows);
  json += ",\"rowsUploaded\":";
  json += String(cursor.rowsUploaded);
  json += ",\"lastUploadUnix\":";
  json += String(cursor.lastUploadUnix);
  json += ",\"retryCount\":";
  json += String(cursor.retryCount);
  json += ",\"flashUsagePct\":";
  json += String(flashUsagePct);
  json += ",\"flashTotalBytes\":";
  json += String((unsigned long)fsTotal);
  json += ",\"flashUsedBytes\":";
  json += String((unsigned long)fsUsed);
  json += "}";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Start + loop
// ---------------------------------------------------------------------------
// GET /api/identity — local, unauthenticated hardware identity for diagnostics
// and installer tooling. Never includes API keys, project IDs, or any secret.
// The MAC here is the same canonical STA MAC shown on the portal card, used for
// the AP suffix, embedded in the hardware QR, and reported in upload status.
static void handleApiIdentity() {
  String body;
  body.reserve(160);
  body += F("{\"hardwareMac\":\"");
  body += hwMacString();
  body += F("\",\"hardwareCode\":\"");
  body += hwCode();
  body += F("\",\"firmwareVersion\":\"");
  body += FW_SEMVER;
  body += F("\"}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

// Shared connection-key provisioning widget markup + logic, used by both the
// standalone /provision page and the setup wizard's Connect step so the flow
// lives in exactly one place. Three ways to get the code in, all funneling into
// the same confirm/apply call:
//   1. URL-fragment auto-detect (arrived via a scanned/tapped QR link) — the
//      credential rides in the fragment, never reaching the hub as a request.
//   2. Manual paste — works on any browser, no camera at all.
//   3. In-page camera scan (getUserMedia + native BarcodeDetector) — decodes
//      frames inside the current tab, never handing off to the OS camera app,
//      which is what dropped some phones off the AP mid-scan.
// `onSuccessJs` runs after a successful POST /provision-apply (the URL fragment
// is always cleared first). It has access to the widget's local `R` (result
// div) and `C` (confirm div); for the standalone page it renders a "connected,
// back home" result, for the wizard it advances to the next step.
static String provisionWidgetHtml(const String& onSuccessJs) {
  String h = F(
    "<div id='pv-empty'>"
    "<p class='muted'>Paste the connection code shown in the FieldMesh dashboard, or scan it.</p>"
    "<textarea id='pv-paste' class='input' rows='3' placeholder='Paste connection code here' "
    "style='font-family:monospace;font-size:13px' autocomplete='off' autocapitalize='off' spellcheck='false'></textarea>"
    "<button id='pv-paste-go' class='btn btn--primary' style='margin-top:8px;width:100%'>Use pasted code</button>"
    "<div style='margin-top:14px'>"
    "<button id='pv-scan-go' class='btn' style='width:100%' hidden>Scan with camera</button>"
    "<p id='pv-scan-unsupported' class='muted' style='display:none'>"
    "Camera scanning isn't supported on this browser — paste the code instead, "
    "or scan the QR with your phone's camera app and open the link it finds.</p>"
    "<video id='pv-video' style='display:none;width:100%;border-radius:8px;margin-top:8px' playsinline muted></video>"
    "</div></div>"
    "<div id='pv-confirm' style='display:none'>"
    "<p>Endpoint: <strong id='pv-host'></strong></p>"
    "<p><span class='chip chip--cfg-ok' style='font-weight:600'>Connection key received</span></p>"
    "<p class='muted'>Applying this will connect the FieldHub to your project and test the connection.</p>"
    "<button id='pv-apply' class='btn btn--primary'>Apply &amp; connect</button> "
    "<button id='pv-cancel' class='btn btn--sm'>Cancel</button></div>"
    "<div id='pv-error' style='display:none'><p><span class='chip chip--bat-low' style='font-weight:600'>"
    "Invalid provisioning code</span></p><p class='muted' id='pv-error-msg'></p>"
    "<button id='pv-retry' class='btn'>Try again</button></div>"
    "<div id='pv-result' style='display:none'></div>");
  // Payload parsing + confirmation. The key is never logged or rendered.
  h += F(
    "<script>(function(){"
    "var E=document.getElementById('pv-empty'),C=document.getElementById('pv-confirm'),"
    "X=document.getElementById('pv-error'),R=document.getElementById('pv-result');"
    "var stream=null;"
    "function stopScan(){if(stream){stream.getTracks().forEach(function(t){t.stop();});stream=null;}"
    "document.getElementById('pv-video').style.display='none';}"
    "function reset(){stopScan();X.style.display='none';R.style.display='none';C.style.display='none';"
    "E.style.display='block';}"
    "function fail(m){stopScan();E.style.display='none';C.style.display='none';R.style.display='none';"
    "X.style.display='block';document.getElementById('pv-error-msg').textContent=m||'';}"
    "document.getElementById('pv-retry').addEventListener('click',reset);"
    "function tryPayload(raw){"
    "raw=(raw||'').trim();"
    "var i=raw.indexOf('FM1.');if(i<0){fail('Unrecognised code — no FM1 code found.');return;}"
    "raw=raw.slice(i);"
    "var b64=raw.slice(4).replace(/-/g,'+').replace(/_/g,'/');"
    "while(b64.length%4){b64+='=';}"
    "var payload;try{payload=JSON.parse(atob(b64));}catch(e){fail('Could not read the code.');return;}"
    "if(payload.v!==1||!payload.endpoint||!payload.connectionKey){fail('Code is missing required fields.');return;}"
    "var host;try{host=new URL(payload.endpoint).host;}catch(e){host=payload.endpoint;}"
    "stopScan();E.style.display='none';X.style.display='none';"
    "document.getElementById('pv-host').textContent=host;C.style.display='block';"
    "C.dataset.raw=raw;"
    "}"
    "document.getElementById('pv-paste-go').addEventListener('click',function(){"
    "tryPayload(document.getElementById('pv-paste').value);});"
    "document.getElementById('pv-cancel').addEventListener('click',reset);"
    "document.getElementById('pv-apply').addEventListener('click',function(){"
    "var btn=this;btn.disabled=true;btn.textContent='Applying...';"
    "fetch('/provision-apply',{method:'POST',headers:{'Content-Type':'text/plain'},body:C.dataset.raw})"
    ".then(function(r){return r.json();}).then(function(j){"
    "C.style.display='none';"
    "if(j.ok){if(history.replaceState){history.replaceState(null,'',location.pathname);}");
  h += onSuccessJs;   // injected success behavior (standalone result vs. wizard advance)
  h += F("}"
    "else{R.style.display='block';"
    "R.innerHTML=\"<p><span class='chip chip--bat-low' style='font-weight:600'>Could not connect</span></p>"
    "<p class='muted' id='pv-rmsg'></p>\";"
    "document.getElementById('pv-rmsg').textContent=j.message||'';"
    "var rb=document.createElement('button');rb.className='btn';rb.textContent='Try again';rb.onclick=reset;"
    "R.appendChild(rb);}"
    "}).catch(function(){C.style.display='none';R.style.display='block';"
    "R.innerHTML=\"<p class='muted'>Network error while applying. Try again.</p>\";"
    "var rb=document.createElement('button');rb.className='btn';rb.textContent='Try again';rb.onclick=reset;"
    "R.appendChild(rb);});"
    "});"
    // In-page camera scan (progressive enhancement): stays inside this tab —
    // no OS camera-app handoff, so the phone's Wi-Fi association to the
    // FieldHub AP is never disturbed by an app switch.
    "if('BarcodeDetector' in window && navigator.mediaDevices && navigator.mediaDevices.getUserMedia){"
    "var scanBtn=document.getElementById('pv-scan-go');scanBtn.hidden=false;"
    "scanBtn.addEventListener('click',function(){"
    "var video=document.getElementById('pv-video');"
    "navigator.mediaDevices.getUserMedia({video:{facingMode:'environment'}}).then(function(s){"
    "stream=s;video.srcObject=s;video.style.display='block';video.play();"
    "var detector=new BarcodeDetector({formats:['qr_code']});"
    "var tick=function(){"
    "if(!stream)return;"
    "detector.detect(video).then(function(codes){"
    "if(codes.length){tryPayload(codes[0].rawValue);}"
    "else{requestAnimationFrame(tick);}"
    "}).catch(function(){requestAnimationFrame(tick);});"
    "};requestAnimationFrame(tick);"
    "}).catch(function(){fail('Camera permission denied. Paste the code instead.');});"
    "});"
    "}else{document.getElementById('pv-scan-unsupported').style.display='block';}"
    // Fragment auto-detect last, so a scanned/tapped QR link still works.
    "var fragRaw=(location.hash||'').replace(/^#/,'');"
    "if(fragRaw){tryPayload(fragRaw);}"
    "})();</script>");
  return h;
}

// GET /provision — standalone connection-key provisioning page. This is the
// documented external contract for the dashboard's QR
// (http://192.168.4.1/provision#FM1.<base64url-payload>); it stays available
// independent of the setup wizard.
static void handleProvisionPage() {
  String html = headCommon("Connect FieldHub", F("<a href='/' class='btn btn--sm'>Back</a>"));
  html += F("<div class='section'><h3>Connect this FieldHub</h3>");
  // Success continues INTO the setup wizard rather than dead-ending here. This
  // matters for the common case where the user scans the dashboard QR with their
  // phone's native camera: that opens this /provision page (often in a different
  // browser than the wizard), and without this the wizard would be abandoned.
  // The wizard is server-driven and resumes from device state, so continuing —
  // even in a new browser — picks up exactly where setup should go next.
  html += provisionWidgetHtml(F(
    "R.style.display='block';"
    "R.innerHTML=\"<p><span class='chip chip--cfg-ok' style='font-weight:600'>Connected</span></p>"
    "<p class='muted'>This FieldHub is now connected. Continue setup to finish.</p>"
    "<a href='/setup' class='btn btn--primary' style='width:100%;text-align:center'>Continue setup</a>\";"));
  html += F("</div>");
  html += footCommon();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

// POST /provision-apply — body is the raw "FM1.<base64url>" payload (text/plain,
// so the key never lands in a URL/query/log). Firmware is authoritative: it
// re-decodes and validates server-side (never trusting the client), enforces
// the endpoint allow-list, and only then persists. A malformed/disallowed
// payload leaves existing stored credentials untouched. The plaintext key is
// never serial-printed, echoed, or included in any response.
static void handleProvisionApply() {
  String payload = server.hasArg("plain") ? server.arg("plain") : String("");
  payload.trim();

  ProvisioningPayload prov;
  ProvisionParseResult r = hwParseProvisioning(payload, prov);
  if (r != PROV_OK) {
    Serial.printf("[PROVISION] rejected: %s\n", hwProvisionResultStr(r));
    const char* msg =
        (r == PROV_ENDPOINT_NOT_ALLOWED) ? "Endpoint not approved for FieldMesh" :
        (r == PROV_BAD_VERSION)          ? "Unsupported provisioning code version" :
                                           "Provisioning code could not be read";
    sendAjaxResult(false, msg);
    return;
  }

  // Apply onto existing settings: connectionKey -> Bearer apiKey, endpoint ->
  // upload URL. All other fields (including any previously stored ones) are
  // preserved. Only reached for a valid, approved payload.
  TransmissionSettings tx;
  loadTransmissionSettings(tx);
  tx.endpointUrl = prov.endpoint;
  tx.apiKey      = prov.connectionKey;
  // Auto-enable cloud upload on a successful provision. Applying a connection
  // key is an explicit "connect this hub" action, so there is no reason to make
  // the user then separately hunt for and tick an "Enable cloud upload" box —
  // that silent extra step was a reported onboarding snag.
  tx.enabled     = true;
  saveTransmissionSettings(tx);
  // Log only non-secret facts — never the key.
  Serial.printf("[PROVISION] applied: endpoint=%s keyLen=%u\n",
                prov.endpoint.c_str(), (unsigned)prov.connectionKey.length());

  sendAjaxResult(true, "Provisioned");
}

// GET /setup — first-run onboarding wizard. One server-rendered page whose
// steps are shown/hidden client-side (same pattern as the provision widget),
// each step reusing the existing per-setting endpoints so there is no duplicate
// server logic. The wizard is intentionally hub-focused — get the FieldHub
// online and under dashboard control — and does NOT deal with node deployment
// (pairing/discovery is the node manager's job):
//   1 Register (manual MAC entry / QR)  2 Connect (shared provision widget)
//   3 Cloud confirmed  4 Dashboard control (/set-remote-management)
//   5 Time (/set-time)  6 Recording interval (/set-wake-interval — the upload/
//   sync cadence is auto-derived from this, never set by the user)  7 Review
//   8 Finish (/start). The start step is computed from device state so a
// mid-flow refresh doesn't force redoing completed steps.
static void handleSetupWizard() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);
  const bool hasKey  = tx.apiKey.length() > 0;
  const bool hasTime = getRTCTimeUnix() >= kMinValidPhaseUnix;
  const bool remoteEnabled = backendControlRemoteManagementEnabled();
  const String mac = hwMacString();

  // Endpoint host for the review line (strip scheme + path).
  String host = tx.endpointUrl;
  int schemeEnd = host.indexOf("://");
  if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
  int pathStart = host.indexOf('/');
  if (pathStart >= 0) host = host.substring(0, pathStart);

  // Resume point on a fresh page load (e.g. after scanning the dashboard QR,
  // which provisions via /provision then continues here, possibly in a new
  // browser). Once connected, land on the first post-connect config step so the
  // rest of setup (Dashboard control → Time → Recording) still gets walked;
  // registration + connect are already done at that point.
  int startStep = hasKey ? 4 : 1;

  String html = headCommon("Set up FieldHub", F("<a href='/' class='btn btn--sm'>Exit</a>"));
  html += F("<div class='section'>"
            "<p class='muted' style='margin:0 0 10px'>Setup &middot; step <span id='wz-cur'>1</span> of 8</p>");

  // Step 1 — Register
  html += F("<div class='wz-step' data-step='1'>"
            "<h3>1. Register this FieldHub</h3>"
            "<p class='muted'>In the FieldMesh dashboard, add a new FieldHub using this address:</p>"
            "<div class='help'><strong>Hardware MAC</strong><br>"
            "<span id='wz-mac' style='font-family:monospace;font-size:20px;font-weight:700'>");
  html += htmlEscape(mac);
  html += F("</span></div>"
            "<button type='button' class='btn btn--sm' style='margin-top:10px' id='wz-copy'>Copy MAC</button>"
            "<details style='margin-top:10px'><summary style='font-size:14px;color:var(--sub);cursor:pointer'>Or scan instead</summary>"
            "<div style='margin-top:8px;text-align:center'>");
  html += renderQrSvg(hwRegisterUri(mac));
  html += F("</div></details>"
            "<div style='margin-top:16px'><button type='button' class='btn btn--primary' data-wz='next'>I've registered — continue</button></div>"
            "</div>");

  // Step 2 — Connect (shared provision widget; success records cloud state +
  // endpoint host for the Review step, then advances to step 3).
  html += F("<div class='wz-step' data-step='2' style='display:none'>"
            "<h3>2. Connect to FieldMesh</h3>");
  html += provisionWidgetHtml(F(
    "if(window.wzConnected)window.wzConnected(document.getElementById('pv-host').textContent);"));
  html += F("<div style='margin-top:12px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button></div>"
            "</div>");

  // Step 3 — Cloud confirmed
  html += F("<div class='wz-step' data-step='3' style='display:none'>"
            "<h3>3. Cloud upload</h3>"
            "<p><span class='chip chip--cfg-ok' style='font-weight:600'>Cloud upload: Enabled</span></p>"
            "<p class='muted'>Connecting this FieldHub turned on cloud upload automatically. Data will upload to your project during collection rounds.</p>"
            "<div style='margin-top:12px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn btn--primary' data-wz='next'>Continue</button></div>"
            "</div>");

  // Step 4 — Dashboard control
  html += F("<div class='wz-step' data-step='4' style='display:none'>"
            "<h3>4. Dashboard control</h3>"
            "<p class='muted'>Let this FieldHub be configured from the dashboard. Changes you make in the dashboard reach nodes during the next sync. You can change this later in Settings.</p>"
            "<label class='label'><input type='checkbox' id='wz-remote'");
  if (remoteEnabled) html += F(" checked");
  html += F("> Allow dashboard control</label>"
            "<div id='wz-remote-result' class='help' style='margin-top:6px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn btn--primary' data-wz='next'>Continue</button></div>"
            "</div>");

  // Step 5 — Time
  html += F("<div class='wz-step' data-step='5' style='display:none'>"
            "<h3>5. Set the clock</h3>"
            "<p class='muted'>Sets the FieldHub clock from this device (stored as UTC). This is the time reference for the whole fleet.</p>"
            "<p>This device (UTC): <strong id='wz-time-now'>--</strong></p>"
            "<button type='button' class='btn btn--primary' id='wz-time-set'>Use this time</button>"
            "<div id='wz-time-result' class='help' style='margin-top:8px'></div>"
            "<div style='margin-top:12px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn btn--primary' id='wz-time-next' data-wz='next'");
  if (!hasTime) html += F(" disabled");  // already-set clock → don't force a re-set on resume
  html += F(">Continue</button></div>"
            "</div>");

  // Step 6 — Recording interval (last config step). The upload/sync cadence is
  // auto-derived from this by handleSetWakeInterval (computeAutoSyncMin) — the
  // user never sets a sync schedule directly, so there is no sync UI here.
  html += F("<div class='wz-step' data-step='6' style='display:none'>"
            "<h3>6. Recording interval</h3>"
            "<p class='muted'>How often should nodes record a reading? Uploads are scheduled automatically from this.</p>"
            "<div id='wz-int-row' style='display:flex;flex-wrap:wrap;gap:6px'>");
  for (size_t i = 0; i < kAllowedCount; ++i) {
    html += F("<button type='button' class='btn btn--sm wz-int");
    if (kAllowedIntervals[i] == gWakeIntervalMin) html += F(" btn--primary");
    html += F("' data-int='");
    html += String(kAllowedIntervals[i]);
    html += F("'>");
    html += String(kAllowedIntervals[i]);
    html += F(" min</button>");
  }
  html += F("</div><div id='wz-int-result' class='help' style='margin-top:6px'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn' data-wz='skip'>Skip</button> "
            "<button type='button' class='btn btn--primary' data-wz='next'>Continue</button></div>"
            "</div>");

  // Step 7 — Review
  html += F("<div class='wz-step' data-step='7' style='display:none'>"
            "<h3>Review</h3>"
            "<div id='wz-review'></div>"
            "<div style='margin-top:14px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button> "
            "<button type='button' class='btn btn--primary' data-wz='goto' data-goto='8'>Looks good</button></div>"
            "</div>");

  // Step 8 — Done. The wizard only sets the hub up; it deliberately does NOT
  // power down here (that would drop the AP). It hands the user back to the main
  // overview, where they press the existing "Finish & Start Recording" button
  // when ready. Node deployment has its own dedicated flow in the node manager,
  // so the hub wizard makes no mention of it.
  html += F("<div class='wz-step' data-step='8' style='display:none'>"
            "<h3><span class='chip chip--cfg-ok' style='font-weight:700'>FieldHub ready</span></h3>"
            "<p class='muted'>Your FieldHub is connected and configured. When everything's in place, "
            "press <strong>Finish &amp; Start Recording</strong> on the home screen.</p>"
            "<a href='/' class='btn btn--primary' style='width:100%;text-align:center'>Go to overview</a>"
            "<div style='margin-top:10px'><button type='button' class='btn btn--sm' data-wz='back'>Back</button></div>"
            "</div>");

  html += F("</div>");  // .section

  // --- Wizard controller ---
  html += F("<script>(function(){var INIT=");
  html += F("{\"startStep\":");        html += String(startStep);
  html += F(",\"cloudEnabled\":");     html += (tx.enabled && hasKey) ? F("true") : F("false");
  html += F(",\"endpointHost\":\"");   html += host; html += F("\"");
  html += F(",\"interval\":");         html += String(gWakeIntervalMin);
  html += F(",\"remote\":");           html += remoteEnabled ? F("true") : F("false");
  html += F("};");
  html += F(
    "var TOTAL=8,cur=INIT.startStep||1;"
    "var S={cloud:INIT.cloudEnabled,host:INIT.endpointHost,interval:INIT.interval,"
    "remote:INIT.remote,time:false};"
    "var steps=[].slice.call(document.querySelectorAll('.wz-step'));"
    "function post(u,d){var b=Object.keys(d).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(d[k]);}).join('&')+'&ajax=1';"
    "return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(function(r){return r.json();});}"
    "function row(k,v){return \"<p style='margin:4px 0'><strong>\"+k+\":</strong> \"+v+\"</p>\";}"
    "function chip(ok,txt){return \"<span class='chip \"+(ok?'chip--cfg-ok':'chip--bat-low')+\"' style='font-weight:600'>\"+txt+\"</span>\";}"
    "function renderReview(){var iv=S.interval?(S.interval+' min'):'not set (uses default)';"
    "document.getElementById('wz-review').innerHTML="
    "row('Cloud upload',S.cloud?('Enabled'+(S.host?(' &rarr; '+S.host):'')):'Not connected')+"
    "row('Dashboard control',S.remote?'On':'Off')+"
    "row('Recording interval',iv);}"
    "function show(n){cur=Math.max(1,Math.min(TOTAL,n));"
    "steps.forEach(function(s){s.style.display=(parseInt(s.dataset.step,10)===cur)?'block':'none';});"
    "document.getElementById('wz-cur').textContent=cur;"
    "if(cur===7)renderReview();"
    "window.scrollTo(0,0);}"
    "window.wzGo=show;"
    "window.wzConnected=function(host){S.cloud=true;if(host)S.host=host;show(3);};"
    "document.addEventListener('click',function(e){var t=e.target;while(t&&t!==document&&!t.getAttribute('data-wz'))t=t.parentNode;"
    "if(!t||t===document)return;var a=t.getAttribute('data-wz');"
    "if(a==='next'||a==='skip')show(cur+1);else if(a==='back')show(cur-1);"
    "else if(a==='goto')show(parseInt(t.getAttribute('data-goto'),10));});"
    // Step 1 copy
    "document.getElementById('wz-copy').addEventListener('click',function(){"
    "var t=document.getElementById('wz-mac').textContent;if(navigator.clipboard)navigator.clipboard.writeText(t);"
    "this.textContent='Copied';var b=this;setTimeout(function(){b.textContent='Copy MAC';},1500);});"
    // Step 4 dashboard control — immediate chip reinforcement
    "document.getElementById('wz-remote').addEventListener('change',function(){"
    "var on=this.checked;post('/set-remote-management',{remote_management:on?1:0}).then(function(j){"
    "S.remote=on;document.getElementById('wz-remote-result').innerHTML="
    "on?chip(true,'Dashboard control on'):\"<span class='chip' style='font-weight:600'>Dashboard control off</span>\";});});"
    // Step 5 time
    "function nowUtc(){var n=new Date(),z=function(x){return String(x).padStart(2,'0');};"
    "return z(n.getUTCHours())+':'+z(n.getUTCMinutes())+':'+z(n.getUTCSeconds())+' '+z(n.getUTCDate())+'-'+z(n.getUTCMonth()+1)+'-'+n.getUTCFullYear();}"
    "function tickClock(){var el=document.getElementById('wz-time-now');if(el)el.textContent=nowUtc();}"
    "setInterval(tickClock,1000);tickClock();"
    "document.getElementById('wz-time-set').addEventListener('click',function(){"
    "post('/set-time',{datetime:nowUtc()}).then(function(j){"
    "document.getElementById('wz-time-result').innerHTML=chip(j.ok,j.ok?'Clock set':(j.message||'Failed'));"
    "if(j.ok){S.time=true;document.getElementById('wz-time-next').disabled=false;}});});"
    // Step 6 recording interval (sync cadence auto-derived server-side)
    "[].forEach.call(document.querySelectorAll('.wz-int'),function(b){b.addEventListener('click',function(){"
    "var v=parseInt(b.getAttribute('data-int'),10);post('/set-wake-interval',{interval:v}).then(function(j){"
    "S.interval=v;[].forEach.call(document.querySelectorAll('.wz-int'),function(x){x.classList.remove('btn--primary');});"
    "b.classList.add('btn--primary');"
    "document.getElementById('wz-int-result').innerHTML=chip(true,'Recording every '+v+' min &middot; uploads scheduled automatically');});});});"
    "show(cur);"
    "})();</script>");
  html += footCommon();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

static void handleShutdown() {
  gShutdownRequested = true;
  if (isAjaxRequest()) {
    sendAjaxResult(true, "Sync & power down — arming sync alarm");
    return;
  }
  String html = headCommon("Shutting Down");
  html += F("<div class='section center'><h3>Sync &amp; Power Down</h3>"
            "<p>Arming phase-aligned sync alarm and powering off...</p>"
            "<p class='muted'>The board will wake at the next sync time.</p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

// POST /set-node-sensors — set a node's configured ("expected") sensor set.
// Args: node_id, mask (integer bitmask of SNAP_PRESENT_* capability bits the
// operator marked installed; VALID bit is added in firmware). Bumps the node's
// desired configVersion so the selection is delivered + reconciled via NODE_CONFIG
// during the next sync window, and refreshes the RAM cache used for fault display.
// GET /node-sensors?id=<nodeId> — the toggle-button sensor picker. Buttons are
// pre-selected from the node's current configured mask; Save posts to
// /set-node-sensors (from_ui=1) which redirects back to the node page.
static void handleNodeSensorsPage() {
  String nodeId = server.arg("id");
  if (nodeId.length() == 0) nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  String back = String("<a href='/station?id=") + nodeId + "' class='btn btn--sm'>Back</a>";
  String html = headCommon("Configure Sensors", back);
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3><a href='/stations' class='btn btn--primary'>Back to Nodes</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  const NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  const uint16_t mask = (dc.sensorMask & NODE_SENSOR_MASK_VALID)
      ? (uint16_t)(dc.sensorMask & ~NODE_SENSOR_MASK_VALID) : 0;

  html += F("<h3>Sensors installed</h3>");
  html += F("<p class='muted'>Tap the sensors fitted to this node, then Save. "
            "Changes reach the node at its next sync check-in.</p>");

  html += F("<form id='sf' action='/set-node-sensors' method='POST'>");
  html += F("<input type='hidden' name='node_id' value='"); html += nodeId; html += F("'>");
  html += F("<input type='hidden' name='from_ui' value='1'>");
  html += F("<input type='hidden' id='mask' name='mask' value='0'>");
  html += F("<div class='sensor-grid'>");

  struct SensorOption { uint16_t bit; const char* label; const char* grp; };
  static const SensorOption kOptions[] = {
    { (uint16_t)(SNAP_PRESENT_AIR_TEMP | SNAP_PRESENT_AIR_RH), "Air (SHT41)",        "" },
    { SNAP_PRESENT_SPECTRAL,                                   "Spectral (AS7343)",  "" },
    { SNAP_PRESENT_SOIL1,                                      "Soil Probe 1",       "" },
    { SNAP_PRESENT_SOIL2,                                      "Soil Probe 2",       "" },
    { SNAP_PRESENT_WIND,                                       "Wind (Reed cup)",    "wind" },
    { NODE_SENSOR_CFG_WIND_ULTRASONIC,                         "Wind (Ultrasonic)",  "wind" },
    { SNAP_PRESENT_AUX1,                                       "Aux 1",              "" },
    { SNAP_PRESENT_AUX2,                                       "Aux 2",              "" },
  };
  for (const auto& opt : kOptions) {
    const bool sel = (mask & opt.bit) != 0;
    html += F("<button type='button' class='sbtn");
    if (sel) html += F(" sbtn--on");
    html += F("' data-bit='"); html += String((unsigned)opt.bit); html += F("'");
    if (opt.grp[0]) { html += F(" data-grp='"); html += opt.grp; html += F("'"); }
    html += F(">"); html += opt.label; html += F("</button>");
  }

  html += F("</div>");
  html += F("<button type='submit' class='btn btn--success' "
            "style='margin-top:16px;width:100%;padding:14px;font-size:16px'>Save Sensors</button>");
  html += F("</form>");

  html += F("<style>"
    ".sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:8px}"
    ".sbtn{padding:16px 10px;border:2px solid #cbd5e1;border-radius:12px;background:#fff;"
    "font-size:15px;font-weight:600;color:#333;cursor:pointer;text-align:center;min-height:60px;line-height:1.25}"
    ".sbtn--on{background:#1f9d55;border-color:#1a8548;color:#fff}"
    "</style>");
  html += F("<script>(function(){"
    "var f=document.getElementById('sf'),m=document.getElementById('mask');"
    "var btns=f.querySelectorAll('.sbtn');"
    "function recompute(){var v=0;btns.forEach(function(b){"
    "if(b.classList.contains('sbtn--on'))v|=parseInt(b.dataset.bit,10);});m.value=v;}"
    "btns.forEach(function(b){b.addEventListener('click',function(){"
    "var on=!b.classList.contains('sbtn--on');"
    "if(on&&b.dataset.grp){btns.forEach(function(o){"
    "if(o!==b&&o.dataset.grp===b.dataset.grp)o.classList.remove('sbtn--on');});}"
    "b.classList.toggle('sbtn--on',on);recompute();});});"
    "recompute();})();</script>");

  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetNodeSensors() {
  String nodeId = server.arg("node_id");
  if (nodeId.length() == 0 || !server.hasArg("mask")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"node_id and mask required\"}");
    return;
  }
  // Keep only the operator-selectable bits (9 present bits + ultrasonic selector).
  const uint16_t capBits = (uint16_t)((uint32_t)server.arg("mask").toInt() & NODE_SENSOR_CFG_ALL_BITS);
  // Always authoritative: even an all-off selection is a deliberate config, so
  // the node stops auto-registering passive sensors. (0 without VALID = auto.)
  const uint16_t storedMask = (uint16_t)(capBits | NODE_SENSOR_MASK_VALID);

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }
  if (!target) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"unknown node\"}");
    return;
  }

  NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  if (dc.sensorMask != storedMask) {
    dc.sensorMask = storedMask;
    const NodeConfigApplyResult applied = applyLocalDesiredConfig(nodeId, dc);
    if (!applied.durable || !applied.registryApplied ||
        (applied.command.outcome != OUT_ACCEPTED &&
         applied.command.outcome != OUT_REPLAY)) {
      server.send(500, "application/json",
                  "{\"ok\":false,\"error\":\"config persistence failed\"}");
      return;
    }
    dc = getDesiredConfig(nodeId.c_str());
  }
  setNodeExpectedSensorMask(nodeId.c_str(), capBits);

  Serial.printf("[CONFIG] %s sensor mask -> 0x%04X desired v%u\n",
                nodeId.c_str(), (unsigned)storedMask, (unsigned)dc.configVersion);

  // Submitted from the on-device picker page — send the operator back to the
  // node detail page to finish deployment. API callers omit from_ui and get JSON.
  if (server.hasArg("from_ui")) {
    server.sendHeader("Location", String("/station?id=") + nodeId);
    server.send(303, "text/plain", "");
    return;
  }

  String resp = String("{\"ok\":true,\"nodeId\":\"") + nodeId +
                "\",\"sensorMask\":" + String((unsigned)capBits) +
                ",\"configVersion\":" + String((unsigned)dc.configVersion) + "}";
  server.send(200, "application/json", resp);
}

// GET /api/control - authoritative revision, cursor, enable flag and results.
// Uses the backend-control serializer so local and cloud views stay identical.
static void handleControlStatus() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", backendControlStatusJson());
}

// ---------------------------------------------------------------------------
// Local mothership self-update (/firmware) — plan §7.1
// Step 1: POST /firmware/manifest  (body = manifest.json, ?sig=<128 hex>)
// Step 2: POST /firmware/image     (multipart file = matching mothership.bin)
// ---------------------------------------------------------------------------
static void handleFirmwarePage() {
  FirmwareIdentity id = fwIdentity(NODE_PROTOCOL_VERSION);
  MothershipOtaStatus s = mothershipOtaGetStatus();
  const esp_partition_t* run = esp_ota_get_running_partition();

  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<h2>Firmware self-update</h2>");
  h += "<p><b>Running:</b> "; h += id.role; h += " v"; h += id.semver;
  h += " build "; h += id.buildId; h += " hw "; h += id.hwTarget;
  h += " slot "; h += (run ? run->label : "?"); h += "</p>";
  h += "<p><b>Staged manifest:</b> "; h += (s.manifestReady ? "yes" : "no");
  if (s.manifestReady) { h += " &rarr; v"; h += s.targetVersion; h += " ("; h += s.targetReleaseId;
                         h += "), "; h += String(s.expectedSize); h += " bytes"; }
  h += "</p><p><b>Last result:</b> "; h += fwReasonStr(s.lastReason); h += "</p>";
  h += F("<hr><p>1. Stage signed manifest, then 2. upload the matching binary. "
         "A bad signature, wrong hardware, or hash mismatch is rejected and the "
         "running firmware is left untouched.</p>"
         "<form method=POST action='/firmware/image' enctype='multipart/form-data'>"
         "<input type=file name=fw> <button>Upload &amp; install</button></form>");
  server.send(200, "text/html", h);
}

static void handleFirmwareManifest() {
  String body = server.arg("plain");
  String sigHex = server.arg("sig");
  if (body.length() == 0 || sigHex.length() != 128) {
    server.send(400, "application/json",
                "{\"error\":\"POST manifest.json as body, ?sig=<128 hex>\"}");
    return;
  }
  uint8_t sig[64];
  if (!fwHexToBytes(sigHex.c_str(), sig, 64)) {
    server.send(400, "application/json", "{\"error\":\"bad signature hex\"}");
    return;
  }
  FwReason r = mothershipOtaVerifyManifest((const uint8_t*)body.c_str(), body.length(), sig);
  MothershipOtaStatus s = mothershipOtaGetStatus();
  String resp = String("{\"reason\":\"") + fwReasonStr(r) +
                "\",\"ready\":" + (s.manifestReady ? "true" : "false") +
                ",\"targetVersion\":\"" + s.targetVersion +
                "\",\"size\":" + String(s.expectedSize) + "}";
  server.send(r == FW_NONE ? 200 : 400, "application/json", resp);
}

// Streamed multipart upload: each chunk goes straight into the inactive slot.
static void handleFirmwareImageData() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_WRITE) {
    mothershipOtaImageChunk(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    mothershipOtaAbort();
  }
}

static void handleFirmwareImageDone() {
  FwReason r = mothershipOtaImageFinish();
  if (r == FW_NONE) {
    server.send(200, "application/json", "{\"reason\":\"NONE\",\"rebooting\":true}");
    delay(400);
    esp_restart();
  } else {
    MothershipOtaStatus s = mothershipOtaGetStatus();
    String resp = String("{\"reason\":\"") + fwReasonStr(r) +
                  "\",\"written\":" + String(s.written) + "}";
    server.send(400, "application/json", resp);
  }
}

void startConfigServer() {
  // Load the shared control revision + recent command results from NVS so a
  // reboot mid-session resumes the same authoritative revision.
  dispatcherInit();
  backendControlInit();

  // Config-mode AP name = "FieldHub(<full MAC>)", so a user can pick the correct
  // hub out of their phone's Wi-Fi list. Derived from the factory STA MAC — the
  // same source as the portal card, /api/identity, the hardware QR, and the
  // upload status MAC. Assigned here (not at static init) so eFuse/WiFi are
  // guaranteed ready.
  ssid = hwApSsid();

  // WiFi AP + STA (AP for web UI, STA for ESP-NOW on same channel).
  // ESP-NOW was already initialised by initEspNowConfig() before this call.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  // Fixed, shared WPA2 password (not a real access-control barrier — it's the
  // same on every hub). An open/no-password AP made some phones' OS more
  // aggressive about dropping the association once it decided the network had
  // "no internet" (observed during provisioning QR scans), so a trivial shared
  // WPA2 key buys connection stability without adding real friction. WPA2-PSK
  // requires 8-63 chars, so this can't be shorter.
  static const char* kApPassword = "fieldmesh";
  bool apOk = WiFi.softAP(ssid.c_str(), kApPassword, ESPNOW_CHANNEL, false, 4);
  if (!apOk) {
    Serial.println("[AP] SoftAP failed to start");
  } else {
    Serial.println("[AP] SoftAP started");
  }
  Serial.print("[AP] SSID: "); Serial.println(WiFi.softAPSSID());
  Serial.print("[AP] IP: ");   Serial.println(WiFi.softAPIP());
  Serial.print("[AP] MAC: ");   Serial.println(WiFi.softAPmacAddress());
  Serial.print("[AP] channel: "); Serial.println(WiFi.channel());

  // Routes
  server.on("/", HTTP_ANY, handleRoot);
  // OS connectivity checks — answer each with its expected "internet OK"
  // signal so phones stay attached to the AP (see the probe responders above).
  server.on("/generate_204", HTTP_ANY, sendProbeNoContent);        // Android
  server.on("/gen_204", HTTP_ANY, sendProbeNoContent);             // Android
  server.on("/hotspot-detect.html", HTTP_ANY, sendProbeAppleSuccess);       // Apple CNA
  server.on("/library/test/success.html", HTTP_ANY, sendProbeAppleSuccess); // Apple CNA
  server.on("/ncsi.txt", HTTP_ANY, sendProbeNcsi);                 // Windows NCSI
  server.on("/connecttest.txt", HTTP_ANY, sendProbeMsConnect);     // Windows
  server.on("/success.txt", HTTP_ANY, sendProbeNoContent);         // Firefox/other
  server.on("/wpad.dat", HTTP_ANY, sendProbeNoContent);
  server.on("/redirect", HTTP_ANY, sendProbeNoContent);
  server.on("/fwlink", HTTP_ANY, sendProbeNoContent);
  server.on("/mobile/status.php", HTTP_ANY, sendProbeNoContent);
  server.on("/favicon.ico", HTTP_ANY, []() { server.send(204); });

  // --- New canonical routes ---
  server.on("/shutdown", HTTP_POST, handleShutdown);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/set-sync-mode", HTTP_POST, handleSetSyncMode);
  server.on("/set-sync-time", HTTP_POST, handleSetSyncTime);
  server.on("/ui-status", HTTP_GET, []() {
    server.send(200, "application/json", buildStatusJson());
  });
  // Lightweight RAM-only live status — polled frequently by the UI. Marked
  // no-store so captive-portal browsers never serve a stale fleet snapshot.
  server.on("/api/live", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(200, "application/json", buildLiveJson());
  });

  server.on("/stations", HTTP_GET, handleStationsPage);
  server.on("/batch-node-action", HTTP_POST, handleBatchNodeAction);
  server.on("/station", HTTP_GET, handleStationDetail);
  server.on("/station", HTTP_POST, handleNodeConfigSave);
  server.on("/station-setup", HTTP_GET, handleStationSetupWizard);
  server.on("/node-sensors", HTTP_GET, handleNodeSensorsPage);
  server.on("/set-node-sensors", HTTP_POST, handleSetNodeSensors);
  server.on("/revert-node", HTTP_POST, handleRevertNode);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/set-transmission", HTTP_POST, handleSetTransmission);
  server.on("/manual-upload", HTTP_POST, handleManualUpload);
  server.on("/upload-status", HTTP_GET, handleUploadStatus);

  // Hardware identity + secure connection-key provisioning.
  server.on("/api/identity", HTTP_GET, handleApiIdentity);
  server.on("/provision", HTTP_GET, handleProvisionPage);
  server.on("/provision-apply", HTTP_POST, handleProvisionApply);

  // First-run onboarding wizard + the lightweight per-setting endpoint it needs.
  server.on("/setup", HTTP_GET, handleSetupWizard);
  server.on("/set-remote-management", HTTP_POST, handleSetRemoteManagement);

  // --- Legacy route aliases (backwards compatibility) ---
  // GET routes redirect to the new canonical paths.
  server.on("/nodes", HTTP_GET, []() {
    server.sendHeader("Location", "/stations", true);
    server.send(302, "text/plain", "");
  });
  server.on("/node-config", HTTP_GET, []() {
    server.sendHeader("Location", "/station", true);
    server.send(302, "text/plain", "");
  });
  server.on("/upload", HTTP_GET, []() {
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/plain", "");
  });
  // POST routes keep working on the old paths (clients may still POST here).
  server.on("/node-config", HTTP_POST, handleNodeConfigSave);

  // --- UI form-action aliases (Field UI form actions) ---
  server.on("/start", HTTP_POST, handleShutdown);
  server.on("/find-stations", HTTP_POST, handleDiscoverNodes);
  server.on("/set-recording-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/save-settings", HTTP_POST, handleSetTransmission);
  server.on("/export", HTTP_GET, []() {
    server.sendHeader("Location", "/download-csv", true);
    server.send(302, "text/plain", "");
  });

  // Shared control revision + local self-update (§4.2, §7.1)
  server.on("/api/control", HTTP_GET, handleControlStatus);
  server.on("/firmware", HTTP_GET, handleFirmwarePage);
  server.on("/firmware/manifest", HTTP_POST, handleFirmwareManifest);
  server.on("/firmware/image", HTTP_POST, handleFirmwareImageDone, handleFirmwareImageData);

  server.onNotFound(sendCaptivePortalLanding);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("[AP] Captive portal DNS enabled");

  server.begin();
  Serial.println("[AP] Web server started");
}

void configServerLoop() {
  if (gShutdownRequested) {
    return;  // Signal to main loop to exit config mode
  }
  dnsServer.processNextRequest();
  server.handleClient();
  espnowConfigPoll();
  discoveryTick();   // non-blocking: schedules discovery broadcasts via millis()
}
