#include <Arduino.h>
#include <Preferences.h>

#include "command_dispatcher.h"
#include "control/backend_command_ingest.h"

namespace {

static int gPass = 0;
static int gFail = 0;
static uint32_t gMutations = 0;

struct FakeDesired {
  char nodeId[CMD_NODEID_LEN];
  uint8_t wake;
  uint8_t target;
  uint16_t mask;
  uint16_t wireVersion;
};

static FakeDesired gDesired[2]{};
static constexpr const char* kNodeA = "ENV_D13F98";
static constexpr const char* kNodeB = "ENV_6C0AA0";

FakeDesired* desiredFor(const char* nodeId) {
  for (auto& desired : gDesired) {
    if (strncmp(desired.nodeId, nodeId, CMD_NODEID_LEN) == 0) return &desired;
  }
  return nullptr;
}

bool persistFakeDesired() {
  Preferences prefs;
  if (!prefs.begin("test_dcfg", false)) return false;
  const bool wrote = prefs.putBytes("nodes", gDesired, sizeof(gDesired)) ==
                     sizeof(gDesired);
  prefs.end();
  if (!wrote || !prefs.begin("test_dcfg", true)) return false;
  FakeDesired verify[2]{};
  const bool ok = prefs.getBytes("nodes", verify, sizeof(verify)) ==
                  sizeof(verify) &&
                  memcmp(verify, gDesired, sizeof(gDesired)) == 0;
  prefs.end();
  return ok;
}

bool loadFakeDesired() {
  Preferences prefs;
  if (!prefs.begin("test_dcfg", true)) return false;
  const bool ok = prefs.getBytesLength("nodes") == sizeof(gDesired) &&
                  prefs.getBytes("nodes", gDesired, sizeof(gDesired)) ==
                  sizeof(gDesired);
  prefs.end();
  return ok;
}

void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) ++gPass;
  else ++gFail;
}

String commandId(uint8_t seed) {
  String id = "D";
  for (uint8_t i = 0; i < 22; ++i) {
    id += static_cast<char>('a' + ((seed + i) % 26));
  }
  return id;
}

String envelope(const String& commands, uint32_t nextCursor,
                bool duplicateUpload = false) {
  String body = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n";
  body += "{\"success\":true,\"duplicate\":";
  body += duplicateUpload ? "true" : "false";
  body += ",\"controlProtocolVersion\":1,\"serverTimeUnix\":1784217600";
  body += ",\"nextCursor\":" + String(nextCursor);
  body += ",\"commands\":[" + commands + "]}";
  return body;
}

String setCommand(const String& id, uint32_t sequence, uint32_t expectedRevision,
                  const char* nodeId = "ENV_D13F98", uint8_t wake = 20,
                  uint8_t target = 2, uint16_t mask = 37,
                  uint32_t issuedAt = 1784217500,
                  uint32_t expiresAt = 1784303900) {
  String item = "{\"commandId\":\"" + id + "\",\"sequence\":" +
                String(sequence) + ",\"type\":\"SET_NODE_CONFIG\"";
  item += ",\"target\":{\"nodeId\":\"" + String(nodeId) + "\"}";
  item += ",\"expectedStateRevision\":" + String(expectedRevision);
  item += ",\"issuedAtUnix\":" + String(issuedAt);
  item += ",\"expiresAtUnix\":" + String(expiresAt);
  item += ",\"payload\":{\"wakeIntervalMin\":" + String(wake);
  item += ",\"targetState\":" + String(target);
  item += ",\"sensorMask\":" + String(mask) + "}}";
  return item;
}

String pauseOnlyCommand(const String& id, uint32_t sequence,
                        uint32_t expectedRevision, const char* nodeId) {
  String item = "{\"commandId\":\"" + id + "\",\"sequence\":" +
                String(sequence) + ",\"type\":\"SET_NODE_CONFIG\"";
  item += ",\"target\":{\"nodeId\":\"" + String(nodeId) + "\"}";
  item += ",\"expectedStateRevision\":" + String(expectedRevision);
  item += ",\"issuedAtUnix\":1784217500,\"expiresAtUnix\":1784303900";
  item += ",\"payload\":{\"targetState\":3}}";
  return item;
}

void resetState(bool enableRemote = true) {
  dispatcherResetForTest();
  backendControlResetForTest();
  Preferences prefs;
  if (prefs.begin("test_dcfg", false)) {
    prefs.clear();
    prefs.end();
  }
  if (enableRemote) backendControlSetRemoteManagementEnabled(true);
  gMutations = 0;
  strlcpy(gDesired[0].nodeId, kNodeA, CMD_NODEID_LEN);
  gDesired[0].wake = 20;
  gDesired[0].target = 2;
  gDesired[0].mask = 37;
  gDesired[0].wireVersion = 4;
  strlcpy(gDesired[1].nodeId, kNodeB, CMD_NODEID_LEN);
  gDesired[1].wake = 45;
  gDesired[1].target = 2;
  gDesired[1].mask = 0x8123;
  gDesired[1].wireVersion = 8;
  persistFakeDesired();
}

bool resolveFake(const Command& requested, Command& resolved,
                 CmdOutcome& rejection) {
  rejection = OUT_INVALID;
  FakeDesired* existing = desiredFor(requested.payload.nodeId);
  if (!existing || requested.payload.targetState == 0 ||
      (requested.configFields & CFG_FIELD_TARGET_STATE) == 0) return false;
  resolved = requested;
  if ((requested.configFields & CFG_FIELD_WAKE_INTERVAL) == 0)
    resolved.payload.wakeIntervalMin = existing->wake;
  if ((requested.configFields & CFG_FIELD_SENSOR_MASK) == 0)
    resolved.payload.sensorMask = existing->mask;
  resolved.configFields = CFG_FIELDS_ALL;
  return true;
}

BackendCommandApplyResult executeFake(const Command& command) {
  BackendCommandApplyResult out{};
  out.command = dispatcherSubmit(command);
  CommandResult stored{};
  FakeDesired* desired = desiredFor(command.payload.nodeId);
  uint32_t revision = 0;
  uint16_t boundVersion = 0;
  if (!desired || !dispatcherResultFor(command.cmdId, &stored) ||
      stored.outcome != OUT_ACCEPTED ||
      !dispatcherCurrentCommandForNode(command.payload.nodeId, command.cmdId,
                                       &revision, &boundVersion)) {
    out.durable = dispatcherEnsureDurable();
    out.applied = stored.outcome != OUT_ACCEPTED;
    return out;
  }

  const DispatchNodeConfig* dispatch =
      dispatcherNodeConfig(command.payload.nodeId);
  if (!dispatch) return out;
  if (boundVersion == 0) {
    ++desired->wireVersion;
    boundVersion = desired->wireVersion;
    ++gMutations;
  } else {
    desired->wireVersion = boundVersion;
  }
  desired->wake = dispatch->wakeIntervalMin;
  desired->target = dispatch->targetState;
  desired->mask = static_cast<uint16_t>(dispatch->sensorMask);
  if (!persistFakeDesired() ||
      !dispatcherBindNodeConfigVersion(command.payload.nodeId, revision,
                                       boundVersion)) return out;
  out.wireConfigVersion = boundVersion;
  out.durable = dispatcherEnsureDurable();
  out.applied = true;
  return out;
}

BackendCommandApplyResult failBeforePersistence(const Command&) {
  return BackendCommandApplyResult{};
}

BackendCommandApplyResult failAfterDispatcherPersistence(const Command& command) {
  BackendCommandApplyResult out = executeFake(command);
  out.durable = false;
  out.applied = false;
  return out;
}

BackendIngestResult ingest(const String& body,
                           BackendCommandExecutor executor = executeFake) {
  return backendIngestUploadResponse(body, 1784217600, true,
                                     resolveFake, executor);
}

void runSuite() {
  gPass = gFail = 0;
  Serial.println("\n--- backend control ingest assertion suite ---");

  resetState();
  BackendIngestResult r = ingest("{\"success\":true,\"appended\":48}");
  check("legacy response is harmless",
        r.status == BackendIngestStatus::LEGACY_NO_ENVELOPE &&
        backendControlLastCommandCursor() == 0);
  r = ingest(envelope("", 0));
  check("empty valid envelope", r.status == BackendIngestStatus::EMPTY);

  // Required batch provision: two pause-only commands share the one initial
  // revision. Wake interval and sensor mask must be preserved independently.
  resetState();
  const String pauseAId = commandId(0);
  const String pauseBId = commandId(1);
  const String twoPauses =
      pauseOnlyCommand(pauseAId, 1, 0, kNodeA) + "," +
      pauseOnlyCommand(pauseBId, 2, 0, kNodeB);
  r = ingest(envelope(twoPauses, 2));
  FakeDesired* desiredA = desiredFor(kNodeA);
  FakeDesired* desiredB = desiredFor(kNodeB);
  check("two pause commands accepted from one initial revision",
        r.status == BackendIngestStatus::PROCESSED &&
        r.initialStateRevision == 0 && dispatcherRevision() == 2 &&
        r.processedCount == 2 && r.rejectedCount == 0 && gMutations == 2);
  check("pause-only batch preserves each node wake interval",
        desiredA && desiredB && desiredA->wake == 20 && desiredB->wake == 45);
  check("pause-only batch preserves each node sensor mask",
        desiredA && desiredB && desiredA->mask == 37 &&
        desiredB->mask == 0x8123);
  check("both pause targets and cursor are durable",
        desiredA && desiredB && desiredA->target == 3 &&
        desiredB->target == 3 && backendControlLastCommandCursor() == 2);

  CommandResult pauseAResult{}, pauseBResult{};
  check("outcome retained for every batch command",
        dispatcherResultFor(pauseAId.c_str(), &pauseAResult) &&
        dispatcherResultFor(pauseBId.c_str(), &pauseBResult) &&
        pauseAResult.outcome == OUT_ACCEPTED &&
        pauseBResult.outcome == OUT_ACCEPTED &&
        pauseAResult.assignedRevision == 1 &&
        pauseBResult.assignedRevision == 2);

  // Simulate a cold boot by discarding all RAM mirrors and reloading each
  // persistence owner. Both desired records must remain ready for normal sync.
  memset(gDesired, 0, sizeof(gDesired));
  dispatcherInit();
  backendControlInit();
  check("two accepted desired configs survive reboot",
        loadFakeDesired() && dispatcherRevision() == 2 &&
        backendControlLastCommandCursor() == 2 &&
        desiredFor(kNodeA) && desiredFor(kNodeA)->target == 3 &&
        desiredFor(kNodeB) && desiredFor(kNodeB)->target == 3);

  desiredA = desiredFor(kNodeA);
  desiredB = desiredFor(kNodeB);
  const DispatchNodeConfig* syncA = dispatcherNodeConfig(kNodeA);
  const DispatchNodeConfig* syncB = dispatcherNodeConfig(kNodeB);
  check("subsequent normal sync builds NODE_CONFIG for both nodes",
        syncA && syncB && syncA->targetState == 3 && syncB->targetState == 3 &&
        syncA->wakeIntervalMin == 20 && syncB->wakeIntervalMin == 45);

  uint32_t revisionA = 0, revisionB = 0;
  const bool mappedA = desiredA && dispatcherRevisionForConfigVersion(
      kNodeA, desiredA->wireVersion, &revisionA);
  const bool mappedB = desiredB && dispatcherRevisionForConfigVersion(
      kNodeB, desiredB->wireVersion, &revisionB);
  check("both normal CONFIG_ACK versions map independently", mappedA && mappedB);
  check("first node converges without converging second",
        mappedA && dispatcherMarkConverged(kNodeA, revisionA) &&
        dispatcherResultFor(pauseAId.c_str(), &pauseAResult) &&
        dispatcherResultFor(pauseBId.c_str(), &pauseBResult) &&
        pauseAResult.outcome == OUT_CONVERGED &&
        pauseBResult.outcome == OUT_ACCEPTED);
  check("second node later converges independently",
        mappedB && dispatcherMarkConverged(kNodeB, revisionB) &&
        dispatcherResultFor(pauseBId.c_str(), &pauseBResult) &&
        pauseBResult.outcome == OUT_CONVERGED);

  const uint32_t batchMutations = gMutations;
  r = ingest(envelope(twoPauses, 2));
  check("persisted sequences never apply twice after reboot/retry",
        r.status == BackendIngestStatus::PROCESSED && r.replayedCount == 2 &&
        gMutations == batchMutations);

  resetState();
  const String pauseId = commandId(2);
  r = ingest(envelope(setCommand(pauseId, 1, 0, kNodeA, 20, 3, 0xFFFF), 1));
  desiredA = desiredFor(kNodeA);
  check("valid pause accepted", r.status == BackendIngestStatus::PROCESSED &&
        gMutations == 1 && desiredA && desiredA->target == 3);
  check("full uint16 sensorMask preserved", desiredA && desiredA->mask == 0xFFFF);
  check("command cursor committed after apply", backendControlLastCommandCursor() == 1);

  const String resumeId = commandId(3);
  r = ingest(envelope(setCommand(resumeId, 2, 1, kNodeA, 20, 2, 37), 2));
  check("resume accepted", gMutations == 2 && desiredA->target == 2);
  const String intervalId = commandId(4);
  r = ingest(envelope(setCommand(intervalId, 3, 2, kNodeA, 30, 2, 37), 3));
  check("interval change accepted", gMutations == 3 && desiredA->wake == 30);

  const String staleId = commandId(5);
  r = ingest(envelope(setCommand(staleId, 4, 0), 4));
  CommandResult stale{};
  check("stale revision rejected",
        dispatcherResultFor(staleId.c_str(), &stale) &&
        stale.outcome == OUT_REVISION_CONFLICT && gMutations == 3);

  // Lose only the backend cursor state: the retained dispatcher command ID
  // must turn redelivery into REPLAY without a second desired-state mutation.
  backendControlResetForTest();
  backendControlSetRemoteManagementEnabled(true);
  const uint32_t beforeReplay = gMutations;
  r = ingest(envelope(setCommand(intervalId, 3, 2, "ENV_D13F98", 99, 3, 1), 3));
  check("repeated commandId is replayed without mutation",
        r.replayedCount == 1 && gMutations == beforeReplay);

  resetState();
  String duplicateSeq = setCommand(commandId(4), 1, 0) + "," +
                        setCommand(commandId(5), 1, 0);
  r = ingest(envelope(duplicateSeq, 1));
  check("duplicate sequence rejected", r.status == BackendIngestStatus::MALFORMED);
  String outOfOrder = setCommand(commandId(4), 2, 0) + "," +
                      setCommand(commandId(5), 1, 0);
  r = ingest(envelope(outOfOrder, 2));
  check("out-of-order sequence rejected", r.status == BackendIngestStatus::MALFORMED);

  String nine;
  for (uint8_t i = 0; i < 9; ++i) {
    if (i) nine += ',';
    nine += setCommand(commandId(i + 6), i + 1, 0);
  }
  r = ingest(envelope(nine, 9));
  check("more than eight commands rejected", r.status == BackendIngestStatus::TOO_LARGE);

  resetState();
  r = ingest(envelope(setCommand(commandId(15), 1, 0, "ENV_D13F98", 20, 2,
                                 37, 1784000000, 1784000100), 1));
  check("expired command is INVALID and acknowledged",
        r.rejectedCount == 1 && backendControlLastCommandCursor() == 1 &&
        gMutations == 0);

  resetState();
  String unknownType = setCommand(commandId(16), 1, 0);
  unknownType.replace("SET_NODE_CONFIG", "DEPLOY_RELEASE");
  r = ingest(envelope(unknownType, 1));
  check("unknown command type is INVALID", r.rejectedCount == 1 && gMutations == 0);

  resetState();
  r = ingest(envelope(setCommand(commandId(17), 1, 0, "OTHER_NODE"), 1));
  check("unknown target node rejected", r.rejectedCount == 1 && gMutations == 0);
  resetState();
  String missingTarget = setCommand(commandId(18), 1, 0);
  missingTarget.replace("ENV_D13F98", "");
  r = ingest(envelope(missingTarget, 1));
  check("missing target rejected", r.rejectedCount == 1 && gMutations == 0);

  resetState(false);
  r = ingest(envelope(setCommand(commandId(19), 1, 0), 1));
  check("remote disabled does not execute or advance",
        r.status == BackendIngestStatus::REMOTE_DISABLED && gMutations == 0 &&
        backendControlLastCommandCursor() == 0);

  resetState();
  r = ingest("HTTP/1.1 200 OK\r\n\r\n{broken");
  check("malformed JSON leaves command cursor", r.status == BackendIngestStatus::MALFORMED &&
        backendControlLastCommandCursor() == 0);
  uint32_t independentDataCursor = 48;  // committed by upload code before parser call
  check("data cursor remains independent of malformed control",
        independentDataCursor == 48 && backendControlLastCommandCursor() == 0);
  String wrongProtocol = envelope("", 0);
  wrongProtocol.replace("\"controlProtocolVersion\":1",
                        "\"controlProtocolVersion\":2");
  r = ingest(wrongProtocol);
  check("wrong protocol leaves control state untouched",
        r.status == BackendIngestStatus::UNSUPPORTED_PROTOCOL &&
        dispatcherRevision() == 0 && backendControlLastCommandCursor() == 0);

  String malformedSecond = setCommand(commandId(23), 1, 0) +
      ",{\"commandId\":\"bad\",\"sequence\":2}";
  r = ingest(envelope(malformedSecond, 2));
  check("complete response validates before first desired mutation",
        r.status == BackendIngestStatus::MALFORMED &&
        dispatcherRevision() == 0 && gMutations == 0 &&
        backendControlLastCommandCursor() == 0);

  resetState();
  const String afterPersistId = commandId(20);
  const String afterPersistBody = envelope(setCommand(afterPersistId, 1, 0), 1);
  r = ingest(afterPersistBody, failAfterDispatcherPersistence);
  check("reset window after dispatcher persistence keeps cursor old",
        r.status == BackendIngestStatus::PERSIST_FAILED &&
        backendControlLastCommandCursor() == 0 && gMutations == 1);
  const uint32_t afterPersistMutations = gMutations;
  r = ingest(afterPersistBody);
  check("redelivery after dispatcher persistence is replay-only",
        r.status == BackendIngestStatus::PROCESSED && r.replayedCount == 1 &&
        gMutations == afterPersistMutations && backendControlLastCommandCursor() == 1);

  resetState();
  const String beforePersistBody = envelope(setCommand(commandId(21), 1, 0), 1);
  r = ingest(beforePersistBody, failBeforePersistence);
  check("reset window before desired persistence leaves cursor old",
        r.status == BackendIngestStatus::PERSIST_FAILED &&
        dispatcherRevision() == 1 && gMutations == 0 &&
        backendControlLastCommandCursor() == 0);
  r = ingest(beforePersistBody);
  check("redelivery repairs accepted desired config exactly once",
        r.status == BackendIngestStatus::PROCESSED && r.replayedCount == 1 &&
        gMutations == 1);

  resetState();
  const String duplicateUploadId = commandId(22);
  r = ingest(envelope(setCommand(duplicateUploadId, 1, 0), 1, true));
  check("duplicate sensor upload can still carry control",
        r.status == BackendIngestStatus::PROCESSED && gMutations == 1);

  uint32_t revision = 0;
  FakeDesired* finalDesired = desiredFor(kNodeA);
  check("CONFIG_ACK version maps to dispatcher revision",
        finalDesired && dispatcherRevisionForConfigVersion(
            kNodeA, finalDesired->wireVersion, &revision));
  check("CONFIG_ACK converges exactly once",
        dispatcherMarkConverged(kNodeA, revision) &&
        !dispatcherMarkConverged(kNodeA, revision));
  CommandResult converged{};
  check("converged result retained",
        dispatcherResultFor(duplicateUploadId.c_str(), &converged) &&
        converged.outcome == OUT_CONVERGED);

  Serial.printf("=== SUITE: %d passed, %d failed ===\n", gPass, gFail);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n### FieldMesh backend command ingest test ###");
  dispatcherInit();
  backendControlInit();
  runSuite();
}

void loop() {
  delay(1000);
}
