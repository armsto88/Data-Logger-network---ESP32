#include "local_queue.h"

#include <Preferences.h>
#include <string.h>

namespace {

// ===== V2 circular byte-slab queue =====
//
// Each record stored in the slab is: [uint16_t recordLen][recordBytes...]
// where recordLen is the total V2 wire size (48 + 6*sensorCount).
// Records are contiguous and wrap around the end of the slab.

static constexpr uint32_t kMagicV2    = 0x4E514D34;  // "NQM4"
static constexpr uint16_t kVersionV2  = 5;
static constexpr uint16_t kSlabCapacity = 3500;       // bytes for record slab

// Legacy V1 blob magic (for migration detection only).
static constexpr uint32_t kLegacyMagicV1 = 0x4E514D33;  // "NQM3"
static constexpr uint16_t kLegacyCapacityV1 = 24;       // V1 fixed-slot count

struct QueueBlobV2 {
  uint32_t magic;          // kMagicV2
  uint16_t version;        // kVersionV2
  uint16_t slabCapacity;   // kSlabCapacity
  uint32_t layoutId;       // sizeof(QueueBlobV2) ^ (slabCapacity << 16)
  uint32_t generation;     // A/B slot generation
  uint32_t nextSeq;        // next sequence number
  uint16_t head;           // byte offset into slab (next write position)
  uint16_t tail;           // byte offset into slab (oldest record)
  uint16_t usedBytes;      // bytes currently used in slab
  uint16_t recordCount;    // number of records
  uint8_t  reserved[4];    // padding
  uint8_t  slab[kSlabCapacity];  // circular byte buffer
  uint32_t checksum;       // FNV1a of everything above
};

static constexpr uint32_t kLayoutIdV2 =
    (uint32_t)sizeof(QueueBlobV2) ^ ((uint32_t)kSlabCapacity << 16);

static_assert(sizeof(QueueBlobV2) < 4000,
              "Queue blob too large for NVS key");

static constexpr const char* kNs = "node_q";
static constexpr const char* kSlotA = "q_a";
static constexpr const char* kSlotB = "q_b";
static constexpr const char* kLegacyBlob = "blob";

// Static globals — keeps ~3.5KB off the 8KB loopTask stack.
QueueBlobV2 g_blob{};
QueueBlobV2 g_candidate{};
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

uint32_t computeChecksum(const QueueBlobV2& b) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&b),
                 sizeof(QueueBlobV2) - sizeof(uint32_t));
}

bool generationNewer(uint32_t a, uint32_t b) {
  return a != b && static_cast<int32_t>(a - b) > 0;
}

void initDefault(QueueBlobV2& b) {
  memset(&b, 0, sizeof(b));
  b.magic = kMagicV2;
  b.version = kVersionV2;
  b.slabCapacity = kSlabCapacity;
  b.layoutId = kLayoutIdV2;
  b.generation = 1;
  b.nextSeq = 1;
  b.head = 0;
  b.tail = 0;
  b.usedBytes = 0;
  b.recordCount = 0;
  b.checksum = computeChecksum(b);
}

bool validateBlob(const QueueBlobV2& b) {
  if (b.magic != kMagicV2 || b.version != kVersionV2 ||
      b.slabCapacity != kSlabCapacity || b.layoutId != kLayoutIdV2) {
    return false;
  }
  if (b.head >= kSlabCapacity || b.tail >= kSlabCapacity ||
      b.usedBytes > kSlabCapacity) {
    return false;
  }
  if (b.nextSeq == 0) return false;
  return b.checksum == computeChecksum(b);
}

bool readSlot(Preferences& p, const char* key, QueueBlobV2& out) {
  if (p.getBytesLength(key) != sizeof(out)) return false;
  const size_t n = p.getBytes(key, &out, sizeof(out));
  return n == sizeof(out) && validateBlob(out);
}

bool readRawSlot(Preferences& p, const char* key, QueueBlobV2& out) {
  if (p.getBytesLength(key) != sizeof(out)) return false;
  const size_t n = p.getBytes(key, &out, sizeof(out));
  return n == sizeof(out);
}

bool writeAndVerifySlot(const char* key, QueueBlobV2& candidate) {
  candidate.checksum = computeChecksum(candidate);

  Preferences p;
  if (!p.begin(kNs, false)) {
    Serial.println("[QUEUE] NVS begin failed");
    ++g_stats.persistenceFailures;
    return false;
  }

  const size_t written = p.putBytes(key, &candidate, sizeof(candidate));
  // Single-threaded: only called from loopTask. Static keeps ~3.5KB off stack.
  static QueueBlobV2 verify{};
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

bool commitCandidate(QueueBlobV2& candidate) {
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

// ===== Slab read/write helpers (handle wraparound) =====

void readSlab(const QueueBlobV2& b, uint16_t offset, uint8_t* dst, size_t len) {
  if (len == 0) return;
  const size_t first = (size_t)offset;
  if (first + len <= kSlabCapacity) {
    memcpy(dst, &b.slab[first], len);
  } else {
    const size_t firstChunk = kSlabCapacity - first;
    memcpy(dst, &b.slab[first], firstChunk);
    memcpy(dst + firstChunk, &b.slab[0], len - firstChunk);
  }
}

void writeSlab(QueueBlobV2& b, uint16_t offset, const uint8_t* src, size_t len) {
  if (len == 0) return;
  const size_t first = (size_t)offset;
  if (first + len <= kSlabCapacity) {
    memcpy(&b.slab[first], src, len);
  } else {
    const size_t firstChunk = kSlabCapacity - first;
    memcpy(&b.slab[first], src, firstChunk);
    memcpy(&b.slab[0], src + firstChunk, len - firstChunk);
  }
}

// Read the 2-byte length prefix at the given tail offset (wraparound-safe).
uint16_t readRecordLen(const QueueBlobV2& b, uint16_t offset) {
  uint8_t lenBuf[2];
  readSlab(b, offset, lenBuf, 2);
  return (uint16_t)((uint16_t)lenBuf[0] | ((uint16_t)lenBuf[1] << 8));
}

// Drop the oldest record from candidate. Returns false if empty.
bool dropOldest(QueueBlobV2& b) {
  if (b.recordCount == 0) return false;
  const uint16_t recLen = readRecordLen(b, b.tail);
  const uint16_t total = (uint16_t)(2u + recLen);
  b.tail = (uint16_t)(((uint32_t)b.tail + total) % kSlabCapacity);
  if (b.usedBytes >= total) b.usedBytes = (uint16_t)(b.usedBytes - total);
  else b.usedBytes = 0;
  b.recordCount = (uint16_t)(b.recordCount - 1);
  return true;
}

// ===== V1 → V2 migration =====
// Reads a legacy V1 QueueBlob (fixed node_snapshot_t slots) and converts each
// record into V2 wire bytes written into a fresh QueueBlobV2.

bool loadLegacyV1(Preferences& p, QueueBlobV2& out) {
  struct LegacyQueueBlob {
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
    node_snapshot_t records[kLegacyCapacityV1];
    uint32_t checksum;
  };

  if (p.getBytesLength(kLegacyBlob) != sizeof(LegacyQueueBlob)) return false;

  // Init-time scratch; static keeps it off the stack.
  static LegacyQueueBlob legacy{};
  const size_t n = p.getBytes(kLegacyBlob, &legacy, sizeof(legacy));
  if (n != sizeof(legacy)) return false;
  const uint32_t legacyChecksum =
      fnv1a32(reinterpret_cast<const uint8_t*>(&legacy),
              sizeof(legacy) - sizeof(uint32_t));
  if (legacy.checksum != legacyChecksum ||
      legacy.magic != kLegacyMagicV1 ||
      legacy.capacity != kLegacyCapacityV1 ||
      legacy.head >= kLegacyCapacityV1 ||
      legacy.tail >= kLegacyCapacityV1 ||
      legacy.used > kLegacyCapacityV1 ||
      legacy.nextSeq == 0) {
    return false;
  }

  initDefault(out);
  out.nextSeq = legacy.nextSeq;

  // Walk the V1 ring in FIFO order, expanding each record into V2 readings.
  uint16_t idx = legacy.tail;
  for (uint16_t i = 0; i < legacy.used; ++i) {
    const node_snapshot_t& snap = legacy.records[idx];
    v2_reading_t r[MAX_READINGS_PER_SNAPSHOT];
    size_t rc = 0;

    if (snap.sensorPresent & SNAP_PRESENT_BAT_V) {
      r[rc].sensorId = SENSOR_ID_BAT_V;    r[rc].value = snap.batVoltage;     ++rc;
    }
    if (snap.sensorPresent & SNAP_PRESENT_AIR_TEMP) {
      r[rc].sensorId = SENSOR_ID_AIR_TEMP; r[rc].value = snap.airTemp;        ++rc;
    }
    if (snap.sensorPresent & SNAP_PRESENT_AIR_RH) {
      r[rc].sensorId = SENSOR_ID_AIR_RH;   r[rc].value = snap.airHumidity;    ++rc;
    }
    if (snap.sensorPresent & SNAP_PRESENT_SPECTRAL) {
      static const uint16_t spectralIds[8] = {
        SENSOR_ID_SPECTRAL_415, SENSOR_ID_SPECTRAL_445, SENSOR_ID_SPECTRAL_480,
        SENSOR_ID_SPECTRAL_515, SENSOR_ID_SPECTRAL_555, SENSOR_ID_SPECTRAL_590,
        SENSOR_ID_SPECTRAL_630, SENSOR_ID_SPECTRAL_680
      };
      for (int s = 0; s < 8 && rc < MAX_READINGS_PER_SNAPSHOT; ++s) {
        r[rc].sensorId = spectralIds[s]; r[rc].value = snap.spectral[s]; ++rc;
      }
    }
    if (snap.sensorPresent & SNAP_PRESENT_WIND) {
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_WIND_SPEED; r[rc].value = snap.windSpeed; ++rc;
      }
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_WIND_DIR;   r[rc].value = snap.windDir;   ++rc;
      }
    }
    if (snap.sensorPresent & SNAP_PRESENT_SOIL1) {
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_SOIL1_VWC;  r[rc].value = snap.soil1Vwc;  ++rc;
      }
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_SOIL1_TEMP; r[rc].value = snap.soil1Temp; ++rc;
      }
    }
    if (snap.sensorPresent & SNAP_PRESENT_SOIL2) {
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_SOIL2_VWC;  r[rc].value = snap.soil2Vwc;  ++rc;
      }
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_SOIL2_TEMP; r[rc].value = snap.soil2Temp; ++rc;
      }
    }
    if (snap.sensorPresent & SNAP_PRESENT_AUX1) {
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_AUX1; r[rc].value = snap.aux1; ++rc;
      }
    }
    if (snap.sensorPresent & SNAP_PRESENT_AUX2) {
      if (rc < MAX_READINGS_PER_SNAPSHOT) {
        r[rc].sensorId = SENSOR_ID_AUX2; r[rc].value = snap.aux2; ++rc;
      }
    }

    // Build a V2 header from the V1 record.
    node_snapshot_v2_t hdr{};
    strncpy(hdr.command, "NODE_SNAPSHOT2", sizeof(hdr.command) - 1);
    strncpy(hdr.nodeId, snap.nodeId, sizeof(hdr.nodeId) - 1);
    hdr.nodeTimestamp   = snap.nodeTimestamp;
    hdr.seqNum          = snap.seqNum;
    hdr.sensorCount     = (uint16_t)rc;
    hdr.qualityFlags    = snap.qualityFlags;
    hdr.configVersion   = snap.configVersion;
    hdr.protocolVersion = NODE_PROTOCOL_VERSION;
    hdr.reserved        = 0;

    const size_t wireLen = snapshotV2WireSize((uint16_t)rc);
    const uint16_t total = (uint16_t)(2u + wireLen);

    // Drop oldest if needed (should not happen on fresh blob, but be safe).
    while (out.recordCount > 0 &&
           (uint32_t)out.usedBytes + total > kSlabCapacity) {
      dropOldest(out);
    }
    if ((uint32_t)out.usedBytes + total > kSlabCapacity) {
      // Record too large for empty slab — skip.
      idx = (uint16_t)((idx + 1) % kLegacyCapacityV1);
      continue;
    }

    // Write length prefix.
    uint8_t lenBuf[2] = {(uint8_t)(wireLen & 0xFF),
                         (uint8_t)((wireLen >> 8) & 0xFF)};
    writeSlab(out, out.head, lenBuf, 2);
    // Write header.
    writeSlab(out, (uint16_t)(out.head + 2),
              reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    // Write readings.
    if (rc > 0) {
      writeSlab(out, (uint16_t)(out.head + 2 + sizeof(hdr)),
                reinterpret_cast<const uint8_t*>(r), rc * sizeof(v2_reading_t));
    }
    out.head = (uint16_t)(((uint32_t)out.head + total) % kSlabCapacity);
    out.usedBytes = (uint16_t)(out.usedBytes + total);
    out.recordCount = (uint16_t)(out.recordCount + 1);

    idx = (uint16_t)((idx + 1) % kLegacyCapacityV1);
  }

  out.checksum = computeChecksum(out);
  return true;
}

bool load() {
  Preferences p;
  if (!p.begin(kNs, true)) return false;

  // Init-time scratch buffers (single-threaded). Static moves ~10KB off stack.
  static QueueBlobV2 a{}, b{};
  const bool aValid = readSlot(p, kSlotA, a);
  const bool bValid = readSlot(p, kSlotB, b);

  static QueueBlobV2 raw{};
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

  // No valid V2 slots — attempt V1 legacy migration.
  static QueueBlobV2 migrated{};
  const bool legacyOk = loadLegacyV1(p, migrated);
  p.end();
  if (legacyOk) {
    g_blob = migrated;
    g_activeSlot = 'a';
    g_ready = true;
    if (writeAndVerifySlot(kSlotA, g_blob)) {
      Serial.printf("[QUEUE] migrated V1 queue: records=%u nextSeq=%lu\n",
                    (unsigned)g_blob.recordCount,
                    (unsigned long)g_blob.nextSeq);
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
    Serial.printf("[QUEUE] loaded slot=%c records=%u used=%u nextSeq=%lu gen=%lu\n",
                  g_activeSlot, (unsigned)g_blob.recordCount,
                  (unsigned)g_blob.usedBytes,
                  (unsigned long)g_blob.nextSeq,
                  (unsigned long)g_blob.generation);
    return true;
  }

  initDefault(g_blob);
  g_activeSlot = 'a';
  g_ready = writeAndVerifySlot(kSlotA, g_blob);
  Serial.printf("[QUEUE] initialized new queue: nextSeq=%lu ok=%d\n",
                (unsigned long)g_blob.nextSeq, g_ready ? 1 : 0);
  return g_ready;
}

bool enqueueV2(const node_snapshot_v2_t& hdr,
               const v2_reading_t* readings,
               size_t count) {
  if (!g_ready && !begin()) return false;
  if (count > MAX_READINGS_PER_SNAPSHOT) return false;

  const size_t wireLen = snapshotV2WireSize((uint16_t)count);
  const uint16_t total = (uint16_t)(2u + wireLen);

  // A single record must always fit in an empty slab.
  if (total > kSlabCapacity) {
    Serial.printf("[QUEUE] record too large for slab (%u > %u)\n",
                  (unsigned)total, (unsigned)kSlabCapacity);
    return false;
  }

  g_candidate = g_blob;
  uint16_t effectiveQualityFlags = hdr.qualityFlags;

  // Drop oldest records until space is available.
  bool droppedAny = false;
  while (g_candidate.recordCount > 0 &&
         (uint32_t)g_candidate.usedBytes + total > kSlabCapacity) {
    const uint16_t oldLen = readRecordLen(g_candidate, g_candidate.tail);
    Serial.printf("[QUEUE] slab full (%u/%u); dropping oldest (len=%u) DROP_OLDEST\n",
                  (unsigned)g_candidate.usedBytes, (unsigned)kSlabCapacity,
                  (unsigned)oldLen);
    dropOldest(g_candidate);
    ++g_stats.droppedDueToCapacity;
    droppedAny = true;
  }
  if (droppedAny) effectiveQualityFlags |= QF_DROPPED;

  // Build the record in a scratch buffer, then write into the slab.
  // Static keeps ~250B off the stack; single-threaded.
  static uint8_t recBuf[2 + sizeof(node_snapshot_v2_t) +
                        MAX_READINGS_PER_SNAPSHOT * sizeof(v2_reading_t)];
  node_snapshot_v2_t outHdr = hdr;
  outHdr.seqNum = g_candidate.nextSeq;
  outHdr.sensorCount = (uint16_t)count;
  outHdr.qualityFlags = effectiveQualityFlags;

  memcpy(&recBuf[0], &outHdr, sizeof(outHdr));
  if (count > 0 && readings) {
    memcpy(&recBuf[sizeof(outHdr)], readings, count * sizeof(v2_reading_t));
  }
  // Length prefix.
  uint8_t lenBuf[2] = {(uint8_t)(wireLen & 0xFF),
                       (uint8_t)((wireLen >> 8) & 0xFF)};

  writeSlab(g_candidate, g_candidate.head, lenBuf, 2);
  writeSlab(g_candidate, (uint16_t)(g_candidate.head + 2), recBuf, wireLen);

  g_candidate.head = (uint16_t)(((uint32_t)g_candidate.head + total) % kSlabCapacity);
  g_candidate.usedBytes = (uint16_t)(g_candidate.usedBytes + total);
  g_candidate.recordCount = (uint16_t)(g_candidate.recordCount + 1);
  g_candidate.nextSeq++;

  return commitCandidate(g_candidate);
}

bool peekV2(uint8_t* outBuf, size_t bufSize, size_t& outLen) {
  if (!g_ready && !begin()) return false;
  if (g_blob.recordCount == 0) return false;

  const uint16_t recLen = readRecordLen(g_blob, g_blob.tail);
  if (recLen > bufSize) {
    Serial.printf("[QUEUE] peek buffer too small (%u > %u)\n",
                  (unsigned)recLen, (unsigned)bufSize);
    outLen = recLen;
    return false;
  }

  readSlab(g_blob, (uint16_t)(g_blob.tail + 2), outBuf, recLen);
  outLen = recLen;
  return true;
}

bool pop() {
  if (!g_ready && !begin()) return false;
  if (g_blob.recordCount == 0) return false;

  g_candidate = g_blob;
  dropOldest(g_candidate);
  return commitCandidate(g_candidate);
}

uint16_t count() {
  if (!g_ready && !begin()) return 0;
  return g_blob.recordCount;
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
  QueueBlobV2 corrupt = g_blob;
  corrupt.checksum ^= 0xA5A5A5A5UL;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  const size_t written = p.putBytes(key, &corrupt, sizeof(corrupt));
  p.end();
  return written == sizeof(corrupt);
}
#endif

}  // namespace local_queue
