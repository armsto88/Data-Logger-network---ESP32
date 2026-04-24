#include "local_queue.h"

#include <Preferences.h>
#include <string.h>

namespace {

// Bump magic/version so existing NVS blobs from the old per-sensor queue are
// automatically invalidated and a fresh queue is started on first boot.
static constexpr uint32_t kMagic    = 0x4E514D32;  // "NQM2" (was NQM1)
static constexpr uint16_t kVersion  = 3;            // was 2
static constexpr uint16_t kCapacity = 24;           // 24 snapshots * 124B = 2976B records
                                                     // + 24B header + 4B checksum = 3004B < 4000B (NVS limit)

struct QueueBlob {
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

static_assert(sizeof(QueueBlob) < 4000, "Queue blob too large for NVS key");

namespace local_queue {

bool begin() {
  if (load()) {
    g_ready = true;
    Serial.printf("[QUEUE] loaded: used=%u nextSeq=%lu\n", (unsigned)g_blob.used, (unsigned long)g_blob.nextSeq);
    return true;
  }

  initDefault();
  g_ready = persist();
  Serial.printf("[QUEUE] initialized new queue: used=%u nextSeq=%lu\n", (unsigned)g_blob.used, (unsigned long)g_blob.nextSeq);
  return g_ready;
}

bool enqueue(const node_snapshot_t& snap) {
  if (!g_ready && !begin()) return false;

  uint16_t effectiveQualityFlags = snap.qualityFlags;

  if (g_blob.used >= kCapacity) {
    Serial.printf("[QUEUE] full (%u/%u); dropping oldest (seq=%lu) DROP_OLDEST\n",
                  (unsigned)g_blob.used, (unsigned)kCapacity,
                  (unsigned long)g_blob.records[g_blob.tail].seqNum);
    g_blob.tail = (uint16_t)((g_blob.tail + 1) % kCapacity);
    g_blob.used--;
    effectiveQualityFlags |= local_queue::QF_DROPPED;
  }

  node_snapshot_t rec = snap;
  rec.seqNum       = g_blob.nextSeq;
  rec.qualityFlags = effectiveQualityFlags;

  g_blob.records[g_blob.head] = rec;
  g_blob.head = (uint16_t)((g_blob.head + 1) % kCapacity);
  g_blob.used++;
  g_blob.nextSeq++;

  return persist();
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

  g_blob.tail = (uint16_t)((g_blob.tail + 1) % kCapacity);
  g_blob.used--;
  return persist();
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
  initDefault();
  if (!persist()) {
    Serial.println("[QUEUE] clear persist failed");
    return;
  }
  g_ready = true;
  Serial.println("[QUEUE] cleared");
}

}  // namespace local_queue

struct QueueBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t capacity;
  uint32_t nextSeq;
  uint16_t head;
  uint16_t tail;
  uint16_t used;
  uint16_t reserved;
  local_queue::QueuedSample records[kCapacity];
  uint32_t checksum;
};

static_assert(sizeof(QueueBlob) < 1900, "Queue blob too large for NVS key");

QueueBlob g_blob{};
bool g_ready = false;

uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t computeChecksum(const QueueBlob& b) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&b), sizeof(QueueBlob) - sizeof(uint32_t));
}

void initDefault() {
  memset(&g_blob, 0, sizeof(g_blob));
  g_blob.magic = kMagic;
  g_blob.version = kVersion;
  g_blob.capacity = kCapacity;
  g_blob.nextSeq = 1;
  g_blob.checksum = computeChecksum(g_blob);
}

bool persist() {
  g_blob.checksum = computeChecksum(g_blob);

  Preferences p;
  if (!p.begin("node_q", false)) {
    Serial.println("[QUEUE] NVS begin failed");
    return false;
  }

  size_t n = p.putBytes("blob", &g_blob, sizeof(g_blob));
  p.end();

  if (n != sizeof(g_blob)) {
    Serial.printf("[QUEUE] putBytes short write (%u/%u)\n", (unsigned)n, (unsigned)sizeof(g_blob));
    return false;
  }
  return true;
}

bool load() {
  Preferences p;
  if (!p.begin("node_q", true)) {
    return false;
  }

  size_t n = p.getBytesLength("blob");
  if (n != sizeof(g_blob)) {
    p.end();
    return false;
  }

  size_t read = p.getBytes("blob", &g_blob, sizeof(g_blob));
  p.end();

  if (read != sizeof(g_blob)) return false;
  if (g_blob.magic != kMagic || g_blob.version != kVersion || g_blob.capacity != kCapacity) return false;
  if (g_blob.checksum != computeChecksum(g_blob)) return false;
  if (g_blob.head >= kCapacity || g_blob.tail >= kCapacity || g_blob.used > kCapacity) return false;
  if (g_blob.nextSeq == 0) return false;
  return true;
}

}  // namespace
