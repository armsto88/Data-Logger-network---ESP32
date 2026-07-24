// One-off fixer: reset the OTA pending-fetch backoff counters to 0 so the next
// wake attempts the download immediately, instead of waiting out an
// exponential backoff accumulated from failed attempts before the
// pre-erase fix landed (2026-07-22/23).
//
// Re-stages the SAME pending releaseId, which per
// mothership_ota_release_store.cpp's otaReleaseStoreSetPending() resets
// pendingAttempts/pendingWakesSkipped to 0 without touching INSTALLED/ARMED
// state. Does not touch the running app or any other NVS data.
//
//   pio run -e mothership-v1-fix-ota-backoff -t upload -t monitor --upload-port COM4 --monitor-port COM4
//
#include <Arduino.h>
#include "ota/mothership_ota_release_store.h"

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[FIX] Clearing OTA pending-fetch backoff...");

  otaReleaseStoreInit();

  char pending[40] = {0};
  bool hadPending = otaReleaseStoreGetPending(pending, sizeof(pending));
  Serial.printf("  pending release: %s (had pending=%d)\n", pending, hadPending);
  Serial.printf("  before: attempts=%u wakesSinceAttempt=%u\n",
                (unsigned)otaReleaseStorePendingAttempts(),
                (unsigned)otaReleaseStorePendingWakesSinceAttempt());

  if (!hadPending) {
    Serial.println("  NOTHING PENDING — nothing to fix. (Was it already cleared/installed?)");
  } else {
    bool ok = otaReleaseStoreSetPending(pending);   // re-stage same id -> resets counters
    Serial.printf("  re-stage result: %s\n", ok ? "OK" : "FAILED");
    Serial.printf("  after:  attempts=%u wakesSinceAttempt=%u\n",
                  (unsigned)otaReleaseStorePendingAttempts(),
                  (unsigned)otaReleaseStorePendingWakesSinceAttempt());
  }

  Serial.println("[FIX] Done. Reflash production firmware now.");
}

void loop() {}
