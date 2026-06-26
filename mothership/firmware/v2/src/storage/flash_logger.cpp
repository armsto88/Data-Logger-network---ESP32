#include "storage/flash_logger.h"
#include "protocol.h"
#include <time.h>
#include <math.h>

static const char* kFlashFile = "/datalog.csv";

// Format a float as "%.3f" or "nan" if NaN. ESP32 newlib snprintf produces
// garbage for NaN with %.3f, so guard with isnan() and emit a literal.
static int appendFloat(char* buf, size_t bufSize, int offset, float val) {
  if (isnan(val)) {
    return snprintf(buf + offset, bufSize - offset, "%s", "nan");
  }
  return snprintf(buf + offset, bufSize - offset, "%.3f", val);
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
      case SENSOR_ID_SPECTRAL_680:   out.sensorPresent |= SNAP_PRESENT_SPECTRAL; break;
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
  if (!p) return snprintf(buf + offset, bufSize - offset, "%s", "nan");
  return appendFloat(buf, bufSize, offset, *p);
}

bool logDecodedSnapshot(const DecodedSnapshot& decoded) {
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
  char row[512];
  int n = snprintf(row, sizeof(row),
    "%s,%.15s,%lu,%u,%u,%u,",
    tsBuf,
    decoded.nodeId,
    (unsigned long)decoded.seqNum,
    (unsigned)decoded.sensorPresent,
    (unsigned)decoded.qualityFlags,
    (unsigned)decoded.configVersion);
  if (n <= 0) return false;

  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_BAT_V);       n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AIR_TEMP);    n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AIR_RH);      n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_415); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_445); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_480); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_515); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_555); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_590); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_630); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SPECTRAL_680); n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_WIND_SPEED);  n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_WIND_DIR);    n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL1_VWC);   n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL1_TEMP);  n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL2_VWC);   n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_SOIL2_TEMP);  n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AUX1);        n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendSensor(row, sizeof(row), n, decoded, SENSOR_ID_AUX2);

  if (n <= 0) return false;
  return flashLogCSVRow(String(row));
}

// CSV header matching node_snapshot_t fields.
// Columns: datetime, nodeId, seqNum, sensorPresent, qualityFlags, configVersion,
//          batVoltage, airTemp, airHumidity, spectral[8], windSpeed, windDir,
//          soil1Vwc, soil1Temp, soil2Vwc, soil2Temp, aux1, aux2
static const char* kCSVHeader =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2";

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
    // Verify header matches; if not, recreate the file.
    File f = LittleFS.open(kFlashFile, "r");
    if (f) {
      String firstLine = f.readStringUntil('\n');
      firstLine.trim();
      f.close();
      if (firstLine != String(kCSVHeader)) {
        Serial.println("[FLASH] Header mismatch — recreating datalog.csv");
        LittleFS.remove(kFlashFile);
        flashCreateCSVHeader();
      }
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
  char row[512];
  int n = snprintf(row, sizeof(row),
    "%s,%.15s,%lu,%u,%u,%u,",
    tsBuf,
    snap->nodeId,
    (unsigned long)snap->seqNum,
    snap->sensorPresent,
    snap->qualityFlags,
    snap->configVersion);
  if (n <= 0) return false;

  n += appendFloat(row, sizeof(row), n, snap->batVoltage);   n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->airTemp);      n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->airHumidity);  n += snprintf(row + n, sizeof(row) - n, ",");
  for (int i = 0; i < 8; i++) {
    n += appendFloat(row, sizeof(row), n, snap->spectral[i]);
    n += snprintf(row + n, sizeof(row) - n, ",");
  }
  n += appendFloat(row, sizeof(row), n, snap->windSpeed);    n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->windDir);      n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil1Vwc);     n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil1Temp);    n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil2Vwc);     n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->soil2Temp);    n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->aux1);         n += snprintf(row + n, sizeof(row) - n, ",");
  n += appendFloat(row, sizeof(row), n, snap->aux2);

  if (n <= 0) return false;
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
    char row[512];
    int n = snprintf(row, sizeof(row),
      "%s,%.15s,%lu,%u,%u,%u,",
      tsBuf,
      snap->nodeId,
      (unsigned long)snap->seqNum,
      snap->sensorPresent,
      snap->qualityFlags,
      snap->configVersion);
    if (n <= 0) continue;

    n += appendFloat(row, sizeof(row), n, snap->batVoltage);   n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->airTemp);      n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->airHumidity);  n += snprintf(row + n, sizeof(row) - n, ",");
    for (int i = 0; i < 8; i++) {
      n += appendFloat(row, sizeof(row), n, snap->spectral[i]);
      n += snprintf(row + n, sizeof(row) - n, ",");
    }
    n += appendFloat(row, sizeof(row), n, snap->windSpeed);    n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->windDir);      n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil1Vwc);     n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil1Temp);    n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil2Vwc);     n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->soil2Temp);    n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->aux1);         n += snprintf(row + n, sizeof(row) - n, ",");
    n += appendFloat(row, sizeof(row), n, snap->aux2);

    if (n > 0) {
      f.println(row);
      written++;
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
