#pragma once

#include <Arduino.h>
#include "protocol.h"

namespace local_queue {

static constexpr uint16_t QF_DROPPED = 0x0001;

// ===== V2 queue API =====
// The queue stores variable-length V2 snapshots (48-byte header + N×6-byte
// readings) in a circular byte slab. One record per wake cycle.

bool begin();

// Enqueue a V2 snapshot (header + readings array). Returns true on success.
// If the slab is full, oldest records are dropped until space is available,
// and QF_DROPPED is OR'd into the new record's qualityFlags.
bool enqueueV2(const node_snapshot_v2_t& hdr,
               const v2_reading_t* readings,
               size_t count);

// Peek the oldest record. Copies the raw V2 wire bytes (header + readings)
// into outBuf and sets outLen. Returns false if the queue is empty or the
// record is larger than bufSize.
bool peekV2(uint8_t* outBuf, size_t bufSize, size_t& outLen);

// Pop the oldest record. Returns false if the queue is empty.
bool pop();

// Number of records currently queued.
uint16_t count();

// Next sequence number that will be assigned to an enqueued record.
uint32_t nextSeq();

// Clear all records (preserves nextSeq).
void clear();

struct QueueStats {
  uint32_t droppedDueToCapacity;
  uint32_t persistenceFailures;
  uint32_t recoveredFromSecondary;
  uint32_t corruptRecords;
};

QueueStats stats();

#ifdef LOCAL_QUEUE_TESTING
void resetForTest();
bool forceCorruptActiveRecordForTest();
#endif

}  // namespace local_queue
