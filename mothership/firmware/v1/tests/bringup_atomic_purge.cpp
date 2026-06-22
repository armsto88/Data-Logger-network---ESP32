// Mothership V1 bringup: power-safe purge recovery and cursor boundaries.

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

void clearCursorNvs() {
  Preferences prefs;
  if (prefs.begin("tx", false)) {
    prefs.clear();
    prefs.end();
  }
}

bool copyFile(const char* from, const char* to) {
  File source = LittleFS.open(from, "r");
  File target = LittleFS.open(to, "w", true);
  if (!source || !target) {
    source.close();
    target.close();
    return false;
  }
  uint8_t buffer[256];
  while (source.available()) {
    int count = source.read(buffer, sizeof(buffer));
    if (count <= 0 || target.write(buffer, count) != static_cast<size_t>(count)) {
      source.close();
      target.close();
      return false;
    }
  }
  source.close();
  target.close();
  return true;
}

uint32_t offsetAfterDataRows(uint32_t rows) {
  File file = LittleFS.open("/datalog.csv", "r");
  if (!file) return 0;
  file.readStringUntil('\n');
  uint32_t seen = 0;
  while (file.available() && seen < rows) {
    if (file.read() == '\n') ++seen;
  }
  uint32_t offset = static_cast<uint32_t>(file.position());
  file.close();
  return seen == rows ? offset : 0;
}

}  // namespace

void setup() {
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Mothership V1 Atomic Purge Bring-up ===");

  reportResult("Flash initialization", initFlash());
  LittleFS.remove("/datalog_tmp.csv");
  LittleFS.remove("/datalog_bak.csv");
  LittleFS.remove("/datalog.csv");
  reportResult("Fresh CSV header", flashCreateCSVHeader());
  clearCursorNvs();

  UploadQueue queue;
  reportResult("Upload queue initialization", queue.init());
  for (int i = 0; i < 6; ++i) {
    flashLogCSVRow(String("2026-06-22T12:00:00,ATOMIC_TEST,") + i + ",0,0,0");
  }

  const uint32_t uploadedOffset = offsetAfterDataRows(3);
  queue.advanceCursor(uploadedOffset, 0);
  reportResult("Backup-then-swap purge succeeds", queue.purgeUploaded());
  reportResult("Committed purge leaves one data file",
               LittleFS.exists("/datalog.csv") &&
               !LittleFS.exists("/datalog_tmp.csv") &&
               !LittleFS.exists("/datalog_bak.csv"));

  reportResult("Create simulated committed backup",
               copyFile("/datalog.csv", "/datalog_bak.csv"));
  UploadQueue committedRecovery;
  reportResult("Recovery keeps committed data and removes backup",
               committedRecovery.init() && LittleFS.exists("/datalog.csv") &&
               !LittleFS.exists("/datalog_bak.csv"));

  reportResult("Create simulated pre-commit backup",
               copyFile("/datalog.csv", "/datalog_bak.csv"));
  LittleFS.remove("/datalog.csv");
  UploadQueue interruptedRecovery;
  reportResult("Recovery restores missing data from backup",
               interruptedRecovery.init() && LittleFS.exists("/datalog.csv") &&
               !LittleFS.exists("/datalog_bak.csv"));

  flashLogCSVRow("2026-06-22T12:00:01,CURSOR_TEST,99,0,0,0");
  const uint32_t headerEnd = static_cast<uint32_t>(strlen(kUploadCSVHeader) + 2);
  Preferences prefs;
  if (prefs.begin("tx", false)) {
    prefs.putUInt("cursor_offset", headerEnd + 5);
    prefs.end();
  }
  UploadQueue cursorRecovery;
  cursorRecovery.init();
  const UploadCursor cursor = cursorRecovery.getCursor();
  File data = LittleFS.open("/datalog.csv", "r");
  bool atBoundary = data && cursor.byteOffset > 0 && data.seek(cursor.byteOffset - 1) &&
                    data.read() == '\n';
  data.close();
  reportResult("Mid-row cursor advances to next newline", atBoundary);

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
