#include "storage/sd_logger.h"

#include "config/deployment_store.h"
#include "storage/csv_schema.h"
#include "system/pins.h"

static SPIClass gSDSPI(VSPI);
static bool gSDReady = false;
static bool gSDWriteError = false;

static const char* kSDReadingsFile = "/fieldmesh_readings.csv";
static const char* kSDDeploymentsFile = "/fieldmesh_deployments.csv";
static const char* kSDDeploymentsHeader =
    "eventId,nodeId,deploymentEpoch,startedUnix,endedUnix,userId,name,latitude,longitude";

static String csvCell(const char* value) {
  String out = "\"";
  if (value) {
    for (const char* p = value; *p; ++p) {
      if (*p == '"') out += '"';
      out += *p;
    }
  }
  out += '"';
  return out;
}

static bool ensureFileHeader(const char* path, const char* header) {
  if (SD.exists(path)) {
    File existing = SD.open(path, FILE_READ);
    if (!existing) return false;
    String found = existing.readStringUntil('\n');
    existing.close();
    found.trim();
    if (found == String(header)) return true;

    // Never overwrite an unrecognised field file. Move it aside under the first
    // unused legacy name so the operator can recover it from the card.
    for (uint16_t suffix = 1; suffix < 1000; ++suffix) {
      String legacy = String(path) + ".legacy-" + String(suffix);
      if (SD.exists(legacy)) continue;
      if (!SD.rename(path, legacy)) return false;
      Serial.printf("[SD] Preserved incompatible %s as %s\n",
                    path, legacy.c_str());
      break;
    }
    if (SD.exists(path)) return false;
  }

  File created = SD.open(path, FILE_WRITE);
  if (!created) return false;
  const size_t written = created.println(header);
  const bool failed = created.getWriteError() || written == 0;
  created.close();
  return !failed;
}

bool initSD() {
  gSDReady = false;
  gSDWriteError = false;
  gSDSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (!SD.begin(PIN_SD_CS, gSDSPI, 40000000)) {
    Serial.println("[SD] SD.begin() failed — no card or wiring issue");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[SD] No card attached");
    return false;
  }

  if (!ensureFileHeader(kSDReadingsFile, kCurrentCSVHeader35) ||
      !ensureFileHeader(kSDDeploymentsFile, kSDDeploymentsHeader)) {
    Serial.println("[SD] Could not prepare FieldMesh archive files");
    gSDWriteError = true;
    return false;
  }

  gSDReady = true;
  Serial.printf("[SD] Archive ready: %.1f MB total, %.1f MB used\n",
                SD.totalBytes() / 1048576.0,
                SD.usedBytes() / 1048576.0);
  return true;
}

bool sdIsReady() { return gSDReady; }
bool sdHadWriteError() { return gSDWriteError; }

bool sdLogCSVRow(const String& row) {
  if (!gSDReady) return false;
  File file = SD.open(kSDReadingsFile, FILE_APPEND);
  if (!file) {
    gSDWriteError = true;
    Serial.println("[SD] Could not open readings archive for append");
    return false;
  }
  const size_t written = file.println(row);
  const bool failed = file.getWriteError() || written != row.length() + 2;
  file.close();
  if (failed) {
    gSDWriteError = true;
    Serial.println("[SD] Readings archive write failed; LittleFS fallback remains active");
  }
  return !failed;
}

bool sdAppendDeploymentEvent(const char* nodeId, const DeploymentEvent& event) {
  if (!gSDReady || event.deploymentEndedUnix == 0) return false;

  // End is committed to LittleFS before the node is told to stop. A power loss
  // can therefore replay this archive write during recovery; key the CSV by the
  // deterministic event ID so recovery fills a missing row without duplicating
  // one that already reached the card.
  const String eventPrefix = csvCell(event.eventId) + ',';
  File existing = SD.open(kSDDeploymentsFile, FILE_READ);
  if (!existing) {
    gSDWriteError = true;
    return false;
  }
  existing.readStringUntil('\n');  // header
  while (existing.available()) {
    String line = existing.readStringUntil('\n');
    if (line.startsWith(eventPrefix)) {
      existing.close();
      return true;
    }
  }
  existing.close();

  String row;
  row.reserve(200);
  row += csvCell(event.eventId);
  row += ',';
  row += csvCell(nodeId);
  row += ',';
  row += String((unsigned)event.deploymentEpoch);
  row += ',';
  row += String(event.deploymentStartedUnix);
  row += ',';
  row += String(event.deploymentEndedUnix);
  row += ',';
  row += csvCell(event.userId);
  row += ',';
  row += csvCell(event.name);
  row += ',';
  if (!isnan(event.latitude) && !isnan(event.longitude) &&
      (event.latitude != 0.0f || event.longitude != 0.0f)) {
    row += String(event.latitude, 6);
    row += ',';
    row += String(event.longitude, 6);
  } else {
    row += ',';
  }

  File file = SD.open(kSDDeploymentsFile, FILE_APPEND);
  if (!file) {
    gSDWriteError = true;
    return false;
  }
  const size_t written = file.println(row);
  const bool failed = file.getWriteError() || written != row.length() + 2;
  file.close();
  if (failed) {
    gSDWriteError = true;
    Serial.println("[SD] Deployment archive write failed");
  }
  return !failed;
}

const char* sdReadingsPath() { return kSDReadingsFile; }
const char* sdDeploymentsPath() { return kSDDeploymentsFile; }

uint64_t sdReadingsFileSize() {
  if (!gSDReady || !SD.exists(kSDReadingsFile)) return 0;
  File file = SD.open(kSDReadingsFile, FILE_READ);
  if (!file) return 0;
  const uint64_t size = file.size();
  file.close();
  return size;
}

uint64_t sdTotalBytes() { return gSDReady ? SD.totalBytes() : 0; }
uint64_t sdUsedBytes() { return gSDReady ? SD.usedBytes() : 0; }
