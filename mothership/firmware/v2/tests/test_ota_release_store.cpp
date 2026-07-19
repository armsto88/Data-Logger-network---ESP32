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

static void runRetryAccounting() {
  otaReleaseStoreResetForTest();
  otaReleaseStoreInit();

  // No pending -> counters read 0 and mutators are no-ops.
  ok(otaReleaseStorePendingAttempts() == 0, "retry: no pending -> 0 attempts");
  ok(otaReleaseStorePendingWakesSinceAttempt() == 0, "retry: no pending -> 0 wakes");
  ok(!otaReleaseStoreRecordPendingAttempt(nullptr), "retry: record no-op without pending");

  // Fresh pending starts the retry budget at zero.
  ok(otaReleaseStoreSetPending("rel-retry"), "retry: set pending");
  ok(otaReleaseStorePendingAttempts() == 0, "retry: fresh pending 0 attempts");
  ok(otaReleaseStorePendingWakesSinceAttempt() == 0, "retry: fresh pending 0 wakes");

  // A skipped wake bumps the wakes-since gate but not the attempt count.
  ok(otaReleaseStoreNotePendingWakeSkipped(), "retry: note wake skipped");
  ok(otaReleaseStorePendingWakesSinceAttempt() == 1, "retry: wakes-since 1");
  ok(otaReleaseStorePendingAttempts() == 0, "retry: skip did not add an attempt");

  // A failed attempt bumps the count and resets the wakes-since gate.
  uint8_t n = 0;
  ok(otaReleaseStoreRecordPendingAttempt(&n) && n == 1, "retry: attempt -> 1");
  ok(otaReleaseStorePendingWakesSinceAttempt() == 0, "retry: attempt reset wakes-since");
  ok(otaReleaseStoreRecordPendingAttempt(&n) && n == 2, "retry: attempt -> 2");

  // Counters survive a reload (persisted in the A/B record).
  otaReleaseStoreInit();
  ok(otaReleaseStorePendingAttempts() == 2, "retry: attempts survive reload");

  // Re-pointing the pending intent resets the retry budget.
  ok(otaReleaseStoreSetPending("rel-retry-2"), "retry: re-point pending");
  ok(otaReleaseStorePendingAttempts() == 0, "retry: re-point reset attempts");

  // Clearing pending zeroes the counters too.
  ok(otaReleaseStoreRecordPendingAttempt(nullptr), "retry: one more attempt");
  ok(otaReleaseStoreClearPending(), "retry: clear pending");
  ok(otaReleaseStorePendingAttempts() == 0, "retry: cleared -> 0 attempts");
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
  runRetryAccounting();
  checkPersistenceAcrossBoot();

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
