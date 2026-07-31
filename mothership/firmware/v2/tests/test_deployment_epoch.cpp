// Deployment-epoch regression suite.
//
// Links the REAL deployment_epoch.cpp and deployment_store.cpp (including its
// LittleFS commit path) and fakes only the node_registry surface those two use.
// The store half must be real: the whole point of the record is that it
// round-trips a cold boot, and a mocked store would prove nothing — the
// wakeIntervalMin bug exists precisely because nothing ever tested that.
//
// Serial contract: RESULT-style [PASS]/[FAIL] lines plus an OVERALL summary.

#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

#include "config/node_registry.h"
#include "config/deployment_store.h"
#include "config/deployment_epoch.h"

// ---------------------------------------------------------------------------
// Fake node_registry
// ---------------------------------------------------------------------------
std::vector<NodeInfo> registeredNodes;

struct MetaEntry { String nodeId, userId, name; };
static std::vector<MetaEntry> gMeta;

static MetaEntry& metaFor(const String& nodeId) {
  for (auto& m : gMeta) {
    if (m.nodeId == nodeId) return m;
  }
  gMeta.push_back({nodeId, "", ""});
  return gMeta.back();
}

String normalizeUserId(String userId) {
  String cleaned;
  userId.trim();
  for (size_t i = 0; i < userId.length(); ++i) {
    char c = userId[i];
    if (c >= '0' && c <= '9') {
      cleaned += c;
      if (cleaned.length() >= 3) break;
    }
  }
  if (cleaned.length() > 0 && cleaned.length() < 3) {
    while (cleaned.length() < 3) cleaned = "0" + cleaned;
  }
  return cleaned;
}

String getNodeUserId(const String& nodeId) { return metaFor(nodeId).userId; }
void   setNodeUserId(const String& nodeId, String userId) {
  metaFor(nodeId).userId = normalizeUserId(userId);
}
String getNodeName(const String& nodeId) { return metaFor(nodeId).name; }
void   setNodeName(const String& nodeId, String name) { metaFor(nodeId).name = name; }

// ---------------------------------------------------------------------------
// Injected collaborators
// ---------------------------------------------------------------------------
static uint32_t gFakeNow = 1753000000UL;   // 2025-07-20, comfortably plausible
static uint32_t fakeNow() { return gFakeNow; }

static bool gConfigApplyOk = true;
static int  gLastTargetState = -1;
static bool fakeConfigApply(const String& nodeId, uint8_t targetState) {
  (void)nodeId;
  gLastTargetState = targetState;
  return gConfigApplyOk;
}

static bool     gLegacyBacklog = false;
static uint32_t gLegacyRows = 0;
static bool fakeLegacyBacklog(uint32_t* rowsOut) {
  if (rowsOut) *rowsOut = gLegacyRows;
  return gLegacyBacklog;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
static int gPass = 0, gFail = 0;

static bool check(const char* name, bool cond) {
  Serial.printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", name);
  cond ? ++gPass : ++gFail;
  return cond;
}

static void addNode(const char* nodeId, uint8_t lastMacByte, NodeState state) {
  NodeInfo n{};
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, lastMacByte};
  memcpy(n.mac, mac, 6);
  n.nodeId = nodeId;
  n.state = state;
  n.latitude = NAN;
  n.longitude = NAN;
  registeredNodes.push_back(n);
}

// Wipe RAM + flash so each fixture starts clean.
static void resetAll() {
  registeredNodes.clear();
  gMeta.clear();
  gConfigApplyOk = true;
  gLegacyBacklog = false;
  gLegacyRows = 0;
  gLastTargetState = -1;
  gFakeNow = 1753000000UL;
  LittleFS.remove("/deploy.bin");
  LittleFS.remove("/deploy.bak");
  LittleFS.remove("/deploy.tmp");
  deploymentStoreBegin();
}

static uint16_t epochOf(const char* nodeId) {
  const DeploymentSlot* s = deploymentFindByNodeId(nodeId);
  return s ? s->epoch : 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testStartIncrementsAndStamps() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);

  DeploymentOpResult r = beginNewDeployment("ENV_A1", "001", "North Hedge",
                                            NAN, NAN, 0);
  check("start: first deployment succeeds", r.status == DEPLOY_OK);
  check("start: epoch becomes 1", epochOf("ENV_A1") == 1);
  check("start: queues ACTIVE (targetState 2)", gLastTargetState == 2);
  check("start: readings after it carry epoch 1",
        resolveEpochForSample("ENV_A1", gFakeNow + 60) == 1);

  // Second deployment.
  gFakeNow += 86400;
  DeploymentOpResult r2 = beginNewDeployment("ENV_A1", "002", "South Gate",
                                             NAN, NAN, 1);
  check("start: second deployment needs the current deployment ended",
        r2.status == DEPLOY_ERR_STATE);

  endDeployment("ENV_A1", 1);
  gFakeNow += 3600;
  DeploymentOpResult r3 = beginNewDeployment("ENV_A1", "002", "South Gate",
                                             NAN, NAN, 1);
  check("start: after End, epoch increments to 2",
        r3.status == DEPLOY_OK && epochOf("ENV_A1") == 2);
  check("start: readings after it carry epoch 2",
        resolveEpochForSample("ENV_A1", gFakeNow + 60) == 2);
}

static void testBufferedReadingKeepsOldEpoch() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  const uint32_t sampledUnderEpoch1 = gFakeNow + 60;

  gFakeNow += 86400;
  endDeployment("ENV_A1", 1);
  gFakeNow += 3600;
  beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 1);

  check("backlog: a sample taken under epoch 1 still resolves to 1",
        resolveEpochForSample("ENV_A1", sampledUnderEpoch1) == 1);
  check("backlog: a sample taken now resolves to 2",
        resolveEpochForSample("ENV_A1", gFakeNow + 60) == 2);
}

// F5: the round-3 design decremented by one, which misfiles an epoch-1 backlog
// as epoch 2 when the node is already on epoch 3.
static void testEpoch1SampleDuringEpoch3() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);

  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  const uint32_t epoch1Sample = gFakeNow + 60;

  gFakeNow += 86400; endDeployment("ENV_A1", 1);
  gFakeNow += 3600;  beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 1);
  const uint32_t epoch2Sample = gFakeNow + 60;

  gFakeNow += 86400; endDeployment("ENV_A1", 2);
  gFakeNow += 3600;  beginNewDeployment("ENV_A1", "003", "C", NAN, NAN, 2);

  check("multi-epoch: current epoch is 3", epochOf("ENV_A1") == 3);
  check("multi-epoch: epoch-1 sample resolves to 1, NOT 2",
        resolveEpochForSample("ENV_A1", epoch1Sample) == 1);
  check("multi-epoch: epoch-2 sample resolves to 2",
        resolveEpochForSample("ENV_A1", epoch2Sample) == 2);
}

// A node with a flat coin cell reports 2000-01-01. Correcting on that would
// misfile a live node's readings into an earlier deployment.
static void testImplausibleClockDoesNotDecrement() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow += 86400; endDeployment("ENV_A1", 1);
  gFakeNow += 3600;  beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 1);

  check("dead clock: 2000-01-01 sample keeps the CURRENT epoch",
        resolveEpochForSample("ENV_A1", 946684800UL) == 2);
  check("dead clock: epoch 0 for an unknown node",
        resolveEpochForSample("ENV_NOPE", gFakeNow) == 0);
}

// Samples taken between End and the node's next check-in belong to the site it
// just left, not to the next one.
static void testInTransitKeepsOldEpoch() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow += 86400;
  endDeployment("ENV_A1", 1);
  const uint32_t inTransit = gFakeNow + 600;

  check("in transit: sample after endedUnix keeps epoch 1, not 0",
        resolveEpochForSample("ENV_A1", inTransit) == 1);
}

static void testStopStartDoesNotChangeEpoch() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);

  DeploymentOpResult r = ensureFirstDeployment("ENV_A1");
  check("stop->start: no epoch change on an active deployment",
        r.status == DEPLOY_OK && epochOf("ENV_A1") == 1);
}

static void testEditingIdentityDoesNotChangeEpoch() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);

  setNodeUserId("ENV_A1", "009");
  setNodeName("ENV_A1", "Renamed");
  check("edit: renumbering leaves the epoch alone", epochOf("ENV_A1") == 1);
  check("edit: identity edit is captured for upload",
        deploymentTouchActiveEvent("ENV_A1"));
}

static void testNumberGuard() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  addNode("ENV_A2", 0x02, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);

  DeploymentGuardResult g = checkNumberAvailable("001", "ENV_A2");
  check("guard: an active number is rejected", !g.free);
  check("guard: the error names the holder", g.holderLabel == "North Hedge");
  check("guard: 1 collides with 001 (normalised)",
        !checkNumberAvailable("1", "ENV_A2").free);

  DeploymentOpResult r = beginNewDeployment("ENV_A2", "001", "Clash", NAN, NAN, 0);
  check("guard: Start with a taken number fails",
        r.status == DEPLOY_ERR_NUMBER_TAKEN);
  check("guard: rejection did NOT mutate stored identity",
        getNodeUserId("ENV_A2") == "");

  // F0.4: an ended deployment keeps its number for the archive record but must
  // not hold it against another node.
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);
  check("guard: ended deployment still carries its number",
        getNodeUserId("ENV_A1") == "001");
  check("guard: an ended deployment does NOT hold the number",
        checkNumberAvailable("001", "ENV_A2").free);
  DeploymentOpResult r2 = beginNewDeployment("ENV_A2", "001", "Reuse", NAN, NAN, 0);
  check("guard: the freed number is accepted", r2.status == DEPLOY_OK);
}

// The workflow this feature exists for: move four nodes and permute numbers.
static void testFourNodePermute() {
  resetAll();
  const char* ids[4] = {"ENV_N1", "ENV_N2", "ENV_N3", "ENV_N4"};
  for (int i = 0; i < 4; ++i) addNode(ids[i], 0x10 + i, DEPLOYED);
  for (int i = 0; i < 4; ++i) {
    char num[4]; snprintf(num, sizeof(num), "00%d", i + 1);
    beginNewDeployment(ids[i], num, "site A", NAN, NAN, 0);
  }
  gFakeNow += 86400;
  for (int i = 0; i < 4; ++i) endDeployment(ids[i], 1);

  gFakeNow += 3600;
  // Permute: 1->3, 2->4, 3->1, 4->2. Impossible without releasing the numbers.
  const char* newNums[4] = {"003", "004", "001", "002"};
  bool allOk = true;
  for (int i = 0; i < 4; ++i) {
    DeploymentOpResult r =
        beginNewDeployment(ids[i], newNums[i], "site B", NAN, NAN, 1);
    if (r.status != DEPLOY_OK) allOk = false;
  }
  check("permute: all four restart with swapped numbers", allOk);
  check("permute: every node is on epoch 2",
        epochOf(ids[0]) == 2 && epochOf(ids[1]) == 2 &&
        epochOf(ids[2]) == 2 && epochOf(ids[3]) == 2);
}

static void testRtcUnsetRejects() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  gFakeNow = 100;   // hub clock never set
  DeploymentOpResult r = beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  check("rtc: Start refused when the hub clock is unset",
        r.status == DEPLOY_ERR_RTC_UNSET);
  check("rtc: no epoch was committed", epochOf("ENV_A1") == 0);

  gFakeNow = 1753000000UL;
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow = 100;
  DeploymentOpResult e = endDeployment("ENV_A1", 1);
  check("rtc: End refused when the hub clock is unset",
        e.status == DEPLOY_ERR_RTC_UNSET);
}

// F4: a lost response must not increment twice.
static void testRetryIdempotency() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);

  // Repeat carrying the stale prior epoch (0), as a double submit would.
  DeploymentOpResult again = beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  check("retry: repeated Start is reported as a replay",
        again.status == DEPLOY_REPLAYED);
  check("retry: epoch did NOT increment twice", epochOf("ENV_A1") == 1);

  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);
  DeploymentOpResult endAgain = endDeployment("ENV_A1", 1);
  check("retry: repeated End is idempotent",
        endAgain.status == DEPLOY_REPLAYED);
}

// F4: a failed desired-config queue must not leave a live epoch the node knows
// nothing about.
static void testStartRollsBackWhenConfigFails() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  gConfigApplyOk = false;
  DeploymentOpResult r = beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  check("rollback: Start fails when ACTIVE cannot be queued",
        r.status == DEPLOY_ERR_CONFIG_QUEUE);
  check("rollback: epoch unchanged", epochOf("ENV_A1") == 0);
  const DeploymentSlot* s = deploymentFindByNodeId("ENV_A1");
  check("rollback: no pending transition left behind",
        s && s->pendingOp == DEPLOY_OP_NONE);
  check("rollback: identity was NOT written", getNodeUserId("ENV_A1") == "");
}

// Review F2: rollback must restore the COMPLETE slot, not just epoch/started/
// ended. Boundary history and staged identity are part of the record; restoring
// a subset leaves it internally inconsistent.
static void testStartRollbackRestoresWholeSlot() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);   // epoch 1
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);
  gFakeNow += 60;

  const DeploymentSlot before = *deploymentFindByNodeId("ENV_A1");

  gConfigApplyOk = false;
  DeploymentOpResult r = beginNewDeployment("ENV_A1", "002", "B", 1.5f, 2.5f, 1);
  check("slot rollback: Start failed", r.status == DEPLOY_ERR_CONFIG_QUEUE);

  const DeploymentSlot* after = deploymentFindByNodeId("ENV_A1");
  check("slot rollback: epoch restored", after && after->epoch == before.epoch);
  check("slot rollback: startedUnix restored",
        after && after->startedUnix == before.startedUnix);
  check("slot rollback: endedUnix restored",
        after && after->endedUnix == before.endedUnix);
  check("slot rollback: boundary history length restored",
        after && after->historyCount == before.historyCount);
  check("slot rollback: staged identity restored",
        after && after->hasStagedIdentity == before.hasStagedIdentity);
  check("slot rollback: no pending op left",
        after && after->pendingOp == DEPLOY_OP_NONE);
}

// Review F2: once ACTIVE is durably queued the node may start recording, so a
// crash before the final commit must COMPLETE the Start on the next boot — not
// abandon it, which would stamp those readings against the previous epoch.
static void testInterruptedStartCompletesForward() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);
  gFakeNow += 60;

  // Reproduce the persisted phase-1 intent exactly as beginNewDeployment writes
  // it, then crash before phase 3.
  DeploymentSlot* s = deploymentSlotFor(registeredNodes[0].mac, "ENV_A1");
  s->pendingOp    = DEPLOY_OP_START;
  s->pendingEpoch = 2;
  s->pendingUnix  = gFakeNow;
  s->hasStagedIdentity = true;
  strncpy(s->stagedUserId, "002", sizeof(s->stagedUserId) - 1);
  strncpy(s->stagedName, "South Gate", sizeof(s->stagedName) - 1);
  s->stagedLat = -27.5f;
  s->stagedLon = 153.0f;
  deploymentStoreCommit();

  deploymentStoreResetForTest();
  deploymentStoreBegin();
  deploymentRecoverPending();

  check("interrupted start: epoch completed forward to 2", epochOf("ENV_A1") == 2);
  const DeploymentSlot* after = deploymentFindByNodeId("ENV_A1");
  check("interrupted start: deployment is active again",
        after && after->endedUnix == 0);
  check("interrupted start: staged number applied",
        getNodeUserId("ENV_A1") == "002");
  check("interrupted start: staged name applied",
        getNodeName("ENV_A1") == "South Gate");
  check("interrupted start: pending cleared",
        after && after->pendingOp == DEPLOY_OP_NONE);
  check("interrupted start: ACTIVE re-asserted", gLastTargetState == 2);
  check("interrupted start: readings now resolve to epoch 2",
        resolveEpochForSample("ENV_A1", gFakeNow + 60) == 2);
  check("interrupted start: the epoch-2 event reached the outbox",
        deploymentOutboxToJson().indexOf("\"deploymentEpoch\":2") >= 0);
}

// Phase-1 intent alone is not enough to publish the epoch. If recovery cannot
// durably establish ACTIVE, it must retain the pending operation and retry on a
// later wake rather than reporting a deployment the node has not entered.
static void testInterruptedStartWaitsForActiveQueue() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);
  gFakeNow += 60;

  DeploymentSlot* s = deploymentSlotFor(registeredNodes[0].mac, "ENV_A1");
  s->pendingOp = DEPLOY_OP_START;
  s->pendingEpoch = 2;
  s->pendingUnix = gFakeNow;
  s->hasStagedIdentity = true;
  strncpy(s->stagedUserId, "002", sizeof(s->stagedUserId) - 1);
  strncpy(s->stagedName, "South Gate", sizeof(s->stagedName) - 1);
  deploymentStoreCommit();

  deploymentStoreResetForTest();
  deploymentStoreBegin();
  gConfigApplyOk = false;
  deploymentRecoverPending();

  const DeploymentSlot* blocked = deploymentFindByNodeId("ENV_A1");
  check("interrupted start failure: previous epoch remains authoritative",
        blocked && blocked->epoch == 1 && blocked->endedUnix != 0);
  check("interrupted start failure: START remains pending",
        blocked && blocked->pendingOp == DEPLOY_OP_START &&
        blocked->pendingEpoch == 2);
  check("interrupted start failure: staged identity is not published",
        getNodeUserId("ENV_A1") == "001" && getNodeName("ENV_A1") == "North Hedge");
  check("interrupted start failure: epoch-2 event is not emitted",
        deploymentOutboxToJson().indexOf("\"deploymentEpoch\":2") < 0);

  gConfigApplyOk = true;
  deploymentRecoverPending();
  const DeploymentSlot* recovered = deploymentFindByNodeId("ENV_A1");
  check("interrupted start retry: epoch 2 completes after ACTIVE is durable",
        recovered && recovered->epoch == 2 && recovered->endedUnix == 0 &&
        recovered->pendingOp == DEPLOY_OP_NONE);
}

// Review F2: a full outbox must be refused BEFORE any intent is persisted,
// because after ACTIVE is queued there is no safe way to unwind.
static void testOutboxFullRefusedBeforeIntent() {
  resetAll();
  // Fill the outbox with unrelated events.
  for (int i = 0; i < (int)kMaxOutboxEvents; ++i) {
    DeploymentEvent e{};
    snprintf(e.eventId, sizeof(e.eventId), "FILLER-%d", i);
    strncpy(e.nodeId, "ENV_FILL", sizeof(e.nodeId) - 1);
    e.deploymentEpoch = 1;
    e.inUse = true;
    deploymentOutboxUpsert(e);
  }
  addNode("ENV_A1", 0x01, DEPLOYED);

  DeploymentOpResult r = beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  check("outbox full: Start refused", r.status == DEPLOY_ERR_OUTBOX_FULL);
  check("outbox full: epoch untouched", epochOf("ENV_A1") == 0);
  const DeploymentSlot* s = deploymentFindByNodeId("ENV_A1");
  check("outbox full: no intent was persisted",
        !s || s->pendingOp == DEPLOY_OP_NONE);
  check("outbox full: node was never told to go ACTIVE", gLastTargetState == -1);
  check("outbox full: identity untouched", getNodeUserId("ENV_A1") == "");
}

// Review F3: unpair must not proceed when the End could not be archived.
static void testFailedEndBlocksUnpair() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);

  // Hub clock unset — End cannot stamp a real timestamp.
  gFakeNow = 100;
  DeploymentOpResult r = endDeploymentForUnpair("ENV_A1");
  check("failed end: reports an error the caller can act on",
        r.status != DEPLOY_OK && r.status != DEPLOY_REPLAYED);
  check("failed end: deployment is still active (not silently ended)",
        deploymentFindByNodeId("ENV_A1")->endedUnix == 0);
  check("failed end: identity intact for the caller to keep",
        getNodeUserId("ENV_A1") == "001");

  // Outbox full is the other way End can fail.
  gFakeNow = 1753000000UL;
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  for (int i = 0; i < (int)kMaxOutboxEvents; ++i) {
    DeploymentEvent e{};
    snprintf(e.eventId, sizeof(e.eventId), "FILLER-%d", i);
    e.inUse = true;
    deploymentOutboxUpsert(e);
  }
  DeploymentOpResult r2 = endDeploymentForUnpair("ENV_A1");
  check("failed end: outbox-full End also reports failure",
        r2.status == DEPLOY_ERR_OUTBOX_FULL);
  check("failed end: deployment still active after outbox-full",
        deploymentFindByNodeId("ENV_A1")->endedUnix == 0);
}

// Review F5: an abandoned wizard must not move a node's recorded location.
static void testStagedCoordinatesNotAppliedUntilStart() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  registeredNodes[0].latitude  = -10.0f;
  registeredNodes[0].longitude = 20.0f;
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);

  const float newLat = -27.5f, newLon = 153.0f;
  deploymentStageIdentity("ENV_A1", nullptr, nullptr, &newLat, &newLon);
  check("staged coords: archived location unchanged while staged",
        registeredNodes[0].latitude == -10.0f &&
        registeredNodes[0].longitude == 20.0f);
  const DeploymentSlot* s = deploymentFindByNodeId("ENV_A1");
  check("staged coords: held in the slot", s && s->stagedLat == newLat);

  gFakeNow += 60;
  beginNewDeployment("ENV_A1", "002", "South Gate", newLat, newLon, 1);
  check("staged coords: applied only when Start commits",
        registeredNodes[0].latitude == newLat &&
        registeredNodes[0].longitude == newLon);
}

// ensureFirstDeployment() is the OTHER path that commits a deployment, so it
// owns the staged identity exactly as beginNewDeployment() does.
//
// Regression: it used to skip publishing entirely. The number and name the
// operator typed were staged (correct — nothing may reach node_meta before a
// deployment commits), then epoch 1 was committed and fillEvent() read the
// still-empty node_meta, so the deployment was archived with a blank label and
// nothing could ever fix it: the node now has an ACTIVE deployment, which makes
// both "Start new deployment" and a further staged edit unreachable.
//
// Reachable whenever deploymentIdentityIsStaged() is true at action=start —
// i.e. any node with no slot yet, and any DEPLOYED node left at epoch 0 because
// deploymentSeedFromRegistry() could not seed it (no deployedSinceUnix and an
// implausible hub clock).
static void testFirstDeploymentPublishesStagedIdentity() {
  resetAll();
  addNode("ENV_A1", 0x01, PAIRED);

  const String number = "007";
  const String name   = "Ridge Top";
  deploymentStageIdentity("ENV_A1", &number, &name, nullptr, nullptr);
  check("first deploy: identity is staged, not written through",
        getNodeUserId("ENV_A1") == "" && getNodeName("ENV_A1") == "");

  const DeploymentOpResult r = ensureFirstDeployment("ENV_A1");
  check("first deploy: succeeds", r.status == DEPLOY_OK);
  check("first deploy: epoch 1", epochOf("ENV_A1") == 1);
  check("first deploy: staged number published to node_meta",
        getNodeUserId("ENV_A1") == "007");
  check("first deploy: staged name published to node_meta",
        getNodeName("ENV_A1") == "Ridge Top");
  check("first deploy: registry mirror carries the number",
        registeredNodes[0].userId == "007");

  const DeploymentSlot* s = deploymentFindByNodeId("ENV_A1");
  check("first deploy: staging cleared once published",
        s && !s->hasStagedIdentity && s->stagedUserId[0] == '\0');

  // The archived record must carry the label, not a blank.
  bool labelled = false;
  for (uint8_t i = 0; i < deploymentOutboxCount(); ++i) {
    const DeploymentEvent* e = deploymentOutboxAt(i);
    if (e && e->deploymentEpoch == 1 && String(e->userId) == "007" &&
        String(e->name) == "Ridge Top") {
      labelled = true;
    }
  }
  check("first deploy: outbox event carries the operator's identity", labelled);

  // And the number is now held against the guard, as an active deployment must.
  const DeploymentGuardResult g = checkNumberAvailable("007", "OTHER");
  check("first deploy: the published number is held", !g.free);
}

// F1: the case status.nodes[] cannot express — end one deployment, start the
// next, and upload only afterwards.
static void testEndThenStartKeepsBothEvents() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 86400;
  endDeployment("ENV_A1", 1);
  gFakeNow += 3600;
  beginNewDeployment("ENV_A1", "002", "South Gate", NAN, NAN, 1);

  char id1[24], id2[24];
  deploymentMakeEventId("ENV_A1", 1, id1, sizeof(id1));
  deploymentMakeEventId("ENV_A1", 2, id2, sizeof(id2));
  check("outbox: distinct event IDs per epoch", String(id1) != String(id2));

  const String json = deploymentOutboxToJson();
  check("outbox: epoch-1 event survives the new deployment",
        json.indexOf(id1) >= 0);
  check("outbox: epoch-2 event is queued too", json.indexOf(id2) >= 0);
  check("outbox: epoch-1 event keeps its OWN number",
        json.indexOf("\"userId\":\"001\"") >= 0);
  check("outbox: epoch-1 event keeps its OWN name",
        json.indexOf("North Hedge") >= 0);
  check("outbox: epoch-1 event carries its end timestamp",
        json.indexOf("\"deploymentEndedUnix\":0,") < 0);
}

static void testUnpairPreservesEndEvent() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 86400;

  endDeploymentForUnpair("ENV_A1");
  // Unpair then clears node_meta, as config_server does.
  setNodeUserId("ENV_A1", "");
  setNodeName("ENV_A1", "");

  const String json = deploymentOutboxToJson();
  check("unpair: the ended deployment keeps its number in the outbox",
        json.indexOf("\"userId\":\"001\"") >= 0);
  check("unpair: and its name", json.indexOf("North Hedge") >= 0);
}

// Backend contract: a CONFLICT did not commit, so the event stays queued and
// retries. The number-collision case clears itself once the other node's End
// event lands, with no operator action — dropping it would lose the deployment
// record for a condition that resolves on its own.
static void testConflictKeepsEventQueued() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);

  char id1[24];
  deploymentMakeEventId("ENV_A1", 1, id1, sizeof(id1));
  const uint8_t before = deploymentOutboxCount();

  const char* reason = "deployment number \"001\" is already held by another active deployment";
  check("conflict: recorded against a queued event",
        deploymentOutboxNoteConflict(id1, reason));
  check("conflict: the event is STILL queued for retry",
        deploymentOutboxCount() == before);
  check("conflict: it is still serialised for the next upload",
        deploymentOutboxToJson().indexOf(id1) >= 0);
  check("conflict: the operator-facing reason is retrievable",
        deploymentOutboxConflictSummary().indexOf("already held") >= 0);
  check("conflict: an unknown event id is not invented",
        !deploymentOutboxNoteConflict("NOT-REAL", reason));

  // Re-upserting (e.g. the operator edits the name) clears the stale reason so
  // the retry is reported fresh rather than showing a resolved complaint.
  deploymentTouchActiveEvent("ENV_A1");
  check("conflict: a fresh upsert clears the stale reason",
        deploymentOutboxConflictSummary().length() == 0);

  // Only an ACK removes it.
  check("conflict: ack still removes the event", deploymentOutboxRemove(id1));
  check("conflict: outbox shrank on ack", deploymentOutboxCount() == before - 1);
}

// Backend contract §1: deploymentStartedUnix is mandatory and must be > 0 on
// EVERY event, End included. A zero start is conflicted and never acked, so it
// would retry forever and wedge the outbox behind it.
static void testStartedUnixAlwaysPositive() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);

  const String json = deploymentOutboxToJson();
  check("started: no event carries a zero start",
        json.indexOf("\"deploymentStartedUnix\":0") < 0);
  check("started: the End event carries the START of the epoch it closed",
        json.indexOf("\"deploymentStartedUnix\":1753000000") >= 0);

  // The store must refuse a malformed event outright.
  DeploymentEvent bad{};
  strncpy(bad.eventId, "BAD-1", sizeof(bad.eventId) - 1);
  strncpy(bad.nodeId, "ENV_A1", sizeof(bad.nodeId) - 1);
  bad.deploymentEpoch = 1;
  bad.deploymentStartedUnix = 0;
  bad.inUse = true;
  check("started: an event with a zero start is refused",
        !deploymentOutboxUpsert(bad));

  DeploymentEvent noId{};
  noId.deploymentEpoch = 1;
  noId.deploymentStartedUnix = 1753000000UL;
  noId.inUse = true;
  check("started: an event with no eventId is refused",
        !deploymentOutboxUpsert(noId));
}

// A pre-epoch node whose deployedSinceUnix is 0 (RTC was unset when it was
// registered) must not be seeded with a zero start.
static void testSeedNeverProducesZeroStart() {
  registeredNodes.clear();
  gMeta.clear();
  LittleFS.remove("/deploy.bin");
  LittleFS.remove("/deploy.bak");
  deploymentStoreBegin();

  addNode("ENV_Z", 0x60, DEPLOYED);
  registeredNodes.back().deployedSinceUnix = 0;   // RTC was dead at registration

  gFakeNow = 1753000000UL;
  deploymentSeedFromRegistry();
  const DeploymentSlot* s = deploymentFindByNodeId("ENV_Z");
  check("seed: a node with no deploy time still gets a positive start",
        s && s->epoch == 1 && s->startedUnix >= kPlausibleSampleFloor);

  // With no deploy time AND no hub clock, refuse to seed at all: epoch 0 lets
  // the backend fall back, which is recoverable; a zero start is not.
  registeredNodes.clear();
  LittleFS.remove("/deploy.bin");
  LittleFS.remove("/deploy.bak");
  deploymentStoreBegin();
  addNode("ENV_Y", 0x61, DEPLOYED);
  registeredNodes.back().deployedSinceUnix = 0;
  gFakeNow = 100;
  deploymentSeedFromRegistry();
  const DeploymentSlot* s2 = deploymentFindByNodeId("ENV_Y");
  check("seed: refuses to seed when neither a deploy time nor a clock exists",
        !s2 || s2->epoch == 0);
  gFakeNow = 1753000000UL;
}

static void testOutboxAckClearsOnlyMatching() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  addNode("ENV_A2", 0x02, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  beginNewDeployment("ENV_A2", "002", "B", NAN, NAN, 0);
  const uint8_t before = deploymentOutboxCount();

  char id1[24];
  deploymentMakeEventId("ENV_A1", 1, id1, sizeof(id1));
  check("ack: an unknown event ID is a no-op",
        !deploymentOutboxRemove("NOT-A-REAL-ID"));
  check("ack: count unchanged after an unknown ID",
        deploymentOutboxCount() == before);
  check("ack: a known ID is removed", deploymentOutboxRemove(id1));
  check("ack: exactly one entry went", deploymentOutboxCount() == before - 1);
  check("ack: the other node's event is untouched",
        deploymentOutboxToJson().indexOf("\"userId\":\"002\"") >= 0);
}

// The regression the wakeIntervalMin bug is a warning about.
static void testSurvivesReboot() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 86400;
  endDeployment("ENV_A1", 1);
  gFakeNow += 3600;
  beginNewDeployment("ENV_A1", "002", "South Gate", NAN, NAN, 1);
  const uint32_t started = deploymentFindByNodeId("ENV_A1")->startedUnix;

  // Simulate the cold boot the FieldHub does between every wake: drop all RAM
  // state and reload from flash.
  deploymentStoreResetForTest();
  check("reboot: RAM state really was cleared", epochOf("ENV_A1") == 0);
  deploymentStoreBegin();

  const DeploymentSlot* s = deploymentFindByNodeId("ENV_A1");
  check("reboot: slot survives", s != nullptr);
  check("reboot: epoch survives", s && s->epoch == 2);
  check("reboot: startedUnix survives", s && s->startedUnix == started);
  check("reboot: endedUnix survives as active", s && s->endedUnix == 0);
  check("reboot: boundary history survives", s && s->historyCount == 2);
  check("reboot: epoch-1 backlog still resolves correctly after reload",
        resolveEpochForSample("ENV_A1", 1753000060UL) == 1);
  check("reboot: outbox survives", deploymentOutboxCount() == 2);
}

// F3: an interrupted commit must resolve to the complete old or complete new
// record, never a mixture.
static void testTornWriteRecovery() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);

  // A crash between the two renames leaves .bak holding the previous complete
  // record and no .bin.
  LittleFS.rename("/deploy.bin", "/deploy.bak");
  deploymentStoreResetForTest();
  deploymentStoreBegin();
  check("torn write: recovered from the backup record",
        epochOf("ENV_A1") == 1);

  // A truncated temp file is never authoritative.
  {
    File f = LittleFS.open("/deploy.tmp", "w", true);
    if (f) { f.write((const uint8_t*)"garbage", 7); f.close(); }
  }
  deploymentStoreResetForTest();
  deploymentStoreBegin();
  check("torn write: a stray temp file is ignored", epochOf("ENV_A1") == 1);
}

static void testLegacyBacklogBlocksStart() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);

  gLegacyBacklog = true;
  gLegacyRows = 142;
  DeploymentOpResult r = beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 1);
  check("legacy: Start is blocked while unstamped rows are queued",
        r.status == DEPLOY_ERR_LEGACY_BACKLOG);
  check("legacy: the message names the pending row count",
        r.message.indexOf("142") >= 0);
  check("legacy: epoch unchanged", epochOf("ENV_A1") == 1);

  gLegacyBacklog = false;
  DeploymentOpResult r2 = beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 1);
  check("legacy: Start succeeds once the backlog has drained",
        r2.status == DEPLOY_OK && epochOf("ENV_A1") == 2);
}

static void testEpochOverflowRejected() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  DeploymentSlot* s = deploymentSlotFor(registeredNodes[0].mac, "ENV_A1");
  s->epoch = 65535;
  s->startedUnix = gFakeNow;
  s->endedUnix = gFakeNow + 10;
  deploymentStoreCommit();

  gFakeNow += 3600;
  DeploymentOpResult r = beginNewDeployment("ENV_A1", "002", "B", NAN, NAN, 65535);
  check("overflow: Start rejected at the counter limit",
        r.status == DEPLOY_ERR_EPOCH_OVERFLOW);
  check("overflow: epoch did NOT wrap to 0", epochOf("ENV_A1") == 65535);
}

static void testStartRejectedOnEndedViaPlainStart() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "A", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);

  DeploymentOpResult r = ensureFirstDeployment("ENV_A1");
  check("plain start: refused on an ended deployment (would re-stitch sites)",
        r.status == DEPLOY_ERR_STATE);
  check("plain start: epoch unchanged", epochOf("ENV_A1") == 1);
}

static void testStagedIdentity() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "North Hedge", NAN, NAN, 0);
  gFakeNow += 3600;
  endDeployment("ENV_A1", 1);

  check("staging: an ended node stages identity rather than writing it",
        deploymentIdentityIsStaged("ENV_A1"));
  const String proposedNum = "002", proposedName = "South Gate";
  deploymentStageIdentity("ENV_A1", &proposedNum, &proposedName, nullptr, nullptr);
  check("staging: the ended deployment's number is untouched",
        getNodeUserId("ENV_A1") == "001");
  check("staging: the ended deployment's name is untouched",
        getNodeName("ENV_A1") == "North Hedge");

  gFakeNow += 60;
  beginNewDeployment("ENV_A1", "002", "South Gate", NAN, NAN, 1);
  check("staging: Start commits the new identity",
        getNodeUserId("ENV_A1") == "002" && getNodeName("ENV_A1") == "South Gate");
  check("staging: an active deployment writes through instead",
        !deploymentIdentityIsStaged("ENV_A1"));
}

static void testSeedFromRegistry() {
  registeredNodes.clear();
  gMeta.clear();
  LittleFS.remove("/deploy.bin");
  LittleFS.remove("/deploy.bak");
  deploymentStoreBegin();

  // A node deployed under pre-epoch firmware: DEPLOYED with deployedSinceUnix.
  addNode("ENV_OLD", 0x50, DEPLOYED);
  registeredNodes.back().deployedSinceUnix = 1750000000UL;
  addNode("ENV_NEW", 0x51, PAIRED);

  deploymentSeedFromRegistry();
  check("migration: an existing deployment becomes epoch 1",
        epochOf("ENV_OLD") == 1);
  const DeploymentSlot* s = deploymentFindByNodeId("ENV_OLD");
  check("migration: start seeded from deployedSinceUnix",
        s && s->startedUnix == 1750000000UL);
  check("migration: a never-deployed node stays at epoch 0",
        epochOf("ENV_NEW") == 0);
  check("migration: a pre-epoch reading resolves to epoch 1",
        resolveEpochForSample("ENV_OLD", 1750000600UL) == 1);
}

// Review F6: node names are operator-supplied and are concatenated straight
// into JSON responses and into the deployment-event outbox. A name containing a
// quote or backslash must not produce malformed JSON.
static bool looksLikeBalancedJson(const String& s) {
  int depth = 0;
  bool inStr = false, esc = false;
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (inStr) {
      if (esc)            esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"')  inStr = false;
      else if ((uint8_t)c < 0x20) return false;   // bare control char
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '[' || c == '{') depth++;
    else if (c == ']' || c == '}') { depth--; if (depth < 0) return false; }
  }
  return depth == 0 && !inStr;
}

static void testAwkwardNamesProduceValidJson() {
  resetAll();
  addNode("ENV_A1", 0x01, DEPLOYED);
  beginNewDeployment("ENV_A1", "001", "He said \"north\"\\hedge", NAN, NAN, 0);

  const String json = deploymentOutboxToJson();
  check("json: a quoted/backslashed name still yields balanced JSON",
        looksLikeBalancedJson(json));
  check("json: the quote is escaped", json.indexOf("\\\"north\\\"") >= 0);
  check("json: the backslash is escaped", json.indexOf("\\\\hedge") >= 0);

  // Newlines and tabs must be escaped rather than emitted bare.
  resetAll();
  addNode("ENV_A2", 0x02, DEPLOYED);
  beginNewDeployment("ENV_A2", "002", "line1\nline2\ttabbed", NAN, NAN, 0);
  const String json2 = deploymentOutboxToJson();
  check("json: control characters escaped, JSON still balanced",
        looksLikeBalancedJson(json2));
  check("json: newline escaped", json2.indexOf("\\n") >= 0);
  check("json: no bare newline in the body", json2.indexOf('\n') < 0);

  // The number guard reports the holder label, which flows into the 409 body.
  const DeploymentGuardResult g = checkNumberAvailable("002", "ENV_A1");
  check("json: holder label carries the raw name for the caller to escape",
        !g.free && g.holderLabel.indexOf("line1") >= 0);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== test_deployment_epoch ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[FAIL] LittleFS mount failed — cannot run");
    Serial.println("RESULT|SUMMARY|0/0|OVERALL:FAIL");
    return;
  }

  deploymentSetConfigApplyFn(fakeConfigApply);
  deploymentSetLegacyBacklogFn(fakeLegacyBacklog);
  deploymentSetNowFn(fakeNow);

  testStartIncrementsAndStamps();
  testBufferedReadingKeepsOldEpoch();
  testEpoch1SampleDuringEpoch3();
  testImplausibleClockDoesNotDecrement();
  testInTransitKeepsOldEpoch();
  testStopStartDoesNotChangeEpoch();
  testEditingIdentityDoesNotChangeEpoch();
  testNumberGuard();
  testFourNodePermute();
  testRtcUnsetRejects();
  testRetryIdempotency();
  testStartRollsBackWhenConfigFails();
  testStartRollbackRestoresWholeSlot();
  testInterruptedStartCompletesForward();
  testInterruptedStartWaitsForActiveQueue();
  testOutboxFullRefusedBeforeIntent();
  testFailedEndBlocksUnpair();
  testStagedCoordinatesNotAppliedUntilStart();
  testFirstDeploymentPublishesStagedIdentity();
  testEndThenStartKeepsBothEvents();
  testUnpairPreservesEndEvent();
  testConflictKeepsEventQueued();
  testStartedUnixAlwaysPositive();
  testSeedNeverProducesZeroStart();
  testOutboxAckClearsOnlyMatching();
  testSurvivesReboot();
  testTornWriteRecovery();
  testLegacyBacklogBlocksStart();
  testEpochOverflowRejected();
  testStartRejectedOnEndedViaPlainStart();
  testStagedIdentity();
  testSeedFromRegistry();
  testAwkwardNamesProduceValidJson();

  const int total = gPass + gFail;
  Serial.printf("\nRESULT|SUMMARY|%d/%d|OVERALL:%s\n",
                gPass, total, gFail == 0 ? "PASS" : "FAIL");
}

void loop() {}
