# Node Local Storage Contract v1

Status: Draft v1 (implementation target)
Scope: ESP32 node firmware local sample persistence and delayed sync to mothership

## 1. Goals

This contract defines the exact rules for local data durability, replay, and acknowledgement when node sampling cadence is decoupled from sync cadence.

Required guarantees:
- Survive reset, brownout, and power loss without corrupting committed samples.
- Preserve strict sample ordering during replay.
- Bound memory use with deterministic full-buffer behavior.
- Ensure idempotent sync so retries do not duplicate logical data.
- Expose clear health signals (backlog depth, oldest unsynced age, drops if policy allows).

Non-goals (v1):
- In-place record editing.
- Compression.
- Per-record cryptographic signatures.

## 2. Core model

Storage model is append-only queue with cumulative acknowledgements.

- Producer: sampling loop (append only).
- Consumer: sync loop (read unsynced window, send batch).
- Commit point: ACK from mothership advances ack pointer.

Sequence model:
- Each sample has a strictly increasing `sample_seq` (uint32).
- ACK is cumulative: `ack_upto_seq` means all `sample_seq <= ack_upto_seq` are durable upstream.

## 3. Record format (fixed size)

All multi-byte fields are little-endian.

```c
#pragma pack(push, 1)
typedef struct {
  uint32_t sample_seq;        // Monotonic, starts at 1
  uint32_t sample_unix_s;     // RTC-based unix seconds
  uint16_t sensor_id;         // Stable enum/id, not display label
  uint16_t quality_flags;     // bitfield (sensor status, rtc confidence, etc.)
  float    value_f32;         // Sensor value
  uint16_t crc16;             // CRC-16 over bytes [0..13]
  uint8_t  commit_marker;     // 0xA5 when record is fully committed
  uint8_t  reserved;          // keep alignment/room for minor extension
} SampleRecordV1;             // 18 bytes
#pragma pack(pop)
```

Commit rule:
- A record is valid only if CRC passes and `commit_marker == 0xA5`.

## 4. Metadata format

Metadata is stored in two slots (A/B) with generation counter. New metadata writes alternate slots to survive interrupted updates.

```c
#pragma pack(push, 1)
typedef struct {
  uint32_t format_version;    // must be 1
  uint32_t generation;        // monotonic metadata generation

  uint32_t capacity_records;  // fixed at format time

  uint32_t write_seq;         // last committed sample_seq (0 if none)
  uint32_t acked_seq;         // last cumulatively ACKed seq (<= write_seq)

  uint32_t write_index;       // ring index for next append
  uint32_t acked_index;       // ring index of first unacked record

  uint32_t dropped_unsynced;  // increments only if overwrite policy enabled

  uint16_t crc16;             // CRC over all prior bytes
} QueueMetaV1;
#pragma pack(pop)
```

Metadata invariants:
- `acked_seq <= write_seq`
- `write_index < capacity_records`
- `acked_index < capacity_records`
- If `write_seq == 0`, then queue is empty and `acked_seq == 0`

## 5. Queue states and invariants

Definitions:
- `pending = write_seq - acked_seq`
- Queue empty iff `pending == 0`
- Queue full when next append would overwrite an unacked slot

Hard invariants:
- Sampling path never modifies ack pointers.
- Sync path never modifies record payload bytes.
- Sequence numbers never decrease or repeat.
- Read/replay order is always ascending `sample_seq`.

## 6. Operations

### 6.1 Append sample

Input: `(sample_unix_s, sensor_id, quality_flags, value_f32)`

Steps:
1. Check if queue has writable slot under selected full policy.
2. Build `SampleRecordV1` with `sample_seq = write_seq + 1`.
3. Compute CRC.
4. Write record body with `commit_marker = 0x00`.
5. Write `commit_marker = 0xA5` as final write.
6. Update metadata (`write_seq`, `write_index`) via A/B slot commit.

Success condition:
- After reboot, record remains readable exactly once in sequence stream.

### 6.2 Build sync batch

Input: `max_records`, `max_payload_bytes`

Steps:
1. Start from first unacked record (`acked_seq + 1`).
2. Add contiguous records in ascending `sample_seq` while limits allow.
3. Do not skip gaps; stop at first missing/invalid candidate.

Batch headers must include:
- `batch_start_seq`
- `batch_end_seq`
- `record_count`

### 6.3 Apply ACK

Input: `ack_upto_seq`

Rules:
- Ignore ACK if `ack_upto_seq <= acked_seq` (duplicate/outdated ACK).
- Clamp to `write_seq` if ACK exceeds local head.
- Advance ack pointer cumulatively to new value.
- Recompute `acked_index` from sequence delta.
- Persist metadata with A/B generation commit.

### 6.4 Garbage collection / space reuse

No per-record delete.

Reclaim policy:
- Ring slots with `sample_seq <= acked_seq` are reclaimable.
- Reuse happens naturally when write cursor wraps.

## 7. Full-buffer policy

Default policy for scientific logging:
- `PROTECT_UNSYNCED` (required default in v1)

Behavior:
- If queue is full and no reclaimable slots exist, reject append.
- Set storage alarm flag (`STORAGE_FULL`).
- Keep retrying sync until ACK frees space.

Optional (compile-time) alternative:
- `OVERWRITE_OLDEST_UNSYNCED` (not default)
- Must increment `dropped_unsynced` and emit explicit data-loss event.

## 8. Boot recovery contract

At boot:
1. Load metadata slot A and B, verify CRC, choose highest valid generation.
2. If neither valid, enter format/rebuild flow.
3. Validate pointers and sequence monotonicity bounds.
4. Probe a small window around `acked_index` and `write_index` to confirm boundary integrity.
5. If mismatch detected, run linear rebuild scan:
   - find longest valid committed run in ring order,
   - reconstruct `acked_seq`, `write_seq`, indices conservatively,
   - never mark unknown records as acked.
6. Persist rebuilt metadata with new generation.

Recovery guarantee:
- Never fabricate records.
- At worst, some tail records may be dropped if partially written at crash boundary.

## 9. Sync and idempotency contract

Node -> mothership includes per record:
- `sample_seq`, `sample_unix_s`, `sensor_id`, `value_f32`, `quality_flags`

Mothership ACK:
- `ack_upto_seq` per node (cumulative)

Idempotency rule:
- Mothership may receive duplicate records due to retries.
- Dedup key is `(node_id, sample_seq)`.
- Duplicate inserts must be ignored or upserted without creating extra rows.

## 10. Time quality contract

`sample_unix_s` source priority:
1. RTC synchronized and valid.
2. RTC unsynced fallback allowed, but set quality flag bit `QF_TIME_UNCERTAIN`.

Suggested quality flags:
- `0x0001` = TIME_UNCERTAIN
- `0x0002` = SENSOR_FAULT
- `0x0004` = VALUE_CLAMPED
- `0x0008` = STORAGE_PRESSURE

## 11. Capacity planning

Formula:

`required_bytes = samples_per_hour * retention_hours * record_size_bytes * 1.3`

Where:
- `samples_per_hour = sensors_per_sample_cycle * (3600 / sample_interval_s)`
- `record_size_bytes = sizeof(SampleRecordV1)`

Example:
- 6 sensors, sample every 60 s
- `samples_per_hour = 360`
- `record_size = 18 bytes`
- 72 h retention target
- `required ~= 360 * 72 * 18 * 1.3 = 606,528 bytes` (~593 KiB)

Recommendation:
- Allocate >= 1.5 MiB partition for queue + metadata headroom for field outages.

## 12. Telemetry surface (required counters)

Expose at node diagnostics endpoint/log:
- `write_seq`
- `acked_seq`
- `pending_records`
- `oldest_pending_age_s`
- `storage_full_events`
- `dropped_unsynced`
- `last_ack_upto_seq`

## 13. Acceptance tests (must pass)

1. Power-loss during append
- Cut power between body write and commit marker.
- After reboot, partially written record is not replayed.

2. ACK replay
- Send same ACK multiple times.
- Queue state remains stable (no pointer regression).

3. Out-of-order ACK
- Apply lower ACK after higher ACK.
- Lower ACK ignored.

4. Extended RF outage
- Fill queue until full under `PROTECT_UNSYNCED`.
- Node stops appending and sets full alarm.
- After reconnect + ACK, appending resumes without corruption.

5. Duplicate batch delivery
- Deliver same batch twice to mothership.
- Exactly one logical copy persists per `(node_id, sample_seq)`.

## 14. Versioning and migration

- `format_version` gates on-disk compatibility.
- Any breaking struct change requires `format_version++` and explicit migration or reformat flow.
- Wire protocol version should be carried alongside batch messages once introduced.

## 15. Recommended v1 defaults

- Full policy: `PROTECT_UNSYNCED`
- ACK mode: cumulative only
- Record type: fixed size (`SampleRecordV1`)
- Metadata: dual-slot A/B with generation + CRC
- Sync ordering: strict ascending contiguous seq
- Dedup key upstream: `(node_id, sample_seq)`

---

This document defines behavior contracts, not specific storage backend APIs. Backend may be raw flash pages, LittleFS file segments, or NVS-backed blobs, as long as all invariants above are preserved.
