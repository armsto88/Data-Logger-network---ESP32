// ====== ESP-NOW Web UI – main.cpp with Node Manager & Node Meta ======

// ---------- Config toggles ----------
#define ENABLE_SPIFFS_ASSETS 0  // set to 1 if you upload /style.css.gz and /app.js.gz
#define ENABLE_WIFI_AP_WEBSERVER 1  // set to 1 to enable AP + web UI
#define ENABLE_BLE_GATT 0  // keep BLE off while stabilizing AP visibility
#define ENABLE_ESPNOW_RUNTIME 1  // production: AP and ESP-NOW both enabled
#define FORCE_AP_TEST_SSID 0  // production: use Logger + DEVICE_ID SSID
#define ENABLE_CAPTIVE_PORTAL 1  // improve phone onboarding to local AP web UI

// ---------- Includes ----------
#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#if ENABLE_SPIFFS_ASSETS
  #include <FS.h>
  #include <SPIFFS.h>
#endif

#include "time/rtc_manager.h"
#include "storage/sd_manager.h"
#include "comms/espnow_manager.h"
#include "ble/ble_manager.h"
#include "protocol.h"
#include <Preferences.h>
#include <RTClib.h>

// ---------- Device identification and WiFi ----------
const char* DEVICE_ID = "001";  // Simplified ID
const char* BASE_SSID = "Logger";
#if FORCE_AP_TEST_SSID
String ssid = "Logger001_TEST";
#else
String ssid = String(BASE_SSID) + String(DEVICE_ID);  // "Logger001"
#endif
const char* password = "logger123";
#define FW_VERSION "v1.0.0"
#define FW_BUILD   __DATE__ " " __TIME__

#ifndef ENABLE_PERIODIC_RTC_SERIAL_LOG
#define ENABLE_PERIODIC_RTC_SERIAL_LOG 0
#endif

#ifndef ENABLE_SYNC_AUDIT_LOG
#define ENABLE_SYNC_AUDIT_LOG 1
#endif

Preferences gPrefs;
int gWakeIntervalMin = 0;          // 0=OFF, >0 applies global wake interval to paired/deployed nodes
int gSyncIntervalMin = 15;         // transport interval still used in sync schedule payload
int gSyncDailyHour = 6;            // local daily sync trigger time (HH)
int gSyncDailyMinute = 0;          // local daily sync trigger time (MM)
long gLastSyncBroadcastEpochDay = -1;
enum SyncMode {
  SYNC_MODE_DAILY = 0,
  SYNC_MODE_INTERVAL = 1,
};
int gSyncMode = SYNC_MODE_DAILY;
unsigned long gLastSyncBroadcastMs = 0;
unsigned long gLastSyncBroadcastUnix = 0;
unsigned long gLastSyncAuditMs = 0;
uint32_t gSyncTriggerCount = 0;
long long gLastSyncIntervalSlot = -1;
const int kAllowedIntervals[] = {1, 5, 10, 20, 30, 60};
const size_t kAllowedCount = sizeof(kAllowedIntervals) / sizeof(kAllowedIntervals[0]);
static uint32_t gBootMs = 0;
static const uint32_t kSyncBootQuietMs = 120000UL;

// ---------- Web server ----------
WebServer server(80);
#if ENABLE_WIFI_AP_WEBSERVER && ENABLE_CAPTIVE_PORTAL
DNSServer dnsServer;
#endif

// NodeInfo is defined in your ESP-NOW / protocol headers
extern std::vector<NodeInfo> registeredNodes;

// ---------- UI helpers ----------
static String formatMac(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

#if ENABLE_WIFI_AP_WEBSERVER && ENABLE_CAPTIVE_PORTAL
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
#endif

static void loadWakeIntervalFromNVS() {
  bool migrated = false;
  if (gPrefs.begin("ui", false)) {
    // One-time migration: force OFF as the new default for global interval trigger.
    if (!gPrefs.getBool("wake_v2_init", false)) {
      gPrefs.putInt("wake_min", 0);
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
    Serial.println("[UI] wake interval migration applied: defaulted global trigger to OFF");
  }
}

static void saveWakeIntervalToNVS(int mins) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("wake_min", mins);
    gPrefs.end();
  }
}

static void loadSyncIntervalFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int v = gPrefs.getInt("sync_min", gSyncIntervalMin);
    gPrefs.end();
    bool ok = false;
    for (size_t i = 0; i < kAllowedCount; i++) {
      if (v == kAllowedIntervals[i]) {
        ok = true;
        break;
      }
    }
    gSyncIntervalMin = ok ? v : 15;
  }
}

static void saveSyncIntervalToNVS(int mins) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("sync_min", mins);
    gPrefs.end();
  }
}

static void loadSyncModeFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int m = gPrefs.getInt("sync_mode", SYNC_MODE_DAILY);
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

static void loadDailySyncTimeFromNVS() {
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

static void loadSyncRuntimeGuardsFromNVS() {
  if (gPrefs.begin("ui", true)) {
    gLastSyncBroadcastEpochDay = gPrefs.getLong("sync_day", -1);
    gLastSyncBroadcastUnix = gPrefs.getULong("sync_last_unix", 0);
    gLastSyncIntervalSlot = gPrefs.getLong64("sync_slot", -1);
    gPrefs.end();
  }
}

static void saveSyncRuntimeGuardsToNVS() {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putLong("sync_day", gLastSyncBroadcastEpochDay);
    gPrefs.putULong("sync_last_unix", gLastSyncBroadcastUnix);
    gPrefs.putLong64("sync_slot", gLastSyncIntervalSlot);
    gPrefs.end();
  }
}

static String formatSyncTimeHHMM(int hh, int mm) {
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

static String formatDateTimeDisplay(const DateTime& dt) {
  char b[20];
  snprintf(b, sizeof(b), "%02d:%02d:%02d %02d-%02d-%04d",
           dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(), dt.year());
  return String(b);
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix);

static String computeNextSyncIsoLocal() {
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
    if (nowUnix < anchorUnix) {
      return anchorUnix;
    }
    const uint32_t elapsedSec = nowUnix - anchorUnix;
    const uint32_t slotsElapsed = elapsedSec / periodSec;
    return anchorUnix + (slotsElapsed + 1UL) * periodSec;
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

  // Prefer a cadence anchored to the node's last contact, not "now + interval".
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

    // If node is currently active, this is a strong estimate (fresh wake anchor).
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

// ---------- Node meta helpers (numeric ID + Name in NVS) ----------
//
// Each real nodeId (from firmware) can have:
//   - userId  → numeric string like "001" (shown as "Node ID", used in CSV)
//   - name    → free-text name like "North Hedge 01"
//
// Both are stored in NVS under namespace "node_meta" as:
//   id_<nodeId>   and   name_<nodeId>

static String loadNodeMeta(const String& nodeId, const char* fieldPrefix) {
  Preferences prefs;
  // readOnly = true is fine *once the namespace exists*
  if (!prefs.begin("node_meta", /*readOnly=*/true)) {
    return "";
  }
  String key = String(fieldPrefix) + nodeId;  // e.g. "id_TEMP_001"
  // Avoid noisy nvs_get_str errors when key does not exist.
  String value = prefs.isKey(key.c_str()) ? prefs.getString(key.c_str(), "") : "";
  prefs.end();
  return value;
}


static void storeNodeMeta(const String& nodeId, const char* fieldPrefix, String value) {
  Preferences prefs;
  // readOnly = false → read/write, can create the namespace
  if (!prefs.begin("node_meta", /*readOnly=*/false)) {
    Serial.println("⚠️ storeNodeMeta: NVS begin failed");
    return;
  }

  String key = String(fieldPrefix) + nodeId;  // e.g. "name_TEMP_001"

  value.trim();
  if (value.length() == 0) {
    prefs.remove(key.c_str());  // empty → clear key
    Serial.printf("[NODES] Cleared %s for %s\n", fieldPrefix, nodeId.c_str());
  } else {
    prefs.putString(key.c_str(), value);
    Serial.printf("[NODES] Set %s for %s → '%s'\n",
                  fieldPrefix, nodeId.c_str(), value.c_str());
  }
  prefs.end();
}


// Numeric Node ID (user-facing, e.g. "001")
String getNodeUserId(const String& nodeId) {
  return loadNodeMeta(nodeId, "id_");
}

// Enforce "purely numeric" up to 3 chars, e.g. "001", "012", "120"
static void setNodeUserId(const String& nodeId, String userId) {
  String cleaned;
  cleaned.reserve(4);
  userId.trim();
  for (size_t i = 0; i < userId.length(); ++i) {
    char c = userId[i];
    if (c >= '0' && c <= '9') {
      cleaned += c;
      if (cleaned.length() >= 3) break;  // max 3 chars
    }
  }

  // Left-pad to 3 digits if non-empty (optional but nice UX)
  if (cleaned.length() > 0 && cleaned.length() < 3) {
    while (cleaned.length() < 3) cleaned = "0" + cleaned;
  }

  storeNodeMeta(nodeId, "id_", cleaned);
}

// Friendly Name (free text)
String getNodeName(const String& nodeId) {
  return loadNodeMeta(nodeId, "name_");
}

static void setNodeName(const String& nodeId, String name) {
  // You could truncate here if you want a hard max length, e.g. 32 chars:
  const size_t kMaxLen = 32;
  if (name.length() > kMaxLen) name = name.substring(0, kMaxLen);
  storeNodeMeta(nodeId, "name_", name);
}

String getNodeNotes(const String& nodeId) {
  return loadNodeMeta(nodeId, "note_");
}

static void setNodeNotes(const String& nodeId, String notes) {
  const size_t kMaxLen = 180;
  if (notes.length() > kMaxLen) notes = notes.substring(0, kMaxLen);
  storeNodeMeta(nodeId, "note_", notes);
}

// Helpers intended for CSV logging (non-static so espnow_manager.cpp can use them)
String getCsvNodeId(const String& nodeId) {
  String userId = getNodeUserId(nodeId);
  if (userId.length() > 0) return userId;
  return nodeId;         // fallback to firmware ID
}

String getCsvNodeName(const String& nodeId) {
  String nm = getNodeName(nodeId);
  return nm;             // may be empty; CSV can handle blank names
}


// ---------- Logging helpers (for serial clarity) ----------

// State → text for logs
static const char* nodeStateToString(int s) {
  switch (s) {
    case UNPAIRED: return "UNPAIRED";
    case PAIRED:   return "PAIRED";
    case DEPLOYED: return "DEPLOYED";
    default:       return "UNKNOWN";
  }
}

// Command logger: one line per ESP-NOW command from mothership
static void logCommandSend(const char* cmd,
                           const String& nodeId,
                           bool ok,
                           const char* extra = nullptr)
{
  Serial.printf("📤 CMD %-14s → %-10s status=%s",
                cmd,
                nodeId.c_str(),
                ok ? "OK" : "FAIL");
  if (extra && extra[0] != '\0') {
    Serial.print(" | ");
    Serial.print(extra);
  }
  Serial.println();
}

// Boot summary once everything is initialised
static void logBootSummary() {
  Serial.println();
  Serial.println("=============== MOTHERSHIP SUMMARY ===============");
  Serial.printf("Device ID: %s\n", DEVICE_ID);
  Serial.printf("SSID: %s\n", ssid.c_str());
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Firmware: %s %s\n", FW_VERSION, FW_BUILD);

  char timeStr[32];
  getRTCTimeString(timeStr, sizeof(timeStr));
  Serial.printf("RTC Time: %s\n", timeStr);
  Serial.printf("Default wake interval: %d min\n", gWakeIntervalMin);
  Serial.printf("Mothership MAC (ESP-NOW): %s\n", getMothershipsMAC().c_str());

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  size_t deployedCount = 0;
  for (const auto& n : allNodes) {
    if (n.state == DEPLOYED) deployedCount++;
  }

  Serial.printf("Fleet: total=%u  unpaired=%u  paired=%u  deployed=%u\n",
                (unsigned)allNodes.size(),
                (unsigned)unpairedNodes.size(),
                (unsigned)pairedNodes.size(),
                (unsigned)deployedCount);

  for (const auto& node : allNodes) {
    String userId = getNodeUserId(node.nodeId);
    String name   = getNodeName(node.nodeId);

    Serial.print("  ↪ FW=");
    Serial.print(node.nodeId);
    Serial.print(" | ID=");
    Serial.print(userId.length() ? userId : String("-"));
    Serial.print(" | Name=");
    Serial.print(name.length() ? name : String("-"));
    Serial.print(" | State=");
    Serial.println(nodeStateToString(node.state));
  }

  Serial.println("==================================================");
}

static String buildBleStatusDataJson() {
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
  json += computeNextSyncIsoLocal();
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
  json += "}";
  return json;
}

static String jsonEscapeLocal(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

static String extractJsonStringFieldLocal(const String& json, const char* key) {
  if (!key || !*key) return "";

  String needle = String("\"") + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) return "";

  int startQuote = json.indexOf('"', colonPos + 1);
  if (startQuote < 0) return "";

  String value;
  bool escape = false;
  for (int i = startQuote + 1; i < (int)json.length(); ++i) {
    const char c = json[i];
    if (escape) {
      value += c;
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') return value;
    value += c;
  }
  return "";
}

static bool extractJsonIntFieldLocal(const String& json, const char* key, long& outValue) {
  String needle = String("\"") + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return false;

  int colonPos = json.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) return false;

  int i = colonPos + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length()) return false;

  int start = i;
  if (json[i] == '-') i++;
  while (i < (int)json.length() && json[i] >= '0' && json[i] <= '9') i++;
  if (i == start || (i == start + 1 && json[start] == '-')) return false;

  outValue = json.substring(start, i).toInt();
  return true;
}

static bool isAllowedInterval(int interval) {
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) return true;
  }
  return false;
}

static String formatBytesUi(uint64_t bytes) {
  char b[24];
  if (bytes < 1024ULL) {
    snprintf(b, sizeof(b), "%lu B", (unsigned long)bytes);
  } else if (bytes < (1024ULL * 1024ULL)) {
    const double kb = (double)bytes / 1024.0;
    snprintf(b, sizeof(b), "%.1f KB", kb);
  } else if (bytes < (1024ULL * 1024ULL * 1024ULL)) {
    const double mb = (double)bytes / (1024.0 * 1024.0);
    snprintf(b, sizeof(b), "%.1f MB", mb);
  } else {
    const double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(b, sizeof(b), "%.2f GB", gb);
  }
  return String(b);
}

static String buildDataStatusSectionHtml() {
  bool hasFile = SD.exists("/datalog.csv");
  uint32_t records = 0;
  uint64_t fileBytes = 0;
  String lastConfirmedSync = "n/a";

  if (hasFile) {
    File file = SD.open("/datalog.csv", FILE_READ);
    if (file) {
      fileBytes = (uint64_t)file.size();

      uint32_t lineNo = 0;
      String lastDataLine;
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        lineNo++;
        if (lineNo == 1) continue; // header

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

  const uint64_t totalBytes = (uint64_t)SD.totalBytes();
  const uint64_t usedBytes  = (uint64_t)SD.usedBytes();
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
           "<div class='stat'><strong>Card free</strong><span class='num' style='font-size:16px'>");
  out += (totalBytes > 0) ? formatBytesUi(freeBytes) : String("n/a");
  out += F("</span></div></div>");

  if (!hasFile) {
    out += F("<p class='muted'>No datalog.csv file yet.</p>");
  }

  if (totalBytes > 0) {
    const uint32_t usedPct = (uint32_t)((usedBytes * 100ULL) / totalBytes);
    out += F("<p class='muted'><strong>Card usage:</strong> ");
    out += String(usedPct);
    out += F("% used (" );
    out += formatBytesUi(usedBytes);
    out += F(" of ");
    out += formatBytesUi(totalBytes);
    out += F(")</p>");
  }

  out += F("<p class='muted'><strong>Last confirmed sync:</strong> ");
  out += lastConfirmedSync;
  out += F("</p>");

  out += F("<p class='muted'>Use the CSV button at the top to export data.</p></div>");
  return out;
}

static String buildListNodesDataJson() {
  auto nodes = getRegisteredNodes();

  String json;
  json.reserve(300 + (nodes.size() * 120));
  json += "{\"nodes\":[";
  for (size_t i = 0; i < nodes.size(); ++i) {
    const auto& n = nodes[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"nodeId\":\"";
    json += jsonEscapeLocal(n.nodeId);
    json += "\",";
    json += "\"nodeType\":\"";
    json += jsonEscapeLocal(n.nodeType);
    json += "\",";
    json += "\"state\":\"";
    json += nodeStateToString(n.state);
    json += "\",";
    json += "\"lastSeenMs\":";
    json += String((unsigned long)n.lastSeen);
    json += ",";
    json += "\"isActive\":";
    json += n.isActive ? "true" : "false";
    json += ",";
    json += "\"queueDepth\":";
    json += String((unsigned)n.lastReportedQueueDepth);
    json += "}";
  }
  json += "]}";
  return json;
}

static String buildExportCsvResultJson(String& errorCode, String& errorMessage) {
  File file = SD.open("/datalog.csv", FILE_READ);
  if (!file) {
    errorCode = "CSV_NOT_FOUND";
    errorMessage = "CSV file not found";
    return "";
  }

  const size_t totalSize = file.size();
  const size_t maxPayloadBytes = 8192;
  String csv;
  csv.reserve(maxPayloadBytes + 8);

  while (file.available() && csv.length() < maxPayloadBytes) {
    int c = file.read();
    if (c < 0) break;
    csv += (char)c;
  }
  const bool truncated = file.available();
  file.close();

  String out;
  out.reserve(300 + csv.length());
  out += "{";
  out += "\"fileName\":\"datalog.csv\",";
  out += "\"totalBytes\":";
  out += String((unsigned long)totalSize);
  out += ",";
  out += "\"returnedBytes\":";
  out += String((unsigned long)csv.length());
  out += ",";
  out += "\"truncated\":";
  out += truncated ? "true" : "false";
  out += ",";
  out += "\"csvData\":\"";
  out += jsonEscapeLocal(csv);
  out += "\"}";
  return out;
}

static void onBleSensorTelemetry(const sensor_data_message_t& sample, const uint8_t mac[6]) {
  blePublishTelemetryEvent(sample, mac);
}

static bool handleBleCommand(
  const String& command,
  const String& payloadJson,
  String& responseType,
  String& responseDataJson,
  String& responseMessage,
  String& errorCode,
  String& errorMessage
) {
  String cmd = command;
  if (cmd.endsWith("_request")) {
    cmd = cmd.substring(0, cmd.length() - 8);
  }

  if (cmd == "discover_nodes") {
    bool sent = sendDiscoveryBroadcast();
    if (!sent) {
      errorCode = "DISCOVERY_FAILED";
      errorMessage = "Failed to send discovery broadcast";
      return false;
    }
    responseType = "discover_nodes_result";
    responseDataJson = "{\"broadcastSent\":true}";
    responseMessage = "Discovery broadcast sent";
    return true;
  }

  if (cmd == "list_nodes") {
    responseType = "list_nodes_result";
    responseDataJson = buildListNodesDataJson();
    return true;
  }

  if (cmd == "set_time") {
    long ts = 0;
    if (!extractJsonIntFieldLocal(payloadJson, "timestampUnix", ts) || ts <= 0) {
      errorCode = "INVALID_PAYLOAD";
      errorMessage = "set_time requires payload.timestampUnix (seconds)";
      return false;
    }
    DateTime dt((uint32_t)ts);
    bool ok = setRTCTime(dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
    if (!ok) {
      errorCode = "SET_TIME_FAILED";
      errorMessage = "Failed to set RTC time";
      return false;
    }
    responseType = "ack";
    responseDataJson = String("{\"command\":\"set_time\",\"rtcUnix\":") + String(getRTCTimeUnix()) + "}";
    responseMessage = "RTC updated";
    return true;
  }

  if (cmd == "set_wake_interval") {
    long iv = 0;
    if (!extractJsonIntFieldLocal(payloadJson, "intervalMinutes", iv)) {
      extractJsonIntFieldLocal(payloadJson, "interval", iv);
    }
    int interval = (int)iv;
    if (!isAllowedInterval(interval)) {
      errorCode = "INVALID_INTERVAL";
      errorMessage = "Interval must be one of: 1, 5, 10, 20, 30, 60";
      return false;
    }

    bool sent = broadcastWakeInterval(interval);
    gWakeIntervalMin = interval;
    saveWakeIntervalToNVS(interval);

    // Bump desired config version so CONFIG_SNAPSHOT is pushed on next NODE_HELLO
    for (const auto& node : registeredNodes) {
      if (node.state == PAIRED || node.state == DEPLOYED) {
        NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
        dc.wakeIntervalMin = (uint8_t)interval;
        dc.configVersion   = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
        setDesiredConfig(node.nodeId.c_str(), dc);
      }
    }

    responseType = "ack";
    responseDataJson = String("{\"command\":\"set_wake_interval\",\"intervalMinutes\":")
      + String(interval)
      + ",\"broadcastSent\":"
      + (sent ? "true" : "false")
      + "}";
    responseMessage = sent ? "Wake schedule broadcast sent" : "No eligible paired/deployed nodes";
    return true;
  }

  if (cmd == "set_sync_interval") {
    long iv = 0;
    if (!extractJsonIntFieldLocal(payloadJson, "intervalMinutes", iv)) {
      extractJsonIntFieldLocal(payloadJson, "syncIntervalMinutes", iv);
    }
    int interval = (int)iv;
    if (!isAllowedInterval(interval)) {
      errorCode = "INVALID_INTERVAL";
      errorMessage = "Interval must be one of: 1, 5, 10, 20, 30, 60";
      return false;
    }

    long phase = 0;
    if (!extractJsonIntFieldLocal(payloadJson, "phaseUnix", phase) || phase <= 0) {
      phase = (long)getRTCTimeUnix();
    }
    phase -= (phase % 60L);

    bool sent = broadcastSyncSchedule(interval, (unsigned long)phase);
    gSyncIntervalMin = interval;
    gSyncMode = SYNC_MODE_INTERVAL;
    gLastSyncBroadcastEpochDay = (long)(((unsigned long)phase) / 86400UL);
    gLastSyncBroadcastUnix = (unsigned long)phase;
    gLastSyncIntervalSlot = (long long)((unsigned long)phase / ((unsigned long)max(interval, 1) * 60UL));
    saveSyncRuntimeGuardsToNVS();
    saveSyncIntervalToNVS(interval);
    saveSyncModeToNVS(gSyncMode);

    responseType = "ack";
    responseDataJson = String("{\"command\":\"set_sync_interval\",\"intervalMinutes\":")
      + String(interval)
      + ",\"phaseUnix\":"
      + String((unsigned long)phase)
      + ",\"broadcastSent\":"
      + (sent ? "true" : "false")
      + "}";
    responseMessage = sent ? "Sync schedule broadcast sent" : "No eligible paired/deployed nodes";
    return true;
  }

  if (cmd == "node_config_apply") {
    const String nodeId = extractJsonStringFieldLocal(payloadJson, "nodeId");
    if (nodeId.length() == 0) {
      errorCode = "INVALID_PAYLOAD";
      errorMessage = "node_config_apply requires payload.nodeId";
      return false;
    }

    long wakeIv = 0;
    const bool hasWake = extractJsonIntFieldLocal(payloadJson, "wakeIntervalMinutes", wakeIv);
    long syncIv = 0;
    const bool hasSync = extractJsonIntFieldLocal(payloadJson, "syncIntervalMinutes", syncIv);
    long phase = 0;
    const bool hasPhase = extractJsonIntFieldLocal(payloadJson, "phaseUnix", phase);

    NodeInfo* target = nullptr;
    for (auto& n : registeredNodes) {
      if (n.nodeId == nodeId) {
        target = &n;
        break;
      }
    }

    bool pairOk = true;
    if (target && target->state == UNPAIRED) {
      pairOk = pairNode(nodeId);
      if (pairOk) {
        target->state = PAIRED;
        savePairedNodes();
      }
    }

    std::vector<String> ids;
    ids.push_back(nodeId);
    const bool deployRequested = deploySelectedNodes(ids);

    bool wakeSent = false;
    if (hasWake) {
      const int iv = (int)wakeIv;
      if (isAllowedInterval(iv)) {
        wakeSent = broadcastWakeInterval(iv);
        gWakeIntervalMin = iv;
        saveWakeIntervalToNVS(iv);
      }
    }

    bool syncSent = false;
    if (hasSync) {
      const int iv = (int)syncIv;
      if (isAllowedInterval(iv)) {
        unsigned long phaseUnix = (hasPhase && phase > 0) ? (unsigned long)phase : getRTCTimeUnix();
        phaseUnix -= (phaseUnix % 60UL);
        syncSent = broadcastSyncSchedule(iv, phaseUnix);
        gSyncIntervalMin = iv;
        gSyncMode = SYNC_MODE_INTERVAL;
        gLastSyncBroadcastEpochDay = (long)(phaseUnix / 86400UL);
        gLastSyncBroadcastUnix = phaseUnix;
        gLastSyncIntervalSlot = (long long)(phaseUnix / ((unsigned long)max(iv, 1) * 60UL));
        saveSyncRuntimeGuardsToNVS();
        saveSyncIntervalToNVS(iv);
        saveSyncModeToNVS(gSyncMode);
      }
    }

    if (!deployRequested) {
      errorCode = "NODE_CONFIG_APPLY_FAILED";
      errorMessage = "Failed to queue deploy request";
      return false;
    }

    responseType = "ack";
    responseDataJson = String("{\"command\":\"node_config_apply\",\"nodeId\":\"")
      + jsonEscapeLocal(nodeId)
      + "\",\"paired\":"
      + (pairOk ? "true" : "false")
      + ",\"deployRequested\":"
      + (deployRequested ? "true" : "false")
      + ",\"wakeBroadcast\":"
      + (wakeSent ? "true" : "false")
      + ",\"syncBroadcast\":"
      + (syncSent ? "true" : "false")
      + "}";
    responseMessage = "Node config applied; deploy request queued";
    return true;
  }

  if (cmd == "node_revert") {
    const String nodeId = extractJsonStringFieldLocal(payloadJson, "nodeId");
    if (nodeId.length() == 0) {
      errorCode = "INVALID_PAYLOAD";
      errorMessage = "node_revert requires payload.nodeId";
      return false;
    }

    NodeInfo* target = nullptr;
    for (auto& n : registeredNodes) {
      if (n.nodeId == nodeId) {
        target = &n;
        break;
      }
    }

    if (!target) {
      errorCode = "NODE_NOT_FOUND";
      errorMessage = "Node not found";
      return false;
    }

    target->state = PAIRED;
    savePairedNodes();
    const bool sent = pairNode(nodeId);

    responseType = "ack";
    responseDataJson = String("{\"command\":\"node_revert\",\"nodeId\":\"")
      + jsonEscapeLocal(nodeId)
      + "\",\"sent\":"
      + (sent ? "true" : "false")
      + "}";
    responseMessage = sent ? "Node reverted to paired" : "Node reverted locally; command send failed";
    return true;
  }

  if (cmd == "node_unpair") {
    const String nodeId = extractJsonStringFieldLocal(payloadJson, "nodeId");
    if (nodeId.length() == 0) {
      errorCode = "INVALID_PAYLOAD";
      errorMessage = "node_unpair requires payload.nodeId";
      return false;
    }

    const bool sent = sendUnpairToNode(nodeId);
    const bool local = unpairNode(nodeId);
    if (!local) {
      errorCode = "NODE_UNPAIR_FAILED";
      errorMessage = "Failed to remove node from local registry";
      return false;
    }

    responseType = "ack";
    responseDataJson = String("{\"command\":\"node_unpair\",\"nodeId\":\"")
      + jsonEscapeLocal(nodeId)
      + "\",\"remoteSent\":"
      + (sent ? "true" : "false")
      + ",\"localRemoved\":"
      + (local ? "true" : "false")
      + "}";
    responseMessage = sent ? "Node unpaired" : "Node unpaired locally; remote command send failed";
    return true;
  }

  if (cmd == "export_csv" || cmd == "export_csv_request") {
    String eCode;
    String eMsg;
    String result = buildExportCsvResultJson(eCode, eMsg);
    if (result.length() == 0) {
      errorCode = eCode;
      errorMessage = eMsg;
      return false;
    }
    responseType = "export_csv_result";
    responseDataJson = result;
    responseMessage = "CSV export ready";
    return true;
  }

  errorCode = "UNKNOWN_COMMAND";
  errorMessage = "Command not implemented in Phase C";
  return false;
}


// ---------- CSS / JS ----------
const char COMMON_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#f1f2eb; --panel:#ffffff; --text:#4a4a48; --sub:#566246; --border:#d8dad3;
  --primary:#566246; --success:#a4c2a5; --warn:#566246; --danger:#8b5e5e;
  --btn-solid:#4F6D7A;
  --input-bg:#f5f7ef;
  --input-bg-active:#edf2e2;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(74,74,72,.12);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{scroll-behior:smooth}
html,body{margin:0;padding:0;background:linear-gradient(180deg,#f1f2eb 0%, #d8dad3 100%);color:var(--text);
  font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif}
a{color:var(--primary);text-decoration:none}
:focus-visible{outline:3px solid rgba(33,150,243,.35);outline-offset:2px}

/* Layout */
.container{max-width:600px;margin:0 auto;padding:var(--sp-3)}
.header{padding:var(--sp-3) 0;text-align:center}
.header-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.header-actions{display:flex;align-items:center;gap:8px}
.h1{font-size:22px;font-weight:700;margin:0}
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
.stats--kpi .stat--deployed-active{background:rgba(164,194,165,.28);border-color:#bcd1bd}
.stats--kpi .stat--paired-active{background:rgba(225,177,122,.28);border-color:#E1B17A}
.stats--kpi .stat--unpaired-active{background:rgba(219,22,47,.14);border-color:#DB162F}

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

/* Chips */
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}
.chip--state-deployed{border-color:#bcd1bd;background:rgba(164,194,165,.28);color:#2f4f35}
.chip--state-paired{border-color:#E1B17A;background:rgba(225,177,122,.28);color:#7a4b13}
.chip--state-unpaired{border-color:#DB162F;background:rgba(219,22,47,.14);color:#7f1020}
.chip--link-awake{border-color:#b3e5fc;background:#e1f5fe;color:#01579b}
.chip--link-asleep{border-color:#e1bee7;background:#f3e5f5;color:#4a148c}
.chip--link-offline{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}
.chip--cfg-pending{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00}
.chip--cfg-ok{border-color:#c8e6c9;background:#f1f8e9;color:#256029}

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
.action-choice--start span{border-color:#bcd1bd;background:#fff;color:#2f4f35}
.action-choice--stop span{border-color:#E1B17A;background:#fff;color:#7a4b13}
.action-choice--unpair span{border-color:#DB162F;background:#fff;color:#7f1020}
.action-choice span:hover{filter:brightness(.98)}
.action-choice input:checked + span{box-shadow:0 0 0 2px rgba(79,109,122,.35) inset;transform:translateY(1px)}
.action-choice--start input:checked + span{background:rgba(164,194,165,.28)}
.action-choice--stop input:checked + span{background:rgba(225,177,122,.28)}
.action-choice--unpair input:checked + span{background:rgba(219,22,47,.14)}
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

@media(min-width:768px){.container{max-width:720px}}
)CSS";

const char COMMON_JS[] PROGMEM = R"JS(
function showUiStatus(message, ok){
  const box = document.getElementById('ui-status');
  if (!box) return;
  box.style.display = 'block';
  box.style.borderColor = ok ? '#16a34a' : '#dc2626';
  box.style.color = ok ? '#166534' : '#991b1b';
  box.textContent = message;
}

function asFormBody(form){
  const data = new FormData(form);
  data.append('ajax', '1');
  return new URLSearchParams(data);
}

async function refreshKpiCards(){
  try {
    const resp = await fetch('/ui-status');
    if (!resp.ok) return;
    const status = await resp.json();

    const fleet = status && status.fleet ? status.fleet : null;
    if (fleet) {
      const deployedEl = document.getElementById('kpi-deployed-num');
      const pairedEl = document.getElementById('kpi-paired-num');
      const unpairedEl = document.getElementById('kpi-unpaired-num');
      if (deployedEl) deployedEl.textContent = String(fleet.deployed ?? deployedEl.textContent);
      if (pairedEl) pairedEl.textContent = String(fleet.paired ?? pairedEl.textContent);
      if (unpairedEl) unpairedEl.textContent = String(fleet.unpaired ?? unpairedEl.textContent);
    }

    const modeEl = document.getElementById('kpi-sync-mode');
    if (modeEl && status && status.syncMode) {
      modeEl.textContent = (status.syncMode === 'interval') ? 'Interval' : 'Daily';
    }

    const activeEl = document.getElementById('kpi-active-sync');
    if (activeEl && status) {
      if (status.syncMode === 'interval') {
        activeEl.textContent = 'Every ' + String(status.syncIntervalMinutes || 0) + ' min';
      } else {
        activeEl.textContent = 'Daily @ ' + String(status.syncDailyTime || '--:--');
      }
    }

    const nextEl = document.getElementById('kpi-next-sync');
    if (nextEl && status && status.nextSyncLocal) {
      nextEl.textContent = status.nextSyncLocal;
    }
  } catch (_) {}
}

function wireAsyncForms(){
  const forms = document.querySelectorAll('form.async-form');
  forms.forEach(form => {
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const btn = form.querySelector('button[type="submit"],input[type="submit"]');
      const original = btn ? (btn.textContent || btn.value) : '';
      if (btn) {
        btn.disabled = true;
        if (btn.tagName === 'INPUT') btn.value = 'Working...';
        else btn.textContent = 'Working...';
      }

      try {
        const resp = await fetch(form.action, {
          method: 'POST',
          body: asFormBody(form),
          headers: {'Content-Type': 'application/x-www-form-urlencoded'}
        });

        const text = await resp.text();
        let msg = text;
        let ok = resp.ok;
        try {
          const json = JSON.parse(text);
          ok = !!json.ok;
          msg = json.message || (ok ? 'Done' : 'Request failed');
        } catch (_) {}

        showUiStatus(msg, ok);

        if (ok && form.action && form.action.indexOf('/discover-nodes') !== -1) {
          // Discovery responses land shortly after broadcast; refresh the list quickly.
          setTimeout(() => location.reload(), 900);
        }

        if (ok && form.action && (
            form.action.indexOf('/set-sync-mode') !== -1 ||
            form.action.indexOf('/set-sync-interval') !== -1 ||
            form.action.indexOf('/set-sync-time') !== -1 ||
            form.action.indexOf('/set-wake-interval') !== -1)) {
          await refreshKpiCards();
        }
      } catch (err) {
        showUiStatus('Request failed: ' + err, false);
      } finally {
        if (btn) {
          btn.disabled = false;
          if (btn.tagName === 'INPUT') btn.value = original;
          else btn.textContent = original;
        }
      }
    });
  });
}

// Set datetime input to browser time
function setCurrentTime(){
  const n=new Date();
  const z=n=>String(n).padStart(2,'0');
  const s=`${z(n.getHours())}:${z(n.getMinutes())}:${z(n.getSeconds())} ${z(n.getDate())}-${z(n.getMonth()+1)}-${n.getFullYear()}`;
  const el=document.getElementById('datetime'); if(el) el.value=s;
}
function toggleSettings(){
  const panel=document.getElementById('settings-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
function toggleSyncSchedule(){
  const panel=document.getElementById('sync-panel');
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
});

// Live 1s ticking of the displayed RTC time
(function(){
  const pad = n => String(n).padStart(2,'0');
  function parseHMSDMY(str){
    if (!str || str.length < 19) return NaN;
    const H = +str.slice(0,2), M = +str.slice(3,5), S = +str.slice(6,8);
    const d = +str.slice(9,11), m = +str.slice(12,14), y = +str.slice(15,19);
    const dt = new Date(y, m-1, d, H, M, S); // local
    return isNaN(dt) ? NaN : dt.getTime();
  }
  function formatHMSDMY(ms){
    const dt = new Date(ms);
    return `${pad(dt.getHours())}:${pad(dt.getMinutes())}:${pad(dt.getSeconds())} ` +
           `${pad(dt.getDate())}-${pad(dt.getMonth()+1)}-${dt.getFullYear()}`;
  }
  function startClock(){
    const el = document.getElementById('rtc-now');
    if (!el) return;
    const initial = (el.textContent || '').trim();
    const rtcMs   = parseHMSDMY(initial);
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatHMSDMY(nowMs);
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

// ---------- Common page frame ----------
static String headCommon(const String& title, const String& actionsHtml = String()){
  String h;
  h.reserve(4500);
  h += F("<!DOCTYPE html><html><head>");
  h += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");

#if ENABLE_SPIFFS_ASSETS
  h += F("<link rel='stylesheet' href='/style.css'>");
  h += F("<script defer src='/app.js'></script>");
#else
  h += F("<style>"); h += FPSTR(COMMON_CSS); h += F("</style>");
  h += F("<script>"); h += FPSTR(COMMON_JS);  h += F("</script>");
#endif

  h += F("</head><body><div class='container'>");
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
  return h;
}
static inline String footCommon(){ return String(F("</div></body></html>")); }

// ---------- Routes / Handlers fwd decl ----------
void handleNodeConfigForm();
void handleNodeConfigSave();
void handleNodesPage();
void handleSetSyncMode();
void handleSetSyncInterval();
void handleSetSyncTime();

// ---------- Routes / Handlers ----------

void handleRevertNode() {
  String nodeId = server.arg("node_id");
  bool found   = false;
  bool sentCmd = false;

  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId && node.state == DEPLOYED) {
      node.state = PAIRED;
      savePairedNodes();
      found = true;

      sentCmd = pairNode(nodeId);  // will send PAIR_NODE + PAIRING_RESPONSE

      logCommandSend("PAIR_NODE", nodeId, sentCmd, "revert to PAIRED via /revert-node");

      if (sentCmd) {
        Serial.print("[MOTHERSHIP] Node reverted to PAIRED + PAIR_NODE sent: ");
      } else {
        Serial.print("[MOTHERSHIP] Node reverted to PAIRED (local only; PAIR_NODE send failed): ");
      }
      Serial.println(nodeId);
      break;
    }
  }

  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section center'>");
  if (found) {
    html += F("<h3>Node reverted to paired state</h3><p>Node <strong>");
    html += nodeId;
    html += F("</strong> is now marked as paired.</p>");
    if (!sentCmd) {
      html += F("<p class='muted'>Warning: could not send PAIR_NODE command to the node.</p>");
    }
  } else {
    html += F("<h3>Node not found or not deployed</h3><p>No action taken.</p>");
  }
  html += F("<a href='/nodes' class='btn btn--primary'>Back to Node manager</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleRoot() {
  String html = headCommon("ESP32 Data Logger");

  // Current RTC time
  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes) {
    if (node.state == DEPLOYED) deployedNodes++;
  }

  html += F("<div class='top-time'><span class='top-time__label'>Mothership time</span><span id='rtc-now' class='top-time__value'>");
  html += currentTime;
  html += F("</span></div>");

  html += F("<div class='quick-row'>"
            "<form class='async-form' action='/discover-nodes' method='POST'>"
            "<button type='submit' class='btn btn--primary'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3c-3.9 0-7.4 1.6-9.9 4.1l1.4 1.4C5.6 6.4 8.7 5 12 5s6.4 1.4 8.5 3.5l1.4-1.4C19.4 4.6 15.9 3 12 3zm0 5c-2.6 0-5 .9-6.9 2.6l1.4 1.4C8 10.5 9.9 10 12 10s4 .5 5.5 2l1.4-1.4C17 8.9 14.6 8 12 8zm0 5c-1.3 0-2.5.5-3.5 1.5l1.4 1.4c.6-.6 1.3-.9 2.1-.9s1.5.3 2.1.9l1.4-1.4c-1-1-2.2-1.5-3.5-1.5zm0 4a2 2 0 100 4 2 2 0 000-4z'/></svg> Discover Nodes</button>"
            "</form>"
            "<button class='btn' type='button' onclick='toggleInfoPanel()'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a10 10 0 1010 10A10 10 0 0012 2zm0 4a1.25 1.25 0 11-1.25 1.25A1.25 1.25 0 0112 6zm2 12h-4v-1.8h1.1v-4.2H10v-1.8h3v6h1z'/></svg> INFO</button>"
            "<a href='/download-csv' class='btn btn--success'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3a1 1 0 011 1v8.59l2.3-2.3 1.4 1.42-4.7 4.7-4.7-4.7 1.4-1.42 2.3 2.3V4a1 1 0 011-1zm-7 14h14v2H5v-2z'/></svg> CSV</a>"
            "<div id='info-panel' class='subpanel'>"
            "<h3><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a10 10 0 1010 10A10 10 0 0012 2zm0 4a1.25 1.25 0 11-1.25 1.25A1.25 1.25 0 0112 6zm2 12h-4v-1.8h1.1v-4.2H10v-1.8h3v6h1z'/></svg> INFO</h3>"
            "<div class='help'><strong>Device ID:</strong> ");
  html += DEVICE_ID;
  html += F("<br><strong>Firmware:</strong> ");
  html += FW_VERSION;
  html += F("<br><strong>Build:</strong> ");
  html += FW_BUILD;
  html += F("</div>"
            "<div class='help'><strong>SSID:</strong> ");
  html += ssid;
  html += F("<br><strong>URL:</strong> http://192.168.4.1/"
            "<br><strong>IP:</strong> 192.168.4.1"
            "<br><strong>MAC:</strong> ");
  html += getMothershipsMAC();
  html += F("</div>"
            "</div>"
            "</div>");

  // --- Overview section ---
  html += F("<div class='section' aria-live='polite'>"
            "<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>");

  html += F("<div class='stats stats--kpi' style='margin:0 0 12px 0'>"
              "<div class='stat");
  if (deployedNodes > 0) html += F(" stat--deployed-active");
  html += F("'><strong>Deployed</strong><span id='kpi-deployed-num' class='num'>");
  html += String(deployedNodes);
  html += F("</span></div>"
              "<div class='stat");
  if (pairedNodes.size() > 0) html += F(" stat--paired-active");
  html += F("'><strong>Paired</strong><span id='kpi-paired-num' class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div>"
              "<div class='stat");
  if (unpairedNodes.size() > 0) html += F(" stat--unpaired-active");
  html += F("'><strong>Unpaired</strong><span id='kpi-unpaired-num' class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div>"
            "</div>");

  html += F("<div class='row'>");
  html += F("<div class='col action-stack'>"
            "<a href='/nodes' class='btn btn--success' style='margin-top:8px'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a4 4 0 110 8 4 4 0 010-8zm-7 12a3 3 0 110 6 3 3 0 010-6zm14 0a3 3 0 110 6 3 3 0 010-6zM9.3 9.8l-3 3.9 1.6 1.2 3-3.9-1.6-1.2zm5.4 0l-1.6 1.2 3 3.9 1.6-1.2-3-3.9z'/></svg> Node manager</a>"
            "</div>");

  html += F("</div>"); // end row

  html += F("</div>"); // end overview section

  int activeSyncIntervalMin = gSyncIntervalMin;
  if (!isAllowedInterval(activeSyncIntervalMin)) activeSyncIntervalMin = 15;

  String activeSyncPlan =
    (gSyncMode == SYNC_MODE_INTERVAL)
      ? (String("Every ") + String(activeSyncIntervalMin) + String(" min"))
      : (String("Daily @ ") + formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute));

  // --- Schedule section ---
  html += F("<div class='section'>"
            "<div class='section-head'>"
            "<h3><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><circle cx='12' cy='12' r='9' fill='none' stroke='currentColor' stroke-width='2'/><path d='M12 7v5l3 2' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/></svg> Global schedules</h3>"
            "<button id='settings-btn' class='btn btn--sm' type='button' onclick='toggleSettings()'>SET TIME</button>"
            "</div>"
            "<div id='settings-panel' class='subpanel' style='margin-top:0;margin-bottom:12px'>"
            "<h3>⚙️ SET TIME</h3>"
            "<p class='muted'>Only needed for initial setup or DS3231 correction.</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set new time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='HH:MM:SS DD-MM-YYYY' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Use browser time</button>"
            "<button type='submit' class='btn btn--success'>Set Time</button>"
            "</div>"
            "<div class='help'>Example: 21:05:00 14-11-2026</div>"
            "</form>"
            "</div>"
            "<div class='stats' style='margin:0 0 12px 0'>"
            "<div class='stat'><strong>Sync mode</strong><span class='num' style='font-size:16px'><span id='kpi-sync-mode'>");
  html += syncModeLabel();
  html += F("</span></span></div>"
            "<div class='stat'><strong>Active sync</strong><span class='num' style='font-size:16px'>");
  html += F("<span id='kpi-active-sync'>");
  html += activeSyncPlan;
  html += F("</span></span></div>"
            "<div class='stat'><strong>Next sync</strong><span class='num' style='font-size:16px'>");
  html += F("<span id='kpi-next-sync'>");
  html += computeNextSyncIsoLocal();
  html += F("</span></span></div>"
            "</div>"
            "<div class='row'>"
            "<div class='col'>"
              "<button id='global-interval-btn' class='btn btn--warn btn--action' type='button' onclick='toggleGlobalInterval()' style='margin-top:10px'>"
              "<svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 4V1L8 5l4 4V6c3.3 0 6 2.7 6 6 0 1-.2 1.9-.7 2.7l1.5 1.3A8 8 0 0020 12c0-4.4-3.6-8-8-8zm-6.8.3A8 8 0 004 12c0 4.4 3.6 8 8 8v3l4-4-4-4v3c-3.3 0-6-2.7-6-6 0-1 .2-2 .7-2.8z'/></svg> Node interval"
              "</button>"
              "<div id='global-interval-panel' style='display:none;margin-top:10px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa'>"
              "<form class='async-form' action='/set-wake-interval' method='POST'>"
              "<label class='label'><strong>Global interval trigger</strong></label>"
              "<select class='input' name='interval'>");

  html += F("<option value='0'");
  if (gWakeIntervalMin <= 0) html += F(" selected");
  html += F(">Off</option>");

  for (size_t i = 0; i < kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<option value='");
    html += String(v);
    html += F("'");
    if (v == gWakeIntervalMin) html += F(" selected");
    html += F(">");
    html += String(v);
    html += F("</option>");
  }

  html += F("</select>"
            "<button type='submit' class='btn btn--primary' style='margin-top:8px'>"
            "Apply global interval</button>"
            "</form>"
            "</div>");

  html += F("<div class='col'>");

  html += F("<button id='sync-btn' class='btn btn--warn btn--action' type='button' onclick='toggleSyncSchedule()' style='margin-top:10px'>"
            "<svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2C7 2 3 3.8 3 6v12c0 2.2 4 4 9 4s9-1.8 9-4V6c0-2.2-4-4-9-4zm0 2c4.4 0 7 .9 7 2s-2.6 2-7 2-7-.9-7-2 2.6-2 7-2zm0 16c-4.4 0-7-.9-7-2v-2c1.6 1.1 4.4 1.7 7 1.7s5.4-.6 7-1.7v2c0 1.1-2.6 2-7 2zm0-6c-4.4 0-7-.9-7-2v-2c1.6 1.1 4.4 1.7 7 1.7s5.4-.6 7-1.7v2c0 1.1-2.6 2-7 2z'/></svg> Data sync interval"
            "</button>");

  html += F("<div id='sync-panel' style='display:none;margin-top:10px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa'>"
            "<form class='async-form' action='/set-sync-mode' method='POST'>"
            "<label class='label'><strong>Sync mode</strong></label>"
            "<select class='input' name='mode'>"
            "<option value='daily'");
  if (gSyncMode == SYNC_MODE_DAILY) html += F(" selected");
  html += F(">Daily (recommended)</option>"
            "<option value='interval'");
  if (gSyncMode == SYNC_MODE_INTERVAL) html += F(" selected");
  html += F(">Interval</option>"
            "</select>"
            "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
            "Save sync mode</button>"
            "</form>");

  if (gSyncMode == SYNC_MODE_DAILY) {
    html += F("<form class='async-form' action='/set-sync-time' method='POST' style='margin-top:12px'>"
              "<label class='label'><strong>Daily sync time</strong></label>"
              "<input class='input' type='time' name='sync_time' value='");
    html += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
    html += F("'>"
              "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
              "Set daily sync time</button>"
              "</form>");
  } else {
    html += F("<form class='async-form' action='/set-sync-interval' method='POST' style='margin-top:12px'>"
              "<div><strong>Current active:</strong> ");
    html += String(activeSyncIntervalMin);
    html += F(" min</div>"
              "<label class='label'><strong>Sync every (minutes)</strong></label>"
              "<select class='input' name='interval'>");

    for (size_t i = 0; i < kAllowedCount; ++i) {
      int v = kAllowedIntervals[i];
      html += F("<option value='");
      html += String(v);
      html += F("'");
      if (v == activeSyncIntervalMin) html += F(" selected");
      html += F(">");
      html += String(v);
      html += F("</option>");
    }

    html += F("</select>"
              "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
              "Set sync interval</button>"
              "</form>");
  }

  html += F("</div>"
            "</div>" // end col
            "</div>" // end row
            "</div>"); // end schedule section

  // --- Data status section ---
  html += buildDataStatusSectionHtml();

  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetTime() {
  String dt = server.arg("datetime");
  int year, mm, dd, hh, mi, ss;
  if (sscanf(dt.c_str(), "%d:%d:%d %d-%d-%d", &hh, &mi, &ss, &dd, &mm, &year) == 6 && year >= 2000) {
    if (setRTCTime(year, mm, dd, hh, mi, ss)) {
      String html = headCommon("ESP32 Data Logger");
      html += F("<div class='section center'><h3>SUCCESS: RTC Time Updated</h3><p>New time:<br><strong>");
      html += dt;
      html += F("</strong></p><a href='/' class='btn btn--primary'>Back to Main Page</a></div>");
      html += footCommon();
      server.send(200, "text/html", html);
    } else {
      String html = headCommon("ESP32 Data Logger");
      html += F("<div class='section center'><h3>ERROR: Failed to Set RTC Time</h3><p>Please try again.</p>"
                "<a href='/' class='btn btn--primary'>Try Again</a></div>");
      html += footCommon();
      server.send(500, "text/html", html);
    }
  } else {
    String html = headCommon("ESP32 Data Logger");
    html += F("<div class='section center'><h3>WARNING: Invalid Time Format</h3>"
              "<p>Please use the format: HH:MM:SS DD-MM-YYYY</p><p>You entered: <em>");
    html += dt;
    html += F("</em></p><a href='/' class='btn btn--primary'>Try Again</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
  }
}

void handleDownloadCSV() {
  File file = SD.open("/datalog.csv");
  if (!file) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=datalog.csv");
  server.sendHeader("Connection", "close");
  server.streamFile(file, "text/csv");
  file.close();
  Serial.println("✅ CSV file downloaded by client");
}

void handleDiscoverNodes() {
  Serial.println("🔍 Starting node discovery...");
  // Send a short burst so sleepy/late responders are less likely to be missed.
  const uint8_t kDiscoveryBursts = 3;
  bool sentAny = false;
  for (uint8_t i = 0; i < kDiscoveryBursts; ++i) {
    sentAny = sendDiscoveryBroadcast() || sentAny;
    if (i + 1 < kDiscoveryBursts) delay(150);
  }

  if (isAjaxRequest()) {
    sendAjaxResult(sentAny, sentAny ? "Discovery scan sent (3 bursts). Refreshing list..." : "Discovery scan failed");
    return;
  }

  String html = headCommon("ESP32 Data Logger");
  html += F("<meta http-equiv='refresh' content='3;url=/'>");
  html += F("<div class='section center'><h3>🔍 Discovery Broadcast Sent</h3>"
            "<div class='muted'>Searching for new sensor nodes...</div>"
            "<div style='margin:16px auto;width:40px;height:40px;border-radius:50%;"
            "border:4px solid #eee;border-top-color:#2196F3;animation:spin 1s linear infinite'></div>"
            "<style>@keyframes spin{0%{transform:rotate(0)}100%{transform:rotate(360deg)}}</style>"
            "<p class='muted'><small>Redirecting back to dashboard in 3 seconds…</small></p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetWakeInterval() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = (interval == 0);
  for (size_t i = 0; i < kAllowedCount; ++i)
    if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 0;

  gWakeIntervalMin = interval;
  saveWakeIntervalToNVS(interval);

  bool sent = false;
  if (interval > 0) {
    sent = broadcastWakeInterval(interval);

    for (const auto& node : registeredNodes) {
      if (node.state == PAIRED || node.state == DEPLOYED) {
        NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
        dc.wakeIntervalMin = (uint8_t)interval;
        dc.configVersion   = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
        setDesiredConfig(node.nodeId.c_str(), dc);
      }
    }
  }

  Serial.printf("[UI] Global wake interval set to %d min (off=0, broadcast=%s)\n",
                interval,
                sent ? "yes" : "no");

  if (isAjaxRequest()) {
    if (interval > 0) {
      sendAjaxResult(true, sent ? "Global interval applied to fleet" : "No eligible paired/deployed nodes");
    } else {
      sendAjaxResult(true, "Global interval trigger is OFF");
    }
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>⏰ Global Wake Interval</h3><p>Selected: ");
  if (interval > 0) {
    html += String(interval);
    html += F(" min.</p><p style='color:#666'>");
    html += sent ? F("Broadcast applied to eligible nodes.") : F("No eligible paired/deployed nodes.");
    html += F("</p>");
  } else {
    html += F("OFF.</p><p style='color:#666'>Global interval trigger disabled.</p>");
  }
  html += F("<a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

void handleSetSyncMode() {
  String mode = server.arg("mode");
  int newMode = (mode == "interval") ? SYNC_MODE_INTERVAL : SYNC_MODE_DAILY;
  gSyncMode = newMode;
  saveSyncModeToNVS(gSyncMode);
  gLastSyncBroadcastEpochDay = -1;
  gLastSyncBroadcastMs = 0;
  gLastSyncBroadcastUnix = 0;
  gLastSyncIntervalSlot = -1;
  saveSyncRuntimeGuardsToNVS();

  const uint32_t modePhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
  const uint16_t modeSyncMin = (gSyncMode == SYNC_MODE_INTERVAL) ? (uint16_t)gSyncIntervalMin : 0u;
  for (const auto& node : registeredNodes) {
    NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
    bool changed = false;
    if (dc.syncIntervalMin != modeSyncMin) {
      dc.syncIntervalMin = modeSyncMin;
      changed = true;
    }
    if (dc.syncPhaseUnix != modePhaseUnix) {
      dc.syncPhaseUnix = modePhaseUnix;
      changed = true;
    }
    if (dc.configVersion == 0) {
      dc.configVersion = 1;
      changed = true;
    } else if (changed) {
      dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
    }
    if (changed) {
      setDesiredConfig(node.nodeId.c_str(), dc);
    }
  }

  Serial.printf("[UI] Sync mode set to %s\n", syncModeLabel());

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Sync mode set to ") + syncModeLabel());
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>📶 Sync Mode Updated</h3><p>Mode: ");
  html += syncModeLabel();
  html += F("</p><p style='color:#666'>Next sync: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

void handleSetSyncInterval() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = false;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) {
      ok = true;
      break;
    }
  }
  if (!ok) interval = 15;

  unsigned long phaseUnix = getRTCTimeUnix();
  phaseUnix -= (phaseUnix % 60UL);
  bool sent = broadcastSyncSchedule(interval, phaseUnix);

  gSyncIntervalMin = interval;
  gSyncMode = SYNC_MODE_INTERVAL;
  gLastSyncBroadcastEpochDay = (long)(phaseUnix / 86400UL);
  gLastSyncBroadcastUnix = phaseUnix;
  gLastSyncBroadcastMs = millis();
  gLastSyncIntervalSlot = (long long)(phaseUnix / ((unsigned long)max(interval, 1) * 60UL));
  saveSyncRuntimeGuardsToNVS();
  saveSyncIntervalToNVS(interval);
  saveSyncModeToNVS(gSyncMode);

  // Keep all desired node snapshots aligned to the same global sync schedule.
  for (const auto& node : registeredNodes) {
    NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
    bool changed = false;
    if (dc.syncIntervalMin != (uint16_t)interval) {
      dc.syncIntervalMin = (uint16_t)interval;
      changed = true;
    }
    if (dc.syncPhaseUnix != phaseUnix) {
      dc.syncPhaseUnix = phaseUnix;
      changed = true;
    }
    if (dc.configVersion == 0) {
      dc.configVersion = 1;
      changed = true;
    } else if (changed) {
      dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
    }
    if (changed) {
      setDesiredConfig(node.nodeId.c_str(), dc);
    }
  }

  Serial.printf("[UI] Sync interval set to %d min phase=%lu -> broadcast %s (mode=%s)\n",
                interval,
                phaseUnix,
                sent ? "SENT" : "FAILED",
                syncModeLabel());

  if (isAjaxRequest()) {
    sendAjaxResult(sent,
      sent ? (String("Sync interval set to ") + String(interval) + " min")
           : String("Sync broadcast failed"));
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>📶 Sync Schedule</h3><p>Broadcasted sync schedule: ");
  html += String(interval);
  html += F(" min.</p><p style='color:#666'>");
  html += sent ? F("Broadcast sent to fleet.") : F("Broadcast failed.");
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

void handleSetSyncTime() {
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
                    "<h3>⚠️ Invalid sync time</h3><p>Please provide HH:MM (24h).</p>"
                    "<a href='/' style='display:inline-block;padding:10px 16px;"
                    "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
    server.send(400, "text/html", html);
    return;
  }

  gSyncDailyHour = hh;
  gSyncDailyMinute = mm;
  saveDailySyncTimeToNVS(hh, mm);
  gLastSyncBroadcastEpochDay = -1;  // allow immediate trigger when schedule matches current day/time
  gLastSyncBroadcastMs = 0;
  gLastSyncBroadcastUnix = 0;
  gLastSyncIntervalSlot = -1;
  saveSyncRuntimeGuardsToNVS();

  if (gSyncMode == SYNC_MODE_DAILY) {
    const uint32_t dailyPhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
    for (const auto& node : registeredNodes) {
      NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
      bool changed = false;
      if (dc.syncIntervalMin != 0u) {
        dc.syncIntervalMin = 0u;
        changed = true;
      }
      if (dc.syncPhaseUnix != dailyPhaseUnix) {
        dc.syncPhaseUnix = dailyPhaseUnix;
        changed = true;
      }
      if (dc.configVersion == 0) {
        dc.configVersion = 1;
        changed = true;
      } else if (changed) {
        dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
      }
      if (changed) {
        setDesiredConfig(node.nodeId.c_str(), dc);
      }
    }
  }

  Serial.printf("[UI] Daily sync time set to %02d:%02d\n", gSyncDailyHour, gSyncDailyMinute);

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Daily sync time set to ") + formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute));
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>📶 Daily Sync Time Updated</h3><p>New daily sync time: ");
  html += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
  html += F("</p><p style='color:#666'>Next sync: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}


// ---------- Configure & Start: form and handler ----------

void handleNodeConfigForm() {
  String nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  String actionsHtml = String("<a href='/nodes' class='btn btn--sm'>Back</a><a href='/node-config?node_id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Configure", actionsHtml);
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3>"
              "<p class='muted'>No node with that ID is currently registered.</p>"
              "<a href='/nodes' class='btn btn--primary'>Back to Node manager</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  String userId  = getNodeUserId(target->nodeId);
  String name    = getNodeName(target->nodeId);
  String notes   = getNodeNotes(target->nodeId);
  String appliedLabel = "";
  if (!target->stateChangePending && target->lastStateAppliedMs > 0) {
    if (target->lastAppliedTargetState == PENDING_TO_PAIRED) appliedLabel = "State applied: Paired";
    else if (target->lastAppliedTargetState == PENDING_TO_UNPAIRED) appliedLabel = "State applied: Unpaired";
    else if (target->lastAppliedTargetState == PENDING_TO_DEPLOYED) appliedLabel = "State applied: Deployed";
    else appliedLabel = "State applied";
  }
  const bool isDeployed = (target->state == DEPLOYED);
  uint8_t observedWakeMin = (target->wakeIntervalMin > 0)
    ? target->wakeIntervalMin
    : target->inferredWakeIntervalMin;
  int activeIntervalMin = (observedWakeMin > 0)
    ? observedWakeMin
    : (isAllowedInterval(gWakeIntervalMin) ? gWakeIntervalMin : 5);

  const char* stateLabel = "Unknown";
  if (target->state == UNPAIRED) stateLabel = "Unpaired";
  else if (target->state == PAIRED) stateLabel = "Paired";
  else if (target->state == DEPLOYED) stateLabel = "Deployed";

  html += F("<div style='display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:10px'>"
            "<div style='display:flex;align-items:center;gap:8px'>"
            "<strong>Status</strong>");
  if (target->state == DEPLOYED) html += F("<span class='chip chip--state-deployed'>Deployed</span>");
  else if (target->state == PAIRED) html += F("<span class='chip chip--state-paired'>Paired</span>");
  else html += F("<span class='chip chip--state-unpaired'>Unpaired</span>");
  if (appliedLabel.length()) {
    html += F("<span class='chip chip--cfg-ok'>");
    html += appliedLabel;
    html += F("</span>");
  }
  html += F("</div>"
            "<div style='display:flex;align-items:center;gap:8px'>"
            "<button type='button' class='btn btn--sm' style='width:auto' "
            "onclick=\"var p=document.getElementById('node-info-panel');p.style.display=(p.style.display==='block')?'none':'block';\">Info</button>"
            );
  if (isDeployed) {
    html += F("<button type='button' class='btn btn--sm' style='width:auto' "
              "onclick=\"var s=document.getElementById('identity-edit-section');var f=document.getElementById('edit-identity-flag');var on=s.style.display==='block';s.style.display=on?'none':'block';f.value=on?'0':'1';\">"
              "<svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M19.14 12.94c.04-.31.06-.63.06-.94s-.02-.63-.06-.94l2.03-1.58a.5.5 0 00.12-.64l-1.92-3.32a.5.5 0 00-.6-.22l-2.39.96a7.2 7.2 0 00-1.63-.94l-.36-2.54a.5.5 0 00-.49-.42h-3.84a.5.5 0 00-.49.42l-.36 2.54c-.58.22-1.12.53-1.63.94l-2.39-.96a.5.5 0 00-.6.22L2.7 8.84a.5.5 0 00.12.64l2.03 1.58c-.04.31-.06.63-.06.94s.02.63.06.94L2.82 14.5a.5.5 0 00-.12.64l1.92 3.32c.13.22.39.31.6.22l2.39-.96c.5.41 1.05.72 1.63.94l.36 2.54c.04.24.25.42.49.42h3.84c.24 0 .45-.18.49-.42l.36-2.54c.58-.22 1.12-.53 1.63-.94l2.39.96c.22.09.47 0 .6-.22l1.92-3.32a.5.5 0 00-.12-.64l-2.03-1.56zM12 15.5A3.5 3.5 0 1112 8a3.5 3.5 0 010 7.5z'/></svg>"
              " Other"
              "</button>");
  }
  html += F("</div>"
            "</div>");

  html += F("<div class='muted' style='margin:0 0 10px 0'>"
            "<strong>Node ID:</strong> ");
  html += (userId.length() ? userId : String("-"));
  html += F(" &nbsp;|&nbsp; <strong>Name:</strong> ");
  html += (name.length() ? name : String("-"));
  html += F(" &nbsp;|&nbsp; <strong>Current active interval:</strong> ");
  html += String(activeIntervalMin);
  html += F(" min</div>");

  html += F("<form action='/node-config' method='POST'>"
            "<input type='hidden' name='node_id' value='");
  html += target->nodeId;
  html += F("'>"
            "<input type='hidden' name='edit_identity' id='edit-identity-flag' value='0'>");

  html += F("<div id='node-info-panel' style='display:none;margin-bottom:10px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa'>"
            "<div><strong>FW ID:</strong> ");
  html += target->nodeId;
  html += F("</div>"
            "<label class='label' style='margin-top:8px'>Notes</label>"
            "<input class='input' type='text' name='notes' maxlength='180' placeholder='Notes for this node' value='");
  html += notes;
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
    html += F("<div id='identity-edit-section' style='display:none;margin-top:10px;padding:10px;border:1px solid #E1B17A;border-radius:8px;background:#fff8e1'>"
              "<div style='font-weight:700;color:#8a4b00;margin-bottom:6px'>Are you sure?</div>"
              "<div class='muted' style='margin-bottom:8px'>Changing Node ID/Name on a deployed node can break tracking if used incorrectly.</div>"
              "<label style='display:flex;gap:8px;align-items:flex-start;margin-bottom:8px'>"
              "<input type='checkbox' name='edit_identity_confirm' value='yes' onchange=\"var en=this.checked;document.getElementById('dep-user-id').disabled=!en;document.getElementById('dep-name').disabled=!en;\">"
              "<span>I understand and want to edit Node ID/Name for this deployed node.</span>"
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

  html += F(

            "<label class='label'>Interval (minutes)</label>"
            "<select class='input' name='interval'>");

  uint8_t intervalSel = isAllowedInterval(gWakeIntervalMin) ? (uint8_t)gWakeIntervalMin : (uint8_t)5;
  NodeDesiredConfig desired = getDesiredConfig(target->nodeId.c_str());
  if (desired.wakeIntervalMin > 0) intervalSel = desired.wakeIntervalMin;
  else if (target->wakeIntervalMin > 0) intervalSel = target->wakeIntervalMin;

  for (size_t i = 0; i < kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<option value='");
    html += String(v);
    html += F("'");
    if (v == intervalSel) html += F(" selected");
    html += F(">");
    html += String(v);
    html += F("</option>");
  }

  html += F("</select>"
            "<div class='action-choices'>"
              "<label class='action-choice action-choice--start'><input type='radio' name='action' value='start' checked><span>Start / deploy</span></label>"
              "<label class='action-choice action-choice--stop'><input type='radio' name='action' value='stop'><span>Stop / keep paired</span></label>"
              "<label class='action-choice action-choice--unpair'><input type='radio' name='action' value='unpair'><span>Unpair / forget</span></label>"
            "</div>"
            "<button type='submit' class='btn btn--success' style='margin-top:12px'>"
            "Confirm</button>"

            "</form>");

  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodeConfigSave() {
  String nodeId   = server.arg("node_id");   // firmware ID
  String userId   = server.arg("user_id");   // numeric
  String name     = server.arg("name");      // friendly name
  String notes    = server.arg("notes");
  bool editIdentityRequested = server.hasArg("edit_identity") && server.arg("edit_identity") == "1";
  bool editIdentityConfirmed = server.hasArg("edit_identity_confirm") && server.arg("edit_identity_confirm") == "yes";
  String action   = server.arg("action");
  const int safeIntervalDefault = isAllowedInterval(gWakeIntervalMin) ? gWakeIntervalMin : 5;
  int interval    = server.hasArg("interval") ? server.arg("interval").toInt() : safeIntervalDefault;

  // --- 2) Clamp interval ---
  bool intervalOk = false;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) {
      intervalOk = true;
      break;
    }
  }
  if (!intervalOk) interval = safeIntervalDefault;

  // --- 3) Find node entry ---
  NodeInfo* target = nullptr;
  for (auto &n : registeredNodes) {
    if (n.nodeId == nodeId) {
      target = &n;
      break;
    }
  }

  // --- 4) Persist user metadata to NVS with deployed safety gate ---
  const bool isCurrentlyDeployed = (target && target->state == DEPLOYED);
  const bool allowIdentityEdit = (!isCurrentlyDeployed) || (editIdentityRequested && editIdentityConfirmed);
  if (allowIdentityEdit) {
    if (server.hasArg("user_id")) setNodeUserId(nodeId, userId);
    if (server.hasArg("name")) setNodeName(nodeId, name);
  }
  if (server.hasArg("notes")) setNodeNotes(nodeId, notes);

  // --- 5) Apply interval for this node only (via desired config snapshot) ---
  NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  bool cfgChanged = false;
  uint32_t globalPhaseUnix = gLastSyncBroadcastUnix;
  if (globalPhaseUnix == 0) {
    globalPhaseUnix = getRTCTimeUnix();
  }
  if (gSyncMode == SYNC_MODE_DAILY) {
    const uint32_t dayStartUnix = (globalPhaseUnix / 86400UL) * 86400UL;
    const uint32_t targetOffset = (uint32_t)gSyncDailyHour * 3600UL + (uint32_t)gSyncDailyMinute * 60UL;
    uint32_t nextDailyUnix = dayStartUnix + targetOffset;
    if (nextDailyUnix <= getRTCTimeUnix()) nextDailyUnix += 86400UL;
    globalPhaseUnix = nextDailyUnix;
  }
  globalPhaseUnix -= (globalPhaseUnix % 60UL);
  if (dc.wakeIntervalMin != (uint8_t)interval) {
    dc.wakeIntervalMin = (uint8_t)interval;
    cfgChanged = true;
  }
  const uint16_t desiredSyncMin = (gSyncMode == SYNC_MODE_DAILY) ? 0u : (uint16_t)gSyncIntervalMin;
  if (dc.syncIntervalMin != desiredSyncMin) {
    dc.syncIntervalMin = desiredSyncMin;
    cfgChanged = true;
  }
  if (dc.syncPhaseUnix != globalPhaseUnix) {
    dc.syncPhaseUnix = globalPhaseUnix;
    cfgChanged = true;
  }
  if (dc.configVersion == 0) {
    dc.configVersion = 1;
    cfgChanged = true;
  } else if (cfgChanged) {
    dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
  }
  setDesiredConfig(nodeId.c_str(), dc);
  if (target) {
    target->wakeIntervalMin = (uint8_t)interval;
  }

  Serial.printf("[CONFIG] Interval for %s set to %d min (desired config v%u; changed=%d; applies on next node wake)\n",
                nodeId.c_str(), interval, (unsigned)dc.configVersion, cfgChanged ? 1 : 0);
  logCommandSend("SET_SCHEDULE", nodeId, true, "stored as per-node desired config");

  bool deployOk = false;
  bool revertOk = false;
  bool pairOk   = false;
  bool unpairOk = false;

  // --- 5) Start / stop logic ---
  if (action == "start") {
    if (target && target->state == UNPAIRED) {
      pairOk = pairNode(nodeId);
      logCommandSend("PAIR_NODE", nodeId, pairOk, "auto-pair before deploy");
      if (pairOk) {
        target->state = PAIRED;
        savePairedNodes();
        Serial.printf("[CONFIG] Node %s paired before deployment\n", nodeId.c_str());
      } else {
        Serial.printf("[CONFIG] Node %s failed to pair\n", nodeId.c_str());
      }
    }

    std::vector<String> ids;
    ids.push_back(nodeId);
    deployOk = deploySelectedNodes(ids);
    logCommandSend("DEPLOY_NODE", nodeId, deployOk, "Configure & Start (request only)");
    Serial.printf("[CONFIG] Start action for %s → deploySelectedNodes: %s\n",
                  nodeId.c_str(), deployOk ? "OK" : "FAIL");

} else if (action == "stop") {
  if (target) {
    NodeState prev = target->state;
    target->state = PAIRED;
    target->deployPending = false;
    savePairedNodes();
    revertOk = pairNode(nodeId);
    Serial.printf("[CONFIG] Stop action for %s from state %d → PAIRED: %s\n",
                  nodeId.c_str(), (int)prev, revertOk ? "OK" : "FAIL");
  } else {
    Serial.printf("[CONFIG] Stop action requested for %s but node not found\n",
                  nodeId.c_str());
  }
}
 else if (action == "unpair") {
    if (target) {
      // 1) Tell the node to reset its own state (best-effort)
      bool sent  = sendUnpairToNode(nodeId);
      bool local = unpairNode(nodeId);

      logCommandSend("UNPAIR_NODE", nodeId, sent,  "remote reset");
      logCommandSend("UNPAIR_LOCAL", nodeId, local, "remove from registry");

      unpairOk = sent && local;

      // 3) Clear user-facing metadata
      setNodeUserId(nodeId, "");  // removes id_<nodeId> key
      setNodeName(nodeId, "");    // removes name_<nodeId> key
      setNodeNotes(nodeId, "");   // removes note_<nodeId> key
      target->userId = "";
      target->name   = "";

      // Mothership-originated CSV writes are disabled during field testing.


      Serial.printf("[CONFIG] Unpair action for %s → send=%s, local=%s\n",
                    nodeId.c_str(),
                    sent  ? "OK" : "FAIL",
                    local ? "OK" : "FAIL");
    } else {
      Serial.printf("[CONFIG] Unpair action requested for %s but node not found\n",
                    nodeId.c_str());
    }
  }


  // --- 6) Resolve final values for display + NodeInfo ---
  // Read back from NVS, but fall back to raw form input if empty
  String finalUserId = getNodeUserId(nodeId);
  String finalName   = getNodeName(nodeId);
  String finalNotes  = getNodeNotes(nodeId);

  if (finalUserId.isEmpty()) finalUserId = userId;
  if (finalName.isEmpty())   finalName   = name;

  // If NodeInfo has userId / name fields, keep them in sync
  if (target) {
    target->userId = finalUserId;
    target->name   = finalName;
  }

  // --- 7) Feedback page ---
  String actionsHtml = String("<a href='/nodes' class='btn btn--sm'>Back</a><a href='/node-config?node_id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Configure", actionsHtml);
  html += F("<div class='section center'>"
            "<h3>Node configuration applied</h3>");

  if (!target) {
    html += F("<p class='muted'>Warning: this node ID is not currently in the registered list. "
              "Commands may not have reached any device.</p>");
  }

  html += F("<p><strong>Firmware ID:</strong> ");
  html += nodeId;
  html += F("<br><strong>Node ID (numeric):</strong> ");
  html += (finalUserId.length() ? finalUserId : String("-"));
  html += F("<br><strong>Name:</strong> ");
  html += (finalName.length() ? finalName : String("-"));
  html += F("<br><strong>Notes:</strong> ");
  html += (finalNotes.length() ? finalNotes : String("-"));
  html += F("<br><strong>Interval:</strong> ");
  html += String(interval);
  html += F(" min<br><strong>Action:</strong> ");
  html += action;
  html += F("</p>");

  html += F("<p class='muted'>"
            "Schedule broadcast: ");
  html += "stored for this node (applies on next wake)";
  html += F("<br>Pair (if unpaired): ");
  html += pairOk ? "OK" : "not requested / failed";
  html += F("<br>Start / deploy: ");
  html += deployOk ? "REQUESTED (awaiting node confirmation)" : "not requested / failed";
  html += F("<br>Stop / revert: ");
  html += revertOk ? "OK" : "not requested / failed";
  html += F("<br>Unpair / forget: ");
  html += unpairOk ? "OK" : "not requested / failed";
  html += F("</p>"
            "<a href='/nodes' class='btn btn--primary'>Back to Node manager</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodesPage() {
  String html = headCommon("Node manager",
    "<a href='/' class='btn btn--sm'>Back</a><a href='/nodes' class='btn btn--sm'>Refresh</a>");
  auto allNodes = getRegisteredNodes();

  html += F("<div class='section'>");

  if (allNodes.empty()) {
    html += F("<p class='muted'>No nodes registered yet. Try discovering and pairing first.</p>");
  } else {
    html += F("<p class='muted' style='margin:0 0 10px 0'>"
          "Interval shows the configured target for each node. "
          "Next wake uses last node contact cadence (est while asleep). "
          "Queue is buffered samples reported by the node at wake start."
          "</p>");
    html += F("<div class='list'>");

        for (auto &node : allNodes) {

      String userId = node.userId;
      String name   = node.name;
      if (userId.isEmpty()) userId = getNodeUserId(node.nodeId);
      if (name.isEmpty())   name   = getNodeName(node.nodeId);
      NodeDesiredConfig nodeDesired = getDesiredConfig(node.nodeId.c_str());
      uint16_t desiredCfgV = nodeDesired.configVersion;
      uint16_t appliedCfgV = node.configVersionApplied;
      bool cfgPending = (desiredCfgV > 0 && appliedCfgV < desiredCfgV);
      uint8_t observedWakeMin = (node.wakeIntervalMin > 0)
        ? node.wakeIntervalMin
        : node.inferredWakeIntervalMin;
      int nodeIntervalCurrentMin = (nodeDesired.wakeIntervalMin > 0)
        ? (int)nodeDesired.wakeIntervalMin
        : ((observedWakeMin > 0)
            ? (int)observedWakeMin
            : (isAllowedInterval(gWakeIntervalMin) ? gWakeIntervalMin : 5));
      const uint8_t nodeQueueDepth = node.lastReportedQueueDepth;
      const String nextWake = computeNextWakeIsoLocal(nodeIntervalCurrentMin, node.lastSeen, node.isActive);
      const String nextWakeTime = (nextWake.length() >= 8) ? nextWake.substring(0, 8) : String("n/a");
      if (cfgPending && node.state == DEPLOYED && !node.deployPending &&
          nodeDesired.wakeIntervalMin > 0 && observedWakeMin > 0 &&
          nodeDesired.wakeIntervalMin == observedWakeMin) {
        // Treat HELLO-reported wake interval match as applied config evidence when ACK is missed.
        cfgPending = false;
      }

      String deployState = "Unpaired";
      if (node.state == DEPLOYED) deployState = "Deployed";
      else if (node.state == PAIRED) deployState = "Paired";

      String appliedLabel = "";
      if (!node.stateChangePending && node.lastStateAppliedMs > 0) {
        if (node.lastAppliedTargetState == PENDING_TO_PAIRED) appliedLabel = "Paired applied";
        else if (node.lastAppliedTargetState == PENDING_TO_UNPAIRED) appliedLabel = "Unpaired applied";
        else if (node.lastAppliedTargetState == PENDING_TO_DEPLOYED) appliedLabel = "";
        else appliedLabel = "Applied";
      }

      String displayId = userId.length() ? userId : node.nodeId;

      html += F("<a href='/node-config?node_id=");
      html += node.nodeId;
      html += F("' class='item item--node'>"
                "<div class='node-row'>"
                "<div class='node-main'>"
                "<strong>");
      html += displayId;
      html += F("</strong>");
      if (name.length()) {
        html += F("<span class='muted node-name'>");
        html += name;
        html += F("</span>");
      }
      html += F("</div>"
                "<div class='node-timing'>"
                "<div class='node-timing-cell'>"
                "<span class='node-timing-label'>Interval</span>"
                "<span class='chip node-timing-value'>");
      html += String(nodeIntervalCurrentMin);
      html += F(" min</span>"
                "</div>"
                "<div class='node-timing-cell'>"
                "<span class='node-timing-label'>Next wake</span>"
                "<span class='chip node-timing-value'>");
      html += nextWakeTime;
              html += F("</span>"
                    "</div>"
                    "<div class='node-timing-cell'>"
                    "<span class='node-timing-label'>Queue</span>"
                    "<span class='chip node-timing-value'>");
              html += String((unsigned)nodeQueueDepth);
      html += F("</span>"
                "</div>"
                "</div>"
                "<div class='node-status'>"
                "<div class='node-status-cell'>"
                "<span class='node-timing-label'>Deploy</span>");

      if (node.state == DEPLOYED) html += F("<span class='chip chip--state-deployed'>Deployed</span>");
      else if (node.state == PAIRED) html += F("<span class='chip chip--state-paired'>Paired</span>");
      else html += F("<span class='chip chip--state-unpaired'>Unpaired</span>");
      if (appliedLabel.length()) {
        html += F("<span class='chip chip--cfg-ok' style='margin-top:4px'>");
        html += appliedLabel;
        html += F("</span>");
      }

      html += F("</div>"
                "<div class='node-status-cell'>"
                "<span class='node-timing-label'>Config</span>");

      if (cfgPending) html += F("<span class='chip chip--cfg-pending'>Config pending</span>");
      else html += F("<span class='chip chip--cfg-ok'>Config updated</span>");

      html += F("</div></div></div></a>");
    }


    html += F("</div>"); // .list
  }
  html += F("</div>"); // .section

  html += F("<script>setTimeout(function(){ location.reload(); }, 15000);</script>");

  html += footCommon();
  server.send(200, "text/html", html);
}


// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  gBootMs = millis();

  Serial.println("Starting RTC setup...");
  setupRTC();

  Serial.println("Starting SD Card setup...");
  setupSD();

  Serial.println("Starting WiFi setup...");
#if ENABLE_WIFI_AP_WEBSERVER
  Serial.println("Starting WiFi AP...");
#if ENABLE_ESPNOW_RUNTIME
  WiFi.mode(WIFI_AP_STA);
#else
  WiFi.mode(WIFI_AP);
#endif
#else
  // Keep WiFi radio in STA mode for ESP-NOW, but disable AP/web server during BLE soak tests.
  WiFi.mode(WIFI_STA);
#endif
#if ENABLE_BLE_GATT
  // BLE + WiFi coexistence on ESP32-S3 requires modem sleep to be enabled.
  WiFi.setSleep(true);
#else
  // Keep AP beacons fully active while BLE is disabled.
  WiFi.setSleep(false);
#endif

  // AP and ESP-NOW must share one RF channel on ESP32 to avoid coexistence issues.
#if ENABLE_WIFI_AP_WEBSERVER
  bool apOk = WiFi.softAP(ssid.c_str(), password, ESPNOW_CHANNEL, false, 4);

  if (!apOk) {
    Serial.println("❌ SoftAP failed to start");
  } else {
    Serial.println("✅ SoftAP started");
  }

  Serial.print("SoftAP SSID: "); Serial.println(WiFi.softAPSSID());
  Serial.print("SoftAP IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("SoftAP MAC: "); Serial.println(WiFi.softAPmacAddress());
  Serial.print("SoftAP channel: "); Serial.println(WiFi.channel());
#else
  Serial.println("WiFi AP/web server disabled for BLE stability test");
  Serial.print("STA MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel: "); Serial.println(WiFi.channel());
#endif
  Serial.print("Device ID: "); Serial.println(DEVICE_ID);
  Serial.print("WiFi Network: "); Serial.println(ssid);
  Serial.print("Firmware: "); Serial.print(FW_VERSION); Serial.print(" "); Serial.println(FW_BUILD);

  delay(1000);
#if ENABLE_ESPNOW_RUNTIME
  Serial.println("Starting ESP-NOW setup...");
  setupESPNOW();
#else
  Serial.println("ESP-NOW runtime disabled (ENABLE_ESPNOW_RUNTIME=0)");
#endif
#if ENABLE_BLE_GATT
  setSensorDataEventCallback(onBleSensorTelemetry);

  Serial.println("Starting BLE GATT setup...");
  bleSetup(DEVICE_ID, FW_VERSION, buildBleStatusDataJson, getRTCTimeUnix, handleBleCommand);
#else
  Serial.println("BLE GATT disabled (ENABLE_BLE_GATT=0)");
#endif

  char timeStr[32];
  getRTCTimeString(timeStr, sizeof(timeStr));
  Serial.print("Current RTC Time: "); Serial.println(timeStr);

  loadWakeIntervalFromNVS();
  loadSyncIntervalFromNVS();
  loadSyncModeFromNVS();
  loadDailySyncTimeFromNVS();
  loadSyncRuntimeGuardsFromNVS();
  Serial.printf("Current wake interval (from NVS): %d min\n", gWakeIntervalMin);
  Serial.printf("Current sync interval payload (from NVS): %d min\n", gSyncIntervalMin);
  Serial.printf("Current daily sync time (from NVS): %s\n", formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute).c_str());
  Serial.printf("Sync guards (from NVS): day=%ld lastUnix=%lu slot=%lld\n",
                gLastSyncBroadcastEpochDay,
                gLastSyncBroadcastUnix,
                gLastSyncIntervalSlot);

  // 🔎 One-shot boot summary so you can see the fleet state in one block
  logBootSummary();

#if ENABLE_WIFI_AP_WEBSERVER && ENABLE_SPIFFS_ASSETS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed; falling back to inline assets.");
  } else {
    server.on("/style.css", HTTP_GET, [](){
      File f = SPIFFS.open("/style.css.gz", "r");
      if (!f) { server.send(404, "text/plain", "style.css.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "text/css"); f.close();
    });
    server.on("/app.js", HTTP_GET, [](){
      File f = SPIFFS.open("/app.js.gz", "r");
      if (!f) { server.send(404, "text/plain", "app.js.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "application/javascript"); f.close();
    });
  }
#endif

#if ENABLE_WIFI_AP_WEBSERVER
  // Routes
  server.on("/", HTTP_ANY, handleRoot);
  #if ENABLE_CAPTIVE_PORTAL
  // Common mobile captive-portal probes: return a local landing page.
  server.on("/generate_204", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/gen_204", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/hotspot-detect.html", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/library/test/success.html", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/connecttest.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/ncsi.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/success.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/wpad.dat", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/redirect", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/fwlink", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/mobile/status.php", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/favicon.ico", HTTP_ANY, [](){ server.send(204); });
  #endif
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/set-sync-mode", HTTP_POST, handleSetSyncMode);
  server.on("/set-sync-interval", HTTP_POST, handleSetSyncInterval);
  server.on("/set-sync-time", HTTP_POST, handleSetSyncTime);
  server.on("/ui-status", HTTP_GET, []() {
    server.send(200, "application/json", buildBleStatusDataJson());
  });

  // Node manager + config routes
  server.on("/nodes", HTTP_GET, handleNodesPage);
  server.on("/node-config", HTTP_GET, handleNodeConfigForm);
  server.on("/node-config", HTTP_POST, handleNodeConfigSave);
  server.on("/revert-node", HTTP_POST, handleRevertNode); // still available if you use it elsewhere

  #if ENABLE_CAPTIVE_PORTAL
  server.onNotFound(sendCaptivePortalLanding);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("Captive portal DNS enabled");
  #endif

  server.begin();
  Serial.println("✅ Web server started!");
#else
  Serial.println("Web server disabled");
#endif
}

void loop() {
#if ENABLE_WIFI_AP_WEBSERVER
  #if ENABLE_CAPTIVE_PORTAL
  dnsServer.processNextRequest();
  #endif
  server.handleClient();
#endif
#if ENABLE_ESPNOW_RUNTIME
  espnow_loop(); // Handle ESP-NOW node management
#endif
#if ENABLE_BLE_GATT
  bleLoop();
#endif

  // Global sync trigger driven by selected sync mode.
#if ENABLE_ESPNOW_RUNTIME
  {
    const bool syncQuietPeriod = ((uint32_t)(millis() - gBootMs) < kSyncBootQuietMs);

    const uint32_t nowUnix = getRTCTimeUnix();
    if (!syncQuietPeriod && nowUnix > 946684800UL) {
    if (gSyncMode == SYNC_MODE_DAILY) {
      static const uint8_t kDailyRetryWindowMin = 15;
      static const uint32_t kDailyRetryIntervalMs = 30000UL;
      const long epochDay = (long)(nowUnix / 86400UL);
      DateTime now(nowUnix);
      const int nowMinOfDay = now.hour() * 60 + now.minute();
      const int targetMinOfDay = gSyncDailyHour * 60 + gSyncDailyMinute;
      int minsSinceTarget = nowMinOfDay - targetMinOfDay;
      if (minsSinceTarget < 0) minsSinceTarget += 24 * 60;

      const bool inDailyRetryWindow = (minsSinceTarget >= 0) && (minsSinceTarget < (int)kDailyRetryWindowMin);
      const bool dayNotCompleted = (gLastSyncBroadcastEpochDay != epochDay);
      const bool retryDue = (gLastSyncBroadcastMs == 0) || ((millis() - gLastSyncBroadcastMs) >= kDailyRetryIntervalMs);

      if (inDailyRetryWindow && dayNotCompleted && retryDue) {
        // Encode the precise target HH:MM in phaseUnix so nodes extract the correct
        // daily time. Using nowUnix would cause the re-armed alarm to drift each day
        // by the broadcast-arrival offset from the configured target.
        const uint32_t dayStartUnix = (nowUnix / 86400UL) * 86400UL;
        const uint32_t phaseUnix = dayStartUnix
            + (uint32_t)gSyncDailyHour * 3600UL
            + (uint32_t)gSyncDailyMinute * 60UL;
        const bool sent = broadcastSyncSchedule(0, phaseUnix);
        const bool markerSent = broadcastSyncWindowOpen(phaseUnix);
        gLastSyncBroadcastMs = millis();
        if (sent || markerSent) {
          gLastSyncBroadcastEpochDay = epochDay;
          gLastSyncBroadcastUnix = phaseUnix;
          saveSyncRuntimeGuardsToNVS();
          gSyncTriggerCount++;
          Serial.printf("[SYNC] Daily trigger %02d:%02d -> schedule=%s marker=%s\n",
                        gSyncDailyHour,
                        gSyncDailyMinute,
                        sent ? "SENT" : "NOT_SENT",
                        markerSent ? "SENT" : "NOT_SENT");
        } else {
          Serial.printf("[SYNC] Daily retry pending: %02d:%02d day=%ld (window+%umin)\n",
                        gSyncDailyHour,
                        gSyncDailyMinute,
                        epochDay,
                        (unsigned)kDailyRetryWindowMin);
        }
      }
    } else {
      const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
      const uint32_t anchorUnix = (gLastSyncBroadcastUnix > 0)
        ? gLastSyncBroadcastUnix
        : (nowUnix - (nowUnix % 60UL));
      const long long slot = (nowUnix >= anchorUnix)
        ? (long long)((nowUnix - anchorUnix) / periodSec)
        : -1LL;
      const bool spacedOk = (gLastSyncBroadcastMs == 0) || ((millis() - gLastSyncBroadcastMs) >= periodSec * 1000UL);
      if ((gLastSyncIntervalSlot < 0 || slot > gLastSyncIntervalSlot) && spacedOk) {
        const uint32_t currentSlotUnix = (slot >= 0)
          ? (anchorUnix + (uint32_t)slot * periodSec)
          : anchorUnix;
        const bool sent = broadcastSyncSchedule(gSyncIntervalMin, anchorUnix);
        const bool markerSent = broadcastSyncWindowOpen(currentSlotUnix);
        gLastSyncBroadcastMs = millis();
        if (gLastSyncBroadcastUnix == 0) {
          gLastSyncBroadcastUnix = anchorUnix;
        }
        gLastSyncIntervalSlot = slot;
        saveSyncRuntimeGuardsToNVS();
        gSyncTriggerCount++;
        Serial.printf("[SYNC] Interval trigger %d min -> schedule=%s marker=%s\n",
                      gSyncIntervalMin,
                      sent ? "SENT" : "NOT_SENT",
                      markerSent ? "SENT" : "NOT_SENT");
      }
    }
#if ENABLE_SYNC_AUDIT_LOG
    if (millis() - gLastSyncAuditMs >= 30000UL) {
      gLastSyncAuditMs = millis();
      const uint32_t nextSyncUnix = computeNextSyncUnix(nowUnix);
      const uint32_t secToNext = (nextSyncUnix > nowUnix) ? (nextSyncUnix - nowUnix) : 0;
      const uint32_t secSinceLast = (gLastSyncBroadcastUnix > 0 && nowUnix >= gLastSyncBroadcastUnix)
        ? (nowUnix - gLastSyncBroadcastUnix)
        : 0;
      Serial.printf("[SYNC_AUDIT] mode=%s syncMin=%d nextIn=%lus lastAgo=%lus count=%lu\n",
                    syncModeLabel(),
                    gSyncIntervalMin,
                    (unsigned long)secToNext,
                    (unsigned long)secSinceLast,
                    (unsigned long)gSyncTriggerCount);
    }
#endif
    }
  }
#endif

#if ENABLE_PERIODIC_RTC_SERIAL_LOG
  // Print current RTC time every 10s (verbose mode)
  static unsigned long lastTimeCheck = 0;
  if (millis() - lastTimeCheck > 10000) {
    char timeBuffer[24];
    getRTCTimeString(timeBuffer, sizeof(timeBuffer));
    Serial.print("Current RTC Time: ");
    Serial.println(timeBuffer);
    lastTimeCheck = millis();
  }
#endif

  // Mothership-originated STATUS rows to SD are disabled.
}

