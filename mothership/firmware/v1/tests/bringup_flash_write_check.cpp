// Mothership V1 bringup: checked LittleFS append failure reporting.

#include <Arduino.h>
#include <LittleFS.h>
#include "system/pins.h"
#include "storage/flash_logger.h"

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
  Serial.println("=== Mothership V1 Flash Write Check ===");
  Serial.println("[WARN] This destructive bring-up test fills LittleFS.");

  const bool ready = initFlash();
  reportResult("LittleFS initialized without auto-format", ready);
  if (!ready) {
    reportResult("Mount failure is exposed", flashMountFailed());
    return;
  }

  String row;
  row.reserve(512);
  row = "2026-06-22T12:00:00,FILL_TEST,1,511,0,1,3.8,22.0,50.0";
  while (row.length() < 500) row += ",123456789";

  uint32_t rowsWritten = 0;
  bool failureObserved = false;
  while (rowsWritten < 20000) {
    if (!flashLogCSVRow(row)) {
      failureObserved = true;
      break;
    }
    ++rowsWritten;
    if (rowsWritten % 500 == 0) {
      Serial.printf("[TEST] rows=%lu used=%u/%u\n",
                    static_cast<unsigned long>(rowsWritten),
                    static_cast<unsigned>(LittleFS.usedBytes()),
                    static_cast<unsigned>(LittleFS.totalBytes()));
    }
  }

  Serial.printf("[TEST] Successful rows before failure: %lu\n",
                static_cast<unsigned long>(rowsWritten));
  reportResult("Full-flash append returns false and logs [FLASH] Write failed",
               failureObserved);
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
