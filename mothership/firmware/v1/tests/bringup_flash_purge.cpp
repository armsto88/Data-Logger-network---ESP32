// Mothership V1 bringup: Flash purge logic
// Validates UploadQueue emergencyPurgeIfFull and purgeUploaded — no antenna
// or modem required.  Tests cursor adjustment after purge and header
// preservation.
//
// Test flow:
//   1. Assert PWR_HOLD, init flash + upload queue.
//   2. Fill flash with test rows until usage > 50% (lowered threshold for
//      speed; production uses 80%).
//   3. emergencyPurgeIfFull(50) → should purge oldest 50% of rows.
//   4. Verify header preserved + cursor valid.
//   5. Write 5 more rows, advance cursor, purgeUploaded → file keeps only
//      unuploaded rows (0 rows after cursor).
//   6. Verify header preserved + cursor at header end.
//
// PASS = all sub-tests pass.

#include <Arduino.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "system/pins.h"
#include "storage/flash_logger.h"
#include "storage/upload_queue.h"

// Expected CSV header (must match flash_logger.cpp / upload_queue.h).
static const char* kExpectedHeader =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2";

static UploadQueue uploadQueue;

// Generate a fake CSV row matching the 25-column header, padded to ~500
// bytes to fill flash faster and reduce write cycles.
static String makeTestRow(int seq) {
  char row[512];
  snprintf(row, sizeof(row),
           "2026-06-21T12:00:%02d,TEST_NODE_%02d,%d,511,0,1,3.85,22.5,55.0,"
           "100,200,300,400,500,600,700,800,1.2,180,0.35,21.0,0.38,20.5,0,0"
           ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
           ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
           seq, seq, seq);
  return String(row);
}

// Clear the NVS "tx" namespace so the test starts from a known cursor state.
static void clearTxNvs() {
  Preferences prefs;
  if (prefs.begin("tx", false)) {
    prefs.clear();
    prefs.end();
    Serial.println("[TEST] Cleared NVS namespace 'tx'");
  } else {
    Serial.println("[TEST] WARN: could not open NVS 'tx' for clear");
  }
}

static void printFlashUsage(const char* label) {
  uint32_t total = LittleFS.totalBytes();
  uint32_t used  = LittleFS.usedBytes();
  uint32_t pct   = (total > 0) ? (used * 100) / total : 0;
  Serial.printf("[FLASH] %s: used=%u / total=%u (%u%%)\n",
                label, (unsigned)used, (unsigned)total, (unsigned)pct);
}

static void printCursor(const char* label) {
  UploadCursor c = uploadQueue.getCursor();
  Serial.printf("[CURSOR] %s: offset=%u rowsUploaded=%u lastUploadUnix=%u\n",
                label,
                (unsigned)c.byteOffset,
                (unsigned)c.rowsUploaded,
                (unsigned)c.lastUploadUnix);
}

// Read the first line of /datalog.csv and return it (trimmed).
static String readFirstLine() {
  File f = LittleFS.open("/datalog.csv", "r");
  if (!f) return String();
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  return line;
}

static void printSummary(bool pass) {
  Serial.println();
  Serial.println("=== Flash Purge Bring-up Complete ===");
  if (pass) {
    Serial.println("OVERALL PASS: All purge sub-tests passed.");
  } else {
    Serial.println("OVERALL FAIL: One or more sub-tests failed — review output above.");
  }
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Flash Purge Bring-up ===");
  Serial.println("Tests emergencyPurgeIfFull and purgeUploaded — flash-only, no antenna.");
  Serial.println("NOTE: Uses 50%% emergency threshold for speed; production uses 80%%.");
  Serial.println();

  bool overallPass = true;

  // --- Step 1: clear NVS, init flash, init upload queue ---
  clearTxNvs();

  Serial.println("[STEP 1] Init flash...");
  if (!initFlash()) {
    Serial.println("FAIL: initFlash() returned false — check LittleFS / partition table.");
    overallPass = false;
    printSummary(overallPass);
    return;
  }
  Serial.println("[STEP 1] initFlash() OK");

  Serial.println("[STEP 1] Init upload queue...");
  if (!uploadQueue.init()) {
    Serial.println("FAIL: uploadQueue.init() returned false.");
    overallPass = false;
    printSummary(overallPass);
    return;
  }
  Serial.println("[STEP 1] uploadQueue.init() OK");

  // --- Step 2: print initial flash usage ---
  Serial.println();
  Serial.println("[STEP 2] Initial flash usage:");
  printFlashUsage("initial");
  printCursor("initial");

  // --- Step 3: fill flash until usage > 50% ---
  Serial.println();
  Serial.println("[STEP 3] Filling flash with test rows until usage > 50%...");
  int rowsWritten = 0;
  uint32_t pct = 0;
  while (true) {
    uint32_t total = LittleFS.totalBytes();
    uint32_t used  = LittleFS.usedBytes();
    pct = (total > 0) ? (used * 100) / total : 0;
    if (pct > 50) {
      Serial.printf("[STEP 3] Reached %u%% usage after %d rows.\n",
                    (unsigned)pct, rowsWritten);
      break;
    }
    // Write a batch of rows before re-checking to reduce overhead.
    for (int i = 0; i < 100; i++) {
      String row = makeTestRow(rowsWritten + 1);
      if (!flashLogCSVRow(row)) {
        Serial.printf("[STEP 3] flashLogCSVRow failed at row %d — flash may be full.\n",
                      rowsWritten + 1);
        break;
      }
      rowsWritten++;
    }
    if (rowsWritten % 500 == 0) {
      Serial.printf("[STEP 3] progress: %d rows, %u%% used\n", rowsWritten, (unsigned)pct);
    }
    // Safety valve: if we've written a lot and still can't hit 50%, stop.
    if (rowsWritten > 20000) {
      Serial.printf("[STEP 3] Aborting fill at %d rows (safety valve). Usage=%u%%\n",
                    rowsWritten, (unsigned)pct);
      break;
    }
  }
  Serial.printf("[STEP 3] Fill complete: %d rows, %u%% usage.\n", rowsWritten, (unsigned)pct);

  // --- Step 4: print flash stats before purge ---
  Serial.println();
  Serial.println("[STEP 4] Flash stats before emergency purge:");
  printFlashUsage("before-purge");
  Serial.printf("  File size:   %u bytes\n", (unsigned)getCSVFileSize());
  Serial.printf("  Record count: %d\n", getCSVRecordCount());

  // --- Step 5: emergencyPurgeIfFull(50) ---
  Serial.println();
  Serial.println("[STEP 5] emergencyPurgeIfFull(50) — should purge oldest 50%% of rows...");
  bool purgeOk = uploadQueue.emergencyPurgeIfFull(50);
  if (!purgeOk) {
    Serial.println("FAIL: emergencyPurgeIfFull returned false.");
    Serial.println("       Guidance: check that /datalog.csv exists and has rows.");
    overallPass = false;
  }

  // --- Step 6: print flash stats after purge ---
  Serial.println();
  Serial.println("[STEP 6] Flash stats after emergency purge:");
  printFlashUsage("after-purge");
  Serial.printf("  File size:   %u bytes\n", (unsigned)getCSVFileSize());
  Serial.printf("  Record count: %d\n", getCSVRecordCount());
  printCursor("after-emergency-purge");

  // --- Step 7: verify header preserved ---
  Serial.println();
  Serial.println("[STEP 7] Verify CSV header preserved after emergency purge...");
  String firstLine = readFirstLine();
  Serial.printf("  First line: %s\n", firstLine.c_str());
  if (firstLine == String(kExpectedHeader)) {
    Serial.println("PASS: Header preserved after emergency purge.");
  } else {
    Serial.println("FAIL: Header missing or mismatched after emergency purge.");
    Serial.println("       Guidance: emergencyPurgeIfFull may not have written the header to the temp file.");
    overallPass = false;
  }

  // --- Step 8: verify cursor valid (not past EOF) — getNewData should not fail ---
  Serial.println();
  Serial.println("[STEP 8] Verify cursor valid — getNewData(65536) should not fail...");
  UploadPayload payload = uploadQueue.getNewData(65536);
  Serial.printf("  payload: byteLength=%u startOffset=%u rowEstimate=%u\n",
                (unsigned)payload.byteLength,
                (unsigned)payload.startOffset,
                (unsigned)payload.rowEstimate);
  // getNewData returning a payload (even 0 bytes) means the file opened and
  // seek succeeded — cursor is valid.  A zero-byte payload is fine here
  // because the cursor may have been reset to header end after purge.
  if (payload.startOffset == uploadQueue.getCursor().byteOffset) {
    Serial.println("PASS: Emergency purge works — cursor valid, getNewData succeeded.");
  } else {
    Serial.println("FAIL: getNewData startOffset does not match cursor — cursor may be invalid.");
    overallPass = false;
  }

  // --- Step 9: post-upload purge test ---
  Serial.println();
  Serial.println("[STEP 9] Post-upload purge test — write 5 rows, advance cursor, purgeUploaded...");
  // Write 5 fresh rows.
  for (int i = 1; i <= 5; i++) {
    String row = makeTestRow(i);
    flashLogCSVRow(row);
  }
  Serial.println("[STEP 9] 5 rows written.");
  printFlashUsage("after-5-rows");

  // Get new data from cursor.
  UploadPayload payload2 = uploadQueue.getNewData(65536);
  Serial.printf("  payload2: byteLength=%u startOffset=%u rowEstimate=%u\n",
                (unsigned)payload2.byteLength,
                (unsigned)payload2.startOffset,
                (unsigned)payload2.rowEstimate);

  if (payload2.rowEstimate != 5) {
    Serial.printf("WARN: Expected 5 new rows, got %u — continuing anyway.\n",
                  (unsigned)payload2.rowEstimate);
  }

  // Advance cursor past the new data.
  uint32_t newOffset = payload2.startOffset + payload2.byteLength;
  uploadQueue.advanceCursor(newOffset, 1234567890);
  printCursor("before-purgeUploaded");

  // --- Step 10: purgeUploaded — should rewrite file keeping only unuploaded rows ---
  Serial.println();
  Serial.println("[STEP 10] purgeUploaded() — should keep only unuploaded rows (0 after cursor)...");
  bool purgeUploadedOk = uploadQueue.purgeUploaded();
  if (!purgeUploadedOk) {
    Serial.println("FAIL: purgeUploaded returned false.");
    Serial.println("       Guidance: check that /datalog_tmp.csv could be created and renamed.");
    overallPass = false;
  }
  printFlashUsage("after-purgeUploaded");
  printCursor("after-purgeUploaded");

  // --- Step 11: verify header preserved after purgeUploaded ---
  Serial.println();
  Serial.println("[STEP 11] Verify header preserved after purgeUploaded...");
  String firstLine2 = readFirstLine();
  Serial.printf("  First line: %s\n", firstLine2.c_str());
  if (firstLine2 == String(kExpectedHeader)) {
    Serial.println("PASS: Header preserved after purgeUploaded.");
  } else {
    Serial.println("FAIL: Header missing or mismatched after purgeUploaded.");
    overallPass = false;
  }

  // --- Step 12: verify cursor at header end + no pending data ---
  Serial.println();
  Serial.println("[STEP 12] Verify cursor at header end and no pending data...");
  UploadCursor c = uploadQueue.getCursor();
  // Arduino-ESP32 println appends CRLF.
  uint32_t expectedOffset = (uint32_t)(strlen(kExpectedHeader) + 2);
  Serial.printf("  cursor offset=%u, expected header end=%u\n",
                (unsigned)c.byteOffset, (unsigned)expectedOffset);

  if (c.byteOffset == expectedOffset) {
    Serial.println("PASS: Cursor at header end after purgeUploaded.");
  } else {
    Serial.printf("FAIL: Cursor offset=%u, expected %u\n",
                  (unsigned)c.byteOffset, (unsigned)expectedOffset);
    Serial.println("       Guidance: purgeUploaded may not have reset the cursor correctly.");
    overallPass = false;
  }

  uint32_t pending = uploadQueue.getPendingBytes();
  Serial.printf("  pending bytes after purgeUploaded: %u\n", (unsigned)pending);
  if (pending == 0) {
    Serial.println("PASS: No pending bytes after purgeUploaded.");
  } else {
    Serial.printf("FAIL: Expected 0 pending bytes, got %u\n", (unsigned)pending);
    overallPass = false;
  }

  // --- Final summary ---
  printSummary(overallPass);
}

void loop() {
  // Idle heartbeat — test runs once in setup().
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    Serial.println("[HEARTBEAT] flash-purge test idle.");
    lastBeat = millis();
  }
}
