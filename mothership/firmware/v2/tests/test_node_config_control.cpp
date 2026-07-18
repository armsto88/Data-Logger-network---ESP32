#include <Arduino.h>

#include "command_dispatcher.h"
#include "config/node_registry.h"
#include "control/node_config_control.h"

// Minimal fake registry for the production node_config_control.cpp. This test
// isolates convergence behavior without touching the deployed registry NVS.
std::vector<NodeInfo> registeredNodes;

namespace {

struct DesiredSlot {
  String nodeId;
  NodeDesiredConfig config{};
};

DesiredSlot gDesired[2];
int gSaveCount = 0;
int gPass = 0;
int gFail = 0;

void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) ++gPass;
  else ++gFail;
}

NodeDesiredConfig* desiredFor(const char* nodeId) {
  for (auto& slot : gDesired) {
    if (slot.nodeId == String(nodeId ? nodeId : "")) return &slot.config;
  }
  return nullptr;
}

NodeInfo makeNode(const char* nodeId, bool paused, uint16_t appliedVersion,
                  bool pending) {
  NodeInfo node{};
  node.nodeId = nodeId;
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
  registeredNodes.push_back(makeNode("ENV_D13F98", paused, appliedVersion,
                                     pending));
  gDesired[0] = DesiredSlot{};
  gDesired[0].nodeId = "ENV_D13F98";
  gDesired[0].config.configVersion = desiredVersion;
  gDesired[0].config.wakeIntervalMin = 20;
  gDesired[0].config.syncIntervalMin = 18;
  gDesired[0].config.targetState = targetState;
  gDesired[0].config.sensorMask = 37;
  gDesired[1] = DesiredSlot{};
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
  registeredNodes[0] = makeNode("ENV_D13F98", true, 0, false);
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

  // Local Field UI multi-selection uses the same durable per-node helper for
  // every selected node. Verify pause and removal preserve each node's other
  // desired fields and create independent pending NODE_CONFIG records.
  dispatcherResetForTest();
  registeredNodes.clear();
  registeredNodes.push_back(makeNode("ENV_D13F98", false, 3, false));
  registeredNodes.push_back(makeNode("ENV_6C0AA0", false, 6, false));
  gDesired[0] = DesiredSlot{};
  gDesired[0].nodeId = "ENV_D13F98";
  gDesired[0].config.configVersion = 3;
  gDesired[0].config.wakeIntervalMin = 20;
  gDesired[0].config.syncIntervalMin = 18;
  gDesired[0].config.targetState = 2;
  gDesired[0].config.sensorMask = 37;
  gDesired[1] = DesiredSlot{};
  gDesired[1].nodeId = "ENV_6C0AA0";
  gDesired[1].config.configVersion = 6;
  gDesired[1].config.wakeIntervalMin = 45;
  gDesired[1].config.syncIntervalMin = 18;
  gDesired[1].config.targetState = 2;
  gDesired[1].config.sensorMask = 0x8123;

  NodeConfigApplyResult pauseA = controlApplyLocalNodeConfig(
      "ENV_D13F98", 20, 3, 37);
  NodeConfigApplyResult pauseB = controlApplyLocalNodeConfig(
      "ENV_6C0AA0", 45, 3, 0x8123);
  check("multi-select pause queues both nodes durably",
        pauseA.durable && pauseA.registryApplied &&
        pauseB.durable && pauseB.registryApplied && dispatcherRevision() == 2);
  check("multi-select pause preserves independent wake and sensor fields",
        gDesired[0].config.targetState == 3 &&
        gDesired[0].config.wakeIntervalMin == 20 &&
        gDesired[0].config.sensorMask == 37 &&
        gDesired[1].config.targetState == 3 &&
        gDesired[1].config.wakeIntervalMin == 45 &&
        gDesired[1].config.sensorMask == 0x8123);
  check("multi-select pause leaves independent pending sync records",
        registeredNodes[0].stateChangePending &&
        registeredNodes[1].stateChangePending);

  NodeConfigApplyOptions removeOptions{};
  removeOptions.allowUnpair = true;
  NodeConfigApplyResult removeA = controlApplyLocalNodeConfig(
      "ENV_D13F98", 20, 0, 37, removeOptions);
  NodeConfigApplyResult removeB = controlApplyLocalNodeConfig(
      "ENV_6C0AA0", 45, 0, 0x8123, removeOptions);
  check("multi-select remove queues both deployed nodes durably",
        removeA.durable && removeA.registryApplied &&
        removeB.durable && removeB.registryApplied && dispatcherRevision() == 4);
  check("multi-select remove waits for independent unpair acknowledgements",
        registeredNodes[0].pendingTargetState == PENDING_TO_UNPAIRED &&
        registeredNodes[1].pendingTargetState == PENDING_TO_UNPAIRED &&
        gDesired[0].config.targetState == 0 &&
        gDesired[1].config.targetState == 0);

  Serial.printf("=== SUITE: %d passed, %d failed ===\n", gPass, gFail);
}

}  // namespace

NodeDesiredConfig getDesiredConfig(const char* nodeId) {
  NodeDesiredConfig* desired = desiredFor(nodeId);
  return desired ? *desired : NodeDesiredConfig{};
}

bool setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg) {
  NodeDesiredConfig* desired = desiredFor(nodeId);
  if (!desired) return false;
  *desired = cfg;
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
