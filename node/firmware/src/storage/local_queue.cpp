#include "local_queue.h"

#include <Preferences.h>
#include <string.h>

namespace {

static constexpr uint32_t kMagic    = 0x4E514D33;  // "NQM3"
static constexpr uint16_t kVersion  = 4;
static constexpr uint16_t kCapacity = 24;          // 24 * 124B + header < 4KB NVS value limit
static constexpr uint32_t kLayoutId = sizeof(node_snapshot_t) ^ (kCapacity << 16);

static constexpr const char* kNs = "node_q";
static constexpr const char* kSlotA = "q_a";
static constexpr const char* kSlotB = "q_b";
static constexpr const char* kLegacyBlob = "blob";

struct QueueBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t capacity;
  uint32_t layoutId;
  uint32_t generation;
  uint32_t nextSeq;
  uint16_t head;
  uint16_t tail;
  uint16_t used;
  uint16_t reserved;
  node_snapshot_t records[kCapacity];
  uint32_t checksum;
};

static_assert(sizeof(QueueBlob) < 4000, "Queue blob too large for NVS key");

QueueBlob g_blob{};
QueueBlob g_candidate{};
bool g_ready = false;
char g_activeSlot = 'a';
local_queue::QueueStats g_stats{};

uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t computeChecksum(const QueueBlob& b) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&b),
                 sizeof(QueueBlob) - sizeof(uint32_t));
}

bool generationNewer(uint32_t a, uint32_t b) {
  return a != b && static_cast<int32_t>(a - b) > 0;
}

void initDefault(QueueBlob& b) {
  memset(&b, 0, sizeof(b));
  b.magic = kMagic;
  b.version = kVersion;
  b.capacity = kCapacity;
  b.layoutId = kLayoutId;
  b.generation = 1;
  b.nextSeq = 1;
  b.checksum = computeChecksum(b);
}

bool validateBlob(const QueueBlob& b) {
  if (b.magic != kMagic || b.version != kVersion ||
      b.capacity != kCapacity || b.layoutId != kLayoutId) {
    return false;
  }
  if (b.head >= kCapacity || b.tail >= kCapacity || b.used > kCapacity) {
    return false;
  }
  if (b.nextSeq == 0) return false;
  return b.checksum == computeChecksum(b);
}

bool readSlot(Preferences& p, const char* key, QueueBlob& out) {
  if (p.getBytesLength(key) != sizeof(out)) return false;
  const size_t n = p.getBytes(key, &out, sizeof(out));
  return n == sizeof(out) && validateBlob(out);
}

bool readRawSlot(Preferences& p, const char* key, QueueBlob& out) {
  if (p.getBytesLength(key) != sizeof(out)) return false;
  const size_t n = p.getBytes(key, &out, sizeof(out));
  return n == sizeof(out);
}

bool writeAndVerifySlot(const char* key, QueueBlob& candidate) {
  candidate.checksum = computeChecksum(candidate);

  Preferences p;
  if (!p.begin(kNs, false)) {
    Serial.println("[QUEUE] NVS begin failed");
    ++g_stats.persistenceFailures;
    return false;
  }

  const size_t written = p.putBytes(key, &candidate, sizeof(candidate));
  QueueBlob verify{};
  const bool readOk = readSlot(p, key, verify);
  p.end();

  if (written != sizeof(candidate) || !readOk ||
      memcmp(&candidate, &verify, sizeof(candidate)) != 0) {
    Serial.printf("[QUEUE] A/B commit verify failed for %s (%u/%u readOk=%d)\n",
                  key, (unsigned)written, (unsigned)sizeof(candidate),
                  readOk ? 1 : 0);
    ++g_stats.persistenceFailures;
    return false;
  }
  return true;
}

bool commitCandidate(QueueBlob& candidate) {
  const char* inactiveKey = (g_activeSlot == 'a') ? kSlotB : kSlotA;
  const char inactiveSlot = (g_activeSlot == 'a') ? 'b' : 'a';
  candidate.generation = g_blob.generation + 1;

  if (!writeAndVerifySlot(inactiveKey, candidate)) {
    return false;
  }

  g_blob = candidate;
  g_activeSlot = inactiveSlot;
  g_ready = true;
  return true;
}

bool loadLegacy(Preferences& p, QueueBlob& out) {
  struct LegacyQueueBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint32_t nextSeq;
    uint16_t head;
    uint16_t tail;
    uint16_t used;
    uint16_t reserved;
    node_snapshot_t records[kCapacity];
    uint32_t checksum;
  };

  if (p.getBytesLength(kLegacyBlob) != sizeof(LegacyQueueBlob)) return false;

  LegacyQueueBlob legacy{};
  const size_t n = p.getBytes(kLegacyBlob, &legacy, sizeof(legacy));
  if (n != sizeof(legacy)) return false;
  const uint32_t legacyChecksum = fnv1a32(reinterpret_cast<const uint8_t*>(&legacy),
                                         sizeof(legacy) - sizeof(uint32_t));
  if (legacy.checksum != legacyChecksum ||
      legacy.capacity != kCapacity ||
      legacy.head >= kCapacity ||
      legacy.tail >= kCapacity ||
      legacy.used > kCapacity ||
      legacy.nextSeq == 0) {
    return false;
  }

  initDefault(out);
  out.nextSeq = legacy.nextSeq;
  out.head = legacy.head;
  out.tail = legacy.tail;
  out.used = legacy.used;
  memcpy(out.records, legacy.records, sizeof(out.records));
  out.checksum = computeChecksum(out);
  return true;
}

bool load() {
  Preferences p;
  if (!p.begin(kNs, true)) return false;

  QueueBlob a{}, b{};
  const bool aValid = readSlot(p, kSlotA, a);
  const bool bValid = readSlot(p, kSlotB, b);

  QueueBlob raw{};
  if (!aValid && readRawSlot(p, kSlotA, raw)) ++g_stats.corruptRecords;
  if (!bValid && readRawSlot(p, kSlotB, raw)) ++g_stats.corruptRecords;

  if (aValid && bValid) {
    if (generationNewer(b.generation, a.generation)) {
      g_blob = b;
      g_activeSlot = 'b';
      ++g_stats.recoveredFromSecondary;
    } else {
      g_blob = a;
      g_activeSlot = 'a';
    }
    p.end();
    return true;
  }
  if (aValid) {
    g_blob = a;
    g_activeSlot = 'a';
    p.end();
    return true;
  }
  if (bValid) {
    g_blob = b;
    g_activeSlot = 'b';
    ++g_stats.recoveredFromSecondary;
    p.end();
    return true;
  }

  QueueBlob migrated{};
  const bool legacyOk = loadLegacy(p, migrated);
  p.end();
  if (legacyOk) {
    g_blob = migrated;
    g_activeSlot = 'a';
    g_ready = true;
    if (writeAndVerifySlot(kSlotA, g_blob)) {
      Serial.printf("[QUEUE] migrated legacy queue: used=%u nextSeq=%lu\n",
                    (unsigned)g_blob.used, (unsigned long)g_blob.nextSeq);
      return true;
    }
  }

  return false;
}

}  // namespace

namespace local_queue {

bool begin() {
  if (load()) {
    g_ready = true;
    Serial.printf("[QUEUE] loaded slot=%c used=%u nextSeq=%lu gen=%lu\n",
                  g_activeSlot, (unsigned)g_blob.used,
                  (unsigned long)g_blob.nextSeq,
                  (unsigned long)g_blob.generation);
    return true;
  }

  initDefault(g_blob);
  g_activeSlot = 'a';
  g_ready = writeAndVerifySlot(kSlotA, g_blob);
  Serial.printf("[QUEUE] initialized new queue: used=%u nextSeq=%lu ok=%d\n",
                (unsigned)g_blob.used, (unsigned long)g_blob.nextSeq,
                g_ready ? 1 : 0);
  return g_ready;
}

bool enqueue(const node_snapshot_t& snap) {
  if (!g_ready && !begin()) return false;

  g_candidate = g_blob;
  uint16_t effectiveQualityFlags = snap.qualityFlags;

  if (g_candidate.used >= kCapacity) {
    Serial.printf("[QUEUE] full (%u/%u); dropping oldest (seq=%lu) DROP_OLDEST\n",
                  (unsigned)g_candidate.used, (unsigned)kCapacity,
                  (unsigned long)g_candidate.records[g_candidate.tail].seqNum);
    g_candidate.tail = (uint16_t)((g_candidate.tail + 1) % kCapacity);
    g_candidate.used--;
    effectiveQualityFlags |= QF_DROPPED;
    ++g_stats.droppedDueToCapacity;
  }

  node_snapshot_t rec = snap;
  rec.seqNum = g_candidate.nextSeq;
  rec.qualityFlags = effectiveQualityFlags;

  g_candidate.records[g_candidate.head] = rec;
  g_candidate.head = (uint16_t)((g_candidate.head + 1) % kCapacity);
  g_candidate.used++;
  g_candidate.nextSeq++;

  return commitCandidate(g_candidate);
}

bool peek(node_snapshot_t& out) {
  if (!g_ready && !begin()) return false;
  if (g_blob.used == 0) return false;
  out = g_blob.records[g_blob.tail];
  return true;
}

bool pop() {
  if (!g_ready && !begin()) return false;
  if (g_blob.used == 0) return false;

  g_candidate = g_blob;
  g_candidate.tail = (uint16_t)((g_candidate.tail + 1) % kCapacity);
  g_candidate.used--;
  return commitCandidate(g_candidate);
}

bool acknowledgeHead(uint32_t seqNum) {
  node_snapshot_t head{};
  if (!peek(head)) return false;
  if (head.seqNum != seqNum) {
    Serial.printf("[QUEUE] ACK seq mismatch: head=%lu ack=%lu\n",
                  (unsigned long)head.seqNum, (unsigned long)seqNum);
    return false;
  }
  return pop();
}

uint16_t count() {
  if (!g_ready && !begin()) return 0;
  return g_blob.used;
}

uint32_t nextSeq() {
  if (!g_ready && !begin()) return 0;
  return g_blob.nextSeq;
}

void clear() {
  if (!g_ready && !begin()) return;
  g_candidate = g_blob;
  const uint32_t next = g_blob.nextSeq > 0 ? g_blob.nextSeq : 1;
  initDefault(g_candidate);
  g_candidate.nextSeq = next;
  if (!commitCandidate(g_candidate)) {
    Serial.println("[QUEUE] clear persist failed");
    return;
  }
  Serial.println("[QUEUE] cleared");
}

QueueStats stats() {
  return g_stats;
}

#ifdef LOCAL_QUEUE_TESTING
void resetForTest() {
  g_ready = false;
  g_activeSlot = 'a';
  memset(&g_blob, 0, sizeof(g_blob));
  memset(&g_candidate, 0, sizeof(g_candidate));
  g_stats = {};
}

bool forceCorruptActiveRecordForTest() {
  if (!g_ready && !begin()) return false;
  const char* key = (g_activeSlot == 'a') ? kSlotA : kSlotB;
  QueueBlob corrupt = g_blob;
  corrupt.checksum ^= 0xA5A5A5A5UL;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  const size_t written = p.putBytes(key, &corrupt, sizeof(corrupt));
  p.end();
  return written == sizeof(corrupt);
}
#endif

}  // namespace local_queue
