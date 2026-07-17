#include "command_dispatcher.h"
#include <Preferences.h>
#include <string.h>

// ---------------------------------------------------------------------------
// In-RAM authoritative state (mirrored to NVS on every accepted change).
// ---------------------------------------------------------------------------
struct NodeSlot {
  DispatchNodeConfig cfg;
  char     pendingCmdId[CMD_ID_LEN];  // last accepted, not-yet-converged command
  uint32_t pendingRevision;
  bool     inUse;
  bool     converged;
};

static uint32_t      gRevision = 0;
static NodeSlot      gNodes[CMD_MAX_NODES];
static CommandResult gResults[CMD_MAX_RESULTS];
static uint8_t       gResultHead = 0;   // next write index (ring)
static uint8_t       gResultCount = 0;

static Preferences   gNvs;
static const char*   kNs = "dispatch";

const char* cmdOutcomeStr(CmdOutcome o) {
  switch (o) {
    case OUT_ACCEPTED:          return "ACCEPTED";
    case OUT_REVISION_CONFLICT: return "REVISION_CONFLICT";
    case OUT_INVALID:           return "INVALID";
    case OUT_SUPERSEDED:        return "SUPERSEDED";
    case OUT_REPLAY:            return "REPLAY";
    default:                    return "??";
  }
}

// ---- NVS persistence (compact blobs; §13.1 "persist coarse durable state") --
static void persist() {
  gNvs.begin(kNs, false);
  gNvs.putUInt("rev", gRevision);
  gNvs.putBytes("nodes", gNodes, sizeof gNodes);
  gNvs.putBytes("results", gResults, sizeof gResults);
  gNvs.putUChar("rhead", gResultHead);
  gNvs.putUChar("rcount", gResultCount);
  gNvs.end();
}

void dispatcherInit() {
  memset(gNodes, 0, sizeof gNodes);
  memset(gResults, 0, sizeof gResults);
  gRevision = 0; gResultHead = 0; gResultCount = 0;

  gNvs.begin(kNs, true);
  gRevision = gNvs.getUInt("rev", 0);
  if (gNvs.isKey("nodes"))   gNvs.getBytes("nodes", gNodes, sizeof gNodes);
  if (gNvs.isKey("results")) gNvs.getBytes("results", gResults, sizeof gResults);
  gResultHead  = gNvs.getUChar("rhead", 0);
  gResultCount = gNvs.getUChar("rcount", 0);
  gNvs.end();
}

void dispatcherResetForTest() {
  gNvs.begin(kNs, false);
  gNvs.clear();
  gNvs.end();
  memset(gNodes, 0, sizeof gNodes);
  memset(gResults, 0, sizeof gResults);
  gRevision = 0; gResultHead = 0; gResultCount = 0;
}

uint32_t dispatcherRevision() { return gRevision; }

// ---- result ring helpers ----
static CommandResult* findResult(const char* cmdId) {
  for (uint8_t i = 0; i < gResultCount; i++)
    if (strncmp(gResults[i].cmdId, cmdId, CMD_ID_LEN) == 0) return &gResults[i];
  return nullptr;
}

static CommandResult* recordResult(const CommandResult& r) {
  CommandResult* slot = &gResults[gResultHead];
  *slot = r;
  gResultHead = (gResultHead + 1) % CMD_MAX_RESULTS;
  if (gResultCount < CMD_MAX_RESULTS) gResultCount++;
  return slot;
}

bool dispatcherKnownCmd(const char* cmdId) { return findResult(cmdId) != nullptr; }

bool dispatcherResultFor(const char* cmdId, CommandResult* out) {
  CommandResult* r = findResult(cmdId);
  if (!r) return false;
  if (out) *out = *r;
  return true;
}

uint8_t dispatcherRecentResults(CommandResult* out, uint8_t max) {
  uint8_t n = (gResultCount < max) ? gResultCount : max;
  for (uint8_t i = 0; i < n; i++) out[i] = gResults[i];
  return n;
}

// ---- node table helpers ----
static NodeSlot* findNode(const char* nodeId) {
  for (uint8_t i = 0; i < CMD_MAX_NODES; i++)
    if (gNodes[i].inUse && strncmp(gNodes[i].cfg.nodeId, nodeId, CMD_NODEID_LEN) == 0)
      return &gNodes[i];
  return nullptr;
}

static NodeSlot* findOrAddNode(const char* nodeId) {
  NodeSlot* n = findNode(nodeId);
  if (n) return n;
  for (uint8_t i = 0; i < CMD_MAX_NODES; i++)
    if (!gNodes[i].inUse) {
      gNodes[i].inUse = true;
      strlcpy(gNodes[i].cfg.nodeId, nodeId, CMD_NODEID_LEN);
      return &gNodes[i];
    }
  return nullptr;  // table full
}

const DispatchNodeConfig* dispatcherNodeConfig(const char* nodeId) {
  NodeSlot* n = findNode(nodeId);
  return n ? &n->cfg : nullptr;
}

void dispatcherMarkConverged(const char* nodeId, uint32_t revision) {
  NodeSlot* n = findNode(nodeId);
  if (n && n->pendingRevision == revision) { n->converged = true; persist(); }
}

static bool validPayload(const Command& c) {
  if (c.type != CMD_SET_NODE_CONFIG) return true;   // REQUEST_STATUS: no payload
  if (c.payload.nodeId[0] == '\0') return false;
  if (c.payload.wakeIntervalMin < 1 || c.payload.wakeIntervalMin > 240) return false;
  // targetState semantics are owned by the node registry (e.g. 0=UNPAIRED,
  // 2=DEPLOYED); the dispatcher stores whatever value it's given.
  return true;
}

CommandResult dispatcherSubmit(const Command& c) {
  CommandResult res{};
  strlcpy(res.cmdId, c.cmdId, CMD_ID_LEN);
  res.currentRevision = gRevision;

  // 1) Idempotency — a repeated commandId returns its stored result, never re-runs.
  CommandResult* prior = findResult(c.cmdId);
  if (prior) return *prior;

  // 2) REQUEST_STATUS is an accepted no-op (no state change, no revision bump).
  if (c.type == CMD_REQUEST_STATUS) {
    res.outcome = OUT_ACCEPTED;
    res.assignedRevision = gRevision;
    recordResult(res);
    persist();
    return res;
  }

  // 3) Validation.
  if (!validPayload(c)) {
    res.outcome = OUT_INVALID;
    recordResult(res);
    return res;   // no revision change; do not persist a state mutation
  }

  // 4) Compare-and-set against the authoritative revision.
  if (c.expectedRevision != gRevision) {
    res.outcome = OUT_REVISION_CONFLICT;
    recordResult(res);
    return res;
  }

  // 5) Accept: apply desired config, handle supersession, allocate new revision.
  NodeSlot* n = findOrAddNode(c.payload.nodeId);
  if (!n) { res.outcome = OUT_INVALID; recordResult(res); return res; }

  // Supersede a prior accepted-but-not-converged command for this node.
  if (n->pendingCmdId[0] != '\0' && !n->converged &&
      strncmp(n->pendingCmdId, c.cmdId, CMD_ID_LEN) != 0) {
    CommandResult* p = findResult(n->pendingCmdId);
    if (p && p->outcome == OUT_ACCEPTED) p->outcome = OUT_SUPERSEDED;
  }

  n->cfg = c.payload;
  gRevision++;
  n->pendingRevision = gRevision;
  n->converged = false;
  strlcpy(n->pendingCmdId, c.cmdId, CMD_ID_LEN);

  res.outcome = OUT_ACCEPTED;
  res.assignedRevision = gRevision;
  res.currentRevision = gRevision;
  recordResult(res);
  persist();
  return res;
}
