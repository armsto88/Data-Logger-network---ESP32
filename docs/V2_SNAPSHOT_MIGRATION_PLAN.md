# V2 Key-Value Snapshot Migration Plan

**Date:** 2026-06-26
**Status:** Planning — tests to be written before implementation
**Scope:** Node firmware + Mothership firmware + Google Sheets

## Problem

The current `node_snapshot_t` is a fixed 124-byte struct with hardcoded fields (airTemp, airHumidity, spectral[8], windSpeed, windDir, soil1Vwc, soil1Temp, soil2Vwc, soil2Temp, aux1, aux2). Adding any new sensor requires changing the struct, which breaks the wire protocol, CSV format, queue storage, and all parsers. Nodes without certain sensors waste space sending NaN values. The roadmap envisions 8 plug-and-play ports with dynamic sensor registration, which the fixed struct cannot support.

## Solution — Key-Value Snapshot (V2)

Replace the fixed struct with a compact key-value format where each sensor reading is a `(sensorId, value)` pair.

### V2 Wire Format

```
Header (48 bytes, packed):
  char     command[16]       "NODE_SNAPSHOT2"
  char     nodeId[16]        e.g. "ENV_6C0AA0"
  uint32_t nodeTimestamp     unix seconds
  uint32_t seqNum            snapshot sequence number
  uint16_t sensorCount       number of KV pairs following
  uint16_t qualityFlags      QF_DROPPED etc.
  uint16_t configVersion     node config version
  uint8_t  protocolVersion   2
  uint8_t  reserved

Body (sensorCount × 6 bytes, packed):
  struct __attribute__((packed)) Reading {
    uint16_t sensorId;        // SENSOR_ID_* constant
    float    value;           // sensor reading
  }
```

Max size: 48 + (33 × 6) = 246 bytes (33-sensor hard limit from ESP-NOW 250-byte max).
Typical size: 48 + (17 × 6) = 150 bytes (current sensor set + battery).

### Backward Compatibility

- V1 packets use command "NODE_SNAPSHOT" (124 bytes)
- V2 packets use command "NODE_SNAPSHOT2" (variable length)
- Mothership detects V1 vs V2 by command string, not by size
- V1 nodes work indefinitely with V2 mothership
- V2 nodes require V2 mothership (documented limitation)
- V1→V2 NVS migration on first V2 boot: convert 124-byte records to V2 wire bytes

## Phases

### Phase 1 — Protocol Definition
**File:** `node/firmware/shared/protocol.h`
- Add V2 struct with `__attribute__((packed))` and `static_assert` on sizes
- Add `snapshotV2WireSize(sensorCount)` helper
- Add `isV2Snapshot()` and `isV1Snapshot()` detection helpers
- Bump `NODE_PROTOCOL_VERSION` to 2
- Keep V1 struct unchanged
- Add `SENSOR_ID_PAR` (1301) — fix PAR resolving to UNKNOWN
- Reserve sensor ID range 5000-5999 for future port-based sensors
- Add `MAX_READINGS_PER_SNAPSHOT = 33` constant

### Phase 2 — Node Firmware
**2a. Sensor registry → key-value pairs**
- Files: `node/firmware/src/sensors/sensors.cpp`, `node/firmware/shared/sensors.h`
- Add `buildReadingsArray()` that iterates `g_sensors[]`, reads each sensor, emits `{sensorId, value}` pairs
- Skip `SENSOR_ID_UNKNOWN` (0)
- Battery voltage appended as `{SENSOR_ID_BAT_V, value}`

**2b. Capture path**
- File: `node/firmware/src/main.cpp` (`captureSensorsToQueue()`)
- Replace V1 struct with V2 header + readings array
- Call `local_queue::enqueueV2()`

**2c. NVS queue — variable-length records**
- Files: `node/firmware/src/storage/local_queue.h`, `local_queue.cpp`
- New `QueueBlobV2` with circular byte slab:
  - Metadata: magic "NQM4", version 5, generation, nextSeq, head (byte offset), tail (byte offset), usedBytes, recordCount
  - Slab: `[uint16_t recordLen][recordBytes]` entries, circular wraparound
  - Capacity: use full ~3900 byte NVS limit (slab ~3500 bytes)
  - A/B slot redundancy preserved (fixed-size blobs padded to max)
  - `static_assert(sizeof(QueueBlobV2) < 4000)`
- API: `enqueueV2()`, `peekV2()`, `pop()`, `count()`
- V1→V2 migration: write to inactive slot first, verify, then swap. Expand group bitmask bits to individual sensor IDs. Idempotent via `migrationVersion` field.
- In-place qualityFlags modification: seek to record header offset in slab, modify 2 bytes

**2d. Transmit path**
- File: `node/firmware/src/main.cpp` (`flushQueuedToMothership()`)
- Replace `peek(snap)` with `peekV2(buf, len)`
- Send variable-length buffer via ESP-NOW

### Phase 3 — Mothership Firmware
**3a. ESP-NOW receive — both V1 and V2**
- Files: `espnow_manager.cpp`, `espnow_config.cpp`
- V2 check FIRST (before V1 size check): read command string, if "NODE_SNAPSHOT2" → validate `len == 48 + 6*sensorCount`, cap sensorCount at 33
- Both V1 and V2 decode to common `DecodedSnapshot`:
  ```
  struct DecodedSnapshot {
    char nodeId[16]; uint32_t nodeTimestamp, seqNum;
    uint16_t qualityFlags, configVersion; uint8_t protocolVersion;
    struct { uint16_t sensorId; float value; } readings[MAX_READINGS_PER_SNAPSHOT];
    size_t readingCount;
  };
  ```
- Fixed-size C array (no heap allocation in callback context)
- `DecodedSnapshot::find(sensorId) -> const float*` helper
- `DecodedSnapshot::hasSensor(sensorId) -> bool` helper

**3b. Dynamic CSV columns — BLOCKER FIX**
- Files: `flash_logger.cpp`, `upload_queue.cpp`, new `csv_format.h`
- **Never rewrite CSV header on existing file with data**
- Strategy: sidecar metadata file `/datalog_cols.json` maps sensor IDs to column positions
- CSV file keeps its original header; new sensor IDs get appended columns on new files only
- `csv_format.h`: `buildCSVHeader(const SensorInventory&)`, `sensorIdToColumnName(uint16_t)`
- SD logger keeps its metadata-rich schema; dynamic sensor columns appended after metadata prefix
- LittleFS logger uses dynamic columns from sidecar metadata

**3c. Upload queue — BLOCKER FIX**
- Files: `upload_queue.cpp`, `upload_queue.h`
- Remove compile-time `kUploadCSVHeader` constant
- Read actual header from `/datalog.csv` first line at init time
- `headerEndOffset()` computed from file, not constant
- `getNewData()` prepends the actual file header, not a constant

**3d. Process and upload**
- File: `main.cpp`
- `processSnapshot()` takes `DecodedSnapshot&`
- `SNAPSHOT_ACK` echoes received protocolVersion (not hardcoded 1)

### Phase 4 — Google Sheets
- Update Apps Script `doPost()` to parse by column name (not position)
- New sensor IDs → new columns automatically
- Document expected CSV format

### Phase 5 — Migration & Rollout
1. Mothership first (handles both V1 and V2)
2. One test node with V2 firmware
3. Gradual node rollout
4. No forced cutover — V1 nodes work indefinitely

### Test matrix
| Node FW | Mothership FW | Result |
|---------|---------------|--------|
| V1 | V1 | Current behavior (baseline) |
| V1 | V2 | V1 snapshot → decoded → dynamic CSV |
| V2 | V2 | V2 snapshot → decoded → dynamic CSV |
| V2 | V1 | Not supported (documented) |

## Blockers Fixed (from review)

1. **Struct packing** — V2 body entry must be `__attribute__((packed))` with `static_assert(sizeof(ReadingEntry) == 6)`
2. **CSV header rewrite** — Never rewrite header on existing file. Use sidecar metadata file for column mapping.
3. **Upload queue cursor** — Read actual header from file at init time, not compile-time constant.

## Test-First Strategy

6 test files to write BEFORE implementation:

| # | Test file | Scope | What it validates |
|---|---|---|---|
| 1 | `test_protocol_v2.cpp` | Node | Struct sizes, field offsets, packing, wire size calculation |
| 2 | `test_queue_migration.cpp` | Node | V1→V2 NVS migration: record count, seqNum, field mapping, bitmask expansion |
| 3 | `test_queue_v2.cpp` | Node | Circular slab: enqueue, peek, pop, wraparound, drop-oldest, A/B recovery, in-place QF modify |
| 4 | `test_espnow_dispatch.cpp` | Mothership | V1/V2 detection, corrupted packet rejection, sensorCount limits, DecodedSnapshot |
| 5 | `test_csv_format.cpp` | Mothership | Dynamic column generation, sensor ID → column name, sidecar metadata |
| 6 | `test_upload_queue.cpp` | Mothership | Cursor validation with dynamic headers, getNewData with actual file header |

## Open Questions

1. SD vs LittleFS CSV schema — keep separate, share sensor column suffix builder
2. V2 node + V1 mothership — unsupported, document clearly
3. configVersion at capture vs flush time — should be set at capture time in V2
4. No rollback plan — V1→V2 NVS migration is one-way. Consider keeping V1 fallback.