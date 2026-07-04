#include "storage/json_payload.h"

// ---------------------------------------------------------------------------
// CSV column index -> Supabase batch field key
// ---------------------------------------------------------------------------
// The Supabase ingest endpoint's canonical batch schema uses field names that
// are IDENTICAL to our CSV column headers (camelCase, with spectral_NNN keeping
// underscores), so the mapping is 1:1 — no renaming. project_id / mothership_id
// are NOT sent (the backend derives them from the device API key).
//
// CSV order (kUploadCSVHeader / flash_logger.cpp logDecodedSnapshot()):
//   0 datetime  1 nodeId  2 seqNum  3 sensorPresent  4 qualityFlags
//   5 configVersion  6 batVoltage  7 airTemp  8 airHumidity
//   9-16 spectral_415..spectral_680  17 windSpeed  18 windDir
//   19 soil1Vwc 20 soil1Temp 21 soil2Vwc 22 soil2Temp  23 aux1  24 aux2
//   25 spectral_clear 26 spectral_nir 27 spectral_gain
//   28 spectral_integration_ms 29 spectral_saturated

static constexpr int kNumCsvColumns = 30;
static constexpr uint16_t kMaxReadingsPerPost = 100;  // backend hard limit

enum CellType {
  CELL_TIMESTAMP,    // ISO 8601 string; CSV omits trailing Z so we append it;
                     // null if missing/"unknown"
  CELL_STRING,       // JSON string
  CELL_INT,          // integer literal straight from the CSV cell
  CELL_INT_BASE0,    // parse decimal or 0x hex, emit as decimal integer
  CELL_NUM_NULLABLE, // numeric literal, or JSON null when the cell is nan/empty
};

struct ColumnMapping {
  int         index;
  const char* key;
  CellType    type;
};

static const ColumnMapping kColumnMappings[kNumCsvColumns] = {
  {0,  "datetime",       CELL_TIMESTAMP},
  {1,  "nodeId",         CELL_STRING},
  {2,  "seqNum",         CELL_INT},
  {3,  "sensorPresent",  CELL_INT_BASE0},
  {4,  "qualityFlags",   CELL_INT_BASE0},
  {5,  "configVersion",  CELL_INT},
  {6,  "batVoltage",     CELL_NUM_NULLABLE},
  {7,  "airTemp",        CELL_NUM_NULLABLE},
  {8,  "airHumidity",    CELL_NUM_NULLABLE},
  {9,  "spectral_415",   CELL_NUM_NULLABLE},
  {10, "spectral_445",   CELL_NUM_NULLABLE},
  {11, "spectral_480",   CELL_NUM_NULLABLE},
  {12, "spectral_515",   CELL_NUM_NULLABLE},
  {13, "spectral_555",   CELL_NUM_NULLABLE},
  {14, "spectral_590",   CELL_NUM_NULLABLE},
  {15, "spectral_630",   CELL_NUM_NULLABLE},
  {16, "spectral_680",   CELL_NUM_NULLABLE},
  {17, "windSpeed",      CELL_NUM_NULLABLE},
  {18, "windDir",        CELL_NUM_NULLABLE},
  {19, "soil1Vwc",       CELL_NUM_NULLABLE},
  {20, "soil1Temp",      CELL_NUM_NULLABLE},
  {21, "soil2Vwc",       CELL_NUM_NULLABLE},
  {22, "soil2Temp",      CELL_NUM_NULLABLE},
  {23, "aux1",           CELL_NUM_NULLABLE},
  {24, "aux2",           CELL_NUM_NULLABLE},
  {25, "spectral_clear",          CELL_NUM_NULLABLE},
  {26, "spectral_nir",            CELL_NUM_NULLABLE},
  {27, "spectral_gain",           CELL_NUM_NULLABLE},
  {28, "spectral_integration_ms", CELL_NUM_NULLABLE},
  {29, "spectral_saturated",      CELL_NUM_NULLABLE},
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isNanCell(const String& v) {
  if (v.length() == 0) return true;
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

static bool appendReadingObject(String& json, const String& line, bool& first,
                                const String& fallbackIso) {
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

    if (!firstKey) json += ",";
    firstKey = false;
    json += "\"";
    json += m.key;
    json += "\":";

    switch (m.type) {
      case CELL_TIMESTAMP:
        // CSV stores ISO 8601 UTC without the trailing Z (gmtime); the backend
        // wants the Z suffix. The backend REJECTS a null datetime (400), so a
        // row with no node timestamp (logged as "unknown") falls back to the
        // mothership RTC time instead of null.
        if (val.length() >= 10 && isDigit(val[0]) && val.indexOf('T') > 0) {
          json += "\"";
          json += escapeJsonString(val);
          json += "Z\"";
        } else if (fallbackIso.length() > 0) {
          json += "\"";
          json += escapeJsonString(fallbackIso);  // already ISO-8601 + Z
          json += "\"";
        } else {
          json += "null";  // RTC also unset — last resort
        }
        break;
      case CELL_STRING:
        json += "\"";
        json += escapeJsonString(val);
        json += "\"";
        break;
      case CELL_INT_BASE0: {
        unsigned long parsed = strtoul(val.c_str(), nullptr, 0);
        json += String((unsigned int)(parsed & 0xFFFFU));
        break;
      }
      case CELL_INT:
        json += (val.length() > 0) ? val : String("0");
        break;
      case CELL_NUM_NULLABLE:
        if (isNanCell(val)) json += "null";
        else json += val;  // CSV already holds a valid numeric literal
        break;
    }
  }

  json += "}";
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

JsonPayload buildJsonUpload(const String& csvChunk,
                            uint16_t maxReadings,
                            const String& fwVersion,
                            const StatusContext* status,
                            uint32_t rtcFallbackUnix) {
  JsonPayload result;
  result.body = String();
  result.byteLength = 0;
  result.rowCount = 0;
  result.ok = false;
  result.csvBytesConsumed = 0;

  if (maxReadings == 0 || maxReadings > kMaxReadingsPerPost) {
    maxReadings = kMaxReadingsPerPost;  // respect the backend's 100/POST limit
  }

  // Pre-format the RTC fallback timestamp (ISO-8601 + Z) used for rows whose
  // own datetime is missing — the backend rejects null datetimes.
  String fallbackIso;
  if (rtcFallbackUnix > 946684800UL) {
    time_t t = (time_t)rtcFallbackUnix;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char tb[24];
    snprintf(tb, sizeof(tb), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    fallbackIso = tb;
  }

  // Each reading object carries every field (null for missing sensors), so
  // budget ~600 bytes/reading plus a small wrapper.
  const uint32_t estBytes = (uint32_t)maxReadings * 600U + 256U;
  if (ESP.getFreeHeap() < estBytes + 8192U) {
    Serial.printf("[JSON] Insufficient heap for build (est=%u free=%u)\n",
                  (unsigned)estBytes, (unsigned)ESP.getFreeHeap());
    return result;
  }

  String readings;
  if (!readings.reserve((uint32_t)maxReadings * 560U + 32U)) {
    Serial.println("[JSON] readings reserve failed");
    return result;
  }

  // Walk the CSV chunk line by line.  Line 0 is the header — skip it.  Parse up
  // to maxReadings data rows; record the byte offset of the first un-parsed row
  // so the caller can advance the cursor precisely. Malformed rows are skipped
  // (still consumed so the cursor moves past them).
  uint16_t emitted = 0;
  uint32_t consumed = 0;
  bool firstReading = true;
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
        // Row cap reached — leave this row for the next chunk.
        break;
      }
      if (appendReadingObject(readings, line, firstReading, fallbackIso)) {
        emitted++;
      }
    }
    consumed = (nl >= 0) ? (uint32_t)(nl + 1) : (uint32_t)len;
    lineStart = (nl >= 0) ? (nl + 1) : len;
    if (lineNo == 0) {
      // The header line is already accounted for by the cursor's startOffset
      // (which points past the header). csvBytesConsumed must count only
      // data-row bytes so the cursor advances by the exact bytes uploaded.
      consumed = 0;
    }
    lineNo++;
  }

  result.rowCount = emitted;
  result.csvBytesConsumed = consumed;

  // Wrap readings in the canonical batch document:
  //   {"readings":[ ... ], "meta":{...}, "status":{...}}
  // No deviceId in meta (any value -> 403 "different device"); the device and
  // project are derived from the API key. status{} is included when the
  // caller provides a StatusContext (every POST from handleSyncWake and
  // handleManualUpload).
  String& body = result.body;
  // Nested status object: scalar fields (~900B) + the pre-built nodes[] and
  // transmission{} strings, whose size scales with the fleet.
  const uint32_t statusEst = status
      ? 900U + status->nodesJson.length() + status->transmissionJson.length()
        + status->modemJson.length() + status->diagnosticsJson.length()
      : 0U;
  body.reserve(readings.length() + fwVersion.length() + 160U + statusEst);
  body += "{\"readings\":[";
  body += readings;
  body += "],\"meta\":{\"firmwareVersion\":\"";
  body += escapeJsonString(fwVersion);
  body += "\"";
  if (status) {
    // NOTE: deviceId must NOT go in meta — the ingest function rejects any
    // meta.deviceId with 403 "Credential is registered to a different device"
    // (verified via dry-run curl). The device is derived from the API key, and
    // deviceId is carried in the status object below instead. The rest of meta
    // (firmwareBuild, uploadTimeUnix, uploadReason) is accepted and feeds the
    // sync_sessions / mothership_status rows.
    body += ",\"firmwareBuild\":\"";
    body += escapeJsonString(status->firmwareBuild ? status->firmwareBuild : "");
    body += "\",\"uploadTimeUnix\":";
    body += String(status->rtcUnix);
    body += ",\"uploadReason\":\"";
    body += escapeJsonString(status->uploadReason);
    body += "\"";
  }
  body += "}";
  if (status) {
    // Nested structure required by the backend RPC schema:
    //   status.{batVoltage, rtcUnix, deviceId, wake/sync minutes, syncMode,
    //           nextSyncLocal, fleet{...}, upload{...}, dataLog{...}}
    char numBuf[24];
    body += ",\"status\":{\"batVoltage\":";
    // Guard NaN — dtostrf would emit "nan", which is invalid JSON (400).
    if (isnan(status->batVoltage)) { body += "null"; }
    else { dtostrf(status->batVoltage, 1, 2, numBuf); body += numBuf; }
    body += ",\"rtcUnix\":"; body += String(status->rtcUnix);
    body += ",\"deviceId\":\""; body += escapeJsonString(status->deviceId); body += "\"";
    body += ",\"wakeIntervalMinutes\":"; body += String(status->wakeIntervalMinutes);
    body += ",\"syncIntervalMinutes\":"; body += String(status->syncIntervalMinutes);
    body += ",\"syncMode\":\""; body += escapeJsonString(status->syncMode); body += "\"";
    body += ",\"syncDailyTime\":\""; body += escapeJsonString(status->syncDailyTime); body += "\"";
    body += ",\"nextSyncLocal\":\""; body += escapeJsonString(status->nextSyncLocal); body += "\"";

    // status.nodes[] — pre-built by the caller (node_registry::buildNodesStatusJson).
    // This is what populates the backend "nodes" table.
    body += ",\"nodes\":";
    body += status->nodesJson.length() ? status->nodesJson : String("[]");

    body += ",\"fleet\":{\"total\":"; body += String((unsigned)status->fleetTotal);
    body += ",\"deployed\":"; body += String((unsigned)status->fleetDeployed);
    body += ",\"paired\":"; body += String((unsigned)status->fleetPaired);
    body += ",\"unpaired\":"; body += String((unsigned)status->fleetUnpaired);
    body += ",\"pending\":"; body += String((unsigned)status->fleetPending);
    body += ",\"paused\":"; body += String((unsigned)status->fleetPaused);
    body += "}";

    // Flash fields stay nested in upload.* (backend-confirmed shape, not a
    // separate status.flash{}).
    body += ",\"upload\":{\"flashUsagePct\":"; body += String(status->flashUsagePct);
    body += ",\"flashTotalBytes\":"; body += String((unsigned long)status->flashTotalBytes);
    body += ",\"flashUsedBytes\":"; body += String((unsigned long)status->flashUsedBytes);
    body += ",\"pendingBytes\":"; body += String(status->pendingBytes);
    body += ",\"pendingRows\":"; body += String(status->pendingRows);
    body += ",\"rowsUploaded\":"; body += String(status->rowsUploaded);
    body += ",\"retryCount\":"; body += String((unsigned)status->retryCount);
    body += ",\"lastUploadUnix\":"; body += String(status->lastUploadUnix);
    body += ",\"enabled\":"; body += status->uploadEnabled ? "true" : "false";
    body += "}";

    body += ",\"dataLog\":{\"records\":"; body += String(status->dataLogRecords);
    body += ",\"csvSizeBytes\":"; body += String((unsigned long)status->dataLogCsvBytes);
    body += ",\"lastConfirmedSync\":\""; body += escapeJsonString(status->dataLogLastSync); body += "\"";
    body += "}";

    body += ",\"projectStarted\":"; body += String(status->projectStartedUnix);
    body += ",\"lastUploadResult\":\"";
    body += escapeJsonString(status->lastUploadResult ? status->lastUploadResult : "");
    body += "\"";

    // status.transmission{} — pre-built (no secrets). Creates/updates the
    // backend mothership_config row when present.
    if (status->transmissionJson.length()) {
      body += ",\"transmission\":";
      body += status->transmissionJson;
    }

    // status.modem{} — LTE link quality + modem identity (this session).
    if (status->modemJson.length()) {
      body += ",\"modem\":";
      body += status->modemJson;
    }

    // status.diagnostics{} — mothership system health (reset reason, boot
    // count, heap, snapshot-queue drops, session timing).
    if (status->diagnosticsJson.length()) {
      body += ",\"diagnostics\":";
      body += status->diagnosticsJson;
    }

    body += "}";
  }
  body += "}";

  result.byteLength = body.length();
  result.ok = true;
  return result;
}
