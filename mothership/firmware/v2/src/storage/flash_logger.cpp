#include "storage/flash_logger.h"
#include "storage/csv_schema.h"
#include "config/node_registry.h"
#include "protocol.h"
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

static const char* kFlashFile = "/datalog.csv";

// Row scratch buffer for every CSV builder below. A 35-column row with all 24
// sensors populated, a 32-character node name and full-width coordinates
// measures ~443 bytes, so 512 left barely any headroom: a node reporting
// legitimately wide values would overflow and have its row REJECTED, which is
// silent data loss. 640 removes that cliff for ~128 bytes of stack.
static constexpr size_t kCsvRowBufBytes = 640;

// Bounded append shared by every row builder below.
//
// snprintf() returns the length it WOULD have written, so the running offset in
// the builders deliberately keeps growing past the end of the row buffer — that
// is how they detect a row that did not fit. The hazard is what the NEXT call
// then computes: `buf + offset` points past the array, and `bufSize - offset`
// is size_t arithmetic that WRAPS to ~4 GB rather than going negative, so the
// bounds argument that should clamp the write instead authorises an unbounded
// one straight through the stack frame.
//
// Once the offset has reached bufSize there is nothing left to write, so skip
// the call entirely and return 0. The caller's total stays >= bufSize, which is
// exactly what its "did this row fit" check tests — overflow is still detected,
// it just can no longer corrupt the stack on the way there.
static int appendFmt(char* buf, size_t bufSize, int offset, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int appendFmt(char* buf, size_t bufSize, int offset, const char* fmt, ...) {
  if (offset < 0 || (size_t)offset >= bufSize) return 0;
  va_list ap;
  va_start(ap, fmt);
  const int need = vsnprintf(buf + offset, bufSize - offset, fmt, ap);
  va_end(ap);
  return need;
}

// Format a float as "%.3f" or "nan" if NaN. ESP32 newlib snprintf produces
// garbage for NaN with %.3f, so guard with isnan() and emit a literal.
static int appendFloat(char* buf, size_t bufSize, int offset, float val) {
  if (isnan(val)) {
    return appendFmt(buf, bufSize, offset, "%s", "nan");
  }
  return appendFmt(buf, bufSize, offset, "%.3f", val);
}

// Latitude/longitude get 6 decimal places (~0.11 m), not the 3 used for
// sensor readings — see FIELDMESH_SPATIAL_LOCATION_PLAN.md.
static int appendCoord(char* buf, size_t bufSize, int offset, float val) {
  if (isnan(val)) {
    return appendFmt(buf, bufSize, offset, "%s", "nan");
  }
  return appendFmt(buf, bufSize, offset, "%.6f", val);
}

// Operator-entered identity text (node name) reaches the CSV verbatim, and the
// name field accepts any character the Field UI form submits. These cells are
// written UNQUOTED, and the upload parser (json_payload.cpp splitCsvRow) is a
// plain comma split with no quote handling, so a comma or newline inside a name
// silently re-frames the row: the column count no longer matches, and
// formatDecodedSnapshotCSVRow's final gate then rejects the row entirely —
// nothing reaches flash or SD and the node is NACKed into re-sending forever.
// Replace the framing characters rather than quoting them, so the column count
// stays exact for every consumer of this file.
static String csvSafeCell(const String& v) {
  String out = v;
  for (size_t i = 0; i < out.length(); ++i) {
    const char c = out[i];
    if (c == ',' || c == '\r' || c == '\n') out.setCharAt(i, ' ');
  }
  return out;
}

// ---------------------------------------------------------------------------
// V1 / V2 decode — Phase 3 of the V2 snapshot migration
// ---------------------------------------------------------------------------

void decodeV1(const node_snapshot_t& snap, DecodedSnapshot& out) {
  memset(&out, 0, sizeof(out));
  memcpy(out.nodeId, snap.nodeId, sizeof(out.nodeId));
  out.nodeTimestamp    = snap.nodeTimestamp;
  out.seqNum           = snap.seqNum;
  out.qualityFlags     = snap.qualityFlags;
  out.configVersion    = snap.configVersion;
  out.protocolVersion  = NODE_PROTOCOL_VERSION;  // V1 packets carry no field
  out.sensorPresent    = snap.sensorPresent;
  out.readingCount     = 0;

  auto add = [&](uint16_t id, float v) {
    if (out.readingCount < MAX_READINGS_PER_SNAPSHOT) {
      out.readings[out.readingCount].sensorId = id;
      out.readings[out.readingCount].value    = v;
      out.readingCount++;
    }
  };

  if (snap.sensorPresent & SNAP_PRESENT_AIR_TEMP)   add(SENSOR_ID_AIR_TEMP, snap.airTemp);
  if (snap.sensorPresent & SNAP_PRESENT_AIR_RH)     add(SENSOR_ID_AIR_RH, snap.airHumidity);
  if (snap.sensorPresent & SNAP_PRESENT_SPECTRAL) {
    add(SENSOR_ID_SPECTRAL_415, snap.spectral[0]);
    add(SENSOR_ID_SPECTRAL_445, snap.spectral[1]);
    add(SENSOR_ID_SPECTRAL_480, snap.spectral[2]);
    add(SENSOR_ID_SPECTRAL_515, snap.spectral[3]);
    add(SENSOR_ID_SPECTRAL_555, snap.spectral[4]);
    add(SENSOR_ID_SPECTRAL_590, snap.spectral[5]);
    add(SENSOR_ID_SPECTRAL_630, snap.spectral[6]);
    add(SENSOR_ID_SPECTRAL_680, snap.spectral[7]);
  }
  if (snap.sensorPresent & SNAP_PRESENT_WIND) {
    add(SENSOR_ID_WIND_SPEED, snap.windSpeed);
    add(SENSOR_ID_WIND_DIR,   snap.windDir);
  }
  if (snap.sensorPresent & SNAP_PRESENT_SOIL1) {
    add(SENSOR_ID_SOIL1_VWC,  snap.soil1Vwc);
    add(SENSOR_ID_SOIL1_TEMP, snap.soil1Temp);
  }
  if (snap.sensorPresent & SNAP_PRESENT_SOIL2) {
    add(SENSOR_ID_SOIL2_VWC,  snap.soil2Vwc);
    add(SENSOR_ID_SOIL2_TEMP, snap.soil2Temp);
  }
  if (snap.sensorPresent & SNAP_PRESENT_AUX1)   add(SENSOR_ID_AUX1, snap.aux1);
  if (snap.sensorPresent & SNAP_PRESENT_AUX2)   add(SENSOR_ID_AUX2, snap.aux2);
  if (snap.sensorPresent & SNAP_PRESENT_BAT_V)  add(SENSOR_ID_BAT_V, snap.batVoltage);
}

bool decodeV2(const uint8_t* data, int len, DecodedSnapshot& out) {
  if (!isV2Snapshot(data, len)) return false;

  const node_snapshot_v2_t* hdr = reinterpret_cast<const node_snapshot_v2_t*>(data);
  memset(&out, 0, sizeof(out));
  memcpy(out.nodeId, hdr->nodeId, sizeof(out.nodeId));
  out.nodeTimestamp    = hdr->nodeTimestamp;
  out.seqNum           = hdr->seqNum;
  out.qualityFlags     = hdr->qualityFlags;
  out.configVersion    = hdr->configVersion;
  out.protocolVersion  = hdr->protocolVersion;
  out.sensorPresent    = 0;  // synthesised below
  out.readingCount     = 0;

  const uint8_t* body = data + sizeof(node_snapshot_v2_t);
  for (uint16_t i = 0; i < hdr->sensorCount; ++i) {
    if (out.readingCount >= MAX_READINGS_PER_SNAPSHOT) break;
    const v2_reading_t* r = reinterpret_cast<const v2_reading_t*>(body + i * sizeof(v2_reading_t));
    out.readings[out.readingCount].sensorId = r->sensorId;
    out.readings[out.readingCount].value    = r->value;
    out.readingCount++;

    // Rebuild the V1 sensorPresent bitmask so the CSV sensorPresent column
    // stays meaningful for downstream tooling.
    switch (r->sensorId) {
      case SENSOR_ID_AIR_TEMP:       out.sensorPresent |= SNAP_PRESENT_AIR_TEMP; break;
      case SENSOR_ID_AIR_RH:         out.sensorPresent |= SNAP_PRESENT_AIR_RH; break;
      case SENSOR_ID_SPECTRAL_415:
      case SENSOR_ID_SPECTRAL_445:
      case SENSOR_ID_SPECTRAL_480:
      case SENSOR_ID_SPECTRAL_515:
      case SENSOR_ID_SPECTRAL_555:
      case SENSOR_ID_SPECTRAL_590:
      case SENSOR_ID_SPECTRAL_630:
      case SENSOR_ID_SPECTRAL_680:
      case SENSOR_ID_SPECTRAL_CLEAR:
      case SENSOR_ID_SPECTRAL_NIR:
      case SENSOR_ID_SPECTRAL_GAIN:
      case SENSOR_ID_SPECTRAL_ATIME:
      case SENSOR_ID_SPECTRAL_SAT:    out.sensorPresent |= SNAP_PRESENT_SPECTRAL; break;
      case SENSOR_ID_WIND_SPEED:
      case SENSOR_ID_WIND_DIR:       out.sensorPresent |= SNAP_PRESENT_WIND; break;
      case SENSOR_ID_SOIL1_VWC:
      case SENSOR_ID_SOIL1_TEMP:     out.sensorPresent |= SNAP_PRESENT_SOIL1; break;
      case SENSOR_ID_SOIL2_VWC:
      case SENSOR_ID_SOIL2_TEMP:     out.sensorPresent |= SNAP_PRESENT_SOIL2; break;
      case SENSOR_ID_AUX1:           out.sensorPresent |= SNAP_PRESENT_AUX1; break;
      case SENSOR_ID_AUX2:           out.sensorPresent |= SNAP_PRESENT_AUX2; break;
      case SENSOR_ID_BAT_V:          out.sensorPresent |= SNAP_PRESENT_BAT_V; break;
      default: break;
    }
  }
  return true;
}

void decodedToV1(const DecodedSnapshot& decoded, node_snapshot_t& out) {
  memset(&out, 0, sizeof(out));
  strncpy(out.command, "NODE_SNAPSHOT", sizeof(out.command) - 1);
  memcpy(out.nodeId, decoded.nodeId, sizeof(out.nodeId));
  out.nodeTimestamp  = decoded.nodeTimestamp;
  out.seqNum         = decoded.seqNum;
  out.sensorPresent  = decoded.sensorPresent;
  out.qualityFlags   = decoded.qualityFlags;
  out.configVersion  = decoded.configVersion;

  // Default everything to NaN so missing sensors are explicit.
  out.batVoltage  = NAN;
  out.airTemp     = NAN;
  out.airHumidity = NAN;
  for (int i = 0; i < 8; ++i) out.spectral[i] = NAN;
  out.windSpeed = NAN;
  out.windDir   = NAN;
  out.soil1Vwc  = NAN;
  out.soil1Temp = NAN;
  out.soil2Vwc  = NAN;
  out.soil2Temp = NAN;
  out.aux1      = NAN;
  out.aux2      = NAN;

  auto setf = [&](uint16_t id, float* dst) {
    const float* p = decoded.find(id);
    if (p && dst) *dst = *p;
  };
  setf(SENSOR_ID_BAT_V,  &out.batVoltage);
  setf(SENSOR_ID_AIR_TEMP, &out.airTemp);
  setf(SENSOR_ID_AIR_RH,   &out.airHumidity);
  setf(SENSOR_ID_SPECTRAL_415, &out.spectral[0]);
  setf(SENSOR_ID_SPECTRAL_445, &out.spectral[1]);
  setf(SENSOR_ID_SPECTRAL_480, &out.spectral[2]);
  setf(SENSOR_ID_SPECTRAL_515, &out.spectral[3]);
  setf(SENSOR_ID_SPECTRAL_555, &out.spectral[4]);
  setf(SENSOR_ID_SPECTRAL_590, &out.spectral[5]);
  setf(SENSOR_ID_SPECTRAL_630, &out.spectral[6]);
  setf(SENSOR_ID_SPECTRAL_680, &out.spectral[7]);
  setf(SENSOR_ID_WIND_SPEED, &out.windSpeed);
  setf(SENSOR_ID_WIND_DIR,   &out.windDir);
  setf(SENSOR_ID_SOIL1_VWC,  &out.soil1Vwc);
  setf(SENSOR_ID_SOIL1_TEMP, &out.soil1Temp);
  setf(SENSOR_ID_SOIL2_VWC,  &out.soil2Vwc);
  setf(SENSOR_ID_SOIL2_TEMP, &out.soil2Temp);
  setf(SENSOR_ID_AUX1, &out.aux1);
  setf(SENSOR_ID_AUX2, &out.aux2);
}

// Emit a CSV cell for a sensor that may be absent. Writes "nan" when the
// sensor is missing or its value is NaN.
static int appendSensor(char* buf, size_t bufSize, int offset,
                        const DecodedSnapshot& d, uint16_t sensorId) {
  const float* p = d.find(sensorId);
  if (!p) return appendFmt(buf, bufSize, offset, "%s", "nan");
  return appendFloat(buf, bufSize, offset, *p);
}

static void traceDecodedSpectralMetadata(const DecodedSnapshot& decoded) {
  const uint16_t ids[] = {
    SENSOR_ID_SPECTRAL_CLEAR, SENSOR_ID_SPECTRAL_NIR,
    SENSOR_ID_SPECTRAL_GAIN, SENSOR_ID_SPECTRAL_ATIME,
    SENSOR_ID_SPECTRAL_SAT,
  };
  const char* labels[] = {"clear", "nir", "gain", "integration_ms", "saturated"};

  size_t present = 0;
  for (uint16_t id : ids) {
    if (decoded.find(id)) ++present;
  }

  Serial.printf("[SNAP-SPEC] node=%.15s seq=%lu proto=%u readings=%u extended=%u/5",
                decoded.nodeId, static_cast<unsigned long>(decoded.seqNum),
                static_cast<unsigned>(decoded.protocolVersion),
                static_cast<unsigned>(decoded.readingCount),
                static_cast<unsigned>(present));
  for (size_t i = 0; i < 5; ++i) {
    Serial.printf(" %s=", labels[i]);
    const float* value = decoded.find(ids[i]);
    if (value && isfinite(*value)) Serial.printf("%.3f", *value);
    else Serial.print(value ? "NONFINITE" : "MISSING");
  }
  Serial.println();
}

bool formatDecodedSnapshotCSVRow(const DecodedSnapshot& decoded, String& outRow) {
  traceDecodedSpectralMetadata(decoded);
  // Convert Unix timestamp to ISO 8601 datetime
  char tsBuf[25];
  uint32_t ts = decoded.nodeTimestamp;
  if (ts > 0) {
    time_t t = (time_t)ts;
    struct tm* tm = gmtime(&t);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
  } else {
    snprintf(tsBuf, sizeof(tsBuf), "unknown");
  }

  // Build the SAME CSV row format as logSnapshotRow(), but source the float
  // values from the DecodedSnapshot readings so V1 and V2 produce identical
  // output. Missing sensors become "nan".
  char row[kCsvRowBufBytes];
  int n = snprintf(row, sizeof(row),
    "%s,%.15s,%lu,%u,%u,%u,",
    tsBuf,
    decoded.nodeId,
    (unsigned long)decoded.seqNum,
    (unsigned)decoded.sensorPresent,
    (unsigned)decoded.qualityFlags,
    (unsigned)decoded.configVersion);
  if (n <= 0) return false;

  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_BAT_V);       n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AIR_TEMP);    n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AIR_RH);      n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_415); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_445); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_480); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_515); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_555); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_590); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_630); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_680); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_WIND_SPEED);  n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_WIND_DIR);    n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL1_VWC);   n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL1_TEMP);  n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL2_VWC);   n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL2_TEMP);  n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AUX1);        n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AUX2);        n += appendFmt(row, sizeof(row), n, ",");
  // Extended AS7341 outputs (appended so existing column indices are stable).
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_CLEAR); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_NIR);   n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_GAIN);  n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_ATIME); n += appendFmt(row, sizeof(row), n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_SAT);
  // Deployment epoch (column 31, appended so every existing index is stable).
  // Integer, not a sensor value: 0 means legacy/unresolved.
  n += appendFmt(row, sizeof(row), n, ",%u", (unsigned)decoded.deploymentEpoch);
  // Node identity appended as CSV columns 32 and 33 for local downloads.
  const String userId = csvSafeCell(getNodeUserId(String(decoded.nodeId)));
  const String nodeName = csvSafeCell(getNodeName(String(decoded.nodeId)));
  n += appendFmt(row, sizeof(row), n, ",%s", userId.c_str());
  n += appendFmt(row, sizeof(row), n, ",%s", nodeName.c_str());
  // Node location appended as CSV columns 34 and 35 (nan when unset).
  n += appendFmt(row, sizeof(row), n, ",");
  n += appendCoord(row, sizeof(row), n, getNodeLatitude(String(decoded.nodeId)));
  n += appendFmt(row, sizeof(row), n, ",");
  n += appendCoord(row, sizeof(row), n, getNodeLongitude(String(decoded.nodeId)));

  if (n <= 0 || n >= static_cast<int>(sizeof(row))) return false;
  outRow = String(row);

  size_t columns = 1;
  for (size_t i = 0; i < outRow.length(); ++i) {
    if (outRow[i] == ',') ++columns;
  }
  Serial.printf("[FLASH-SPEC] seq=%lu columns=%u extended_csv=%s,%s,%s,%s,%s\n",
                static_cast<unsigned long>(decoded.seqNum),
                static_cast<unsigned>(columns),
                decoded.find(SENSOR_ID_SPECTRAL_CLEAR) ? "number" : "nan",
                decoded.find(SENSOR_ID_SPECTRAL_NIR) ? "number" : "nan",
                decoded.find(SENSOR_ID_SPECTRAL_GAIN) ? "number" : "nan",
                decoded.find(SENSOR_ID_SPECTRAL_ATIME) ? "number" : "nan",
                decoded.find(SENSOR_ID_SPECTRAL_SAT) ? "number" : "nan");
  return columns == kCurrentCSVColumnCount;
}

bool logDecodedSnapshot(const DecodedSnapshot& decoded) {
  String row;
  if (!formatDecodedSnapshotCSVRow(decoded, row)) return false;
  return flashLogCSVRow(row);
}

// CSV header matching node_snapshot_t fields.
// Columns: datetime, nodeId, seqNum, sensorPresent, qualityFlags, configVersion,
//          batVoltage, airTemp, airHumidity, spectral[8], windSpeed, windDir,
//          soil1Vwc, soil1Temp, soil2Vwc, soil2Temp, aux1, aux2,
//          spectral_clear, spectral_nir, spectral_gain, spectral_integration_ms,
//          spectral_saturated, deploymentEpoch, userId, name, latitude, longitude
static const char* kCSVHeader = kCurrentCSVHeader35;

static bool gFlashReady = false;
static bool gFlashMountFailed = false;

bool initFlash() {
  // formatOnFail=true: after a flash erase (NVS + LittleFS wiped), the
  // LittleFS partition has no valid filesystem. Auto-format on mount failure
  // so flash logging recovers automatically instead of staying disabled.
  if (!LittleFS.begin(true)) {
    Serial.println("[FLASH] LittleFS begin(true) failed — attempting explicit format");
    LittleFS.end();
    delay(100);
    if (!LittleFS.format()) {
      Serial.println("[FLASH] Explicit LittleFS format failed");
      gFlashReady = false;
      gFlashMountFailed = true;
      return false;
    }
    if (!LittleFS.begin(false)) {
      Serial.println("[FLASH] LittleFS mount failed after explicit format");
      gFlashReady = false;
      gFlashMountFailed = true;
      return false;
    }
    Serial.println("[FLASH] LittleFS formatted and mounted after explicit format");
  }

  gFlashMountFailed = false;

  Serial.printf("[FLASH] LittleFS mounted: %u bytes total, %u bytes used\n",
                (unsigned)LittleFS.totalBytes(),
                (unsigned)LittleFS.usedBytes());

  // Ensure datalog.csv exists with the correct header.
  if (!LittleFS.exists(kFlashFile)) {
    Serial.println("[FLASH] datalog.csv not found, creating with header");
    if (!flashCreateCSVHeader()) {
      Serial.println("[FLASH] Failed to create CSV header");
      gFlashReady = false;
      return false;
    }
  } else {
    // Never delete queued rows merely because the schema gained appended
    // columns. A legacy 25-column file remains positionally compatible with
    // new 30-column rows. UploadQueue presents the current header to uploads
    // and upgrades the on-disk header after the queue is fully drained.
    File f = LittleFS.open(kFlashFile, "r");
    if (!f) {
      Serial.println("[FLASH] Existing datalog.csv could not be opened");
      gFlashReady = false;
      return false;
    }
    String firstLine = f.readStringUntil('\n');
    const bool hasDataRows = f.available();
    firstLine.trim();
    f.close();

    // EVERY legacy header must be recognised here. An unrecognised header sets
    // gFlashReady = false and stops all logging, so omitting a prior column
    // count when the current header gains columns would brick logging on
    // every hub that upgrades.
    const bool isLegacy25 = (firstLine == String(kLegacyCSVHeader25));
    const bool isLegacy30 = (firstLine == String(kLegacyCSVHeader30));
    const bool isLegacy31 = (firstLine == String(kLegacyCSVHeader31));
    const bool isLegacy33 = (firstLine == String(kLegacyCSVHeader33));
    if (isLegacy25 || isLegacy30 || isLegacy31 || isLegacy33) {
      const int legacyCols = isLegacy25 ? 25 : (isLegacy30 ? 30 : (isLegacy31 ? 31 : 33));
      if (hasDataRows) {
        Serial.printf("[FLASH] Legacy %d-column CSV has queued rows; preserving until upload drain\n",
                      legacyCols);
      } else {
        Serial.printf("[FLASH] Empty legacy %d-column CSV; upgrading header to %u columns\n",
                      legacyCols, (unsigned)kCurrentCSVColumnCount);
        if (!flashCreateCSVHeader()) {
          gFlashReady = false;
          return false;
        }
      }
    } else if (firstLine != String(kCSVHeader)) {
      Serial.println("[FLASH] Unknown CSV header; preserving file and refusing incompatible appends");
      gFlashReady = false;
      return false;
    }
  }

  gFlashReady = true;
  return true;
}

bool flashCreateCSVHeader() {
  File f = LittleFS.open(kFlashFile, "w", true);
  if (!f) {
    Serial.println("[FLASH] Failed to create datalog.csv");
    return false;
  }
  f.println(kCSVHeader);
  f.close();
  Serial.println("[FLASH] CSV header created");
  return true;
}

bool flashCsvSchemaIsCurrent() {
  // Read the on-disk header rather than trusting a cached flag: purgeUploaded()
  // upgrades the header mid-session once the queue drains, and the answer must
  // follow that within the same session.
  if (!LittleFS.exists(kFlashFile)) return true;  // will be created current
  File f = LittleFS.open(kFlashFile, "r");
  if (!f) return false;                            // unknown — assume not current
  String header = f.readStringUntil('\n');
  f.close();
  header.trim();
  return header == String(kCSVHeader);
}

bool logSnapshotRow(const node_snapshot_t* snap) {
  if (!snap) return false;

  // Convert Unix timestamp to ISO 8601 datetime
  char tsBuf[25];
  uint32_t ts = snap->nodeTimestamp;
  if (ts > 0) {
    time_t t = (time_t)ts;
    struct tm* tm = gmtime(&t);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
  } else {
    snprintf(tsBuf, sizeof(tsBuf), "unknown");
  }

  // Build CSV row from node_snapshot_t fields. Non-float fields first, then
  // each float via appendFloat() to avoid newlib NaN garbage from %.3f.
  char row[kCsvRowBufBytes];
  int n = snprintf(row, sizeof(row),
    "%s,%.15s,%lu,%u,%u,%u,",
    tsBuf,
    snap->nodeId,
    (unsigned long)snap->seqNum,
    snap->sensorPresent,
    snap->qualityFlags,
    snap->configVersion);
  if (n <= 0) return false;

  n += appendFloat(row, sizeof(row), n, snap->batVoltage);   n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->airTemp);      n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->airHumidity);  n += appendFmt(row, sizeof(row), n, ",");
  for (int i = 0; i < 8; i++) {
    n += appendFloat(row, sizeof(row), n, snap->spectral[i]);
    n += appendFmt(row, sizeof(row), n, ",");
  }
  n += appendFloat(row, sizeof(row), n, snap->windSpeed);    n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->windDir);      n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil1Vwc);     n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil1Temp);    n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil2Vwc);     n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil2Temp);    n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->aux1);         n += appendFmt(row, sizeof(row), n, ",");
  n += appendFloat(row, sizeof(row), n, snap->aux2);
  // Extended AS7341 columns — absent in the V1 snapshot struct, emit nan.
  // Trailing 0 is deploymentEpoch: this path has no registry lookup, so the
  // epoch is unresolved and the backend falls back.
  const String userId = csvSafeCell(getNodeUserId(String(snap->nodeId)));
  const String nodeName = csvSafeCell(getNodeName(String(snap->nodeId)));
  n += appendFmt(row, sizeof(row), n, ",nan,nan,nan,nan,nan,0,%s,%s,",
                userId.c_str(), nodeName.c_str());
  n += appendCoord(row, sizeof(row), n, getNodeLatitude(String(snap->nodeId)));
  n += appendFmt(row, sizeof(row), n, ",");
  n += appendCoord(row, sizeof(row), n, getNodeLongitude(String(snap->nodeId)));

  // Same overflow gate as formatDecodedSnapshotCSVRow: a truncated row is short
  // a trailing column and must not reach the queue as a mis-framed line.
  if (n <= 0 || n >= static_cast<int>(sizeof(row))) return false;
  return flashLogCSVRow(String(row));
}

bool logSnapshotBatch(const node_snapshot_t* snapshots, int count) {
  if (!gFlashReady || !snapshots || count <= 0) return false;

  File f = LittleFS.open(kFlashFile, "a");
  if (!f) {
    Serial.println("[FLASH] Failed to open datalog.csv for batch append");
    return false;
  }

  int written = 0;
  for (int i = 0; i < count; i++) {
    const node_snapshot_t* snap = &snapshots[i];

    // Convert Unix timestamp to ISO 8601 datetime
    char tsBuf[25];
    uint32_t ts = snap->nodeTimestamp;
    if (ts > 0) {
      time_t t = (time_t)ts;
      struct tm* tm = gmtime(&t);
      snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
               tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
      snprintf(tsBuf, sizeof(tsBuf), "unknown");
    }

    // Build CSV row. Non-float fields first, then each float via
    // appendFloat() to avoid newlib NaN garbage from %.3f.
    char row[kCsvRowBufBytes];
    int n = snprintf(row, sizeof(row),
      "%s,%.15s,%lu,%u,%u,%u,",
      tsBuf,
      snap->nodeId,
      (unsigned long)snap->seqNum,
      snap->sensorPresent,
      snap->qualityFlags,
      snap->configVersion);
    if (n <= 0) continue;

    n += appendFloat(row, sizeof(row), n, snap->batVoltage);   n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->airTemp);      n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->airHumidity);  n += appendFmt(row, sizeof(row), n, ",");
    for (int i = 0; i < 8; i++) {
      n += appendFloat(row, sizeof(row), n, snap->spectral[i]);
      n += appendFmt(row, sizeof(row), n, ",");
    }
    n += appendFloat(row, sizeof(row), n, snap->windSpeed);    n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->windDir);      n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil1Vwc);     n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil1Temp);    n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil2Vwc);     n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil2Temp);    n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->aux1);         n += appendFmt(row, sizeof(row), n, ",");
    n += appendFloat(row, sizeof(row), n, snap->aux2);
    // Extended AS7341 columns — absent in the V1 snapshot struct, emit nan.
    const String userId = csvSafeCell(getNodeUserId(String(snap->nodeId)));
    const String nodeName = csvSafeCell(getNodeName(String(snap->nodeId)));
    n += appendFmt(row, sizeof(row), n, ",nan,nan,nan,nan,nan,0,%s,%s,",
                  userId.c_str(), nodeName.c_str());
    n += appendCoord(row, sizeof(row), n, getNodeLatitude(String(snap->nodeId)));
    n += appendFmt(row, sizeof(row), n, ",");
    n += appendCoord(row, sizeof(row), n, getNodeLongitude(String(snap->nodeId)));

    // Same overflow gate as formatDecodedSnapshotCSVRow: a row that did not fit
    // is short a trailing column, so writing it would append a mis-framed line
    // to the queue rather than a detectably absent one.
    if (n > 0 && n < static_cast<int>(sizeof(row))) {
      f.println(row);
      written++;
    } else if (n >= static_cast<int>(sizeof(row))) {
      Serial.printf("[FLASH] Batch row %d exceeded the %u-byte row buffer; skipped\n",
                    i, (unsigned)sizeof(row));
    }
  }
  f.close();

  Serial.printf("[FLASH] Batch write: %d/%d snapshots logged\n", written, count);
  return written > 0;
}

bool flashLogCSVRow(const String& row) {
  if (!gFlashReady) return false;

  File f = LittleFS.open(kFlashFile, "a");
  if (!f) {
    Serial.println("[FLASH] Failed to open datalog.csv for append");
    return false;
  }
  const size_t written = f.println(row);
  const bool writeError = f.getWriteError();
  f.close();

  // Arduino-ESP32 Print::println() appends CRLF (two bytes).
  const size_t expected = row.length() + 2;
  if (writeError || written != expected) {
    Serial.printf("[FLASH] Write failed: wrote %u of %u bytes, error=%d\n",
                  static_cast<unsigned>(written), static_cast<unsigned>(expected),
                  writeError);
    return false;
  }
  return true;
}

String flashGetCSVStats() {
  if (!gFlashReady) return "Flash not ready";

  File f = LittleFS.open(kFlashFile, "r");
  if (!f) return "Cannot open datalog.csv";

  int lineCount = 0;
  while (f.available()) {
    if (f.read() == '\n') lineCount++;
  }
  f.close();

  int dataLines = lineCount > 0 ? lineCount - 1 : 0;
  char buf[128];
  snprintf(buf, sizeof(buf), "Flash records: %d, Used: %u/%u bytes",
           dataLines,
           (unsigned)LittleFS.usedBytes(),
           (unsigned)LittleFS.totalBytes());
  return String(buf);
}

bool flashIsReady() {
  return gFlashReady;
}

bool flashMountFailed() {
  return gFlashMountFailed;
}

bool flashFormatExplicit() {
  gFlashReady = false;
  LittleFS.end();

  if (!LittleFS.format()) {
    Serial.println("[FLASH] Explicit format failed");
    gFlashMountFailed = true;
    return false;
  }
  if (!LittleFS.begin(false)) {
    Serial.println("[FLASH] Mount failed after explicit format");
    gFlashMountFailed = true;
    return false;
  }
  if (!flashCreateCSVHeader()) {
    Serial.println("[FLASH] Header creation failed after explicit format");
    gFlashMountFailed = true;
    return false;
  }

  gFlashReady = true;
  gFlashMountFailed = false;
  Serial.println("[FLASH] Explicit format complete");
  return true;
}

// ---------------------------------------------------------------------------
// CSV download helpers (for future WiFi AP web server integration)
// ---------------------------------------------------------------------------

String readCSVFile() {
  if (!gFlashReady) return String();

  File f = LittleFS.open(kFlashFile, "r");
  if (!f) return String();

  String contents = f.readString();
  f.close();
  return contents;
}

size_t getCSVFileSize() {
  if (!gFlashReady) return 0;
  File f = LittleFS.open(kFlashFile, "r");
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

int getCSVRecordCount() {
  if (!gFlashReady) return 0;
  File f = LittleFS.open(kFlashFile, "r");
  if (!f) return 0;

  int lineCount = 0;
  while (f.available()) {
    if (f.read() == '\n') lineCount++;
  }
  f.close();
  return lineCount > 0 ? lineCount - 1 : 0;
}
