#include "config/deployment_store.h"

#include <LittleFS.h>
#include <string.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// On-flash record
// ---------------------------------------------------------------------------

static const char* kDeployFile   = "/deploy.bin";
static const char* kDeployTmp    = "/deploy.tmp";
static const char* kDeployBak    = "/deploy.bak";

static constexpr uint32_t kDeployMagic   = 0x4650444DUL;  // "FPDM"
static constexpr uint16_t kDeployVersion = 1;

struct DeploymentRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;              // sizeof(DeploymentRecord) — guards struct drift
  uint32_t generation;        // higher wins when .bin and .bak both validate
  DeploymentSlot  nodes[kMaxDeployNodes];
  DeploymentEvent outbox[kMaxOutboxEvents];
  uint8_t  outboxCount;
  uint8_t  reserved[3];
  uint32_t epochClampCount;
  uint32_t checksum;          // FNV-1a over the record with checksum zeroed
};

// ~10 KB — deliberately a file static, never a stack local.
static DeploymentRecord gRecord;
static bool gReady        = false;
static bool gCreatedFresh = false;

// Pin the size so the outbox/table cannot be grown carelessly into something
// that strains a 768 KB LittleFS shared with the reading buffer.
static_assert(sizeof(DeploymentRecord) < 16384,
              "DeploymentRecord too large for the shared LittleFS partition");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

// By CONST REFERENCE, and hashing the prefix rather than a zeroed copy.
//
// This used to take the record by value to zero the checksum field before
// hashing. DeploymentRecord is ~9.3 KB, so every call pushed a 9.3 KB copy onto
// a stack the Arduino loop task sizes at 8 KB — deploymentStoreCommit() alone
// reserved 9,344 bytes. Hashing [0, offsetof(checksum)) is equivalent (checksum
// is the last member, so there is no trailing padding to skip) and costs no
// stack at all.
static uint32_t checksumFor(const DeploymentRecord& record) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&record),
                 offsetof(DeploymentRecord, checksum));
}

static bool recordValid(const DeploymentRecord& record) {
  if (record.magic != kDeployMagic) return false;
  if (record.version != kDeployVersion) return false;
  if (record.size != sizeof(DeploymentRecord)) return false;
  if (record.outboxCount > kMaxOutboxEvents) return false;
  return checksumFor(record) == record.checksum;
}

// Reads straight into the caller's record — never through a temporary. The
// caller passes gRecord, so no copy of the ~9.3 KB record exists anywhere.
static bool readRecordFile(const char* path, DeploymentRecord& out) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  if (f.size() != sizeof(DeploymentRecord)) {
    f.close();
    return false;
  }
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&out), sizeof(out));
  f.close();
  if (got != sizeof(out)) return false;
  return recordValid(out);
}

// Just enough of the header to compare generations without loading a whole
// record. Lets deploymentStoreBegin() decide which file to load without ever
// holding two records at once.
struct DeploymentRecordHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
};

static bool peekRecordGeneration(const char* path, uint32_t& generationOut) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeploymentRecordHeader hdr{};
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr));
  const size_t fileSize = f.size();
  f.close();
  if (got != sizeof(hdr) || fileSize != sizeof(DeploymentRecord)) return false;
  if (hdr.magic != kDeployMagic || hdr.version != kDeployVersion) return false;
  if (hdr.size != sizeof(DeploymentRecord)) return false;
  generationOut = hdr.generation;
  return true;
}

static void initFreshRecord() {
  memset(&gRecord, 0, sizeof(gRecord));
  gRecord.magic   = kDeployMagic;
  gRecord.version = kDeployVersion;
  gRecord.size    = sizeof(DeploymentRecord);
  gRecord.generation = 0;
}

static void copyStr(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static String jsonEscape(const char* v) {
  String out;
  if (!v) return out;
  for (const char* p = v; *p; ++p) {
    const char c = *p;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool deploymentStoreBegin() {
  gReady = false;
  gCreatedFresh = false;

  // A commit that was interrupted mid-rename can leave the temp file behind.
  // It is never authoritative — the record is only live once it is at
  // kDeployFile — so drop it rather than trying to interpret it.
  if (LittleFS.exists(kDeployTmp)) {
    LittleFS.remove(kDeployTmp);
  }

  // Load candidates directly into gRecord, one at a time. Holding two ~9.3 KB
  // records as locals here reserved 18,656 bytes of stack against an 8 KB loop
  // task — a guaranteed overflow on the first boot that found a record.
  const bool havePrimary = readRecordFile(kDeployFile, gRecord);
  const uint32_t primaryGen = havePrimary ? gRecord.generation : 0;

  uint32_t backupGen = 0;
  const bool backupPresent = peekRecordGeneration(kDeployBak, backupGen);

  bool loaded = havePrimary;
  if (backupPresent && (!havePrimary || backupGen > primaryGen)) {
    // The backup either is all we have, or it advanced further than the primary
    // (an interrupted commit). Either way it is a COMPLETE record — never a mix.
    if (readRecordFile(kDeployBak, gRecord)) {
      if (!havePrimary) {
        Serial.println("[DEPLOY] Primary record missing/corrupt — recovering from backup");
      }
      loaded = true;
    } else if (havePrimary) {
      // Backup failed its checksum after all; reload the primary we displaced.
      loaded = readRecordFile(kDeployFile, gRecord);
    }
  }

  if (!loaded) {
    initFreshRecord();
    gCreatedFresh = true;
    Serial.println("[DEPLOY] No deployment record — creating");
  }

  gReady = true;

  if (gCreatedFresh && !deploymentStoreCommit()) {
    Serial.println("[DEPLOY] Failed to create deployment record");
    gReady = false;
    return false;
  }

  Serial.printf("[DEPLOY] Store ready (gen=%lu, outbox=%u, fresh=%d)\n",
                (unsigned long)gRecord.generation,
                (unsigned)gRecord.outboxCount,
                gCreatedFresh ? 1 : 0);
  return true;
}

bool deploymentStoreReady() { return gReady; }
bool deploymentStoreWasCreatedFresh() { return gCreatedFresh; }

// ---------------------------------------------------------------------------
// Atomic commit
// ---------------------------------------------------------------------------

bool deploymentStoreCommit() {
  if (!gReady) return false;

  gRecord.magic   = kDeployMagic;
  gRecord.version = kDeployVersion;
  gRecord.size    = sizeof(DeploymentRecord);
  gRecord.generation++;
  gRecord.checksum = checksumFor(gRecord);

  {
    File f = LittleFS.open(kDeployTmp, "w", true);
    if (!f) {
      Serial.println("[DEPLOY] commit: cannot open temp file");
      gRecord.generation--;
      return false;
    }
    const size_t written = f.write(reinterpret_cast<const uint8_t*>(&gRecord),
                                   sizeof(gRecord));
    const bool writeError = f.getWriteError();
    f.close();
    if (writeError || written != sizeof(gRecord)) {
      Serial.printf("[DEPLOY] commit: short write (%u/%u)\n",
                    (unsigned)written, (unsigned)sizeof(gRecord));
      LittleFS.remove(kDeployTmp);
      gRecord.generation--;
      return false;
    }
  }

  // Backup-then-swap, same shape as upload_queue's commitTempDataFile(): at
  // every instant at least one complete record exists on flash.
  LittleFS.remove(kDeployBak);
  const bool hadPrimary = LittleFS.exists(kDeployFile);
  if (hadPrimary && !LittleFS.rename(kDeployFile, kDeployBak)) {
    Serial.println("[DEPLOY] commit: cannot move primary to backup");
    LittleFS.remove(kDeployTmp);
    gRecord.generation--;
    return false;
  }
  if (!LittleFS.rename(kDeployTmp, kDeployFile)) {
    Serial.println("[DEPLOY] commit: temp->primary rename failed; restoring backup");
    if (hadPrimary) LittleFS.rename(kDeployBak, kDeployFile);
    LittleFS.remove(kDeployTmp);
    gRecord.generation--;
    return false;
  }
  if (hadPrimary) LittleFS.remove(kDeployBak);
  return true;
}

// ---------------------------------------------------------------------------
// Slot access
// ---------------------------------------------------------------------------

const DeploymentSlot* deploymentFindByNodeId(const char* nodeId) {
  if (!gReady || !nodeId || !nodeId[0]) return nullptr;
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    const DeploymentSlot& s = gRecord.nodes[i];
    if (s.inUse && strncmp(s.nodeId, nodeId, kDeployNodeIdLen) == 0) return &s;
  }
  return nullptr;
}

const DeploymentSlot* deploymentFindByMac(const uint8_t* mac) {
  if (!gReady || !mac) return nullptr;
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    const DeploymentSlot& s = gRecord.nodes[i];
    if (s.inUse && memcmp(s.mac, mac, 6) == 0) return &s;
  }
  return nullptr;
}

DeploymentSlot* deploymentSlotFor(const uint8_t* mac, const char* nodeId) {
  if (!gReady) return nullptr;

  // MAC first — it is the registry's real identity and survives a nodeId change.
  if (mac) {
    for (size_t i = 0; i < kMaxDeployNodes; ++i) {
      DeploymentSlot& s = gRecord.nodes[i];
      if (s.inUse && memcmp(s.mac, mac, 6) == 0) {
        if (nodeId && nodeId[0]) copyStr(s.nodeId, kDeployNodeIdLen, nodeId);
        return &s;
      }
    }
  }
  if (nodeId && nodeId[0]) {
    for (size_t i = 0; i < kMaxDeployNodes; ++i) {
      DeploymentSlot& s = gRecord.nodes[i];
      if (s.inUse && strncmp(s.nodeId, nodeId, kDeployNodeIdLen) == 0) {
        if (mac) memcpy(s.mac, mac, 6);
        return &s;
      }
    }
  }
  for (size_t i = 0; i < kMaxDeployNodes; ++i) {
    DeploymentSlot& s = gRecord.nodes[i];
    if (!s.inUse) {
      memset(&s, 0, sizeof(s));
      if (mac) memcpy(s.mac, mac, 6);
      copyStr(s.nodeId, kDeployNodeIdLen, nodeId);
      s.inUse = true;
      return &s;
    }
  }
  Serial.println("[DEPLOY] slot table full");
  return nullptr;
}

void deploymentPushBoundary(DeploymentSlot& slot, uint16_t epoch, uint32_t startedUnix) {
  // Replace in place if this epoch is already the newest entry (a repeated
  // commit of the same transition must not grow the ring).
  if (slot.historyCount > 0 &&
      slot.history[slot.historyCount - 1].epoch == epoch) {
    slot.history[slot.historyCount - 1].startedUnix = startedUnix;
    return;
  }
  if (slot.historyCount < kBoundaryHistory) {
    slot.history[slot.historyCount].epoch = epoch;
    slot.history[slot.historyCount].startedUnix = startedUnix;
    slot.historyCount++;
    return;
  }
  for (size_t i = 1; i < kBoundaryHistory; ++i) {
    slot.history[i - 1] = slot.history[i];
  }
  slot.history[kBoundaryHistory - 1].epoch = epoch;
  slot.history[kBoundaryHistory - 1].startedUnix = startedUnix;
}

// ---------------------------------------------------------------------------
// Outbox
// ---------------------------------------------------------------------------

void deploymentMakeEventId(const char* nodeId, uint16_t epoch, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  const char* id = (nodeId && nodeId[0]) ? nodeId : "UNKNOWN";
  const uint32_t h = fnv1a32(reinterpret_cast<const uint8_t*>(id), strlen(id));
  snprintf(out, outLen, "%08lX-%u", (unsigned long)h, (unsigned)epoch);
}

bool deploymentOutboxUpsert(const DeploymentEvent& event) {
  if (!gReady) return false;

  // deploymentStartedUnix is mandatory and must be positive on EVERY event,
  // including End events where it is the start of the epoch being closed. The
  // backend records a zero start as a conflict and never acknowledges it, so
  // queueing one would retry forever and wedge the outbox behind it. Refuse
  // here rather than let a bad record escape.
  if (event.deploymentStartedUnix == 0) {
    Serial.printf("[DEPLOY] Refusing event %s: deploymentStartedUnix is 0\n",
                  event.eventId);
    return false;
  }
  if (event.eventId[0] == '\0') {
    Serial.println("[DEPLOY] Refusing event with an empty eventId");
    return false;
  }

  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, event.eventId, kDeployEventIdLen) == 0) {
      const bool wasInUse = e.inUse;
      e = event;
      e.inUse = wasInUse;
      return true;
    }
  }
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (!e.inUse) {
      e = event;
      e.inUse = true;
      if (gRecord.outboxCount < kMaxOutboxEvents) gRecord.outboxCount++;
      return true;
    }
  }
  // Full. The caller must refuse the operation: dropping an event here would
  // lose exactly the deployment history this outbox exists to preserve.
  Serial.println("[DEPLOY] outbox full — refusing to drop a deployment event");
  return false;
}

bool deploymentOutboxHasRoomFor(const char* eventId) {
  if (!gReady) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& e = gRecord.outbox[i];
    if (!e.inUse) return true;                            // free slot
    if (eventId && eventId[0] &&
        strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      return true;                                        // upsert in place
    }
  }
  return false;
}

uint8_t deploymentOutboxCount() {
  if (!gReady) return 0;
  uint8_t n = 0;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    if (gRecord.outbox[i].inUse) n++;
  }
  return n;
}

const DeploymentEvent* deploymentOutboxAt(uint8_t index) {
  if (!gReady) return nullptr;
  uint8_t n = 0;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    if (!gRecord.outbox[i].inUse) continue;
    if (n == index) return &gRecord.outbox[i];
    n++;
  }
  return nullptr;
}

bool deploymentOutboxNoteConflict(const char* eventId, const char* reason) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      copyStr(e.conflictReason, sizeof(e.conflictReason),
              (reason && reason[0]) ? reason : "Rejected by the dashboard");
      return true;   // deliberately still queued — it did not commit
    }
  }
  return false;
}

String deploymentOutboxConflictSummary() {
  if (!gReady) return String();
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    const DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && e.conflictReason[0]) return String(e.conflictReason);
  }
  return String();
}

bool deploymentOutboxRemove(const char* eventId) {
  if (!gReady || !eventId || !eventId[0]) return false;
  for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
    DeploymentEvent& e = gRecord.outbox[i];
    if (e.inUse && strncmp(e.eventId, eventId, kDeployEventIdLen) == 0) {
      memset(&e, 0, sizeof(e));
      if (gRecord.outboxCount > 0) gRecord.outboxCount--;
      return true;
    }
  }
  return false;
}

String deploymentOutboxToJson() {
  String out = "[";
  if (gReady) {
    bool first = true;
    for (size_t i = 0; i < kMaxOutboxEvents; ++i) {
      const DeploymentEvent& e = gRecord.outbox[i];
      if (!e.inUse) continue;
      if (!first) out += ",";
      first = false;
      out += "{\"eventId\":\"";              out += jsonEscape(e.eventId);
      out += "\",\"nodeId\":\"";             out += jsonEscape(e.nodeId);
      out += "\",\"deploymentEpoch\":";      out += String((unsigned)e.deploymentEpoch);
      out += ",\"deploymentStartedUnix\":";  out += String(e.deploymentStartedUnix);
      out += ",\"deploymentEndedUnix\":";    out += String(e.deploymentEndedUnix);
      out += ",\"userId\":\"";               out += jsonEscape(e.userId);
      out += "\",\"name\":\"";               out += jsonEscape(e.name);
      out += "\"";
      char nb[16];
      out += ",\"latitude\":";
      if (isnan(e.latitude)) { out += "null"; }
      else { dtostrf(e.latitude, 1, 5, nb); out += nb; }
      out += ",\"longitude\":";
      if (isnan(e.longitude)) { out += "null"; }
      else { dtostrf(e.longitude, 1, 5, nb); out += nb; }
      out += "}";
    }
  }
  out += "]";
  return out;
}

// ---------------------------------------------------------------------------
// Diagnostics / test seam
// ---------------------------------------------------------------------------

uint32_t deploymentEpochClampCount() { return gReady ? gRecord.epochClampCount : 0; }

void deploymentNoteEpochClamp() {
  if (gReady) gRecord.epochClampCount++;
}

void deploymentStoreResetForTest() {
  initFreshRecord();
  gReady = true;
  gCreatedFresh = true;
}
