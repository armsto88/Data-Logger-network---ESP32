// Mothership V1 bringup: bounded chunked upload payload construction.

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "system/pins.h"
#include "storage/flash_logger.h"
#include "storage/upload_queue.h"

namespace {

int gPassed = 0;
int gFailed = 0;

void reportResult(const char* name, bool passed) {
  Serial.printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  passed ? ++gPassed : ++gFailed;
}

}  // namespace

void setup() {
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 Upload Chunk Bring-up ===");

  reportResult("Flash initialization", initFlash());
  LittleFS.remove("/datalog.csv");
  flashCreateCSVHeader();
  Preferences prefs;
  if (prefs.begin("tx", false)) {
    prefs.clear();
    prefs.end();
  }

  String row;
  row.reserve(256);
  row = "2026-06-22T12:00:00,CHUNK_TEST,1,0,0,0";
  while (row.length() < 220) row += ",123456789";
  while (getCSVFileSize() < 300UL * 1024UL) {
    if (!flashLogCSVRow(row)) break;
  }
  reportResult("Created at least 300 KB of rows", getCSVFileSize() >= 300UL * 1024UL);

  UploadQueue queue;
  queue.init();
  const uint32_t heapBefore = ESP.getFreeHeap();
  UploadPayload large = queue.getNewData(262144UL);
  if (static_cast<uint64_t>(heapBefore) < 524288ULL) {
    reportResult("256 KB request is safely rejected when heap is insufficient",
                 large.byteLength == 0);
  } else {
    reportResult("256 KB payload ends on newline",
                 large.byteLength > 0 && large.csvData.endsWith("\n"));
  }

  const uint32_t safeRequest = 32768UL;
  const uint32_t safeHeapBefore = ESP.getFreeHeap();
  UploadPayload payload = queue.getNewData(safeRequest);
  const uint32_t safeHeapAfter = ESP.getFreeHeap();
  reportResult("Chunked payload produced", payload.byteLength >= safeRequest);
  reportResult("Payload ends on a complete row",
               payload.csvData.length() > 0 && payload.csvData.endsWith("\n"));
  reportResult("byteLength matches consumed file bytes",
               payload.startOffset + payload.byteLength <= getCSVFileSize());
  reportResult("Row estimate is populated", payload.rowEstimate > 0);
  Serial.printf("[TEST] heap before=%u after=%u payload=%u rows=%u\n",
                static_cast<unsigned>(safeHeapBefore),
                static_cast<unsigned>(safeHeapAfter),
                static_cast<unsigned>(payload.byteLength),
                static_cast<unsigned>(payload.rowEstimate));

  Serial.printf("=== RESULT: %s (%d passed, %d failed) ===\n",
                gFailed == 0 ? "PASS" : "FAIL", gPassed, gFailed);
}

void loop() {
  static uint32_t lastHeartbeatMs = 0;
  if (millis() - lastHeartbeatMs >= 5000) {
    lastHeartbeatMs = millis();
    Serial.printf("[IDLE] heartbeat - result remains %s\n",
                  gFailed == 0 ? "PASS" : "FAIL");
  }
  delay(50);
}
