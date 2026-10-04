// SD archive pathway regression suite.
//
// Exercises the real production storage/sd_logger.cpp — not a raw SPI
// bring-up sketch — because the thing that needed fixing was the archive
// LOGIC (header recognition, legacy-file preservation, idempotent deployment
// replay), not the wiring. sd_logger.cpp is self-contained (only pulls in
// header-only constants from csv_schema.h / deployment_store.h / pins.h), so
// it links here with no other production module required.
//
// Needs a physical SD card in the hub and nothing else: no SIM, no antenna,
// no modem, no WiFi, no ESP-NOW peer. Requires FieldMesh acceptance doc item
// 5 ("insert/remove/fill an SD card and interrupt power around snapshot and
// End writes") — this suite covers the software-observable half of that (header
// recognition, legacy preservation, dedup). The power-interruption half is
// physical and MUST still be done by hand; see the manual checklist printed
// at the end of the run.
//
// /fieldmesh_readings.csv and /fieldmesh_deployments.csv on the card ARE
// touched here, so any pre-existing archive is backed up before the run and
// restored after, same pattern as test_upload_queue.cpp uses for /datalog.csv.

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config/deployment_store.h"
#include "storage/csv_schema.h"
#include "storage/sd_logger.h"
#include "system/pins.h"

static int gPass = 0, gFail = 0;

static bool check(const char* name, bool cond) {
  Serial.printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", name);
  cond ? ++gPass : ++gFail;
  return cond;
}

static const char* kReadingsFile    = "/fieldmesh_readings.csv";
static const char* kDeploymentsFile = "/fieldmesh_deployments.csv";
static const char* kReadingsBackup    = "/fieldmesh_readings.csv.testbak";
static const char* kDeploymentsBackup = "/fieldmesh_deployments.csv.testbak";

static const char* kRow1 =
    "2026-09-27T10:00:00,ENV_A1,1,0x0007,0,3,"
    "3.900,21.500,55.000,"
    "1.000,2.000,3.000,4.000,5.000,6.000,7.000,8.000,"
    "0.000,0.000,nan,nan,nan,nan,nan,nan,"
    "12000.000,6800.000,4.000,50.040,0.000,1,001,North Hedge,nan,nan";
static const char* kRow2 =
    "2026-09-27T10:15:00,ENV_A1,2,0x0007,0,3,"
    "3.895,21.600,54.800,"
    "1.100,2.100,3.100,4.100,5.100,6.100,7.100,8.100,"
    "0.500,10.000,nan,nan,nan,nan,nan,nan,"
    "12010.000,6810.000,4.000,50.040,0.000,1,001,North Hedge,nan,nan";

static String readLine(File& f) {
  String line = f.readStringUntil('\n');
  line.trim();
  return line;
}

static int countLines(const char* path) {
  if (!SD.exists(path)) return -1;
  File f = SD.open(path, FILE_READ);
  if (!f) return -1;
  int n = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    ++n;
  }
  f.close();
  return n;
}

static void backupIfPresent(const char* path, const char* backupPath) {
  if (SD.exists(path)) {
    SD.remove(backupPath);
    SD.rename(path, backupPath);
  }
}

static void restoreBackup(const char* path, const char* backupPath) {
  SD.remove(path);
  // Also sweep any .legacy-N files this run created so repeat runs stay clean.
  for (uint16_t suffix = 1; suffix < 1000; ++suffix) {
    String legacy = String(path) + ".legacy-" + String(suffix);
    if (SD.exists(legacy)) SD.remove(legacy.c_str());
  }
  if (SD.exists(backupPath)) SD.rename(backupPath, path);
}

// ---------------------------------------------------------------------------

static void testFreshMountCreatesCanonicalFiles() {
  check("initSD() succeeds with a card present", initSD());
  check("sdIsReady() true after mount", sdIsReady());
  check("sdHadWriteError() false on a clean mount", !sdHadWriteError());

  File readings = SD.open(kReadingsFile, FILE_READ);
  const bool haveReadings = (bool)readings;
  const String readingsHeader = haveReadings ? readLine(readings) : String();
  if (haveReadings) readings.close();
  check("readings file created", haveReadings);
  check("readings header matches kCurrentCSVHeader35",
        readingsHeader == String(kCurrentCSVHeader35));

  File deployments = SD.open(kDeploymentsFile, FILE_READ);
  const bool haveDeployments = (bool)deployments;
  if (haveDeployments) deployments.close();
  check("deployments file created", haveDeployments);

  check("readings path accessor matches", String(sdReadingsPath()) == String(kReadingsFile));
  check("deployments path accessor matches", String(sdDeploymentsPath()) == String(kDeploymentsFile));
}

static void testReadingsRowRoundTrip() {
  const uint64_t sizeBefore = sdReadingsFileSize();
  check("sdLogCSVRow row 1 succeeds", sdLogCSVRow(kRow1));
  check("sdLogCSVRow row 2 succeeds", sdLogCSVRow(kRow2));
  const uint64_t sizeAfter = sdReadingsFileSize();
  check("file size grew by exactly the two appended rows",
        sizeAfter == sizeBefore + strlen(kRow1) + 2 + strlen(kRow2) + 2);

  File f = SD.open(kReadingsFile, FILE_READ);
  bool contentOk = false;
  if (f) {
    readLine(f);  // header, already checked above
    const String line1 = readLine(f);
    const String line2 = readLine(f);
    contentOk = (line1 == String(kRow1)) && (line2 == String(kRow2));
    f.close();
  }
  check("readings round-trip byte-for-byte, in order", contentOk);
  check("sdHadWriteError() still false after normal writes", !sdHadWriteError());
}

static void testDeploymentEventDedup() {
  DeploymentEvent ev = {};
  strncpy(ev.eventId, "TESTEVT001", sizeof(ev.eventId) - 1);
  strncpy(ev.nodeId, "ENV_A1", sizeof(ev.nodeId) - 1);
  ev.deploymentEpoch = 1;
  ev.deploymentStartedUnix = 1790000000;
  ev.deploymentEndedUnix = 1790003600;
  strncpy(ev.userId, "001", sizeof(ev.userId) - 1);
  strncpy(ev.name, "North Hedge", sizeof(ev.name) - 1);
  ev.latitude = NAN;
  ev.longitude = NAN;

  const int linesBefore = countLines(kDeploymentsFile);
  check("first append of a new event succeeds", sdAppendDeploymentEvent(ev.nodeId, ev));
  const int linesAfterFirst = countLines(kDeploymentsFile);
  check("first append adds exactly one line", linesAfterFirst == linesBefore + 1);

  // Same eventId again — simulates the End-write replay a power loss between
  // the LittleFS commit and the SD mirror write can produce.
  check("replayed append of the same eventId still reports success",
        sdAppendDeploymentEvent(ev.nodeId, ev));
  const int linesAfterReplay = countLines(kDeploymentsFile);
  check("replayed append does not duplicate the row", linesAfterReplay == linesAfterFirst);

  DeploymentEvent openEv = ev;
  strncpy(openEv.eventId, "TESTEVT002", sizeof(openEv.eventId) - 1);
  openEv.deploymentEndedUnix = 0;  // still open — must not be archived
  check("an open (unended) deployment is not archived",
        !sdAppendDeploymentEvent(openEv.nodeId, openEv));
  check("archive line count unchanged by the open-deployment attempt",
        countLines(kDeploymentsFile) == linesAfterReplay);
}

static void testIncompatibleHeaderIsPreservedNotOverwritten() {
  // Simulate a canonical-named file left behind by a firmware version this
  // build doesn't recognise — the exact scenario ensureFileHeader() guards.
  SD.remove(kReadingsFile);
  File foreign = SD.open(kReadingsFile, FILE_WRITE);
  bool wrote = false;
  if (foreign) {
    foreign.println("datetime,nodeId,someFutureColumn");
    foreign.println("2099-01-01T00:00:00,FUTURE_NODE,999");
    foreign.close();
    wrote = true;
  }
  check("foreign canonical-named file staged", wrote);

  check("re-running initSD() still reports success", initSD());
  check("sdHadWriteError() false — foreign file was preserved, not fatal",
        !sdHadWriteError());

  const bool legacyExists = SD.exists("/fieldmesh_readings.csv.legacy-1");
  check("foreign file moved aside as .legacy-1", legacyExists);

  bool legacyContentIntact = false;
  if (legacyExists) {
    File legacy = SD.open("/fieldmesh_readings.csv.legacy-1", FILE_READ);
    if (legacy) {
      const String header = readLine(legacy);
      const String row = readLine(legacy);
      legacyContentIntact = (header == "datetime,nodeId,someFutureColumn") &&
                             (row == "2099-01-01T00:00:00,FUTURE_NODE,999");
      legacy.close();
    }
  }
  check("preserved legacy file content is byte-for-byte intact", legacyContentIntact);

  File fresh = SD.open(kReadingsFile, FILE_READ);
  bool freshOk = false;
  if (fresh) {
    const String header = readLine(fresh);
    freshOk = (header == String(kCurrentCSVHeader35)) && !fresh.available();
    fresh.close();
  }
  check("a fresh canonical file with the current header replaces it", freshOk);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== test_sd_logger (SD archive pathway) ===");

  SPIClass* preflightSPI = new SPIClass(VSPI);
  preflightSPI->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  const bool cardPresent = SD.begin(PIN_SD_CS, *preflightSPI, 40000000) &&
                            SD.cardType() != CARD_NONE;
  if (!cardPresent) {
    Serial.println("[FAIL] No SD card detected — insert a card and re-run");
    Serial.println("RESULT|SUMMARY|0/0|OVERALL:FAIL");
    return;
  }
  Serial.printf("[SD] Card detected: %.1f MB total\n", SD.totalBytes() / 1048576.0);

  const bool hadReadings = SD.exists(kReadingsFile);
  const bool hadDeployments = SD.exists(kDeploymentsFile);
  backupIfPresent(kReadingsFile, kReadingsBackup);
  backupIfPresent(kDeploymentsFile, kDeploymentsBackup);
  if (hadReadings || hadDeployments) {
    Serial.println("[SD] Existing archive backed up for the duration of this run");
  }

  testFreshMountCreatesCanonicalFiles();
  testReadingsRowRoundTrip();
  testDeploymentEventDedup();
  testIncompatibleHeaderIsPreservedNotOverwritten();

  restoreBackup(kReadingsFile, kReadingsBackup);
  restoreBackup(kDeploymentsFile, kDeploymentsBackup);
  Serial.println("[SD] Original archive (if any) restored; test files removed");

  const int total = gPass + gFail;
  Serial.printf("\nRESULT|SUMMARY|%d/%d|OVERALL:%s\n",
                gPass, total, gFail == 0 ? "PASS" : "FAIL");

  Serial.println("\n--- Still required by hand (physical, not automatable) ---");
  Serial.println("1. Pull the card, boot, confirm sdIsReady()=false and the hub");
  Serial.println("   still logs to LittleFS with no crash (check Data page).");
  Serial.println("2. Re-insert mid-session, confirm the archive resumes without");
  Serial.println("   re-copying rows already written to LittleFS-only.");
  Serial.println("3. Cut power mid-write to fieldmesh_readings.csv (during a real");
  Serial.println("   snapshot on -main firmware) and confirm the file still opens");
  Serial.println("   with only the interrupted row possibly short, header intact.");
  Serial.println("4. Cut power between a real End-deployment's LittleFS commit and");
  Serial.println("   its SD mirror write, reboot, force a retry, confirm exactly");
  Serial.println("   one archived row for that event (this suite proved the dedup");
  Serial.println("   logic; this step proves it survives a real power cut).");
  Serial.println("5. Fill the card near-full and confirm writes fail loudly");
  Serial.println("   (sdHadWriteError()=true, surfaced on the Data page) rather");
  Serial.println("   than silently dropping rows.");
}

void loop() {}
