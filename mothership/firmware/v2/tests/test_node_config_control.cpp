#include <Arduino.h>

#include "command_dispatcher.h"
#include "config/node_registry.h"
#include "control/node_config_control.h"

// Minimal fake registry for the production node_config_control.cpp. This test
// isolates convergence behavior without touching the deployed registry NVS.
std::vector<NodeInfo> registeredNodes;

namespace {

NodeDesiredConfig gDesired{};
int gSaveCount = 0;
int gPass = 0;
int gFail = 0;

void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) ++gPass;
  else ++gFail;
}

NodeInfo makeNode(bool paused, uint16_t appliedVersion, bool pending) {
  NodeInfo node{};
  node.nodeId = "ENV_D13F98";
  node.state = DEPLOYED;
  node.recordingPaused = paused;
  node.configVersionApplied = appliedVersion;
  node.stateChangePending = pending;
  node.pendingTargetState = pending ? PENDING_TO_DEPLOYED : PENDING_NONE;
  return node;
}

void bindDispatcherRevision(uint16_t wireVersion, uint8_t targetState) {
  Command command{};
  strlcpy(command.cmdId, "TEST-CONVERGENCE-1", CMD_ID_LEN);
  command.type = CMD_SET_NODE_CONFIG;
  command.source = SRC_LOCAL_UI;
  command.expectedRevision = dispatcherRevision();
  strlcpy(command.payload.nodeId, "ENV_D13F98", CMD_NODEID_LEN);
  command.payload.wakeIntervalMin = 20;
  command.payload.targetState = targetState;
  command.payload.sensorMask = 37;
  const CommandResult accepted = dispatcherSubmit(command);
  check("dispatcher command accepted", accepted.outcome == OUT_ACCEPTED);
  check("wire version bound",
        dispatcherBindNodeConfigVersion("ENV_D13F98",
                                        accepted.assignedRevision,
                                        wireVersion));
}

void resetFixture(uint16_t desiredVersion, uint8_t targetState,
                  bool paused, uint16_t appliedVersion, bool pending) {
  dispatcherResetForTest();
  registeredNodes.clear();
  registeredNodes.push_back(makeNode(paused, appliedVersion, pending));
  gDesired = NodeDesiredConfig{};
  gDesired.configVersion = desiredVersion;
  gDesired.wakeIntervalMin = 20;
  gDesired.syncIntervalMin = 18;
  gDesired.targetState = targetState;
  gDesired.sensorMask = 37;
  gSaveCount = 0;
}

void runSuite() {
  gPass = gFail = 0;
  Serial.println("\n--- node config convergence assertion suite ---");

  resetFixture(7, 2, true, 0, true);
  bindDispatcherRevision(7, 2);
  check("first CONFIG_ACK/HELLO converges",
        controlMarkNodeConfigConverged("ENV_D13F98", 7));
  check("resume repairs registry",
        registeredNodes[0].configVersionApplied == 7 &&
        !registeredNodes[0].recordingPaused &&
        !registeredNodes[0].stateChangePending &&
        registeredNodes[0].pendingTargetState == PENDING_NONE &&
        gSaveCount == 1);

  // Simulate the next FieldHub cold wake: dispatcher NVS still says CONVERGED,
  // but the RAM registry was restored stale. A repeated HELLO must repair it.
  registeredNodes[0] = makeNode(true, 0, false);
  gSaveCount = 0;
  check("repeated HELLO remains a valid convergence observation",
        controlMarkNodeConfigConverged("ENV_D13F98", 7));
  check("already-converged dispatcher still repairs cold-wake RAM",
        registeredNodes[0].configVersionApplied == 7 &&
        !registeredNodes[0].recordingPaused && gSaveCount == 1);

  resetFixture(9, 3, false, 8, true);
  check("stale node version cannot converge",
        !controlMarkNodeConfigConverged("ENV_D13F98", 8));
  check("stale observation leaves registry untouched",
        registeredNodes[0].configVersionApplied == 8 &&
        !registeredNodes[0].recordingPaused &&
        registeredNodes[0].stateChangePending && gSaveCount == 0);

  // Legacy/local desired state may predate a dispatcher binding. Matching
  // device evidence must still repair the authoritative registry view.
  resetFixture(10, 3, false, 0, true);
  check("matching version without dispatcher binding repairs registry",
        controlMarkNodeConfigConverged("ENV_D13F98", 10));
  check("legacy pause state repaired",
        registeredNodes[0].configVersionApplied == 10 &&
        registeredNodes[0].recordingPaused &&
        !registeredNodes[0].stateChangePending && gSaveCount == 1);

  Serial.printf("=== SUITE: %d passed, %d failed ===\n", gPass, gFail);
}

}  // namespace

NodeDesiredConfig getDesiredConfig(const char*) {
  return gDesired;
}

bool setDesiredConfig(const char*, const NodeDesiredConfig& cfg) {
  gDesired = cfg;
  return true;
}

void setNodeExpectedSensorMask(const char*, uint16_t) {}

void savePairedNodes() {
  ++gSaveCount;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n### FieldMesh node config control test ###");
  dispatcherInit();
  runSuite();
}

void loop() {
  delay(1000);
}
