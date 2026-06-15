#include "storage/sd_logger.h"
#include "system/pins.h"

static SPIClass gSDSPI(VSPI);
static bool gSDReady = false;
static String gCurrentFile;

// CSV header matching NodeSnapshot fields
static const char* kCSVHeader = "timestamp_ms,node_id,sensor_type,sensor_label,sensor_id,value,node_timestamp,quality_flags";

bool initSD() {
  gSDSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (!SD.begin(PIN_SD_CS, gSDSPI, 40000000)) {
    Serial.println("[SD] SD.begin() failed — no card or wiring issue");
    gSDReady = false;
    return false;
  }

  Serial.printf("[SD] Card type: ");
  switch (SD.cardType()) {
    case CARD_MMC:    Serial.println("MMC"); break;
    case CARD_SD:     Serial.println("SD"); break;
    case CARD_SDHC:   Serial.println("SDHC"); break;
    case CARD_UNKNOWN: Serial.println("UNKNOWN"); break;
    default:          Serial.println("NONE"); break;
  }

  uint64_t totalBytes = SD.totalBytes();
  Serial.printf("[SD] Total: %.1f MB\n", totalBytes / 1048576.0);

  // Create today's log file with header
  char filename[32];
  snprintf(filename, sizeof(filename), "/log_%lu.csv", millis() / 1000);
  gCurrentFile = String(filename);

  File f = SD.open(gCurrentFile, FILE_WRITE, true);
  if (f) {
    f.println(kCSVHeader);
    f.close();
    Serial.printf("[SD] Created log file: %s\n", gCurrentFile.c_str());
  } else {
    Serial.printf("[SD] Failed to create log file: %s\n", gCurrentFile.c_str());
  }

  gSDReady = true;
  return true;
}

bool logSnapshot(const NodeSnapshot* snap) {
  if (!gSDReady || !gCurrentFile.length()) return false;

  char row[256];
  snprintf(row, sizeof(row), "%lu,%s,%s,%s,%u,%.4f,%lu,%u",
           millis(),
           snap->nodeId,
           snap->sensorType,
           snap->sensorLabel,
           snap->sensorId,
           snap->value,
           snap->nodeTimestamp,
           snap->qualityFlags);

  return logCSVRow(String(row));
}

bool logCSVRow(const String& row) {
  if (!gSDReady || !gCurrentFile.length()) return false;

  File f = SD.open(gCurrentFile, FILE_APPEND);
  if (!f) {
    Serial.println("[SD] Failed to open log file for append");
    return false;
  }

  f.println(row);
  f.close();
  return true;
}

String getCSVStats() {
  if (!gSDReady || !gCurrentFile.length()) {
    return "SD not ready";
  }

  File f = SD.open(gCurrentFile, FILE_READ);
  if (!f) {
    return "Cannot open file";
  }

  int lineCount = 0;
  while (f.available()) {
    if (f.read() == '\n') lineCount++;
  }
  f.close();

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();

  char buf[128];
  snprintf(buf, sizeof(buf), "Lines: %d, Used: %.1f/%.1f MB",
           lineCount, usedBytes / 1048576.0, totalBytes / 1048576.0);
  return String(buf);
}

bool createCSVHeader() {
  if (!gSDReady || !gCurrentFile.length()) return false;

  File f = SD.open(gCurrentFile, FILE_WRITE, true);
  if (!f) return false;
  f.println(kCSVHeader);
  f.close();
  return true;
}

bool sdIsReady() {
  return gSDReady;
}