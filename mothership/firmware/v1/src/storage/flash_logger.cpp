#include "storage/flash_logger.h"
#include "protocol.h"
#include <time.h>

static const char* kFlashFile = "/datalog.csv";

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
  if (!LittleFS.begin(false)) {
    Serial.println("[FLASH] LittleFS mount failed");
    gFlashReady = false;
    gFlashMountFailed = true;
    return false;
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

  // Build CSV row from node_snapshot_t fields.
  char row[512];
  int n = snprintf(row, sizeof(row),
    "%s,%.15s,%lu,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
    tsBuf,
    snap->nodeId,
    (unsigned long)snap->seqNum,
    snap->sensorPresent,
    snap->qualityFlags,
    snap->configVersion,
    snap->batVoltage,
    snap->airTemp,
    snap->airHumidity,
    snap->spectral[0], snap->spectral[1], snap->spectral[2], snap->spectral[3],
    snap->spectral[4], snap->spectral[5], snap->spectral[6], snap->spectral[7],
    snap->windSpeed,
    snap->windDir,
    snap->soil1Vwc,
    snap->soil1Temp,
    snap->soil2Vwc,
    snap->soil2Temp,
    snap->aux1,
    snap->aux2);

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

    char row[512];
    int n = snprintf(row, sizeof(row),
      "%s,%.15s,%lu,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
      tsBuf,
      snap->nodeId,
      (unsigned long)snap->seqNum,
      snap->sensorPresent,
      snap->qualityFlags,
      snap->configVersion,
      snap->batVoltage,
      snap->airTemp,
      snap->airHumidity,
      snap->spectral[0], snap->spectral[1], snap->spectral[2], snap->spectral[3],
      snap->spectral[4], snap->spectral[5], snap->spectral[6], snap->spectral[7],
      snap->windSpeed,
      snap->windDir,
      snap->soil1Vwc,
      snap->soil1Temp,
      snap->soil2Vwc,
      snap->soil2Temp,
      snap->aux1,
      snap->aux2);

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
