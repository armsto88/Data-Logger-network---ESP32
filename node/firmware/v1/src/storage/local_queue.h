#pragma once

#include <Arduino.h>
#include "protocol.h"

namespace local_queue {

static constexpr uint16_t QF_DROPPED = 0x0001;

// The queue now stores one node_snapshot_t per wake cycle rather than
// individual sensor readings. This reduces ESP-NOW packet count from
// N_sensors packets/node/wake to 1 packet/node/wake.
bool begin();
bool enqueue(const node_snapshot_t& snap);
bool peek(node_snapshot_t& out);
bool pop();
bool acknowledgeHead(uint32_t seqNum);
uint16_t count();
uint32_t nextSeq();
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
