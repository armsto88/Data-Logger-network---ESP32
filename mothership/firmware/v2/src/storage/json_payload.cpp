#include "storage/json_payload.h"
#include "storage/flash_logger.h"  // flashIsReady()
#include "config/node_registry.h"
#include "config/config_server.h"  // SYNC_MODE_INTERVAL / SYNC_MODE_DAILY

#include <LittleFS.h>
#include <vector>

// ---------------------------------------------------------------------------
// CSV column index → JSON key mapping
// ---------------------------------------------------------------------------
// Order is fixed by kUploadCSVHeader / flash_logger.cpp logDecodedSnapshot():
//   0  datetime          (string, quoted)
//   1  nodeId            (string, quoted)
//   2  seqNum            (uint32, unquoted)
//   3  sensorPresent     (hex string "0x%04X")
//   4  qualityFlags      (hex string "0x%04X")
//   5  configVersion     (uint16, unquoted)
//   6  batVoltage        (float, omit if nan/empty)
//   7  airTemp           (float, omit if nan/empty)
//   8  airHumidity       (float, omit if nan/empty)
//   9  spectral_415      (float, omit if nan/empty)
//   10 spectral_445      (float, omit if nan/empty)
//   11 spectral_480      (float, omit if nan/empty)
//   12 spectral_515      (float, omit if nan/empty)
//   13 spectral_555      (float, omit if nan/empty)
//   14 spectral_590      (float, omit if nan/empty)
//   15 spectral_630      (float, omit if nan/empty)
//   16 spectral_680      (float, omit if nan/empty)
//   17 windSpeed         (float, omit if nan/empty)
//   18 windDir           (float, omit if nan/empty)
//   19 soil1Vwc          (float, omit if nan/empty)
//   20 soil1Temp         (float, omit if nan/empty)
//   21 soil2Vwc          (float, omit if nan/empty)
//   22 soil2Temp         (float, omit if nan/empty)
//   23 aux1              (float, omit if nan/empty)
//   24 aux2              (float, omit if nan/empty)

static constexpr int kNumCsvColumns = 25;

struct ColumnMapping {
  int         index;
  const char* key;
  bool        quoted;   // true = emit as JSON string
  bool        hex;      // true = format as "0x%04X" string
  bool        omitOnNan; // true = omit key when value is nan/empty
};

static const ColumnMapping kColumnMappings[kNumCsvColumns] = {
  {0,  "datetime",        true,  false, false},
  {1,  "nodeId",          true,  false, false},
  {2,  "seqNum",          false, false, false},
  {3,  "sensorPresent",   true,  true,  false},
  {4,  "qualityFlags",    true,  true,  false},
  {5,  "configVersion",   false, false, false},
  {6,  "batVoltage",      false, false, true},
  {7,  "airTemp",         false, false, true},
  {8,  "airHumidity",     false, false, true},
  {9,  "spectral_415",    false, false, true},
  {10, "spectral_445",    false, false, true},
  {11, "spectral_480",    false, false, true},
  {12, "spectral_515",    false, false, true},
  {13, "spectral_555",    false, false, true},
  {14, "spectral_590",    false, false, true},
  {15, "spectral_630",    false, false, true},
  {16, "spectral_680",    false, false, true},
  {17, "windSpeed",       false, false, true},
  {18, "windDir",         false, false, true},
  {19, "soil1Vwc",        false, false, true},
  {20, "soil1Temp",       false, false, true},
  {21, "soil2Vwc",        false, false, true},
  {22, "soil2Temp",       false, false, true},
  {23, "aux1",            false, false, true},
  {24, "aux2",            false, false, true},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isNanCell(const String& v) {
  if (v.length() == 0) return true;
  // Case-insensitive compare to "nan"
  if (v.length() != 3) return false;
  return (v[0] == 'n' || v[0] == 'N') &&
         (v[1] == 'a' || v[1] == 'A') &&
         (v[2] == 'n' || v[2] == 'N');
}

static String escapeJsonString(const String& v) {
  String out;
  out.reserve(v.length() + 8);
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') { out += "\\n"; }
    else if (c == '\r') { out += "\\r"; }
    else if (c == '\t') { out += "\\t"; }
    else { out += c; }
  }
  return out;
}

// Split one CSV row line into fields by comma.  Returns field count (0 on
// empty line).  fields[] is filled up to maxFields.
static int splitCsvRow(const String& line, String* fields, int maxFields) {
  if (line.length() == 0) return 0;
  int count = 0;
  int start = 0;
  while (count < maxFields) {
    int comma = line.indexOf(',', start);
    if (comma < 0) {
      fields[count++] = line.substring(start);
      break;
    }
    fields[count++] = line.substring(start, comma);
    start = comma + 1;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Reading object builder — emits one JSON object for one CSV row.
// Returns true on success, false on malformed row (skipped by caller).
// ---------------------------------------------------------------------------

static bool appendReadingObject(String& json, const String& line, bool& first) {
  String fields[kNumCsvColumns];
  const int n = splitCsvRow(line, fields, kNumCsvColumns);
  if (n < 6) {
    Serial.printf("[JSON] Skipping malformed CSV row (%d fields): %.60s\n",
                  n, line.c_str());
    return false;
  }

  if (!first) json += ",";
  first = false;
  json += "{";

  bool firstKey = true;
  for (int i = 0; i < kNumCsvColumns; ++i) {
    const ColumnMapping& m = kColumnMappings[i];
    if (m.index >= n) break;  // row shorter than expected — stop here
    const String& val = fields[m.index];

    if (m.omitOnNan && isNanCell(val)) continue;

    if (!firstKey) json += ",";
    firstKey = false;

    json += "\"";
    json += m.key;
    json += "\":";

    if (m.hex) {
      // Parse CSV cell as uint16 (auto-detect base: decimal or 0x hex),
      // emit as a decimal integer — JSON has no hex literal type.
      unsigned long parsed = strtoul(val.c_str(), nullptr, 0);
      json += String((unsigned int)(parsed & 0xFFFFU));
    } else if (m.quoted) {
      json += "\"";
      json += escapeJsonString(val);
      json += "\"";
    } else {
      // Numeric — emit unquoted.  For integer fields this is fine; for
      // floats the CSV already contains a valid numeric literal.
      json += val;
    }
  }

  json += "}";
  return true;
}

// ---------------------------------------------------------------------------
// Meta section
// ---------------------------------------------------------------------------

static void appendMetaSection(String& json, const JsonUploadContext& ctx,
                              uint16_t rowCount, bool hasStatus) {
  json += "\"meta\":{";
  json += "\"deviceId\":\"";
  json += escapeJsonString(String(ctx.deviceId ? ctx.deviceId : ""));
  json += "\",\"firmwareVersion\":\"";
  json += escapeJsonString(String(ctx.fwVersion ? ctx.fwVersion : ""));
  json += "\",\"firmwareBuild\":\"";
  json += escapeJsonString(String(ctx.fwBuild ? ctx.fwBuild : ""));
  json += "\",\"uploadTimeUnix\":";
  json += String(ctx.uploadTimeUnix);
  json += ",\"uploadReason\":\"";
  json += escapeJsonString(String(ctx.uploadReason ? ctx.uploadReason : "scheduled"));
  json += "\",\"format\":\"json\"";
  json += ",\"csvRowsIncluded\":";
  json += String((unsigned)rowCount);
  if (hasStatus) {
    json += ",\"includesStatus\":true";
  } else {
    json += ",\"includesStatus\":false";
  }
  json += "}";
}

// ---------------------------------------------------------------------------
// Fleet + nodes section
// ---------------------------------------------------------------------------

static const char* nodeStateToString(int s) {
  switch (s) {
    case UNPAIRED: return "UNPAIRED";
    case PAIRED:   return "PAIRED";
    case DEPLOYED: return "DEPLOYED";
    default:       return "UNKNOWN";
  }
}

static const char* pendingStateToString(NodePendingState s) {
  switch (s) {
    case PENDING_NONE:        return "NONE";
    case PENDING_TO_UNPAIRED: return "UNPAIRED";
    case PENDING_TO_PAIRED:   return "PAIRED";
    case PENDING_TO_DEPLOYED: return "DEPLOYED";
    default:                  return "NONE";
  }
}

static String formatMac(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

static void appendFleetSection(String& json, const std::vector<NodeInfo>& allNodes) {
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

  json += "\"fleet\":{";
  json += "\"total\":";       json += String((unsigned)allNodes.size());
  json += ",\"unpaired\":";   json += String((unsigned)unpairedCount);
  json += ",\"paired\":";     json += String((unsigned)pairedCount);
  json += ",\"deployed\":";   json += String((unsigned)deployedCount);
  json += ",\"pending\":";    json += String((unsigned)pendingCount);
  json += ",\"pendingToPaired\":";   json += String((unsigned)pendingToPairedCount);
  json += ",\"pendingToUnpaired\":"; json += String((unsigned)pendingToUnpairedCount);
  json += "}";
}

static void appendNodesArray(String& json, const std::vector<NodeInfo>& allNodes,
                             uint32_t nowUnix) {
  json += ",\"nodes\":[";
  bool first = true;
  for (const auto& n : allNodes) {
    if (!first) json += ",";
    first = false;
    json += "{";
    json += "\"nodeId\":\"";       json += escapeJsonString(n.nodeId); json += "\"";
    json += ",\"userId\":\"";      json += escapeJsonString(getNodeUserId(n.nodeId)); json += "\"";
    json += ",\"name\":\"";        json += escapeJsonString(getNodeName(n.nodeId)); json += "\"";
    json += ",\"mac\":\"";         json += formatMac(n.mac); json += "\"";
    json += ",\"state\":\"";       json += nodeStateToString(n.state); json += "\"";
    if (n.deployedSinceUnix > 0) {
      json += ",\"deployedSinceUnix\":"; json += String(n.deployedSinceUnix);
    }
    // lastSeenUnix: approximate from millis()-based lastSeen.
    if (n.lastSeen > 0 && nowUnix > 0) {
      const uint32_t elapsedSec = (millis() - n.lastSeen) / 1000UL;
      const uint32_t lastSeenUnix = (elapsedSec < nowUnix) ? (nowUnix - elapsedSec) : 0;
      json += ",\"lastSeenUnix\":"; json += String(lastSeenUnix);
    } else {
      json += ",\"lastSeenUnix\":0";
    }
    json += ",\"wakeIntervalMin\":";            json += String((unsigned)n.wakeIntervalMin);
    json += ",\"inferredWakeIntervalMin\":";    json += String((unsigned)n.inferredWakeIntervalMin);
    json += ",\"lastReportedBatV\":";
    if (isnan(n.lastReportedBatV)) {
      json += "null";
    } else {
      char b[16];
      snprintf(b, sizeof(b), "%.2f", n.lastReportedBatV);
      json += b;
    }
    json += ",\"configVersion\":";              json += String((unsigned)n.configVersionApplied);
    json += ",\"notes\":\"";                    json += escapeJsonString(getNodeNotes(n.nodeId)); json += "\"";
    if (!isnan(n.latitude) && !isnan(n.longitude)) {
      json += ",\"latitude\":";  json += String(n.latitude, 6);
      json += ",\"longitude\":"; json += String(n.longitude, 6);
    }
    json += ",\"isActive\":";                   json += (n.isActive ? "true" : "false");
    json += ",\"deployPending\":";              json += (n.deployPending ? "true" : "false");
    json += ",\"stateChangePending\":";         json += (n.stateChangePending ? "true" : "false");
    json += ",\"pendingTargetState\":\"";       json += pendingStateToString(n.pendingTargetState); json += "\"";
    json += "}";
  }
  json += "]";
}

// ---------------------------------------------------------------------------
// Status section
// ---------------------------------------------------------------------------

static void appendStatusSection(String& json, const JsonUploadContext& ctx) {
  json += "\"status\":{";

  // Top-level sync schedule fields
  json += "\"rtcUnix\":";               json += String(ctx.uploadTimeUnix);
  json += ",\"wakeIntervalMinutes\":";  json += String(ctx.wakeIntervalMin);
  json += ",\"syncIntervalMinutes\":";  json += String(ctx.syncIntervalMin);
  json += ",\"syncMode\":\"";
  json += (ctx.syncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily";
  json += "\",\"syncDailyTime\":\"";    json += escapeJsonString(ctx.syncDailyTimeHHMM); json += "\"";
  json += ",\"nextSyncLocal\":\"";      json += escapeJsonString(ctx.nextSyncLocalIso); json += "\"";

  // Dashboard status fields
  json += ",\"batVoltage\":";          json += String(ctx.batVoltage, 2);

  json += ",\"projectStarted\":";       json += String(ctx.projectStartedUnix);

  json += ",\"lastUploadResult\":\"";  json += escapeJsonString(ctx.lastUploadResult ? ctx.lastUploadResult : "pending"); json += "\"";

  // Fleet
  auto allNodes = getRegisteredNodes();
  json += ",";
  appendFleetSection(json, allNodes);

  // Upload subsystem
  const uint32_t flashUsagePct = (ctx.flashTotalBytes > 0)
    ? (uint32_t)((ctx.flashUsedBytes * 100ULL) / ctx.flashTotalBytes) : 0;
  json += ",\"upload\":{";
  json += "\"enabled\":";          json += (ctx.tx.enabled ? "true" : "false");
  json += ",\"pendingBytes\":";    json += String(ctx.pendingBytes);
  json += ",\"pendingRows\":";     json += String(ctx.pendingRows);
  json += ",\"rowsUploaded\":";    json += String(ctx.cursor.rowsUploaded);
  json += ",\"lastUploadUnix\":";  json += String(ctx.cursor.lastUploadUnix);
  json += ",\"retryCount\":";      json += String((unsigned)ctx.cursor.retryCount);
  json += ",\"flashUsagePct\":";   json += String(flashUsagePct);
  json += ",\"flashTotalBytes\":"; json += String((unsigned long)ctx.flashTotalBytes);
  json += ",\"flashUsedBytes\":";  json += String((unsigned long)ctx.flashUsedBytes);
  json += "}";

  // Transmission settings
  json += ",\"transmission\":{";
  json += "\"enabled\":";                json += (ctx.tx.enabled ? "true" : "false");
  json += ",\"endpointUrl\":\"";         json += escapeJsonString(ctx.tx.endpointUrl); json += "\"";
  json += ",\"siteId\":\"";              json += escapeJsonString(ctx.tx.siteId); json += "\"";
  json += ",\"deploymentId\":\"";        json += escapeJsonString(ctx.tx.deploymentId); json += "\"";
  json += ",\"uploadIntervalMin\":";     json += String(ctx.tx.uploadIntervalMin);
  json += ",\"minBatteryMv\":";          json += String(ctx.tx.minBatteryMv);
  json += ",\"maxBytesPerSession\":";    json += String(ctx.tx.maxBytesPerSession);
  json += ",\"maxRetriesPerWindow\":";   json += String((unsigned)ctx.tx.maxRetriesPerWindow);
  json += ",\"allowManualUpload\":";     json += (ctx.tx.allowManualUpload ? "true" : "false");
  json += ",\"useJsonUpload\":";         json += (ctx.tx.useJsonUpload ? "true" : "false");
  json += "}";

  // DataLog
  json += ",\"dataLog\":{";
  json += "\"records\":";          json += String(ctx.dataLogRecords);
  json += ",\"csvSizeBytes\":";    json += String((unsigned long)ctx.dataLogCsvBytes);
  json += ",\"lastConfirmedSync\":\""; json += escapeJsonString(ctx.lastConfirmedSyncIso); json += "\"";
  json += "}";

  // Nodes array
  appendNodesArray(json, allNodes, ctx.uploadTimeUnix);

  json += "}";
}

// ---------------------------------------------------------------------------
// DataLog stats helper — reads /datalog.csv for record count + last row datetime.
// Called by main.cpp via readDataLogStats() declared in json_payload.h.
// ---------------------------------------------------------------------------

DataLogStats readDataLogStats() {
  DataLogStats s;
  if (!flashIsReady() || !LittleFS.exists("/datalog.csv")) return s;
  File f = LittleFS.open("/datalog.csv", "r");
  if (!f) return s;
  s.csvBytes = (uint64_t)f.size();
  uint32_t lineNo = 0;
  String lastDataLine;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    lineNo++;
    if (lineNo == 1) continue;  // header
    s.records++;
    lastDataLine = line;
  }
  f.close();
  if (lastDataLine.length() > 0) {
    const int comma = lastDataLine.indexOf(',');
    if (comma > 0) {
      s.lastConfirmedSyncIso = lastDataLine.substring(0, comma);
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

JsonPayload buildJsonUpload(const JsonUploadContext& ctx,
                            const String& csvChunk,
                            uint16_t maxReadings,
                            bool includeStatus) {
  JsonPayload result;
  result.body = String();
  result.byteLength = 0;
  result.rowCount = 0;
  result.ok = false;
  result.csvBytesConsumed = 0;

  // Walk the CSV chunk line by line.  Line 0 is the header — skip it.
  // Parse up to maxReadings data rows; record the byte offset of the
  // first un-parsed row so the caller can advance the cursor precisely.
  uint16_t emitted = 0;
  uint32_t consumed = 0;
  bool firstReading = true;

  // Pre-scan to estimate size for reserve().
  // Estimate: ~250 bytes per reading + 2048 status overhead.
  const uint32_t estBytes = (uint32_t)maxReadings * 250U + 2048U;
  if (ESP.getFreeHeap() < estBytes + 8192U) {
    Serial.printf("[JSON] Insufficient heap for build (est=%u free=%u)\n",
                  (unsigned)estBytes, (unsigned)ESP.getFreeHeap());
    return result;
  }

  String readings;
  if (!readings.reserve((uint32_t)maxReadings * 220U + 32U)) {
    Serial.println("[JSON] readings reserve failed");
    return result;
  }

  int lineStart = 0;
  int lineNo = 0;
  const int len = csvChunk.length();
  while (lineStart < len) {
    int nl = csvChunk.indexOf('\n', lineStart);
    int lineEnd = (nl >= 0) ? nl : len;
    String line = csvChunk.substring(lineStart, lineEnd);
    line.trim();

    if (lineNo > 0 && line.length() > 0) {
      if (emitted >= maxReadings) {
        // Reached the row cap.  Do not consume this row — leave it for
        // the next chunk.  csvBytesConsumed points at lineStart.
        break;
      }
      if (appendReadingObject(readings, line, firstReading)) {
        emitted++;
      }
    }
    consumed = (nl >= 0) ? (uint32_t)(nl + 1) : (uint32_t)len;
    lineStart = (nl >= 0) ? (nl + 1) : len;
    if (lineNo == 0) {
      // The header line is already accounted for by the cursor's
      // startOffset (which points past the header).  csvBytesConsumed
      // must count only data-row bytes so the cursor advances by the
      // exact bytes uploaded, otherwise rows are permanently skipped.
      consumed = 0;
    }
    lineNo++;
  }

  result.rowCount = emitted;
  result.csvBytesConsumed = consumed;

  // Build the full document.
  String& body = result.body;
  body.reserve(estBytes + 256U);
  body += "{";
  appendMetaSection(body, ctx, emitted, includeStatus);
  if (includeStatus) {
    body += ",";
    appendStatusSection(body, ctx);
  }
  body += ",\"readings\":[";
  body += readings;
  body += "]}";

  result.byteLength = body.length();
  result.ok = true;
  return result;
}

JsonPayload buildJsonStatusOnly(const JsonUploadContext& ctx) {
  JsonPayload result;
  result.body = String();
  result.byteLength = 0;
  result.rowCount = 0;
  result.ok = false;
  result.csvBytesConsumed = 0;

  if (ESP.getFreeHeap() < 4096U) {
    Serial.println("[JSON] Insufficient heap for status-only build");
    return result;
  }

  String& body = result.body;
  body.reserve(2048U);
  body += "{";
  appendMetaSection(body, ctx, 0, true);
  body += ",";
  appendStatusSection(body, ctx);
  body += ",\"readings\":[]}";
  result.byteLength = body.length();
  result.ok = true;
  return result;
}