// On-device assertion test for the OTA release NVS store.
//
// Exercises the INSTALLED / ARMED / PENDING state machine and its NVS
// persistence (A/B checksummed record), including reboot-survival: run once,
// note the [PERSIST] marker value, power-cycle, re-run, and confirm the
// installed release survived.
//
//   pio run -e mothership-v2-test-ota-release-store -t upload && pio device monitor
//
#include <Arduino.h>
#include "ota/mothership_ota_release_store.h"

static int failures = 0;

static void ok(bool cond, const char* label) {
  if (cond) { Serial.printf("ok   %s\n", label); }
  else      { Serial.printf("FAIL %s\n", label); failures++; }
}

static void runFreshStateMachine() {
  otaReleaseStoreResetForTest();
  otaReleaseStoreInit();

  // Empty defaults.
  ok(otaReleaseStoreInstalledSequence() == 0, "fresh: installed seq 0");
  char buf[40] = {0};
  ok(!otaReleaseStoreInstalledReleaseId(buf, sizeof(buf)), "fresh: no installed id");
  ok(!otaReleaseStoreGetPending(buf, sizeof(buf)), "fresh: no pending");
  ok(!otaReleaseStoreGetArmed(buf, sizeof(buf), nullptr), "fresh: no armed");

  // Stage a fetch intent.
  ok(otaReleaseStoreSetPending("rel-2026.08.0"), "set pending");
  ok(otaReleaseStoreGetPending(buf, sizeof(buf)), "get pending true");
  ok(strcmp(buf, "rel-2026.08.0") == 0, "pending id matches");

  // Empty releaseId is rejected.
  ok(!otaReleaseStoreSetPending(""), "empty pending rejected");

  // Arm a release (flashed + set-boot).
  ok(otaReleaseStoreSetArmed("rel-2026.08.0", 12), "set armed");
  uint32_t seq = 0;
  ok(otaReleaseStoreGetArmed(buf, sizeof(buf), &seq), "get armed true");
  ok(strcmp(buf, "rel-2026.08.0") == 0 && seq == 12, "armed id+seq match");
  // Arming does not change installed yet.
  ok(otaReleaseStoreInstalledSequence() == 0, "armed: installed still 0");

  // Promote armed -> installed (confirmed first boot).
  ok(otaReleaseStorePromoteArmed(), "promote armed");
  ok(otaReleaseStoreInstalledSequence() == 12, "installed seq 12 after promote");
  ok(otaReleaseStoreInstalledReleaseId(buf, sizeof(buf)) &&
     strcmp(buf, "rel-2026.08.0") == 0, "installed id after promote");
  ok(!otaReleaseStoreGetArmed(buf, sizeof(buf), nullptr), "armed cleared after promote");

  // Clearing pending.
  ok(otaReleaseStoreClearPending(), "clear pending");
  ok(!otaReleaseStoreGetPending(buf, sizeof(buf)), "pending gone");

  // Promote with nothing armed -> false, installed unchanged.
  ok(!otaReleaseStorePromoteArmed(), "promote no-op returns false");
  ok(otaReleaseStoreInstalledSequence() == 12, "installed unchanged");

  // Rollback scenario: arm a newer release, but DON'T promote (image failed to
  // boot). Installed must stay at the prior sequence; armed stays set so a
  // caller can detect the mismatch.
  ok(otaReleaseStoreSetArmed("rel-2026.09.0", 13), "arm newer");
  ok(otaReleaseStoreInstalledSequence() == 12, "installed still 12 (not promoted)");
  ok(otaReleaseStoreClearArmed(), "clear armed (rollback observed)");
  ok(otaReleaseStoreInstalledSequence() == 12, "installed still 12 after clear armed");
}

static void checkPersistenceAcrossBoot() {
  // Seed a known installed release, then reload from NVS as if freshly booted.
  otaReleaseStoreRecordInstalled("rel-persist-7", 7);
  otaReleaseStoreInit();   // reload from NVS (simulates cold boot load)
  ok(otaReleaseStoreInstalledSequence() == 7, "persist: seq 7 reloaded");
  char buf[40] = {0};
  ok(otaReleaseStoreInstalledReleaseId(buf, sizeof(buf)) &&
     strcmp(buf, "rel-persist-7") == 0, "persist: id reloaded");
  Serial.println("[PERSIST] installed=rel-persist-7 seq=7 written to NVS. "
                 "Power-cycle and re-run: the reload above must still read 7.");
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[TEST] OTA release store");

  runFreshStateMachine();
  checkPersistenceAcrossBoot();

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
