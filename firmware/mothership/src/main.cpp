// ====== ESP-NOW Web UI – main.cpp with Node Manager & Node Meta ======

// ---------- Config toggles ----------
#define ENABLE_SPIFFS_ASSETS 0  // set to 1 if you upload /style.css.gz and /app.js.gz
#define ENABLE_WIFI_AP_WEBSERVER 1  // set to 1 to enable AP + web UI

// ---------- Includes ----------
#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <WebServer.h>
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
String ssid = String(BASE_SSID) + String(DEVICE_ID);  // "Logger001"
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
int gWakeIntervalMin = 5;          // fleet default shown in UI; per-node interval is authoritative
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

// ---------- Web server ----------
WebServer server(80);

// NodeInfo is defined in your ESP-NOW / protocol headers
extern std::vector<NodeInfo> registeredNodes;

// ---------- UI helpers ----------
static String formatMac(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

static void loadWakeIntervalFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int v = gPrefs.getInt("wake_min", gWakeIntervalMin);
    gPrefs.end();
    bool ok = false;
    for (size_t i = 0; i < kAllowedCount; i++)
      if (v == kAllowedIntervals[i]) { ok = true; break; }
    gWakeIntervalMin = ok ? v : 5;
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

static String computeNextSyncIsoLocal() {
  const uint32_t nowUnix = getRTCTimeUnix();
  DateTime next(nowUnix);

  if (gSyncMode == SYNC_MODE_INTERVAL) {
    const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
    const uint32_t nextUnix = ((nowUnix / periodSec) + 1UL) * periodSec;
    next = DateTime(nextUnix);
  } else {
    DateTime now(nowUnix);
    next = DateTime(now.year(), now.month(), now.day(), gSyncDailyHour, gSyncDailyMinute, 0);
    if (now.unixtime() >= next.unixtime()) {
      DateTime tomorrow(now.unixtime() + 24UL * 60UL * 60UL);
      next = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), gSyncDailyHour, gSyncDailyMinute, 0);
    }
  }

  char b[20];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
           next.year(), next.month(), next.day(), next.hour(), next.minute(), next.second());
  return String(b);
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix) {
  if (nowUnix <= 946684800UL) return 0;

  if (gSyncMode == SYNC_MODE_INTERVAL) {
    const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
    return ((nowUnix / periodSec) + 1UL) * periodSec;
  }

  DateTime now(nowUnix);
  DateTime next(now.year(), now.month(), now.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  if (now.unixtime() >= next.unixtime()) {
    DateTime tomorrow(now.unixtime() + 24UL * 60UL * 60UL);
    next = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  }
  return next.unixtime();
}

static String computeNextWakeIsoLocal(int intervalMin) {
  if (intervalMin <= 0) return String("n/a");

  const uint32_t nowUnix = getRTCTimeUnix();
  DateTime now(nowUnix);

  DateTime base(now.year(), now.month(), now.day(), now.hour(), now.minute(), 0);
  if (now.second() != 0) {
    base = base + TimeSpan(0, 0, 1, 0);
  }

  const int mod = base.minute() % intervalMin;
  const int addMin = (mod == 0) ? intervalMin : (intervalMin - mod);
  DateTime next = base + TimeSpan(0, 0, addMin, 0);

  const uint32_t deltaSec = (next.unixtime() > now.unixtime()) ? (next.unixtime() - now.unixtime()) : 0;
  const uint32_t deltaMin = (deltaSec + 59UL) / 60UL;

  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d (in %lu min)",
           next.year(), next.month(), next.day(), next.hour(), next.minute(), next.second(),
           (unsigned long)deltaMin);
  return String(b);
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
  String value = prefs.getString(key.c_str(), "");
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
  for (const auto& n : allNodes) {
    if (n.state == DEPLOYED) deployedCount++;
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
        dc.configVersion   = max((int)dc.configVersion + 1, 1);
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

    bool sent = broadcastSyncSchedule(interval, (unsigned long)phase);
    gSyncIntervalMin = interval;
    gLastSyncBroadcastEpochDay = (long)(((unsigned long)phase) / 86400UL);
    gLastSyncBroadcastUnix = (unsigned long)phase;
    gLastSyncIntervalSlot = (long long)((unsigned long)phase / ((unsigned long)max(interval, 1) * 60UL));
    saveSyncIntervalToNVS(interval);

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
        const unsigned long phaseUnix = (hasPhase && phase > 0) ? (unsigned long)phase : getRTCTimeUnix();
        syncSent = broadcastSyncSchedule(iv, phaseUnix);
        gSyncIntervalMin = iv;
        gLastSyncBroadcastEpochDay = (long)(phaseUnix / 86400UL);
        gLastSyncBroadcastUnix = phaseUnix;
        gLastSyncIntervalSlot = (long long)(phaseUnix / ((unsigned long)max(iv, 1) * 60UL));
        saveSyncIntervalToNVS(iv);
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
  --bg:#f5f5f5; --panel:#ffffff; --text:#1b1f23; --sub:#5f6b7a; --border:#e5e7eb;
  --primary:#2196F3; --success:#4CAF50; --warn:#ff9800; --danger:#f44336;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(0,0,0,.08);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{scroll-behior:smooth}
html,body{margin:0;padding:0;background:var(--bg);color:var(--text);
  font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif}
a{color:var(--primary);text-decoration:none}
:focus-visible{outline:3px solid rgba(33,150,243,.35);outline-offset:2px}

/* Layout */
.container{max-width:600px;margin:0 auto;padding:var(--sp-3)}
.header{padding:var(--sp-3) 0;text-align:center}
.h1{font-size:22px;font-weight:700;margin:0 0 var(--sp-2)}
.section{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:var(--sp-3);box-shadow:var(--shadow);margin:var(--sp-3) 0}
.section h3{margin:0 0 var(--sp-2);font-size:18px}
.muted{color:var(--sub);font-size:.95rem}

/* Stats */
.stats{display:grid;grid-template-columns:1fr 1fr 1fr;gap:var(--sp-1);text-align:center}
.stat{background:#fafafa;border:1px solid var(--border);border-radius:8px;padding:10px}
.stat strong{display:block;font-size:13px;color:var(--sub);margin-bottom:2px}
.stat .num{font-size:18px;font-weight:700}

/* Lists/cards */
.list{display:grid;gap:var(--sp-1)}
.item{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px;display:block;color:inherit}
.item-row{display:flex;align-items:center;justify-content:space-between;gap:12px}

/* Chips */
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}
.chip--state-deployed{border-color:#c8e6c9;background:#f1f8e9;color:#256029}
.chip--state-paired{border-color:#ffe0b2;background:#fff3e0;color:#e65100}
.chip--state-unpaired{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}
.chip--link-awake{border-color:#b3e5fc;background:#e1f5fe;color:#01579b}
.chip--link-asleep{border-color:#e1bee7;background:#f3e5f5;color:#4a148c}
.chip--link-offline{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}
.chip--cfg-pending{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00}
.chip--cfg-ok{border-color:#c8e6c9;background:#f1f8e9;color:#256029}

/* Forms */
.label{display:block;margin:8px 0 6px;color:var(--sub);font-size:.95rem}
.input, input[type="text"], input[type="number"], select{
  width:100%;padding:12px;border:1px solid var(--border);border-radius:8px;background:#fff
}
.help{color:var(--sub);font-size:.85rem;margin-top:6px}
.row{display:flex;gap:var(--sp-1);flex-wrap:wrap}
.col{flex:1 1 220px;min-width:0}

/* Buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;
  padding:12px 16px;border-radius:8px;border:1px solid var(--border);background:#fff;color:var(--text);
  cursor:pointer;width:100%;margin-top:8px;text-decoration:none}
.btn--primary{background:var(--primary);color:#fff;border-color:transparent}
.btn--success{background:var(--success);color:#fff;border-color:transparent}
.btn--warn{background:var(--warn);color:#fff;border-color:transparent}
.btn:disabled{opacity:.6;cursor:not-allowed}

/* Utility */
.center{text-align:center}
.badge{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;color:var(--sub);font-size:.85rem}
.footer-bar{
  position:sticky;bottom:0;background:var(--panel);border:1px solid var(--border);
  padding:calc(var(--sp-2) + env(safe-area-inset-bottom)) var(--sp-3);
  border-radius:12px;box-shadow:var(--shadow);display:flex;gap:12px;justify-content:space-between
}

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
  const s=`${n.getFullYear()}-${z(n.getMonth()+1)}-${z(n.getDate())} ${z(n.getHours())}:${z(n.getMinutes())}:${z(n.getSeconds())}`;
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
window.addEventListener('DOMContentLoaded', () => {
  setCurrentTime();
  wireAsyncForms();
});

// Live 1s ticking of the displayed RTC time
(function(){
  const pad = n => String(n).padStart(2,'0');
  function parseYMDHMS(str){
    if (!str || str.length < 19) return NaN;
    const y = +str.slice(0,4), m = +str.slice(5,7), d = +str.slice(8,10);
    const H = +str.slice(11,13), M = +str.slice(14,16), S = +str.slice(17,19);
    const dt = new Date(y, m-1, d, H, M, S); // local
    return isNaN(dt) ? NaN : dt.getTime();
  }
  function formatYMDHMS(ms){
    const dt = new Date(ms);
    return `${dt.getFullYear()}-${pad(dt.getMonth()+1)}-${pad(dt.getDate())} ` +
           `${pad(dt.getHours())}:${pad(dt.getMinutes())}:${pad(dt.getSeconds())}`;
  }
  function startClock(){
    const el = document.getElementById('rtc-now');
    if (!el) return;
    const initial = (el.textContent || '').trim();
    const rtcMs   = parseYMDHMS(initial);
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatYMDHMS(nowMs);
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
static String headCommon(const String& title){
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
  h += F("<div class='h1'>"); h += title; h += F("</div>");
  h += F("<div class='badge'>Device ID: <strong>");
  h += DEVICE_ID;
  h += F("</strong></div>");
  h += F("<div class='muted'>Wi-Fi: ");
  h += ssid;
  h += F(" • ");
  h += FW_VERSION;
  h += F(" — ");
  h += FW_BUILD;
  h += F("</div></div>");
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
  html += F("<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleRoot() {
  String html = headCommon("ESP32 Data Logger");

  // Current RTC time
  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));

  String csvStats = getCSVStats();

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes) {
    if (node.state == DEPLOYED) deployedNodes++;
  }

  // --- Timing & RTC section ---
  html += F("<div class='section' aria-live='polite'>"
            "<h3>⏱ Timing &amp; RTC</h3>"
            "<p class='muted'>Live DS3231 clock plus fleet default timing templates.</p>"
            "<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>"
            "<div class='row'>");

  // Left: RTC
  html += F("<div class='col'>"
              "<strong>Current RTC Time</strong><br>"
              "<div id='rtc-now' style='font-size:18px;color:#1976D2;margin-top:6px'>");
  html += currentTime;
  html += F("</div>"
            "<div class='help'>Clock is driven by the DS3231 on this mothership.</div>"
            "</div>");

  // Right: fleet default wake interval template
  html += F("<div class='col'>"
              "<form class='async-form' action='/set-wake-interval' method='POST'>"
              "<label class='label'><strong>Default wake interval (minutes)</strong></label>"
              "<select class='input' name='interval'>");

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
            "Save default</button>"
            "<div class='help'>Used as a template for Node Manager intervals. Current default: <strong>");
  html += String(gWakeIntervalMin);
  html += F(" min</strong></div>"
            "</form>");

  html += F("<div class='help' style='margin-top:10px'>Current sync mode: <strong>");
  html += syncModeLabel();
  html += F("</strong><br>Next sync: <strong>");
  html += computeNextSyncIsoLocal();
  html += F("</strong></div>");

  html += F("<button id='sync-btn' class='btn btn--warn' type='button' onclick='toggleSyncSchedule()' style='margin-top:10px'>"
            "⚙️ Sync schedule controls"
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
  html += F(">Interval (testing / advanced)</option>"
            "</select>"
            "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
            "Save sync mode</button>"
            "<div class='help'>Sets how global sync triggers are generated.</div>"
            "</form>");

  if (gSyncMode == SYNC_MODE_DAILY) {
    html += F("<form class='async-form' action='/set-sync-time' method='POST' style='margin-top:12px'>"
              "<label class='label'><strong>Daily sync time</strong></label>"
              "<input class='input' type='time' name='sync_time' value='");
    html += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
    html += F("'>"
              "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
              "Set daily sync time</button>"
              "<div class='help'>Runs once per day at the selected time.</div>"
              "</form>");
  } else {
    html += F("<form class='async-form' action='/set-sync-interval' method='POST' style='margin-top:12px'>"
              "<label class='label'><strong>Sync every (minutes)</strong></label>"
              "<select class='input' name='interval'>");

    for (size_t i = 0; i < kAllowedCount; ++i) {
      int v = kAllowedIntervals[i];
      html += F("<option value='");
      html += String(v);
      html += F("'");
      if (v == gSyncIntervalMin) html += F(" selected");
      html += F(">");
      html += String(v);
      html += F("</option>");
    }

    html += F("</select>"
              "<button type='submit' class='btn btn--warn' style='margin-top:8px'>"
              "Set sync interval</button>"
              "<div class='help'>Runs at fixed RTC-aligned interval boundaries.</div>"
              "</form>");
  }

  html += F("</div>"
            "</div>"); // end col

  html += F("</div>"); // end row

  // RTC settings toggle + panel
  html += F("<div style='margin-top:12px'>"
            "<button id='settings-btn' class='btn' type='button' onclick='toggleSettings()'>"
            "⚙️ Set RTC time…"
            "</button>"
            "</div>");

  html += F("<div id='settings-panel' class='section' style='display:none;margin-top:12px'>"
            "<h3>⚙️ RTC Time Configuration</h3>"
            "<p class='muted'>Only needed for initial setup or DS3231 correction.</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set new time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='YYYY-MM-DD HH:MM:SS' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Use browser time</button>"
            "<button type='submit' class='btn btn--success'>Set RTC</button>"
            "</div>"
            "<div class='help'>Example: 2025-11-14 21:05:00</div>"
            "</form>"
            "</div>");

  html += F("</div>"); // end Timing & RTC section

  // --- Data logging section ---
  html += F("<div class='section'>"
            "<h3>📊 Data Logging</h3>"
            "<p class='muted'><strong>Status:</strong> ");
  html += csvStats;
  html += F("</p>"
            "<a href='/download-csv' class='btn btn--success'>⬇️ Download CSV Data</a>"
            "<div class='help'>Downloads all logged sensor data from /datalog.csv.</div>"
            "</div>");

  // --- Node discovery & fleet overview ---
  html += F("<div class='section'>"
            "<h3>📡 Node Discovery &amp; Fleet Overview</h3>"
            "<p class='muted'><strong>Mothership MAC:</strong> ");
  html += getMothershipsMAC();
  html += F("</p>"
            "<div class='stats' style='margin:12px 0'>"
              "<div class='stat'><strong>Deployed</strong><span class='num'>");
  html += String(deployedNodes);
  html += F("</span></div>"
              "<div class='stat'><strong>Paired</strong><span class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div>"
              "<div class='stat'><strong>Unpaired</strong><span class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div>"
            "</div>"
            "<form class='async-form' action='/discover-nodes' method='POST'>"
            "<button type='submit' class='btn btn--primary'>🔍 Discover New Nodes</button>"
            "</form>"
            "<a href='/nodes' class='btn btn--success' style='margin-top:8px'>🧩 Open Node Manager</a>"
            "</div>");

  // --- Footer bar ---
  html += F("<div class='footer-bar'>"
            "<a href='/' class='btn'>🔄 Refresh</a>"
            "<a href='/download-csv' class='btn btn--success'>⬇️ CSV</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetTime() {
  String dt = server.arg("datetime");
  int yy, mm, dd, hh, mi, ss;
  if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss) == 6) {
    if (setRTCTime(yy, mm, dd, hh, mi, ss)) {
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
              "<p>Please use the format: YYYY-MM-DD HH:MM:SS</p><p>You entered: <em>");
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
  sendDiscoveryBroadcast();

  if (isAjaxRequest()) {
    sendAjaxResult(true, "Discovery broadcast sent");
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
  bool ok = false;
  for (size_t i = 0; i < kAllowedCount; ++i)
    if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 5;

  gWakeIntervalMin = interval;
  saveWakeIntervalToNVS(interval);

  Serial.printf("[UI] Default wake interval template set to %d min (no fleet broadcast)\n",
                interval);

  if (isAjaxRequest()) {
    sendAjaxResult(true, "Default wake interval saved (Node Manager per-node intervals are unchanged)");
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>⏰ Default Wake Interval</h3><p>Saved template: ");
  html += String(interval);
  html += F(" min.</p><p style='color:#666'>Existing node intervals are unchanged. Configure each node in Node Manager.</p>");
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
  bool sent = broadcastSyncSchedule(interval, phaseUnix);

  gSyncIntervalMin = interval;
  gLastSyncBroadcastEpochDay = (long)(phaseUnix / 86400UL);
  gLastSyncBroadcastUnix = phaseUnix;
  gLastSyncBroadcastMs = millis();
  gLastSyncIntervalSlot = (long long)(phaseUnix / ((unsigned long)max(interval, 1) * 60UL));
  saveSyncIntervalToNVS(interval);

  Serial.printf("[UI] Sync interval set to %d min phase=%lu -> broadcast %s (mode=%s)\n",
                interval,
                phaseUnix,
                sent ? "SENT" : "FAILED",
                syncModeLabel());

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

  String html = headCommon("Configure Node");
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3>"
              "<p class='muted'>No node with that ID is currently registered.</p>"
              "<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  String userId  = getNodeUserId(target->nodeId);
  String name    = getNodeName(target->nodeId);

  const char* stateLabel = "Unknown";
  if (target->state == UNPAIRED) stateLabel = "Unpaired";
  else if (target->state == PAIRED) stateLabel = "Paired";
  else if (target->state == DEPLOYED) stateLabel = "Deployed";

  html += F("<h3>⚙️ Configure &amp; Start</h3>"
            "<p class='muted'>Set a numeric Node ID and a descriptive name, then start or stop the node.</p>");

  html += F("<p><strong>Firmware ID:</strong> ");
  html += target->nodeId;
  html += F("<br><strong>Current state:</strong> ");
  html += stateLabel;
  html += F("</p>");

  html += F("<form action='/node-config' method='POST'>"
            "<input type='hidden' name='node_id' value='");
  html += target->nodeId;
  html += F("'>"

            "<label class='label'>Node ID (numeric, e.g. 001)</label>"
            "<input class='input' type='text' name='user_id' maxlength='3' "
            "placeholder='001' value='");
  html += userId;
  html += F("'>"

            "<label class='label'>Name</label>"
            "<input class='input' type='text' name='name' "
            "placeholder='e.g. North Hedge 01' value='");
  html += name;
  html += F("'>"

            "<label class='label'>Interval (minutes)</label>"
            "<select class='input' name='interval'>");

  uint8_t intervalSel = gWakeIntervalMin;
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

            "<label class='label'>Action</label>"
            "<div class='row'>"
              "<label style='flex:1'><input type='radio' name='action' value='start' checked> Start / deploy</label>"
              "<label style='flex:1'><input type='radio' name='action' value='stop'>  Stop / keep paired (or pair-only if currently unpaired)</label>"
              "<label style='flex:1'><input type='radio' name='action' value='unpair'> Unpair / forget</label>"
            "</div>"
            "<button type='submit' class='btn btn--success' style='margin-top:12px'>"
            "Apply &amp; send</button>"

            "</form>"
           "<div class='help'>"
            "If the node is unpaired, <em>Start</em> will attempt to pair then deploy. "
            "If it is deployed, <em>Stop</em> will revert it to the paired state. "
            "<em>Unpair</em> will forget this node on the mothership and tell the node to reset itself."
            "</div>");

  html += F("<a href='/nodes' class='btn' style='margin-top:12px'>↩️ Back to Node Manager</a>");
  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodeConfigSave() {
  String nodeId   = server.arg("node_id");   // firmware ID
  String userId   = server.arg("user_id");   // numeric
  String name     = server.arg("name");      // friendly name
  String action   = server.arg("action");
  int interval    = server.hasArg("interval") ? server.arg("interval").toInt() : gWakeIntervalMin;

  // --- 1) Persist numeric ID + name to NVS ---
  setNodeUserId(nodeId, userId);
  setNodeName(nodeId, name);

  // --- 2) Clamp interval ---
  bool intervalOk = false;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) {
      intervalOk = true;
      break;
    }
  }
  if (!intervalOk) interval = gWakeIntervalMin;

  // --- 3) Find node entry ---
  NodeInfo* target = nullptr;
  for (auto &n : registeredNodes) {
    if (n.nodeId == nodeId) {
      target = &n;
      break;
    }
  }

  // --- 4) Apply interval for this node only (via desired config snapshot) ---
  NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  bool cfgChanged = false;
  if (dc.wakeIntervalMin != (uint8_t)interval) {
    dc.wakeIntervalMin = (uint8_t)interval;
    cfgChanged = true;
  }
  if (dc.syncIntervalMin == 0) {
    dc.syncIntervalMin = (uint16_t)gSyncIntervalMin;
    cfgChanged = true;
  }
  if (dc.syncPhaseUnix == 0) {
    dc.syncPhaseUnix = getRTCTimeUnix();
    cfgChanged = true;
  }
  if (dc.configVersion == 0) {
    dc.configVersion = 1;
    cfgChanged = true;
  } else if (cfgChanged) {
    dc.configVersion = (uint8_t)max((int)dc.configVersion + 1, 1);
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

  if (finalUserId.isEmpty()) finalUserId = userId;
  if (finalName.isEmpty())   finalName   = name;

  // If NodeInfo has userId / name fields, keep them in sync
  if (target) {
    target->userId = finalUserId;
    target->name   = finalName;
  }

  // --- 7) Feedback page ---
  String html = headCommon("Configure Node");
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
            "<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodesPage() {
  String html = headCommon("Node Manager");
  const unsigned long nowMs = millis();
  auto allNodes = getRegisteredNodes();

  html += F("<div class='section'>"
            "<h3>🧩 Node Manager</h3>"
            "<p class='muted'>Each row shows only operational status: Next wake, Config status, and Awake state."
            " Auto-refresh: every 15 seconds.</p>");

  if (allNodes.empty()) {
    html += F("<p class='muted'>No nodes registered yet. Try discovering and pairing first.</p>");
  } else {
    html += F("<div class='list'>");

        for (auto &node : allNodes) {

      String userId = node.userId;
      String name   = node.name;
      if (userId.isEmpty()) userId = getNodeUserId(node.nodeId);
      if (name.isEmpty())   name   = getNodeName(node.nodeId);
      NodeDesiredConfig nodeDesired = getDesiredConfig(node.nodeId.c_str());
      uint8_t desiredCfgV = nodeDesired.configVersion;
      uint8_t appliedCfgV = node.configVersionApplied;
      bool cfgPending = (desiredCfgV > 0 && appliedCfgV < desiredCfgV);
      uint8_t observedWakeMin = (node.wakeIntervalMin > 0)
        ? node.wakeIntervalMin
        : node.inferredWakeIntervalMin;
      int nodeIntervalCurrentMin = (observedWakeMin > 0)
        ? observedWakeMin
        : gWakeIntervalMin;
      const String nextWake = computeNextWakeIsoLocal(nodeIntervalCurrentMin);
      const unsigned long ageSec = (nowMs >= node.lastSeen) ? ((nowMs - node.lastSeen) / 1000UL) : 0;

      if (cfgPending && node.state == DEPLOYED && !node.deployPending &&
          nodeDesired.wakeIntervalMin > 0 && observedWakeMin > 0 &&
          nodeDesired.wakeIntervalMin == observedWakeMin) {
        // Treat HELLO-reported wake interval match as applied config evidence when ACK is missed.
        cfgPending = false;
      }

      html += F("<a href='/node-config?node_id=");
      html += node.nodeId;
      html += F("' class='item'>"
                "<div class='item-row'>"
                  "<div>");

      // First line: Node ID (numeric if set) + firmware ID in muted text
      html += F("<strong>");
      if (userId.length()) {
        html += userId;
      } else {
        html += node.nodeId;
      }
      html += F("</strong>");

      html += F("<br><span class='muted'>FW: ");
      html += node.nodeId;
      html += F("</span>");

      // Name (if any)
      if (name.length()) {
        html += F("<br><span class='muted'>");
        html += name;
        html += F("</span>");
      }

      html += F("<br><div class='muted' style='font-size:.8rem;margin-top:4px'><strong>Next wake trigger:</strong> ");
      if (node.state == DEPLOYED) {
        if (observedWakeMin > 0) {
          html += nextWake;
        } else {
          html += F("Awaiting node check-in");
        }
      } else {
        html += F("After deploy (interval currently ");
        html += String(nodeIntervalCurrentMin);
        html += F(" min)");
      }
      html += F("</div>");

      html += F("<div class='muted' style='font-size:.8rem;margin-top:4px'>");
      if (node.state == DEPLOYED) {
        html += F("Sampling every ");
        html += String(nodeIntervalCurrentMin);
        html += F(" min. Last contact ");
        html += String(ageSec);
        html += F("s ago. ");
        if (cfgPending) {
          html += F("Config pending");
          if (nodeDesired.wakeIntervalMin > 0 && observedWakeMin > 0) {
            html += F(" (desired ");
            html += String((int)nodeDesired.wakeIntervalMin);
            html += F(" min, observed ");
            html += String((int)observedWakeMin);
            html += F(" min).");
          } else if (nodeDesired.wakeIntervalMin > 0) {
            html += F(" (desired ");
            html += String((int)nodeDesired.wakeIntervalMin);
            html += F(" min, waiting for apply evidence).");
          } else {
            html += F(" (waiting for apply evidence).");
          }
          html += F(" ");
        } else {
          html += F("Config is current. ");
        }
        html += node.isActive ? F("Node is awake now.") : F("Node is asleep between intervals.");
      } else if (node.state == PAIRED) {
        html += F("Paired and waiting for deploy. Configure interval and start when ready.");
      } else {
        html += F("Discovered but not paired yet.");
      }
      html += F("</div>");

      html += F("</div>"); // left column

      // Right column: only requested status chips
      html += F("<div style='text-align:right;min-width:150px'>");
      if (cfgPending) {
        html += F("<span class='chip chip--cfg-pending'>Config pending</span>");
      } else {
        html += F("<span class='chip chip--cfg-ok'>Config updated</span>");
      }
      html += F("<br>");
      if (node.isActive) {
        html += F("<span class='chip chip--link-awake'>Awake</span>");
      } else {
        html += F("<span class='chip chip--link-asleep'>Asleep</span>");
      }
      html += F("</div>"
                "</div>"   // .item-row
              "</a>");
    }


    html += F("</div>"); // .list
  }

  html += F("<a href='/' class='btn' style='margin-top:12px'>↩️ Back to Dashboard</a>");
  html += F("</div>"); // .section

  html += F("<script>setTimeout(function(){ location.reload(); }, 15000);</script>");

  html += footCommon();
  server.send(200, "text/html", html);
}


// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);

  Serial.println("Starting RTC setup...");
  setupRTC();

  Serial.println("Starting SD Card setup...");
  setupSD();

  Serial.println("Starting WiFi setup...");
#if ENABLE_WIFI_AP_WEBSERVER
  Serial.println("Starting WiFi AP (AP+STA mode)...");
  WiFi.mode(WIFI_AP_STA);
#else
  // Keep WiFi radio in STA mode for ESP-NOW, but disable AP/web server during BLE soak tests.
  WiFi.mode(WIFI_STA);
#endif
  // BLE + WiFi coexistence on ESP32-S3 requires modem sleep to be enabled.
  WiFi.setSleep(true);

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
  Serial.println("Starting ESP-NOW setup...");
  setupESPNOW();
  setSensorDataEventCallback(onBleSensorTelemetry);

  Serial.println("Starting BLE GATT setup...");
  bleSetup(DEVICE_ID, FW_VERSION, buildBleStatusDataJson, getRTCTimeUnix, handleBleCommand);

  char timeStr[32];
  getRTCTimeString(timeStr, sizeof(timeStr));
  Serial.print("Current RTC Time: "); Serial.println(timeStr);

  loadWakeIntervalFromNVS();
  loadSyncIntervalFromNVS();
  loadSyncModeFromNVS();
  loadDailySyncTimeFromNVS();
  Serial.printf("Current wake interval (from NVS): %d min\n", gWakeIntervalMin);
  Serial.printf("Current sync interval payload (from NVS): %d min\n", gSyncIntervalMin);
  Serial.printf("Current daily sync time (from NVS): %s\n", formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute).c_str());

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
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/set-sync-mode", HTTP_POST, handleSetSyncMode);
  server.on("/set-sync-interval", HTTP_POST, handleSetSyncInterval);
  server.on("/set-sync-time", HTTP_POST, handleSetSyncTime);

  // Node manager + config routes
  server.on("/nodes", HTTP_GET, handleNodesPage);
  server.on("/node-config", HTTP_GET, handleNodeConfigForm);
  server.on("/node-config", HTTP_POST, handleNodeConfigSave);
  server.on("/revert-node", HTTP_POST, handleRevertNode); // still available if you use it elsewhere

  server.begin();
  Serial.println("✅ Web server started!");
#else
  Serial.println("Web server disabled");
#endif
}

void loop() {
#if ENABLE_WIFI_AP_WEBSERVER
  server.handleClient();
#endif
  espnow_loop(); // Handle ESP-NOW node management
  bleLoop();

  // Global sync trigger driven by selected sync mode.
  {
    const uint32_t nowUnix = getRTCTimeUnix();
    if (nowUnix > 946684800UL) {
    if (gSyncMode == SYNC_MODE_DAILY) {
      const long epochDay = (long)(nowUnix / 86400UL);
      DateTime now(nowUnix);
      if (now.hour() == gSyncDailyHour && now.minute() == gSyncDailyMinute && gLastSyncBroadcastEpochDay != epochDay) {
        const bool sent = broadcastSyncSchedule(gSyncIntervalMin, nowUnix);
        gLastSyncBroadcastEpochDay = epochDay;
        gLastSyncBroadcastUnix = nowUnix;
        gLastSyncBroadcastMs = millis();
        gSyncTriggerCount++;
        Serial.printf("[SYNC] Daily trigger %02d:%02d -> broadcast %s\n",
                      gSyncDailyHour,
                      gSyncDailyMinute,
                      sent ? "SENT" : "NOT_SENT");
      }
    } else {
      const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
      const long long slot = (long long)(nowUnix / periodSec);
      const bool spacedOk = (gLastSyncBroadcastUnix == 0) || (nowUnix >= (gLastSyncBroadcastUnix + periodSec));
      if ((gLastSyncIntervalSlot < 0 || slot > gLastSyncIntervalSlot) && spacedOk) {
        const bool sent = broadcastSyncSchedule(gSyncIntervalMin, nowUnix);
        gLastSyncBroadcastMs = millis();
        gLastSyncBroadcastUnix = nowUnix;
        gLastSyncIntervalSlot = slot;
        gSyncTriggerCount++;
        Serial.printf("[SYNC] Interval trigger %d min -> broadcast %s\n",
                      gSyncIntervalMin,
                      sent ? "SENT" : "NOT_SENT");
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
