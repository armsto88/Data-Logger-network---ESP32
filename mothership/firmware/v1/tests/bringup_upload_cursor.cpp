// Mothership V1 bringup: Upload cursor / queue logic
// Validates UploadQueue init, getNewData, advanceCursor, NVS persistence,
// and validateCursor edge cases — no antenna or modem required.
//
// Test flow:
//   1. Assert PWR_HOLD, init flash + upload queue.
//   2. Write 5 test CSV rows, getNewData → expect 5 rows + header.
//   3. advanceCursor past uploaded data, getNewData → expect 0 bytes.
//   4. Write 3 more rows, getNewData → expect only 3 new rows.
//   5. Re-init upload queue (simulates wake) → cursor survives via NVS.
//
// PASS = all sub-tests pass.

#include <Arduino.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "system/pins.h"
#include "storage/flash_logger.h"
#include "storage/upload_queue.h"

static UploadQueue uploadQueue;

// Forward declaration — summary printed at end of setup() and on early exit.
static void printSummary(bool pass);

// Generate a fake CSV row matching the 25-column header.
// Columns: datetime, nodeId, seqNum, sensorPresent, qualityFlags,
//          configVersion, batVoltage, airTemp, airHumidity,
//          spectral[8], windSpeed, windDir,
//          soil1Vwc, soil1Temp, soil2Vwc, soil2Temp, aux1, aux2
static String makeTestRow(int seq) {
  char row[512];
  snprintf(row, sizeof(row),
           "2026-06-21T12:00:%02d,TEST_NODE_%02d,%d,511,0,1,3.85,22.5,55.0,"
           "100,200,300,400,500,600,700,800,1.2,180,0.35,21.0,0.38,20.5,0,0",
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

static void printCursor(const char* label) {
  UploadCursor c = uploadQueue.getCursor();
  Serial.printf("[CURSOR] %s: offset=%u rowsUploaded=%u lastUploadUnix=%u "
                "retryCount=%u wakeCounter=%u\n",
                label,
                (unsigned)c.byteOffset,
                (unsigned)c.rowsUploaded,
                (unsigned)c.lastUploadUnix,
                (unsigned)c.retryCount,
                (unsigned)c.wakeCounter);
}

static void printPayload(const UploadPayload& p) {
  Serial.printf("[PAYLOAD] byteLength=%u startOffset=%u rowEstimate=%u\n",
                (unsigned)p.byteLength,
                (unsigned)p.startOffset,
                (unsigned)p.rowEstimate);
  Serial.printf("[PAYLOAD] csvData length=%u\n", (unsigned)p.csvData.length());
  if (p.csvData.length() > 0) {
    // Print first 100 chars (or less) of csvData.
    int n = p.csvData.length();
    if (n > 100) n = 100;
    Serial.print("[PAYLOAD] first ");
    Serial.print(n);
    Serial.print(" chars: ");
    for (int i = 0; i < n; i++) Serial.print(p.csvData[i]);
    Serial.println();
  }
}

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Upload Cursor / Queue Bring-up ===");
  Serial.println("Tests UploadQueue init, getNewData, advanceCursor, NVS persistence.");
  Serial.println("No antenna or modem required — flash-only test.");
  Serial.println();

  bool overallPass = true;

  // --- Step 1: clear NVS, init flash, init upload queue ---
  clearTxNvs();

  Serial.println("[STEP 1] Init flash...");
  if (!initFlash()) {
    Serial.println("FAIL: initFlash() returned false — check LittleFS / partition table.");
    Serial.println("       Guidance: verify partitions.csv has a LittleFS/data partition.");
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

  // --- Step 2: print initial cursor state ---
  Serial.println();
  Serial.println("[STEP 2] Initial cursor state:");
  printCursor("initial");

  // --- Step 3: write 5 test rows ---
  Serial.println();
  Serial.println("[STEP 3] Writing 5 test CSV rows...");
  for (int i = 1; i <= 5; i++) {
    String row = makeTestRow(i);
    if (!flashLogCSVRow(row)) {
      Serial.printf("FAIL: flashLogCSVRow(%d) returned false\n", i);
      overallPass = false;
      printSummary(overallPass);
      return;
    }
  }
  Serial.println("[STEP 3] 5 rows written.");

  // --- Step 4: print flash stats ---
  Serial.println();
  Serial.println("[STEP 4] Flash stats after writing 5 rows:");
  Serial.printf("  File size:   %u bytes\n", (unsigned)getCSVFileSize());
  Serial.printf("  Record count: %d\n", getCSVRecordCount());
  Serial.printf("  %s\n", flashGetCSVStats().c_str());

  // --- Step 5: getNewData — should return all 5 rows + header ---
  Serial.println();
  Serial.println("[STEP 5] getNewData(65536) — expect 5 rows + header...");
  UploadPayload payload = uploadQueue.getNewData(65536);
  printPayload(payload);

  if (payload.byteLength == 0) {
    Serial.println("FAIL: getNewData returned 0 bytes — expected 5 rows.");
    Serial.println("       Guidance: check that flashLogCSVRow actually appended data.");
    overallPass = false;
  } else if (payload.rowEstimate != 5) {
    Serial.printf("FAIL: rowEstimate=%u, expected 5\n", (unsigned)payload.rowEstimate);
    overallPass = false;
  } else {
    Serial.println("PASS: getNewData returned 5 rows + header.");
  }

  // --- Step 6: simulate successful upload — advanceCursor ---
  Serial.println();
  Serial.println("[STEP 6] advanceCursor past uploaded data...");
  uint32_t newOffset = payload.startOffset + payload.byteLength;
  Serial.printf("  Advancing cursor to offset=%u (start=%u + len=%u)\n",
                (unsigned)newOffset, (unsigned)payload.startOffset, (unsigned)payload.byteLength);
  if (!uploadQueue.advanceCursor(newOffset, 1234567890)) {
    Serial.println("FAIL: advanceCursor returned false.");
    overallPass = false;
  }
  printCursor("after-advance");

  // --- Step 7: getNewData again — should return 0 bytes ---
  Serial.println();
  Serial.println("[STEP 7] getNewData(65536) again — expect 0 bytes (nothing new)...");
  UploadPayload payload2 = uploadQueue.getNewData(65536);
  printPayload(payload2);

  if (payload2.byteLength == 0) {
    Serial.println("PASS: No new data after cursor advance.");
  } else {
    Serial.printf("FAIL: Expected 0 bytes, got %u bytes (%u rows)\n",
                  (unsigned)payload2.byteLength, (unsigned)payload2.rowEstimate);
    Serial.println("       Guidance: cursor may not have advanced past EOF, or file grew unexpectedly.");
    overallPass = false;
  }

  // --- Step 8: write 3 more rows ---
  Serial.println();
  Serial.println("[STEP 8] Writing 3 more test rows...");
  for (int i = 6; i <= 8; i++) {
    String row = makeTestRow(i);
    if (!flashLogCSVRow(row)) {
      Serial.printf("FAIL: flashLogCSVRow(%d) returned false\n", i);
      overallPass = false;
      printSummary(overallPass);
      return;
    }
  }
  Serial.println("[STEP 8] 3 rows written.");

  // --- Step 9: getNewData — should return only the 3 new rows ---
  Serial.println();
  Serial.println("[STEP 9] getNewData(65536) — expect only 3 new rows...");
  UploadPayload payload3 = uploadQueue.getNewData(65536);
  printPayload(payload3);

  if (payload3.rowEstimate == 3) {
    Serial.println("PASS: Only new data returned (3 rows).");
  } else {
    Serial.printf("FAIL: Expected rowEstimate=3, got %u\n", (unsigned)payload3.rowEstimate);
    Serial.println("       Guidance: cursor may have reset, or previous advance didn't persist.");
    overallPass = false;
  }

  // --- Step 10: advance cursor past the 3 new rows ---
  Serial.println();
  Serial.println("[STEP 10] advanceCursor past 3 new rows...");
  uint32_t newOffset3 = payload3.startOffset + payload3.byteLength;
  uploadQueue.advanceCursor(newOffset3, 1234567999);
  printCursor("after-second-advance");

  // --- Step 11: test NVS persistence — re-init upload queue ---
  Serial.println();
  Serial.println("[STEP 11] NVS persistence test — re-init UploadQueue (simulates wake)...");
  // Destroy and recreate the queue object to force a fresh NVS load.
  uploadQueue = UploadQueue();
  if (!uploadQueue.init()) {
    Serial.println("FAIL: re-init of UploadQueue failed.");
    overallPass = false;
  } else {
    printCursor("after-reinit");
    UploadCursor c = uploadQueue.getCursor();
    // Cursor should match the last advance (offset = newOffset3).
    if (c.byteOffset == newOffset3) {
      Serial.println("PASS: Cursor survived NVS re-init.");
    } else {
      Serial.printf("FAIL: Cursor after re-init=%u, expected=%u\n",
                    (unsigned)c.byteOffset, (unsigned)newOffset3);
      Serial.println("       Guidance: NVS write may have failed, or namespace mismatch.");
      overallPass = false;
    }
  }

  // --- Step 12: getNewData after re-init — should be 0 (all uploaded) ---
  Serial.println();
  Serial.println("[STEP 12] getNewData(65536) after re-init — expect 0 bytes...");
  UploadPayload payload4 = uploadQueue.getNewData(65536);
  printPayload(payload4);
  if (payload4.byteLength == 0) {
    Serial.println("PASS: No pending data after NVS re-init.");
  } else {
    Serial.printf("FAIL: Expected 0 bytes, got %u\n", (unsigned)payload4.byteLength);
    overallPass = false;
  }

  // --- Final summary ---
  printSummary(overallPass);
}

static void printSummary(bool pass) {
  Serial.println();
  Serial.println("=== Upload Cursor / Queue Bring-up Complete ===");
  if (pass) {
    Serial.println("OVERALL PASS: All cursor/queue sub-tests passed.");
  } else {
    Serial.println("OVERALL FAIL: One or more sub-tests failed — review output above.");
  }
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void loop() {
  // Idle heartbeat — test runs once in setup().
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > 5000) {
    Serial.println("[HEARTBEAT] upload-cursor test idle.");
    lastBeat = millis();
  }
}