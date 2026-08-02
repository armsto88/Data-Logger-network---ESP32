// Maintenance utility: erase the FieldHub deployment store.
//
// WHY THIS EXISTS
// ---------------
// test_deployment_epoch.cpp links the real deployment_store.cpp and writes a
// real /deploy.bin to LittleFS. When that suite finishes, the hub is left
// holding a store full of FIXTURE state — fake nodes (ENV_A1, ENV_A2) and,
// critically, whatever deployment events the last test queued in the outbox.
//
// Production firmware then loads that store verbatim on its next boot
// (`fresh=0`) and uploads the leftover events to the backend on the next sync.
// Because those fixtures claim real-looking deployment numbers, they collide
// with the numbers live nodes actually hold: the backend rejects them against
// node_deployments_active_number_idx, returns CONFLICT, never acknowledges
// them, and the hub retries the same event forever — a permanent conflict chip
// in the Field UI and an outbox slot that never frees.
//
// Run this after the epoch test suite and BEFORE flashing production firmware
// for field use. Only deployment-store files are touched; the reading
// buffer (/datalog.csv) and all NVS state (paired nodes, upload cursor, sync
// anchor) are deliberately left alone.
//
// Serial contract matches the test suites: RESULT|SUMMARY|n/n|OVERALL:PASS.

#include <Arduino.h>
#include <LittleFS.h>

static int gPass = 0, gFail = 0;

static void check(const char* name, bool cond) {
  Serial.printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", name);
  cond ? ++gPass : ++gFail;
}

static void removeIfPresent(const char* path) {
  if (!LittleFS.exists(path)) {
    Serial.printf("[WIPE] %s: not present\n", path);
    return;
  }
  const bool ok = LittleFS.remove(path);
  Serial.printf("[WIPE] %s: %s\n", path, ok ? "removed" : "REMOVE FAILED");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== wipe_deployment_store ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[FAIL] LittleFS mount failed");
    Serial.println("RESULT|SUMMARY|0/1|OVERALL:FAIL");
    return;
  }

  // Report what the reading buffer looks like before and after, so it is
  // evident this did not touch field data.
  const bool hadCsv = LittleFS.exists("/datalog.csv");
  size_t csvBefore = 0;
  if (hadCsv) {
    File f = LittleFS.open("/datalog.csv", "r");
    if (f) { csvBefore = f.size(); f.close(); }
  }
  Serial.printf("[WIPE] /datalog.csv before: %s (%u bytes)\n",
                hadCsv ? "present" : "absent", (unsigned)csvBefore);

  removeIfPresent("/deploy.bin");
  removeIfPresent("/deploy.bak");
  removeIfPresent("/deploy.tmp");
  removeIfPresent("/deploy.v1");
  removeIfPresent("/deploy.v1.tmp");

  check("deploy.bin is gone", !LittleFS.exists("/deploy.bin"));
  check("deploy.bak is gone", !LittleFS.exists("/deploy.bak"));
  check("deploy.tmp is gone", !LittleFS.exists("/deploy.tmp"));
  check("deploy.v1 is gone", !LittleFS.exists("/deploy.v1"));
  check("deploy.v1.tmp is gone", !LittleFS.exists("/deploy.v1.tmp"));

  size_t csvAfter = 0;
  const bool haveCsv = LittleFS.exists("/datalog.csv");
  if (haveCsv) {
    File f = LittleFS.open("/datalog.csv", "r");
    if (f) { csvAfter = f.size(); f.close(); }
  }
  Serial.printf("[WIPE] /datalog.csv after:  %s (%u bytes)\n",
                haveCsv ? "present" : "absent", (unsigned)csvAfter);
  check("reading buffer untouched", haveCsv == hadCsv && csvAfter == csvBefore);

  Serial.printf("\nRESULT|SUMMARY|%d/%d|OVERALL:%s\n",
                gPass, gPass + gFail, gFail == 0 ? "PASS" : "FAIL");
  Serial.println("[WIPE] Now reflash mothership-v1-main; the store will be "
                 "recreated fresh and seeded from the registry.");
}

void loop() { delay(1000); }
