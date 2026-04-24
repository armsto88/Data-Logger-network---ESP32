#pragma once

#include <Arduino.h>
#include "../../../../shared/protocol.h"

namespace local_queue {

static constexpr uint16_t QF_DROPPED = 0x0001;

// The queue now stores one node_snapshot_t per wake cycle rather than
// individual sensor readings. This reduces ESP-NOW packet count from
// N_sensors packets/node/wake to 1 packet/node/wake.
bool begin();
bool enqueue(const node_snapshot_t& snap);
bool peek(node_snapshot_t& out);
bool pop();
uint16_t count();
uint32_t nextSeq();
void clear();

}  // namespace local_queue
