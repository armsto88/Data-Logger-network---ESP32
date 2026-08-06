// Upload queue / CSV schema migration regression suite.
//
// This file previously printed ten [SKIP] lines and counted them as failures,
// and its build env linked ONLY this file — no production code at all — so it
// could not have caught a schema-migration regression. It now links the real
// flash_logger / upload_queue / json_payload and asserts against them.
//
// Scope note: the schema assertions below deliberately avoid
// UploadQueue::init(), which reads the NVS "tx" cursor. Clobbering a bench
// hub's real upload cursor to run a test is not a trade worth making.
// Purge/drain behaviour is covered by the bench acceptance run instead.
// init()'s own contract — idempotency and the retryable failure path — IS
// covered, at the end of this file. Those cases DO drive the real cursor path,
// so they back up and restore the NVS cursor keys around themselves; see the
// note there.
//
// /datalog.csv IS written here, so it is backed up and restored around the run.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "storage/csv_schema.h"
#include "storage/flash_logger.h"
#include "storage/upload_queue.h"
#include "storage/json_payload.h"
#include "config/node_registry.h"

static int gPass = 0, gFail = 0;

static bool check(const char* name, bool cond) {
  Serial.printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", name);
  cond ? ++gPass : ++gFail;
  return cond;
}

static const char* kDataFile   = "/datalog.csv";
static const char* kBackupFile = "/datalog_testbak.csv";

// A canonical 30-column row (pre-epoch), its 31-column successor (epoch
// stamped), and the 33-column identity-stamped row that precedes latitude/
// longitude.
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
// The header ACTUALLY shipped in the field (commit ea98b05, before any of the
// userId/name/location work): deploymentEpoch appended, nothing else. A real
// hub upgrading straight from that firmware carries this exact 31-column file
// on disk — this is not a synthetic case, see testRealFieldHeaderIsRecognised.
static const char* kRow31Field =
    "2026-07-30T09:00:00,ENV_A1,41,0x0007,0,3,"
    "3.900,21.500,55.000,"
    "1.000,2.000,3.000,4.000,5.000,6.000,7.000,8.000,"
    "0.000,0.000,nan,nan,nan,nan,nan,nan,"
    "12000.000,6800.000,4.000,50.040,0.000,1";
static const char* kRow33 =
    "2026-07-30T12:00:00,ENV_A1,44,0x0007,0,3,"
    "3.900,21.500,55.000,"
    "1.000,2.000,3.000,4.000,5.000,6.000,7.000,8.000,"
    "0.000,0.000,nan,nan,nan,nan,nan,nan,"
    "12000.000,6800.000,4.000,50.040,0.000,2,001,North Hedge";

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
  check("schema: current column count is 35", kCurrentCSVColumnCount == 35);
  check("schema: legacy 33 retained", kLegacyCSVColumnCount33 == 33);
  check("schema: legacy 30 retained", kLegacyCSVColumnCount30 == 30);
  check("schema: legacy 25 retained", kLegacyCSVColumnCount == 25);
  check("schema: current header ends with latitude,longitude",
        String(kCurrentCSVHeader35).endsWith(",latitude,longitude"));
  check("schema: current header keeps identity columns before location",
        String(kCurrentCSVHeader35)
            .endsWith(",deploymentEpoch,userId,name,latitude,longitude") &&
        String(kCurrentCSVHeader35)
            .indexOf(",deploymentEpoch,userId,name,latitude,longitude") > 0);
  check("schema: legacy-33 header extends the legacy-30 header",
        String(kLegacyCSVHeader33).startsWith(String(kLegacyCSVHeader30)));
  check("schema: legacy-30 header extends the legacy-25 header",
        String(kLegacyCSVHeader30).startsWith(String(kLegacyCSVHeader25)));
  check("schema: header column counts agree",
        columnCount(String(kCurrentCSVHeader35)) == kCurrentCSVColumnCount &&
        columnCount(String(kLegacyCSVHeader33)) == kLegacyCSVColumnCount33 &&
        columnCount(String(kLegacyCSVHeader30)) == kLegacyCSVColumnCount30);
}

// The exact regression seen on real hardware: a hub carrying the field-shipped
// 31-column file (ea98b05, deploymentEpoch appended, no identity/location)
// upgrades to this firmware and calls initFlash(). Before kLegacyCSVHeader31
// existed as a real constant, this header matched NOTHING recognised, so
// initFlash() hit the "Unknown CSV header" branch and set gFlashReady=false —
// every snapshot for the rest of the session then logged "No storage accepted
// the snapshot" and nothing reached flash or SD.
static void testRealFieldHeaderIsRecognised() {
  const String threeRows = String(kRow31Field) + "\n" + kRow31Field + "\n";
  writeDataFile(kLegacyCSVHeader31, threeRows.c_str());

  check("field-header: initFlash() succeeds on the real shipped 31-column file",
        initFlash());
  check("field-header: flash logging is NOT disabled by an unrecognised header",
        flashIsReady());

  // hasDataRows=true -> preserved as-is (not upgraded, not deleted) until the
  // upload queue drains it. Confirms the header on disk is unchanged, i.e. the
  // "Unknown header, refusing to touch the file" branch did not fire.
  File f = LittleFS.open(kDataFile, "r");
  String firstLine = f ? f.readStringUntil('\n') : String();
  if (f) f.close();
  firstLine.trim();
  check("field-header: the on-disk header is preserved verbatim while queued",
        firstLine == String(kLegacyCSVHeader31));

  uint32_t rows = 0;
  check("field-header: the real 31-column backlog is detected for the UI",
        uploadQueueHasLegacyRows(&rows) && rows == 2);
}

// F2: a queued pre-epoch backlog must be detectable, because those rows would
// otherwise follow the backend's fallback into whatever deployment is active.
static void testLegacyBacklogDetection() {
  uint32_t rows = 0;

  writeDataFile(kCurrentCSVHeader35, (String(kRow31) + "\n").c_str());
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

  writeDataFile(kLegacyCSVHeader33, "");
  check("legacy: an EMPTY 33-column file does not block (it upgrades in place)",
        !uploadQueueHasLegacyRows(&rows));

  writeDataFile(kLegacyCSVHeader33, (String(kRow33) + "\n").c_str());
  check("legacy: a 33-column file WITH rows reports a backlog",
        uploadQueueHasLegacyRows(&rows));
  check("legacy: the pending row count is reported for a 33-column backlog",
        rows == 1);
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

  writeDataFile(kLegacyCSVHeader33, (String(kRow33) + "\n").c_str());
  check("gate: schema NOT current with a 33-column header",
        !flashCsvSchemaIsCurrent());

  writeDataFile(kCurrentCSVHeader35, (String(kRow33) + ",nan,nan\n").c_str());
  check("gate: schema IS current with the 35-column header",
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

  setNodeUserId("ENV_A1", "001");
  setNodeName("ENV_A1", "North Hedge");

  String row;
  check("roundtrip: a decoded snapshot formats",
        formatDecodedSnapshotCSVRow(snap, row));
  check("roundtrip: the row has 35 columns",
        columnCount(row) == kCurrentCSVColumnCount);
  check("roundtrip: the row carries the stamped epoch, identity, and (unset) location",
        row.endsWith(",5,001,North Hedge,nan,nan"));

  const String chunk = String(kCurrentCSVHeader35) + "\n" + row + "\n";
  JsonPayload json = buildJsonUpload(chunk, 1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: it builds one reading", json.ok && json.rowCount == 1);
  check("roundtrip: the epoch reaches the payload",
        json.body.indexOf("\"deploymentEpoch\":5") >= 0);
  check("roundtrip: unset location reaches the payload as null",
        json.body.indexOf("\"latitude\":null") >= 0 &&
        json.body.indexOf("\"longitude\":null") >= 0);

  // A registered node's location must round-trip into the row and payload.
  registeredNodes.push_back(NodeInfo{});
  registeredNodes.back().nodeId    = "ENV_A1";
  registeredNodes.back().latitude  = -27.469771f;
  registeredNodes.back().longitude = 153.025124f;
  String rowWithLoc;
  formatDecodedSnapshotCSVRow(snap, rowWithLoc);
  registeredNodes.clear();
  check("roundtrip: a registered node's coordinates are stamped at 6dp",
        rowWithLoc.endsWith(",5,001,North Hedge,-27.469771,153.025124"));
  JsonPayload jsonLoc = buildJsonUpload(String(kCurrentCSVHeader35) + "\n" + rowWithLoc + "\n",
                                        1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: the stamped location reaches the payload",
        jsonLoc.body.indexOf("\"latitude\":-27.469771") >= 0 &&
        jsonLoc.body.indexOf("\"longitude\":153.025124") >= 0);

  // A name containing a comma must NOT re-frame the row. The Field UI accepts
  // any character here, and an unsanitised comma pushed the row to 36 columns,
  // failing the column gate — which dropped the reading from flash AND SD and
  // NACKed the node into re-sending it forever.
  setNodeName("ENV_A1", "Plot A, North");
  String rowComma;
  const bool commaFormatted = formatDecodedSnapshotCSVRow(snap, rowComma);
  check("roundtrip: a name containing a comma still formats", commaFormatted);
  check("roundtrip: a comma in the name does not add a column",
        columnCount(rowComma) == kCurrentCSVColumnCount);
  check("roundtrip: the comma is replaced, not dropped",
        rowComma.indexOf("Plot A  North") >= 0);
  JsonPayload jsonComma = buildJsonUpload(String(kCurrentCSVHeader35) + "\n" + rowComma + "\n",
                                          1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: a comma-named node still uploads",
        jsonComma.ok && jsonComma.rowCount == 1);
  setNodeName("ENV_A1", "North Hedge");

  // 0 is the legacy/unresolved sentinel and must survive as an explicit 0
  // rather than being dropped, so the backend can tell it apart from absent.
  snap.deploymentEpoch = 0;
  String row0;
  formatDecodedSnapshotCSVRow(snap, row0);
  check("roundtrip: an unresolved epoch is written as 0",
        row0.endsWith(",0,001,North Hedge,nan,nan"));
  JsonPayload json0 = buildJsonUpload(String(kCurrentCSVHeader35) + "\n" + row0 + "\n",
                                      1, "roundtrip", nullptr, 1753000000UL);
  check("roundtrip: an unresolved epoch reaches the payload as 0",
        json0.body.indexOf("\"deploymentEpoch\":0") >= 0);
}

// A node reporting absurd-but-finite sensor values must be REJECTED, not allowed
// to run the row builder's offset past the end of its stack buffer. Before the
// bounded-append fix, `snprintf(row + n, sizeof(row) - n, ...)` with n > sizeof
// computed a destination past the array and a size_t size that wrapped to ~4 GB,
// smashing the stack — observed as a Guru Meditation reboot every time the
// offending node reported, and a reboot loop once it retried the unacked reading.
//
// NaN and infinity are already safe (they format to 3-4 characters); only large
// FINITE values are wide enough to overrun, so that is what this drives.
static void testOversizedRowIsRejectedNotOverflowed() {
  const uint16_t kAllSensorIds[] = {
    SENSOR_ID_BAT_V, SENSOR_ID_AIR_TEMP, SENSOR_ID_AIR_RH,
    SENSOR_ID_SPECTRAL_415, SENSOR_ID_SPECTRAL_445, SENSOR_ID_SPECTRAL_480,
    SENSOR_ID_SPECTRAL_515, SENSOR_ID_SPECTRAL_555, SENSOR_ID_SPECTRAL_590,
    SENSOR_ID_SPECTRAL_630, SENSOR_ID_SPECTRAL_680,
    SENSOR_ID_WIND_SPEED, SENSOR_ID_WIND_DIR,
    SENSOR_ID_SOIL1_VWC, SENSOR_ID_SOIL1_TEMP,
    SENSOR_ID_SOIL2_VWC, SENSOR_ID_SOIL2_TEMP,
    SENSOR_ID_AUX1, SENSOR_ID_AUX2,
    SENSOR_ID_SPECTRAL_CLEAR, SENSOR_ID_SPECTRAL_NIR, SENSOR_ID_SPECTRAL_GAIN,
    SENSOR_ID_SPECTRAL_ATIME, SENSOR_ID_SPECTRAL_SAT,
  };
  const size_t kCount = sizeof(kAllSensorIds) / sizeof(kAllSensorIds[0]);

  DecodedSnapshot huge{};
  strncpy(huge.nodeId, "ENV_A1", sizeof(huge.nodeId) - 1);
  huge.nodeTimestamp = 1753000000UL;
  huge.seqNum = 99;
  huge.deploymentEpoch = 1;
  huge.readingCount = 0;
  for (size_t i = 0; i < kCount && i < MAX_READINGS_PER_SNAPSHOT; ++i) {
    huge.readings[huge.readingCount].sensorId = kAllSensorIds[i];
    huge.readings[huge.readingCount].value    = 3.4e38f;  // float32 max, ~43 chars at %.3f
    huge.readingCount++;
  }
  // A 32-character name, the widest the Field UI allows, so the identity and
  // location columns are at their worst case too.
  setNodeName("ENV_A1", "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345");

  String oversized;
  const bool formatted = formatDecodedSnapshotCSVRow(huge, oversized);
  // Reaching this line at all is the point: the old code corrupted the stack here.
  check("overflow: an oversized row is rejected, not written", !formatted);

  // The builder must still be usable afterwards — proves nothing was clobbered.
  setNodeName("ENV_A1", "North Hedge");
  DecodedSnapshot normal{};
  strncpy(normal.nodeId, "ENV_A1", sizeof(normal.nodeId) - 1);
  normal.nodeTimestamp = 1753000000UL;
  normal.seqNum = 100;
  normal.deploymentEpoch = 1;
  normal.readingCount = 1;
  normal.readings[0].sensorId = SENSOR_ID_AIR_TEMP;
  normal.readings[0].value = 21.5f;
  String after;
  check("overflow: the builder still works after a rejected row",
        formatDecodedSnapshotCSVRow(normal, after) &&
        columnCount(after) == kCurrentCSVColumnCount);
}

static void testUploadAckCompatibilityHookKeepsCurrentHistory() {
  writeDataFile(kCurrentCSVHeader35, (String(kRow31) + "\n").c_str());
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

// --- init() contract -------------------------------------------------------
//
// The scope note at the top of this file says the schema assertions avoid
// init() because it touches the NVS "tx" cursor. That reasoning still holds for
// them, but it left init() itself — including the failure path every caller now
// has to honour — with no coverage at all.
//
// These cases DO drive the real cursor path, and that path can WRITE:
// validateCursor() calls saveCursor() whenever the stored offset does not match
// the file on disk. The fixture written here is a one-row file, so on any bench
// hub carrying genuine upload history the stored offset is far beyond it and
// that reset branch fires — persisting a wiped cursor to the live namespace and
// making the hub re-send its whole retained backlog at the next sync.
//
// So the cursor keys are saved and put back afterwards, exactly as /datalog.csv
// is around the whole run. Restoring is verified, not assumed: a silent failure
// to put the cursor back is the one outcome worse than not running the test.
static const char* kTxNamespaceForTest = "tx";

// Every key saveCursor() writes. validateCursor() -> saveCursor() is the write
// path that makes this necessary; the poison keys are not touched by it.
struct SavedCursorNvs {
  bool     hadOffset, hadRows, hadLastUpload, hadRetry,
           hadWake, hadNextAttempt, hadLocalRemoved;
  uint32_t offset, rows, lastUpload, wake, nextAttempt, localRemoved;
  uint8_t  retry;
  bool     opened;
};

static void backupCursorNvs(SavedCursorNvs& b) {
  b = SavedCursorNvs{};
  Preferences p;
  if (!p.begin(kTxNamespaceForTest, true)) {   // read-only
    Serial.println("[WARN] could not open \"tx\" to back up the cursor");
    return;
  }
  b.opened = true;
  b.hadOffset       = p.isKey("cursor_offset"); b.offset       = p.getUInt("cursor_offset", 0);
  b.hadRows         = p.isKey("rows_uploaded"); b.rows         = p.getUInt("rows_uploaded", 0);
  b.hadLastUpload   = p.isKey("last_upload");   b.lastUpload   = p.getUInt("last_upload", 0);
  b.hadRetry        = p.isKey("retry_count");   b.retry        = p.getUChar("retry_count", 0);
  b.hadWake         = p.isKey("wake_counter");  b.wake         = p.getUInt("wake_counter", 0);
  b.hadNextAttempt  = p.isKey("next_attempt");  b.nextAttempt  = p.getUInt("next_attempt", 0);
  b.hadLocalRemoved = p.isKey("local_removed"); b.localRemoved = p.getUInt("local_removed", 0);
  p.end();
}

// A key absent before the run is removed again, not written as 0 — otherwise a
// fresh hub would come out of the test with a cursor it never had.
static void restoreCursorNvs(const SavedCursorNvs& b) {
  if (!b.opened) return;
  Preferences p;
  if (!p.begin(kTxNamespaceForTest, false)) {   // read-write
    check("init: cursor NVS restored after the init cases", false);
    return;
  }
  if (b.hadOffset)       p.putUInt("cursor_offset", b.offset);       else p.remove("cursor_offset");
  if (b.hadRows)         p.putUInt("rows_uploaded", b.rows);         else p.remove("rows_uploaded");
  if (b.hadLastUpload)   p.putUInt("last_upload", b.lastUpload);     else p.remove("last_upload");
  if (b.hadRetry)        p.putUChar("retry_count", b.retry);         else p.remove("retry_count");
  if (b.hadWake)         p.putUInt("wake_counter", b.wake);          else p.remove("wake_counter");
  if (b.hadNextAttempt)  p.putUInt("next_attempt", b.nextAttempt);   else p.remove("next_attempt");
  if (b.hadLocalRemoved) p.putUInt("local_removed", b.localRemoved); else p.remove("local_removed");

  const bool restored =
      p.getUInt("cursor_offset", 0) == (b.hadOffset ? b.offset : 0) &&
      p.getUInt("rows_uploaded", 0) == (b.hadRows ? b.rows : 0) &&
      p.getUInt("last_upload", 0)   == (b.hadLastUpload ? b.lastUpload : 0) &&
      p.getUChar("retry_count", 0)  == (b.hadRetry ? b.retry : 0);
  p.end();
  check("init: cursor NVS restored after the init cases", restored);
}

static void testInitIsIdempotent() {
  writeDataFile(kCurrentCSVHeader35, (String(kRow31) + "\n").c_str());

  UploadQueue queue;
  check("init: first call succeeds", queue.init());
  check("init: reports initialised after success", queue.isInitialised());
  const UploadCursor first = queue.getCursor();

  // A settings page render calls init() again. It must be a no-op returning
  // true, not a repeat of recovery + NVS read (that is the "one [UQ] init: per
  // boot" behaviour the bench check looks for).
  check("init: second call is idempotent and still succeeds", queue.init());
  const UploadCursor second = queue.getCursor();
  check("init: idempotent call leaves the cursor unchanged",
        first.byteOffset == second.byteOffset &&
        first.rowsUploaded == second.rowsUploaded &&
        first.lastUploadUnix == second.lastUploadUnix);
}

#ifdef UQ_TEST_INIT_FAILURE_HOOK
static void testFailedInitIsRetryableAndConsumesNothing() {
  writeDataFile(kCurrentCSVHeader35, (String(kRow31) + "\n").c_str());

  UploadQueue queue;
  UploadQueue::testForceInitFailure(true);
  const bool failed = queue.init();
  check("init: injected failure is reported to the caller", !failed);
  check("init: failed init does not mark the queue initialised",
        !queue.isInitialised());

  // The caller's obligation: having seen false, it performs no queue operation.
  // What the test can assert is the other half — the queue did not quietly
  // half-initialise and start serving a cursor as if it were real.
  const UploadCursor afterFail = queue.getCursor();
  check("init: failed init leaves a default cursor, not a loaded one",
        afterFail.byteOffset == 0 && afterFail.rowsUploaded == 0 &&
        afterFail.lastUploadUnix == 0);

  // Retryable: clearing the injected fault and calling again must succeed,
  // rather than the object being permanently wedged by one bad boot.
  UploadQueue::testForceInitFailure(false);
  check("init: a later call retries after a failure", queue.init());
  check("init: retry marks the queue initialised", queue.isInitialised());
}
#endif

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
  testRealFieldHeaderIsRecognised();
  testLegacyBacklogDetection();
  testSchemaCurrentGate();
  testMixedWidthRowsParse();
  testStampedRowRoundTrip();
  testOversizedRowIsRejectedNotOverflowed();
  testUploadAckCompatibilityHookKeepsCurrentHistory();

  // The init() cases drive the real cursor path, which can write to the live
  // "tx" cursor (see the note above them). Save it, run them, put it back.
  {
    SavedCursorNvs cursorBak;
    backupCursorNvs(cursorBak);
    testInitIsIdempotent();
#ifdef UQ_TEST_INIT_FAILURE_HOOK
    testFailedInitIsRetryableAndConsumesNothing();
#else
    Serial.println("[SKIP] failed-init case needs -D UQ_TEST_INIT_FAILURE_HOOK");
#endif
    restoreCursorNvs(cursorBak);
  }

  LittleFS.remove(kDataFile);
  if (hadData) LittleFS.rename(kBackupFile, kDataFile);

  const int total = gPass + gFail;
  Serial.printf("\nRESULT|SUMMARY|%d/%d|OVERALL:%s\n",
                gPass, total, gFail == 0 ? "PASS" : "FAIL");
}

void loop() {}
