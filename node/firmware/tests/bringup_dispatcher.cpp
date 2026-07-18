// bringup_dispatcher.cpp — command dispatcher + revision store proof.
//
// Proves the Phase 2 core (plan §4.2/§4.3) in isolation on the node bench:
// compare-and-set revisioning, idempotent replay, supersession, validation,
// pause-node desired state, convergence, and reboot persistence (NVS).
//
// Menu @115200:
//   r  - submit a marker command then reboot (proves NVS persistence)
//   any other key - re-run the in-memory assertion suite

#include <Arduino.h>
#include "command_dispatcher.h"

static int gPass = 0, gFail = 0;
static void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) gPass++; else gFail++;
}

static Command mkCfg(const char* id, CmdSource src, uint32_t expRev,
                     const char* nodeId, uint8_t wake, uint8_t target) {
  Command c{};
  strlcpy(c.cmdId, id, CMD_ID_LEN);
  c.type = CMD_SET_NODE_CONFIG;
  c.source = src;
  c.expectedRevision = expRev;
  strlcpy(c.payload.nodeId, nodeId, CMD_NODEID_LEN);
  c.payload.wakeIntervalMin = wake;
  c.payload.targetState = target;
  c.payload.sensorMask = 0;
  return c;
}

static void runSuite() {
  gPass = gFail = 0;
  dispatcherResetForTest();
  Serial.println("\n--- dispatcher assertion suite ---");

  // 1) Fresh accept -> revision 1.
  CommandResult a = dispatcherSubmit(mkCfg("A", SRC_LOCAL_UI, 0, "N1", 10, 2));
  check("A accepted", a.outcome == OUT_ACCEPTED && a.assignedRevision == 1);
  check("revision is 1", dispatcherRevision() == 1);

  // 2) Replay of A -> stored result, no re-exec, revision unchanged.
  CommandResult a2 = dispatcherSubmit(mkCfg("A", SRC_LOCAL_UI, 0, "N1", 99, 2));
  check("A replay reports REPLAY", a2.outcome == OUT_REPLAY);
  check("A replay returns stored (rev 1)", a2.assignedRevision == 1);
  check("replay did not advance revision", dispatcherRevision() == 1);
  check("replay did not mutate config",
        dispatcherNodeConfig("N1") && dispatcherNodeConfig("N1")->wakeIntervalMin == 10);

  // 3) Stale compare-and-set (dashboard saw rev 0, current is 1) -> conflict.
  CommandResult b = dispatcherSubmit(mkCfg("B", SRC_DASHBOARD, 0, "N1", 20, 2));
  check("stale B -> REVISION_CONFLICT", b.outcome == OUT_REVISION_CONFLICT);
  check("conflict reports current rev 1", b.currentRevision == 1);
  check("conflict did not advance revision", dispatcherRevision() == 1);

  // 4) Correct CAS -> accept rev 2, and supersede the not-yet-converged A.
  CommandResult c = dispatcherSubmit(mkCfg("C", SRC_DASHBOARD, 1, "N1", 20, 2));
  check("C accepted at rev 2", c.outcome == OUT_ACCEPTED && c.assignedRevision == 2);
  CommandResult aStored;
  check("A now SUPERSEDED",
        dispatcherResultFor("A", &aStored) && aStored.outcome == OUT_SUPERSEDED);
  check("config updated to wake 20",
        dispatcherNodeConfig("N1")->wakeIntervalMin == 20);

  // 5) Invalid payload (wake 0) -> rejected, no revision change.
  CommandResult d = dispatcherSubmit(mkCfg("D", SRC_LOCAL_UI, 2, "N2", 0, 2));
  check("invalid D -> INVALID", d.outcome == OUT_INVALID);
  check("invalid did not advance revision", dispatcherRevision() == 2);

  // 6) Pause a different node (explicit STANDBY=3) -> accept rev 3.
  CommandResult e = dispatcherSubmit(mkCfg("E", SRC_DASHBOARD, 2, "N2", 15, 3));
  check("E (pause N2) accepted rev 3", e.outcome == OUT_ACCEPTED && e.assignedRevision == 3);
  check("N2 targetState = STANDBY(3)", dispatcherNodeConfig("N2")->targetState == 3);

  // 7) Converged command is NOT superseded by a later change.
  check("wire config version bound", dispatcherBindNodeConfigVersion("N1", 2, 7));
  uint32_t wireRevision = 0;
  check("wire version resolves revision",
        dispatcherRevisionForConfigVersion("N1", 7, &wireRevision) && wireRevision == 2);
  check("C convergence recorded once", dispatcherMarkConverged("N1", wireRevision));
  check("C duplicate convergence ignored", !dispatcherMarkConverged("N1", wireRevision));
  CommandResult f = dispatcherSubmit(mkCfg("F", SRC_LOCAL_UI, 3, "N1", 30, 2));
  check("F accepted rev 4", f.outcome == OUT_ACCEPTED && f.assignedRevision == 4);
  CommandResult cStored;
  check("converged C stays CONVERGED (not superseded)",
        dispatcherResultFor("C", &cStored) && cStored.outcome == OUT_CONVERGED);
  check("N1 config now wake 30", dispatcherNodeConfig("N1")->wakeIntervalMin == 30);

  Serial.printf("=== SUITE: %d passed, %d failed (revision=%u) ===\n",
                gPass, gFail, dispatcherRevision());
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n\n### FieldMesh command dispatcher test ###");

  dispatcherInit();  // load whatever was persisted
  if (dispatcherKnownCmd("persist-marker")) {
    CommandResult m;
    dispatcherResultFor("persist-marker", &m);
    Serial.printf(">> PERSISTED ACROSS REBOOT: revision=%u, persist-marker known "
                  "(outcome=%s, rev=%u) — idempotent replay confirmed\n",
                  dispatcherRevision(), cmdOutcomeStr(m.outcome), m.assignedRevision);
  } else {
    Serial.println(">> no persisted marker yet — press 'r' to submit one and reboot");
  }

  runSuite();
  Serial.println("menu: r=submit marker+reboot (persistence)  other=re-run suite");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  int ch = Serial.read();
  if (ch == '\n' || ch == '\r') return;
  if (ch == 'r') {
    uint32_t rev = dispatcherRevision();
    CommandResult m = dispatcherSubmit(mkCfg("persist-marker", SRC_DASHBOARD, rev, "P1", 7, 2));
    Serial.printf("[persist] submitted marker: %s rev=%u -> rebooting...\n",
                  cmdOutcomeStr(m.outcome), m.assignedRevision);
    delay(600);
    esp_restart();
  } else {
    runSuite();
  }
}
