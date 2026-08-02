// Upload queue / CSV schema migration regression suite.
//
// This file previously printed ten [SKIP] lines and counted them as failures,
// and its build env linked ONLY this file — no production code at all — so it
// could not have caught a schema-migration regression. It now links the real
// flash_logger / upload_queue / json_payload and asserts against them.
//
// Scope note: the assertions below deliberately avoid UploadQueue::init(),
// which reads and writes the NVS "tx" cursor. Clobbering a bench hub's real
// upload cursor to run a test is not a trade worth making. Purge/drain
// behaviour is covered by the bench acceptance run instead.
//
// /datalog.csv IS written here, so it is backed up and restored around the run.

#include <Arduino.h>
#include <LittleFS.h>

#include "storage/csv_schema.h"
#include "storage/flash_logger.h"
#include "storage/upload_queue.h"
#include "storage/json_payload.h"

static int gPass = 0, gFail = 0;

static bool check(const char* name, bool cond) {
  Serial.printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", name);
  cond ? ++gPass : ++gFail;
  return cond;
}

static const char* kDataFile   = "/datalog.csv";
static const char* kBackupFile = "/datalog_testbak.csv";

// A canonical 30-column row (pre-epoch) and its 31-column successor.
static const char* kRow30 =
    "2026-07-30T10:00:00,ENV_A1,42,0x0007,0,3,"
    "3.900,21.500,55.000,"
    "1.000,2.000,3.000,4.000,5.000,6.000,7.000,8.000,"
    "0.000,0.000,nan,nan,nan,nan,nan,nan,"
    "12000.000,6800.000,4.000,50.040,0.000";
static const char* kRow31 =
    "2026-07-30T11:00:00,ENV_A1,43,0x0007,0,3,"
    "3.900,21.500,55.000,"
    "1.000,2.000,3.000,4.000,5.000,6.000,7.000,8.000,"
    "0.000,0.000,nan,nan,nan,nan,nan,nan,"
    "12000.000,6800.000,4.000,50.040,0.000,2";

static void writeDataFile(const char* header, const char* rows) {
  File f = LittleFS.open(kDataFile, "w", true);
  if (!f) return;
  f.println(header);
  if (rows && rows[0]) f.print(rows);
  f.close();
}

static size_t columnCount(const String& row) {
  size_t n = 1;
  for (size_t i = 0; i < row.length(); ++i) {
    if (row[i] == ',') ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------

static void testSchemaConstants() {
  check("schema: current column count is 31", kCurrentCSVColumnCount == 31);
  check("schema: legacy 30 retained", kLegacyCSVColumnCount30 == 30);
  check("schema: legacy 25 retained", kLegacyCSVColumnCount == 25);
  check("schema: current header ends with deploymentEpoch",
        String(kCurrentCSVHeader31).endsWith(",deploymentEpoch"));
  check("schema: current header is the legacy-30 header plus one column",
        String(kCurrentCSVHeader31) ==
            String(kLegacyCSVHeader30) + ",deploymentEpoch");
  check("schema: legacy-30 header extends the legacy-25 header",
        String(kLegacyCSVHeader30).startsWith(String(kLegacyCSVHeader25)));
  check("schema: header column counts agree",
        columnCount(String(kCurrentCSVHeader31)) == kCurrentCSVColumnCount &&
        columnCount(String(kLegacyCSVHeader30)) == kLegacyCSVColumnCount30);
}

// F2: a queued pre-epoch backlog must be detectable, because those rows would
// otherwise follow the backend's fallback into whatever deployment is active.
static void testLegacyBacklogDetection() {
  uint32_t rows = 0;

  writeDataFile(kCurrentCSVHeader31, (String(kRow31) + "\n").c_str());
  check("legacy: a current-schema file reports no backlog",
        !uploadQueueHasLegacyRows(&rows));

  writeDataFile(kLegacyCSVHeader30, "");
  check("legacy: an EMPTY legacy file does not block (it upgrades in place)",
        !uploadQueueHasLegacyRows(&rows));

  const String threeRows = String(kRow30) + "\n" + kRow30 + "\n" + kRow30 + "\n";
  writeDataFile(kLegacyCSVHeader30, threeRows.c_str());
  check("legacy: a 30-column file WITH rows reports a backlog",
        uploadQueueHasLegacyRows(&rows));
  check("legacy: the pending row count is reported for the UI message",
        rows == 3);

  writeDataFile(kLegacyCSVHeader25, (String(kRow30) + "\n").c_str());
  check("legacy: a 25-column file with rows also reports a backlog",
        uploadQueueHasLegacyRows(&rows));
}

// The gate for deploymentTrackingVersion: never claim epoch support while
// unstamped rows are still queued.
static void testSchemaCurrentGate() {
  writeDataFile(kLegacyCSVHeader30, (String(kRow30) + "\n").c_str());
  check("gate: schema NOT current with a 30-column header",
        !flashCsvSchemaIsCurrent());

  writeDataFile(kLegacyCSVHeader25, (String(kRow30) + "\n").c_str());
  check("gate: schema NOT current with a 25-column header",
        !flashCsvSchemaIsCurrent());

  writeDataFile(kCurrentCSVHeader31, (String(kRow31) + "\n").c_str());
  check("gate: schema IS current with the 31-column header",
        flashCsvSchemaIsCurrent());
}

// Mixed-width rows coexist in one file while a legacy backlog drains, so both
// must parse and the short one must simply omit the newer key.
static void testMixedWidthRowsParse() {
  const String mixed = String(kLegacyCSVHeader30) + "\n" +
                       kRow30 + "\n" + kRow31 + "\n";
  JsonPayload json = buildJsonUpload(mixed, 100, "upload-queue-test", nullptr,
                                     1753000000UL);
  check("mixed: both rows build", json.ok && json.rowCount == 2);
  check("mixed: the 31-column row carries deploymentEpoch",
        json.body.indexOf("\"deploymentEpoch\":2") >= 0);

  // The legacy row must produce NO deploymentEpoch key at all — not 0, not
  // null — so the backend can fall back rather than trust a fabricated value.
  const int firstObjEnd = json.body.indexOf("},{");
  const String firstObj = firstObjEnd > 0 ? json.body.substring(0, firstObjEnd)
                                          : json.body;
  check("mixed: the 30-column row omits deploymentEpoch entirely",
        firstObj.indexOf("deploymentEpoch") < 0);
  check("mixed: the legacy row still carries its other columns",
        firstObj.indexOf("spectral_saturated") >= 0);
  check("mixed: consumed byte count covers both rows",
        json.csvBytesConsumed == strlen(kRow30) + 1 + strlen(kRow31) + 1);
}

// A row written by the logger must round-trip to the JSON key the backend reads.
static void testStampedRowRoundTrip() {
  DecodedSnapshot snap{};
  strncpy(snap.nodeId, "ENV_A1", sizeof(snap.nodeId) - 1);
  snap.nodeTimestamp = 1753000000UL;
  snap.seqNum = 7;
  snap.sensorPresent = 0x0003;
  snap.deploymentEpoch = 5;
  snap.readingCount = 1;
  snap.readings[0].sensorId = SENSOR_ID_AIR_TEMP;
  snap.readings[0].value = 21.5f;

  String row;
  check("roundtrip: a decoded snapshot formats",
        formatDecodedSnapshotCSVRow(snap, row));
  check("roundtrip: the row has 31 columns",
        columnCount(row) == kCurrentCSVColumnCount);
  check("roundtrip: the row ends with the stamped epoch", row.endsWith(",5"));

  const String chunk = String(kCurrentCSVHeader31) + "\n" + row + "\n";
  JsonPayload json = buildJsonUpload(chunk, 1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: it builds one reading", json.ok && json.rowCount == 1);
  check("roundtrip: the epoch reaches the payload",
        json.body.indexOf("\"deploymentEpoch\":5") >= 0);

  // 0 is the legacy/unresolved sentinel and must survive as an explicit 0
  // rather than being dropped, so the backend can tell it apart from absent.
  snap.deploymentEpoch = 0;
  String row0;
  formatDecodedSnapshotCSVRow(snap, row0);
  check("roundtrip: an unresolved epoch is written as 0", row0.endsWith(",0"));
  JsonPayload json0 = buildJsonUpload(String(kCurrentCSVHeader31) + "\n" + row0 + "\n",
                                      1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: an unresolved epoch reaches the payload as 0",
        json0.body.indexOf("\"deploymentEpoch\":0") >= 0);
}

static void testUploadAckCompatibilityHookKeepsCurrentHistory() {
  writeDataFile(kCurrentCSVHeader31, (String(kRow31) + "\n").c_str());
  File beforeFile = LittleFS.open(kDataFile, "r");
  const size_t before = beforeFile ? beforeFile.size() : 0;
  if (beforeFile) beforeFile.close();

  UploadQueue queue;
  check("retention: compatibility hook accepts a current-schema file",
        queue.purgeUploadedIfLegacyDrained());
  File afterFile = LittleFS.open(kDataFile, "r");
  const size_t after = afterFile ? afterFile.size() : 0;
  if (afterFile) afterFile.close();
  check("retention: upload acknowledgement does not delete current local history",
        before > 0 && after == before);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== test_upload_queue (CSV schema migration) ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[FAIL] LittleFS mount failed — cannot run");
    Serial.println("RESULT|SUMMARY|0/0|OVERALL:FAIL");
    return;
  }

  // Preserve any real buffered readings on a bench hub.
  const bool hadData = LittleFS.exists(kDataFile);
  if (hadData) {
    LittleFS.remove(kBackupFile);
    LittleFS.rename(kDataFile, kBackupFile);
  }

  testSchemaConstants();
  testLegacyBacklogDetection();
  testSchemaCurrentGate();
  testMixedWidthRowsParse();
  testStampedRowRoundTrip();
  testUploadAckCompatibilityHookKeepsCurrentHistory();

  LittleFS.remove(kDataFile);
  if (hadData) LittleFS.rename(kBackupFile, kDataFile);

  const int total = gPass + gFail;
  Serial.printf("\nRESULT|SUMMARY|%d/%d|OVERALL:%s\n",
                gPass, total, gFail == 0 ? "PASS" : "FAIL");
}

void loop() {}
