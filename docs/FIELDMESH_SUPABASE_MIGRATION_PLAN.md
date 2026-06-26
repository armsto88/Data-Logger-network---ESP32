# fieldMesh — Supabase Migration Plan

**Date:** 2026-06-26
**Status:** Design document (planning only — no code changes)
**Project:** fieldMesh — ESP32 environmental sensor network
**Companion to:** `docs/FIELDMESH_DASHBOARD_DESIGN.md`, `docs/FIELDMESH_ABOUT_SECTION_DESIGN.md`
**Companion docs:** `docs/FIELDMESH_DASHBOARD_DESIGN.md`, `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` (JSON upload protocol spec from frontend team)

---

## 1. Executive Summary

### 1.1 Why migrate from Google Sheets to Supabase

The current fieldMesh backend is Google Sheets, written to via Google Apps Script `doPost()`. This works for a showcase but has hard limits for a production environmental monitoring system:

- **No query capability** — Apps Script reads the entire sheet range into memory and filters in JavaScript. `getAllLatest` scans every row to find the last reading per node. This does not scale past a few thousand rows.
- **CSV text overhead** — each snapshot row is ~150 bytes of ASCII text. A V2 snapshot with 5 readings is 78 bytes on the wire (48-byte header + 5×6-byte readings) but becomes ~150 bytes as CSV — nearly 2× expansion.
- **appendRow slowness** — Sheets `appendRow` / `getValues` round-trips are slow (seconds per call) and rate-limited (20,000 reads/day on Apps Script quotas).
- **No real-time updates** — the dashboard polls every 60s. There is no push mechanism.
- **No row-level security** — anyone with the Apps Script URL can write. The `?token=xxx` query-param auth is weak.
- **No schema enforcement** — Sheets accepts any text. Bad rows silently corrupt downstream analysis.

Supabase (hosted PostgreSQL + auto-generated REST + real-time + storage) solves all of these: SQL queries, binary ingestion via Edge Functions, real-time subscriptions, RLS, and a typed schema.

### 1.2 What changes

| Layer | Change |
|---|---|
| **Firmware (mothership)** | New upload path: send raw V2 binary snapshots instead of CSV. New content-type `application/octet-stream`. New endpoint URL + API key. CSV path kept as fallback. |
| **Backend** | Google Apps Script → Supabase Edge Function (Deno/TypeScript) that decodes V2 binary and batch-inserts into PostgreSQL. |
| **Dashboard** | Google Apps Script `doGet` endpoints → Supabase JS client + real-time subscriptions. |

### 1.3 What stays the same

- **ESP-NOW** node-to-mothership radio path (channel 11, V2 snapshot protocol).
- **V2 binary protocol** — `node_snapshot_v2_t` (48-byte header) + `v2_reading_t` (6-byte packed). No wire-format change.
- **A7670G LTE modem hardware** — same SIM, same SSL/TLS CCH* API, same chunked CCHSEND.
- **LittleFS local logging** — `/datalog.csv` remains the durable local record; binary upload is an additional egress path.
- **Node firmware** — untouched. Nodes already produce V2 snapshots.

---

## 2. Current Architecture (Google Sheets)

### 2.1 Data flow

```
Sensor Node (ESP32 + RTC + sensors)
    ↓ ESP-NOW (WiFi ch11) — node_snapshot_v2_t (48B) + N×v2_reading_t (6B each)
Mothership (ESP32 + A7670G + LittleFS)
    ↓ decodeV2() → DecodedSnapshot → logDecodedSnapshot() → /datalog.csv (CSV text)
    ↓ UploadQueue::getNewData() reads CSV rows from cursor → String payload
    ↓ ModemDriver::httpsPost() — SSL/TLS via CCH* API, chunked CCHSEND (1024B)
Google Apps Script (doPost → appendSensorData)
    ↓
Google Sheet (single "Data" sheet, 25 columns)
    ↓ doGet?action=getData / getAllLatest (full-range scan + JS filter)
fieldMesh Dashboard (React + Vite, polls every 60s)
```

### 2.2 CSV payload format

The CSV header (from `upload_queue.h` / `flash_logger.cpp`):

```
datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,
batVoltage,airTemp,airHumidity,
spectral_415,spectral_445,spectral_480,spectral_515,
spectral_555,spectral_590,spectral_630,spectral_680,
windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2
```

25 columns. A typical row (5-min interval, all sensors present):

```
2026-06-26T10:59:46,ENV_6C0AA0,1,0x0137,0x0000,1,4.120,28.050,48.880,1.000,3.000,2.000,4.000,9.000,9.000,6.000,10.000,3.500,180.000,0.120,27.780,2.640,27.920,nan,nan
```

**~150 bytes per row** (including comma separators and CRLF).

### 2.3 Limitations

| Limitation | Impact |
|---|---|
| CSV text overhead | ~150 bytes/row vs ~78 bytes binary (V2 header + 5 readings). 1.9× bandwidth waste on LTE. |
| `appendRow` slowness | Apps Script `appendSensorData` writes one row at a time. 30 nodes × 5 sensors × 12 reads/hour = 1,800 rows/hour. |
| No query capability | `getAllLatest` does `sheet.getDataRange().getValues()` then loops all rows in JS. O(n) over the entire sheet. |
| Apps Script quotas | 20,000 reads/day, 6 min execution limit, 20 MB/day URL-fetch. A busy day of dashboard polling can hit the read quota. |
| No real-time | Dashboard polls every 60s. No push. |
| No schema enforcement | A malformed CSV row (missing column, text in a float field) is silently stored. |
| No scaling | A single Google Sheet gets slow past ~100k rows. 30 nodes at 5-min intervals produce ~1.05M rows/year. |
| Weak auth | `?token=xxx` in the URL query string. Token visible in modem AT logs and Apps Script deployment history. |

### 2.4 Current upload settings

From `transmission_settings.h`:

```cpp
struct TransmissionSettings {
  bool     enabled;
  String   endpointUrl;        // Google Cloud Function URL
  String   authToken;          // token appended as ?token=xxx
  String   siteId;
  String   deploymentId;
  uint16_t uploadIntervalMin;
  uint32_t uploadPhaseUnix;
  uint16_t minBatteryMv;       // default 3700
  uint32_t maxBytesPerSession; // default 98304 (96 KB)
  uint8_t  maxRetriesPerWindow;// default 3
  bool     allowManualUpload;  // default true
};
```

The upload URL is built as `endpointUrl + ?token=xxx&siteId=yyy&deploymentId=zzz`. The modem sends `Authorization: Bearer <authToken>` and `Content-Type: text/plain`.

---

## 3. Target Architecture (Supabase)

### 3.1 Data flow

```
Sensor Node (ESP32 + RTC + sensors)
    ↓ ESP-NOW — node_snapshot_v2_t (48B) + N×v2_reading_t (6B each)  [UNCHANGED]
Mothership (ESP32 + A7670G + LittleFS)
    ↓ Store raw V2 snapshots in LittleFS (/snapshots.bin) alongside /datalog.csv
    ↓ UploadQueue::getNewDataBinary() packs snapshots into a batch with a small header
    ↓ ModemDriver::httpsPost() — SSL/TLS via CCH* API, chunked CCHSEND  [SAME MODEM]
    ↓ Content-Type: application/octet-stream
    ↓ Authorization: Bearer <supabase_anon_or_service_key>
Supabase Edge Function (Deno/TypeScript)
    ↓ Decode batch header + V2 snapshots
    ↓ Batch INSERT into readings + nodes (upsert)
PostgreSQL (Supabase)
    ↓ Auto-generated REST API (PostgREST) + real-time subscriptions
fieldMesh Dashboard (React + Vite + @supabase/supabase-js)
    ↓ Real-time subscriptions on `readings` table
    ↓ SQL queries via Supabase client
```

### 3.2 Benefits

| Benefit | Detail |
|---|---|
| Binary payload | ~78 bytes/snapshot (48B header + 5×6B readings) vs ~150 bytes CSV. **~1.9× smaller**. On LTE, this halves upload airtime and cost. |
| SQL queries | `SELECT DISTINCT ON (node_id) ... ORDER BY node_id, timestamp DESC` for latest-per-node — indexed, milliseconds. |
| Real-time | Supabase real-time broadcasts row inserts on `readings`. Dashboard updates instantly without polling. |
| Row-level security | RLS policies: mothership API key can INSERT only; dashboard anon key can SELECT only. |
| Auto REST API | PostgREST exposes `readings`, `nodes`, `sync_sessions` as REST endpoints automatically. No hand-written API layer. |
| Schema enforcement | PostgreSQL columns are typed. A bad float is rejected at insert. |
| Scaling | PostgreSQL handles millions of rows with proper indexes. Supabase Pro tier ($25/mo) gives 8GB DB. |

### 3.3 Payload size comparison

| Format | 1 snapshot (5 readings) | 30 nodes × 12 reads/hr | 24h total |
|---|---|---|---|
| CSV (current) | ~150 bytes | 30 × 12 × 150 = 54,000 B/hr | ~1.30 MB/day |
| V2 binary (target) | 48 + 5×6 = 78 bytes | 30 × 12 × 78 = 28,080 B/hr | ~674 KB/day |
| **Savings** | **~48% smaller** | | **~48% less LTE data** |

With a batch header (8 bytes per batch, see §6.3), a 96 KB upload session carries ~1,230 binary snapshots vs ~640 CSV rows.

---

## 4. Supabase Setup Guide

### 4.1 Account and project setup

1. Create an account at **supabase.com**.
2. Create a new project. Choose a region close to the deployment (e.g. `ap-southeast-2` for Australia, `eu-central-1` for Europe).
3. Note the project URL: `https://<project-ref>.supabase.co`.
4. Note two keys from **Settings → API**:
   - `anon` (public) key — safe to embed in the dashboard frontend.
   - `service_role` key — secret, used only by the Edge Function and never on the mothership.
5. Create a dedicated **mothership API key** — see §11. The mothership uses a custom auth token validated by the Edge Function, not the raw service_role key.

### 4.2 Database schema design (overview)

Four core tables:

| Table | Purpose | Key columns |
|---|---|---|
| `deployments` | A field deployment (site + fleet of nodes) | `id`, `site_id`, `name`, `description`, `created_at` |
| `nodes` | One row per sensor node, upserted on first sighting | `node_id`, `deployment_id`, `paired_at`, `last_seen`, `battery_mv`, `firmware_version` |
| `readings` | One row per sensor reading (long format) | `id`, `node_id`, `seq_num`, `timestamp`, `sensor_id`, `sensor_name`, `value`, `unit`, `quality_flags` |
| `sync_sessions` | One row per mothership upload session | `id`, `mothership_id`, `started_at`, `completed_at`, `nodes_heard`, `rows_uploaded`, `bytes_uploaded` |

**Design choice — long format for `readings`:** The V2 protocol is key-value (sensorId + value). A long/narrow table (one row per reading) matches the wire format exactly and handles new sensor IDs without schema migration. The dashboard pivots in SQL or in JS. *[Confirmed decision]*

### 4.3 Row Level Security (RLS)

- `readings`: **INSERT** only for the mothership role (authenticated via Edge Function). **SELECT** for the anon role (dashboard).
- `nodes`: **INSERT/UPDATE** for mothership role. **SELECT** for anon.
- `sync_sessions`: **INSERT** for mothership. **SELECT** for anon.
- `deployments`: **SELECT** for anon. Writes are manual (via Supabase dashboard or a separate admin role).

### 4.4 API key authentication

The mothership does **not** use the Supabase `service_role` key directly (it is too powerful and cannot be safely rotated per-device). Instead:

- The Edge Function accepts a custom `Authorization: Bearer <MOTHERSHIP_TOKEN>` header.
- The token is validated against a `mothership_tokens` table (or a hardcoded allow-list in the Edge Function env for a single-mothership deployment).
- The Edge Function then uses the `service_role` key internally to insert rows.

### 4.5 Storage buckets

| Bucket | Purpose | Access |
|---|---|---|
| `firmware-ota` | Node firmware binaries for future OTA | Private; mothership reads via signed URL. |
| `config-files` | Node config snapshots (JSON) | Private; mothership reads. |
| `exports` | CSV exports generated on demand for the dashboard | Private; dashboard generates signed URLs. |

Storage buckets are not required for Phase 1–3 but are noted here for completeness.

---

## 5. Database Schema (detailed SQL DDL)

Run this in the Supabase SQL editor.

```sql
-- =========================================================================
-- fieldMesh Supabase schema
-- =========================================================================

-- ---------------------------------------------------------------------------
-- Enum: sensor_id_enum
-- Maps to the V2 protocol SENSOR_ID_* constants in node/firmware/v2/shared/protocol.h
-- ---------------------------------------------------------------------------
DO $$ BEGIN
  CREATE TYPE sensor_id_enum AS ENUM (
    '1001',  -- AIR_TEMP
    '1002',  -- AIR_RH
    '1101',  -- SPECTRAL_415
    '1102',  -- SPECTRAL_445
    '1103',  -- SPECTRAL_480
    '1104',  -- SPECTRAL_515
    '1105',  -- SPECTRAL_555
    '1106',  -- SPECTRAL_590
    '1107',  -- SPECTRAL_630
    '1108',  -- SPECTRAL_680
    '1201',  -- WIND_SPEED
    '1202',  -- WIND_DIR
    '1301',  -- PAR
    '2001',  -- SOIL1_VWC
    '2002',  -- SOIL2_VWC
    '2003',  -- SOIL1_TEMP
    '2004',  -- SOIL2_TEMP
    '3001',  -- AUX1
    '3002',  -- AUX2
    '4001'   -- BAT_V
  );
EXCEPTION WHEN duplicate_object THEN NULL; END $$;

-- ---------------------------------------------------------------------------
-- Table: deployments
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS deployments (
  id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  site_id     TEXT NOT NULL,
  name        TEXT NOT NULL,
  description TEXT,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ---------------------------------------------------------------------------
-- Table: nodes
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nodes (
  node_id          TEXT PRIMARY KEY,            -- e.g. "ENV_6C0AA0"
  deployment_id    UUID REFERENCES deployments(id) ON DELETE SET NULL,
  site_id          TEXT,
  paired_at        TIMESTAMPTZ,
  last_seen        TIMESTAMPTZ,
  battery_mv       INTEGER,                     -- last reported battery in mV
  firmware_version TEXT,
  config_version   INTEGER,
  protocol_version INTEGER,
  sensor_present   INTEGER,                     -- bitmask from last snapshot
  created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
  updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ---------------------------------------------------------------------------
-- Table: readings  (long format — one row per sensor reading)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS readings (
  id             BIGSERIAL PRIMARY KEY,
  node_id        TEXT NOT NULL REFERENCES nodes(node_id) ON DELETE CASCADE,
  seq_num        INTEGER NOT NULL,              -- snapshot sequence number
  timestamp      TIMESTAMPTZ NOT NULL,          -- node RTC unix at capture
  sensor_id      sensor_id_enum NOT NULL,
  sensor_name    TEXT NOT NULL,                 -- human-readable, e.g. "air_temp"
  value          DOUBLE PRECISION,              -- NULL if NaN
  unit           TEXT,                          -- e.g. "°C", "%", "m/s"
  quality_flags  INTEGER NOT NULL DEFAULT 0,
  config_version INTEGER,
  received_at    TIMESTAMPTZ NOT NULL DEFAULT now(),  -- server ingest time
  sync_session_id UUID REFERENCES sync_sessions(id) ON DELETE SET NULL
);

-- Indexes for common dashboard query patterns
CREATE INDEX IF NOT EXISTS idx_readings_node_ts
  ON readings (node_id, timestamp DESC);

CREATE INDEX IF NOT EXISTS idx_readings_sensor_ts
  ON readings (sensor_id, timestamp DESC);

CREATE INDEX IF NOT EXISTS idx_readings_node_sensor_ts
  ON readings (node_id, sensor_id, timestamp DESC);

CREATE INDEX IF NOT EXISTS idx_readings_seq
  ON readings (node_id, seq_num DESC);

-- ---------------------------------------------------------------------------
-- Table: sync_sessions
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sync_sessions (
  id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  mothership_id   TEXT NOT NULL,
  started_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
  completed_at    TIMESTAMPTZ,
  nodes_heard     INTEGER NOT NULL DEFAULT 0,
  rows_uploaded   INTEGER NOT NULL DEFAULT 0,
  bytes_uploaded  INTEGER NOT NULL DEFAULT 0,
  status          TEXT NOT NULL DEFAULT 'in_progress'  -- in_progress|ok|error
);

-- ---------------------------------------------------------------------------
-- Table: mothership_tokens (auth for the Edge Function)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mothership_tokens (
  token          TEXT PRIMARY KEY,
  mothership_id  TEXT NOT NULL,
  deployment_id  UUID REFERENCES deployments(id),
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  revoked_at     TIMESTAMPTZ
);

-- ---------------------------------------------------------------------------
-- Table: mothership_status (system status from JSON upload)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mothership_status (
  id BIGSERIAL PRIMARY KEY,
  device_id TEXT NOT NULL,
  reported_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  rtc_unix BIGINT,
  wake_interval_minutes INT,
  sync_interval_minutes INT,
  sync_mode TEXT,
  sync_daily_time TEXT,
  next_sync_local TEXT,
  fleet_total INT,
  fleet_deployed INT,
  fleet_paired INT,
  fleet_unpaired INT,
  upload_enabled BOOLEAN,
  pending_bytes BIGINT,
  pending_rows BIGINT,
  rows_uploaded BIGINT,
  last_upload_unix BIGINT,
  retry_count INT,
  flash_usage_pct INT,
  flash_total_bytes BIGINT,
  flash_used_bytes BIGINT,
  firmware_version TEXT,
  firmware_build TEXT
);

-- ---------------------------------------------------------------------------
-- Table: mothership_config (transmission settings from JSON upload)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mothership_config (
  id BIGSERIAL PRIMARY KEY,
  device_id TEXT NOT NULL,
  reported_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  endpoint_url TEXT,
  site_id TEXT,
  deployment_id TEXT,
  upload_interval_min INT,
  min_battery_mv INT,
  max_bytes_per_session BIGINT,
  max_retries_per_window INT,
  allow_manual_upload BOOLEAN
);

-- ---------------------------------------------------------------------------
-- Trigger: update nodes.last_seen on readings insert
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION update_node_last_seen()
RETURNS TRIGGER AS $$
BEGIN
  UPDATE nodes
     SET last_seen       = NEW.timestamp,
         battery_mv      = CASE WHEN NEW.sensor_id = '4001'
                                THEN CAST(NEW.value * 1000 AS INTEGER)
                                ELSE battery_mv END,
         config_version  = COALESCE(NEW.config_version, config_version),
         updated_at      = now()
   WHERE node_id = NEW.node_id;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_readings_update_node ON readings;
CREATE TRIGGER trg_readings_update_node
  AFTER INSERT ON readings
  FOR EACH ROW
  EXECUTE FUNCTION update_node_last_seen();

-- ---------------------------------------------------------------------------
-- Trigger: auto-upsert nodes on first reading (creates node row if missing)
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION upsert_node_on_reading()
RETURNS TRIGGER AS $$
BEGIN
  INSERT INTO nodes (node_id, last_seen, created_at, updated_at)
  VALUES (NEW.node_id, NEW.timestamp, now(), now())
  ON CONFLICT (node_id) DO NOTHING;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_readings_upsert_node ON readings;
CREATE TRIGGER trg_readings_upsert_node
  BEFORE INSERT ON readings
  FOR EACH ROW
  EXECUTE FUNCTION upsert_node_on_reading();

-- ---------------------------------------------------------------------------
-- Row Level Security
-- ---------------------------------------------------------------------------
ALTER TABLE readings       ENABLE ROW LEVEL SECURITY;
ALTER TABLE nodes          ENABLE ROW LEVEL SECURITY;
ALTER TABLE sync_sessions  ENABLE ROW LEVEL SECURITY;
ALTER TABLE deployments    ENABLE ROW LEVEL SECURITY;
ALTER TABLE mothership_tokens ENABLE ROW LEVEL SECURITY;

-- Dashboard (anon key) can read all data tables
CREATE POLICY "anon_read_readings"  ON readings  FOR SELECT TO anon USING (true);
CREATE POLICY "anon_read_nodes"     ON nodes     FOR SELECT TO anon USING (true);
CREATE POLICY "anon_read_sessions"  ON sync_sessions FOR SELECT TO anon USING (true);
CREATE POLICY "anon_read_deploys"   ON deployments FOR SELECT TO anon USING (true);

-- The Edge Function uses the service_role key, which bypasses RLS.
-- Mothership never connects to Postgres directly — it goes through the
-- Edge Function, which validates the bearer token against mothership_tokens.

-- Lock down mothership_tokens so anon cannot read it
CREATE POLICY "no_anon_tokens" ON mothership_tokens FOR ALL TO anon
  USING (false) WITH CHECK (false);
```

### 5.1 Sensor ID → name/unit mapping

The Edge Function and dashboard both need this mapping. It mirrors `SENSOR_ID_*` in `protocol.h`:

| sensor_id | sensor_name | unit | V2 constant |
|---|---|---|---|
| 1001 | air_temp | °C | `SENSOR_ID_AIR_TEMP` |
| 1002 | air_humidity | % | `SENSOR_ID_AIR_RH` |
| 1101 | spectral_415 | counts | `SENSOR_ID_SPECTRAL_415` |
| 1102 | spectral_445 | counts | `SENSOR_ID_SPECTRAL_445` |
| 1103 | spectral_480 | counts | `SENSOR_ID_SPECTRAL_480` |
| 1104 | spectral_515 | counts | `SENSOR_ID_SPECTRAL_515` |
| 1105 | spectral_555 | counts | `SENSOR_ID_SPECTRAL_555` |
| 1106 | spectral_590 | counts | `SENSOR_ID_SPECTRAL_590` |
| 1107 | spectral_630 | counts | `SENSOR_ID_SPECTRAL_630` |
| 1108 | spectral_680 | counts | `SENSOR_ID_SPECTRAL_680` |
| 1201 | wind_speed | m/s | `SENSOR_ID_WIND_SPEED` |
| 1202 | wind_dir | degrees | `SENSOR_ID_WIND_DIR` |
| 1301 | par | µmol/m²/s | `SENSOR_ID_PAR` |
| 2001 | soil1_vwc | m³/m³ | `SENSOR_ID_SOIL1_VWC` |
| 2002 | soil2_vwc | m³/m³ | `SENSOR_ID_SOIL2_VWC` |
| 2003 | soil1_temp | °C | `SENSOR_ID_SOIL1_TEMP` |
| 2004 | soil2_temp | °C | `SENSOR_ID_SOIL2_TEMP` |
| 3001 | aux1 | varies | `SENSOR_ID_AUX1` |
| 3002 | aux2 | varies | `SENSOR_ID_AUX2` |
| 4001 | bat_voltage | V | `SENSOR_ID_BAT_V` |

---

## 6. Firmware Changes

This section describes what `Coder` should change. **No code is written here** — this is a design spec.

### 6.1 New content type and endpoint

**Current** (`transmission_settings.h`):
- `endpointUrl` = Google Cloud Function URL
- `authToken` = `?token=xxx` query param
- Content-Type: `text/plain` (CSV)

**Target**:
- `endpointUrl` = `https://<project-ref>.supabase.co/functions/v1/ingest-fieldmesh`
- `authToken` = Supabase mothership bearer token (stored in NVS, see §11)
- Content-Type: `application/octet-stream`
- `siteId` and `deploymentId` move into the binary batch header (§6.3) rather than the URL query string.

### 6.2 TransmissionSettings additions

`Coder` should extend `TransmissionSettings` in `transmission_settings.h`:

```cpp
struct TransmissionSettings {
  // ... existing fields ...
  bool     useBinaryUpload;   // true = V2 binary, false = CSV (fallback)
  String   binaryEndpointUrl; // Supabase Edge Function URL
  String   binaryAuthToken;   // mothership bearer token (NVS-stored)
};
```

Defaults: `useBinaryUpload = true`, `binaryEndpointUrl` empty (set via web UI), `binaryAuthToken` empty (set via web UI, stored in NVS namespace `tx`).

### 6.3 Batch binary format

Multiple V2 snapshots are concatenated into a single upload body with a small batch header. This lets the mothership send a full 96 KB session in one HTTPS POST.

**Batch header (16 bytes, little-endian):**

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `0x464D4258` ("FMBX" — fieldMesh batch, binary) |
| 4 | 1 | version | `1` |
| 5 | 1 | reserved | `0` |
| 6 | 2 | snapshotCount | number of V2 snapshots in this batch |
| 8 | 4 | mothershipTimestamp | mothership RTC unix at upload time |
| 12 | 4 | reserved | `0` (future: CRC32) |

**Body:** `snapshotCount` × raw `node_snapshot_v2_t` (48B header + `sensorCount`×6B readings), back-to-back.

Each snapshot is self-describing: the `sensorCount` field in the V2 header tells the decoder how many 6-byte readings follow. The Edge Function walks the buffer snapshot by snapshot.

**Example batch (3 snapshots, 5 readings each):**
- Header: 16 bytes
- Snapshot 1: 48 + 30 = 78 bytes
- Snapshot 2: 48 + 30 = 78 bytes
- Snapshot 3: 48 + 30 = 78 bytes
- **Total: 250 bytes** for 15 sensor readings from 3 nodes.

### 6.4 Changes to `upload_queue.cpp` / `upload_queue.h`

`Coder` should add a new method alongside the existing `getNewData()`:

```cpp
struct BinaryUploadPayload {
  uint8_t* data;         // heap-allocated batch buffer
  uint32_t byteLength;   // total batch size (header + snapshots)
  uint32_t snapshotCount;
  uint32_t startCursor;  // LittleFS byte offset where this batch starts
  uint32_t endCursor;    // LittleFS byte offset after the last snapshot
};

BinaryUploadPayload UploadQueue::getNewDataBinary(uint32_t maxBytes);
```

**Implementation approach:**

1. The mothership must store raw V2 snapshots to LittleFS in addition to (or instead of) CSV. `Coder` should add a binary log file `/snapshots.bin` that appends raw V2 wire bytes as they arrive from ESP-NOW (in the ESP-NOW receive callback, before/after `logDecodedSnapshot()`).
2. `getNewDataBinary()` reads from `/snapshots.bin` starting at a separate binary cursor (persisted in NVS key `bin_cursor_offset`).
3. It packs snapshots into a `uint8_t` buffer up to `maxBytes`, prepends the 16-byte batch header, and returns the buffer.
4. `advanceCursorBinary(uint32_t newOffset)` advances the binary cursor after a successful upload.
5. `purgeUploadedBinary()` trims `/snapshots.bin` analogous to the CSV purge.

**Heap constraint:** The existing `getNewData()` already clamps to available heap (`freeHeap - 8192`). The binary path should do the same — a 96 KB binary buffer is fine on ESP32 (320 KB heap) but must be checked.

### 6.5 Changes to `modem_driver.cpp`

The existing `httpsPost()` already accepts `contentType` and `authToken` as parameters. **No structural change needed.** The caller passes:
- `contentType = "application/octet-stream"`
- `authToken = binaryAuthToken` (sent as `Authorization: Bearer ...`)
- `payload` = the binary batch buffer (as a `String` — see note below)

**Note on `String` vs binary:** `ModemDriver::httpsPost()` takes `const String& payload`. `String` can hold arbitrary bytes including nulls **if** constructed via `String((const char*)buf, len)` or `concat()`. However, `String::length()` uses `strlen` internally on some Arduino cores, which would truncate at the first `0x00`. `Coder` must verify that the ESP32 Arduino `String` correctly handles embedded nulls, or refactor `httpsPost()` to accept `const uint8_t* + size_t`. The chunked CCHSEND path (`Serial2.write(data.c_str() + offset, thisChunk)`) already writes raw bytes, so the only risk is `String::length()` misreporting. *[Open question — see §12]*

### 6.6 Backward compatibility — CSV fallback

- `useBinaryUpload` flag in `TransmissionSettings` controls which path runs.
- If the binary endpoint returns HTTP 5xx or the modem fails SSL, the firmware can retry with the CSV path (`useBinaryUpload = false`) as a fallback.
- The CSV path (`getNewData()`, `text/plain`, Google Cloud Function URL) remains intact until Phase 6.
- Both cursors (CSV and binary) are independent in NVS.

### 6.7 JSON Upload Protocol Integration

The frontend team has specified a structured JSON upload format (see `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`) that carries not just sensor readings but also system status, node metadata, and config data. This replaces the raw CSV upload and gives the dashboard full visibility into the field system.

**Payload structure:**

```json
{
  "meta": { ... },       // Mothership identity + upload metadata
  "status": { ... },     // System status (fleet, upload queue, flash, config, nodes)
  "readings": [ ... ]    // Sensor data (batch of snapshots)
}
```

**Integration with Supabase:**

- The JSON payload is sent to the Supabase Edge Function instead of Google Apps Script.
- Content-Type: `application/json` (not `application/octet-stream`).
- The Edge Function parses the JSON and distributes data across tables:
  - `meta` → logged in `sync_sessions` table
  - `status.fleet` → upserted in `mothership_status` table
  - `status.upload` → upserted in `mothership_status` table
  - `status.transmission` → upserted in `mothership_config` table
  - `status.nodes[]` → upserted in `nodes` table
  - `readings[]` → inserted in `readings` table

**Two payload formats supported:**

1. **JSON format** (from frontend spec) — `application/json`, carries status + readings. Use for Google Sheets interim and Supabase.
2. **Binary V2 format** (from §6.3) — `application/octet-stream`, carries readings only. Use for maximum efficiency when status isn't needed.

The Edge Function accepts both formats:

- `Content-Type: application/json` → parse JSON, distribute across tables
- `Content-Type: application/octet-stream` → decode V2 binary batch, insert readings only

**Firmware implementation priority:**

1. First: implement JSON format (gives dashboard full visibility immediately).
2. Later: add binary format as an optimization for large payloads.

**ArduinoJson dependency:**

The JSON payload should be built with ArduinoJson (available in PlatformIO). The mothership has ~236KB free heap after modem init — a 100-reading JSON payload (~24KB) fits comfortably. Use a static `StaticJsonDocument` or `JsonDocument` with `serializeJson()` to build the payload string, then send via the existing chunked CCHSEND path.

**Data sources (all already available in firmware):**

See §8 of `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` for the complete mapping of JSON fields to firmware variables.

---

## 7. Cloud Function / Edge Function

Supabase Edge Functions run Deno (TypeScript/JavaScript) and are invoked at `https://<project-ref>.supabase.co/functions/v1/<name>`.

### 7.1 Edge Function: `ingest-fieldmesh`

This function receives the binary batch, validates the mothership bearer token, decodes each V2 snapshot, and batch-inserts readings into PostgreSQL.

> **Dual-format support:** The Edge Function should accept both `application/json` (from the frontend protocol spec, `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`) and `application/octet-stream` (binary V2 batch from §6.3). The JSON format is the **primary implementation** — it carries system status, node metadata, and config data alongside readings, giving the dashboard full visibility. The binary V2 format is a later optimization for large reading-only payloads. See §6.7 for the integration plan.

```typescript
// supabase/functions/ingest-fieldmesh/index.ts
// Receives a fieldMesh binary batch from the mothership LTE modem.
// Batch format: 16-byte header + N × node_snapshot_v2_t (48B + sensorCount×6B)
//
// Wire structs (must match node/firmware/v2/shared/protocol.h):
//   node_snapshot_v2_t: 48 bytes, packed, little-endian
//     command[16] char, nodeId[16] char, nodeTimestamp u32, seqNum u32,
//     sensorCount u16, qualityFlags u16, configVersion u16,
//     protocolVersion u8, reserved u8
//   v2_reading_t: 6 bytes, packed
//     sensorId u16, value f32 (little-endian IEEE 754)

import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const SENSOR_MAP: Record<number, { name: string; unit: string }> = {
  1001: { name: "air_temp",      unit: "°C" },
  1002: { name: "air_humidity",  unit: "%" },
  1101: { name: "spectral_415",  unit: "counts" },
  1102: { name: "spectral_445",  unit: "counts" },
  1103: { name: "spectral_480",  unit: "counts" },
  1104: { name: "spectral_515",  unit: "counts" },
  1105: { name: "spectral_555",  unit: "counts" },
  1106: { name: "spectral_590",  unit: "counts" },
  1107: { name: "spectral_630",  unit: "counts" },
  1108: { name: "spectral_680",  unit: "counts" },
  1201: { name: "wind_speed",    unit: "m/s" },
  1202: { name: "wind_dir",      unit: "degrees" },
  1301: { name: "par",           unit: "µmol/m²/s" },
  2001: { name: "soil1_vwc",     unit: "m³/m³" },
  2002: { name: "soil2_vwc",     unit: "m³/m³" },
  2003: { name: "soil1_temp",    unit: "°C" },
  2004: { name: "soil2_temp",    unit: "°C" },
  3001: { name: "aux1",          unit: "" },
  3002: { name: "aux2",          unit: "" },
  4001: { name: "bat_voltage",   unit: "V" },
};

const BATCH_MAGIC = 0x464D4258; // "FMBX"
const HEADER_SIZE = 48;         // node_snapshot_v2_t
const READING_SIZE = 6;         // v2_reading_t
const MAX_READINGS = 33;

// Little-endian readers
const u16 = (d: Uint8Array, o: number) => d[o] | (d[o + 1] << 8);
const u32 = (d: Uint8Array, o: number) =>
  (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
const f32 = (d: Uint8Array, o: number) => {
  const buf = new ArrayBuffer(4);
  const view = new DataView(buf);
  view.setUint8(0, d[o]);
  view.setUint8(1, d[o + 1]);
  view.setUint8(2, d[o + 2]);
  view.setUint8(3, d[o + 3]);
  return view.getFloat32(0, true); // little-endian
};

function decodeString(d: Uint8Array, offset: number, len: number): string {
  // V2 nodeId/command are fixed-size char arrays, null-padded.
  let s = "";
  for (let i = 0; i < len; i++) {
    if (d[offset + i] === 0) break;
    s += String.fromCharCode(d[offset + i]);
  }
  return s;
}

interface DecodedSnapshot {
  nodeId: string;
  nodeTimestamp: number; // unix seconds
  seqNum: number;
  qualityFlags: number;
  configVersion: number;
  protocolVersion: number;
  readings: { sensorId: number; value: number }[];
}

function decodeSnapshot(d: Uint8Array, offset: number): DecodedSnapshot | null {
  if (offset + HEADER_SIZE > d.length) return null;
  const command = decodeString(d, offset, 16);
  if (command !== "NODE_SNAPSHOT2") return null;

  const nodeId = decodeString(d, offset + 16, 16);
  const nodeTimestamp = u32(d, offset + 32);
  const seqNum = u32(d, offset + 36);
  const sensorCount = u16(d, offset + 40);
  const qualityFlags = u16(d, offset + 42);
  const configVersion = u16(d, offset + 44);
  const protocolVersion = d[offset + 46];

  if (sensorCount > MAX_READINGS) return null;
  const bodyLen = sensorCount * READING_SIZE;
  if (offset + HEADER_SIZE + bodyLen > d.length) return null;

  const readings: { sensorId: number; value: number }[] = [];
  for (let i = 0; i < sensorCount; i++) {
    const ro = offset + HEADER_SIZE + i * READING_SIZE;
    const sensorId = u16(d, ro);
    const value = f32(d, ro + 2);
    readings.push({ sensorId, value });
  }

  return {
    nodeId, nodeTimestamp, seqNum, qualityFlags,
    configVersion, protocolVersion, readings,
  };
}

Deno.serve(async (req: Request) => {
  if (req.method !== "POST") {
    return new Response(JSON.stringify({ error: "method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    });
  }

  // --- Auth: validate mothership bearer token ---
  const authHeader = req.headers.get("Authorization") || "";
  const token = authHeader.startsWith("Bearer ") ? authHeader.slice(7) : "";
  if (!token) {
    return new Response(JSON.stringify({ error: "missing token" }), {
      status: 401,
      headers: { "Content-Type": "application/json" },
    });
  }

  const supabase = createClient(
    Deno.env.get("SUPABASE_URL")!,
    Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  );

  // Validate token against mothership_tokens table
  const { data: tokenRow, error: tokenErr } = await supabase
    .from("mothership_tokens")
    .select("mothership_id, deployment_id, revoked_at")
    .eq("token", token)
    .is("revoked_at", null)
    .single();

  if (tokenErr || !tokenRow) {
    return new Response(JSON.stringify({ error: "invalid token" }), {
      status: 403,
      headers: { "Content-Type": "application/json" },
    });
  }

  // --- Read binary body ---
  const bodyBuf = await req.arrayBuffer();
  const data = new Uint8Array(bodyBuf);

  if (data.length < 16) {
    return new Response(JSON.stringify({ error: "batch too small" }), {
      status: 400,
      headers: { "Content-Type": "application/json" },
    });
  }

  // --- Decode batch header ---
  const magic = u32(data, 0);
  if (magic !== BATCH_MAGIC) {
    return new Response(JSON.stringify({ error: "bad magic" }), {
      status: 400,
      headers: { "Content-Type": "application/json" },
    });
  }
  const version = data[4];
  const snapshotCount = u16(data, 6);
  const mothershipTs = u32(data, 8);

  // --- Create sync_session ---
  const { data: session, error: sessionErr } = await supabase
    .from("sync_sessions")
    .insert({
      mothership_id: tokenRow.mothership_id,
      started_at: new Date().toISOString(),
      status: "in_progress",
    })
    .select("id")
    .single();

  const sessionId = session?.id;

  // --- Decode snapshots and build reading rows ---
  let offset = 16;
  const rowsToInsert: any[] = [];
  const nodesHeard = new Set<string>();

  for (let s = 0; s < snapshotCount; s++) {
    const snap = decodeSnapshot(data, offset);
    if (!snap) {
      console.error(`snapshot ${s} decode failed at offset ${offset}`);
      break;
    }
    nodesHeard.add(snap.nodeId);

    for (const r of snap.readings) {
      const meta = SENSOR_MAP[r.sensorId] || { name: `sensor_${r.sensorId}`, unit: "" };
      // NaN check: IEEE 754 NaN is not equal to itself
      const isNaN = r.value !== r.value;
      rowsToInsert.push({
        node_id: snap.nodeId,
        seq_num: snap.seqNum,
        timestamp: new Date(snap.nodeTimestamp * 1000).toISOString(),
        sensor_id: String(r.sensorId),
        sensor_name: meta.name,
        value: isNaN ? null : r.value,
        unit: meta.unit,
        quality_flags: snap.qualityFlags,
        config_version: snap.configVersion,
        sync_session_id: sessionId,
      });
    }

    offset += HEADER_SIZE + snap.readings.length * READING_SIZE;
  }

  // --- Batch insert readings (chunked to avoid payload limits) ---
  const CHUNK = 500;
  let inserted = 0;
  for (let i = 0; i < rowsToInsert.length; i += CHUNK) {
    const slice = rowsToInsert.slice(i, i + CHUNK);
    const { error } = await supabase.from("readings").insert(slice);
    if (error) {
      console.error("insert error:", error.message);
    } else {
      inserted += slice.length;
    }
  }

  // --- Close sync_session ---
  if (sessionId) {
    await supabase.from("sync_sessions").update({
      completed_at: new Date().toISOString(),
      nodes_heard: nodesHeard.size,
      rows_uploaded: inserted,
      bytes_uploaded: data.length,
      status: "ok",
    }).eq("id", sessionId);
  }

  return new Response(JSON.stringify({
    success: true,
    snapshots: snapshotCount,
    rows: inserted,
    nodes: nodesHeard.size,
    bytes: data.length,
  }), {
    status: 200,
    headers: { "Content-Type": "application/json" },
  });
});
```

### 7.2 Edge Function environment variables

Set in Supabase dashboard → Edge Functions → Secrets:

| Variable | Value |
|---|---|
| `SUPABASE_URL` | `https://<project-ref>.supabase.co` |
| `SUPABASE_SERVICE_ROLE_KEY` | the project service_role key |

### 7.3 Error handling and response codes

| HTTP status | Meaning | Mothership action |
|---|---|---|
| 200 | Success — batch ingested | Advance binary cursor |
| 400 | Bad magic / malformed batch | Do not retry same batch; log and skip |
| 401 | Missing bearer token | Check NVS token; alert via web UI |
| 403 | Invalid/revoked token | Re-provision token via web UI |
| 405 | Wrong method | Firmware bug |
| 500 | Edge Function crash | Retry with backoff (existing `maxRetriesPerWindow`) |

The mothership treats 200 as success and advances the cursor. Any other status triggers the existing retry policy (`incrementRetryCount`, `nextAttemptUnix` cooldown).

---

## 8. Dashboard Integration

### 8.1 Supabase JS client setup

```typescript
// src/lib/supabaseClient.ts
import { createClient } from "@supabase/supabase-js";

const supabaseUrl = import.meta.env.VITE_SUPABASE_URL;
const supabaseAnonKey = import.meta.env.VITE_SUPABASE_ANON_KEY;

export const supabase = createClient(supabaseUrl, supabaseAnonKey, {
  realtime: { params: { eventsPerSecond: 2 } },
});
```

Environment variables (`.env`):
```
VITE_SUPABASE_URL=https://<project-ref>.supabase.co
VITE_SUPABASE_ANON_KEY=<anon-key>
```

### 8.2 Query patterns (replacing Apps Script endpoints)

#### getAllLatest → latest reading per node per sensor

```typescript
// src/hooks/useAllLatest.ts
import { supabase } from "../lib/supabaseClient";

export async function getAllLatest() {
  // DISTINCT ON gives the latest row per (node_id, sensor_id)
  const { data, error } = await supabase.rpc("get_latest_per_node_sensor");
  if (error) throw error;
  return data;
}
```

Corresponding SQL view/function in Supabase:

```sql
CREATE OR REPLACE FUNCTION get_latest_per_node_sensor()
RETURNS TABLE (
  node_id TEXT, sensor_id sensor_id_enum, sensor_name TEXT,
  value DOUBLE PRECISION, unit TEXT, timestamp TIMESTAMPTZ
) AS $$
  SELECT DISTINCT ON (node_id, sensor_id)
         node_id, sensor_id, sensor_name, value, unit, timestamp
    FROM readings
   ORDER BY node_id, sensor_id, timestamp DESC;
$$ LANGUAGE sql STABLE;
```

#### getData → time-series for a node/sensor

```typescript
export async function getNodeTimeSeries(nodeId: string, sensorId: string, hours: number) {
  const since = new Date(Date.now() - hours * 3600 * 1000).toISOString();
  const { data, error } = await supabase
    .from("readings")
    .select("timestamp, value, quality_flags")
    .eq("node_id", nodeId)
    .eq("sensor_id", sensorId)
    .gte("timestamp", since)
    .order("timestamp", { ascending: true });
  if (error) throw error;
  return data;
}
```

#### getNodes → node list with metadata

```typescript
export async function getNodes() {
  const { data, error } = await supabase
    .from("nodes")
    .select("node_id, deployment_id, last_seen, battery_mv, firmware_version, config_version, sensor_present")
    .order("node_id");
  if (error) throw error;
  return data;
}
```

#### Aggregate stats across all nodes

```typescript
export async function getAggregateStats() {
  const since = new Date(Date.now() - 24 * 3600 * 1000).toISOString();
  const { count } = await supabase
    .from("readings")
    .select("*", { count: "exact", head: true })
    .gte("timestamp", since);
  return { dataPointsToday: count ?? 0 };
}
```

#### Sync session history

```typescript
export async function getSyncSessions(limit = 20) {
  const { data, error } = await supabase
    .from("sync_sessions")
    .select("id, mothership_id, started_at, completed_at, nodes_heard, rows_uploaded, bytes_uploaded, status")
    .order("started_at", { ascending: false })
    .limit(limit);
  if (error) throw error;
  return data;
}
```

### 8.3 Real-time subscriptions

```typescript
// src/hooks/useRealtimeReadings.ts
import { useEffect } from "react";
import { supabase } from "../lib/supabaseClient";

export function useRealtimeReadings(onNewReading: (row: any) => void) {
  useEffect(() => {
    const channel = supabase
      .channel("readings-realtime")
      .on(
        "postgres_changes",
        { event: "INSERT", schema: "public", table: "readings" },
        (payload) => onNewReading(payload.new),
      )
      .subscribe();

    return () => { supabase.removeChannel(channel); };
  }, [onNewReading]);
}
```

This replaces the 60-second polling loop. The dashboard's `Dashboard.jsx` page subscribes to new inserts and updates sensor cards in real time.

### 8.4 Mapping old endpoints to new

| Apps Script endpoint | Supabase replacement |
|---|---|
| `GET ?action=getData&nodeId=X&hours=24` | `supabase.from("readings").select(...).eq("node_id",X).gte("timestamp",since)` |
| `GET ?action=getLatest&nodeId=X` | `supabase.rpc("get_latest_per_node_sensor")` filtered to X |
| `GET ?action=getAllLatest` | `supabase.rpc("get_latest_per_node_sensor")` |
| `GET ?action=getNodes` | `supabase.from("nodes").select(...)` |
| `GET ?action=getSystemStatus` | `supabase.from("sync_sessions").select(...).limit(1).order(...)` + `nodes` aggregate |
| `GET ?action=getConfig` | Future: `supabase.from("config").select(...)` (not in Phase 1) |
| `POST ?action=uploadData` | Supabase Edge Function `ingest-fieldmesh` (binary) |

### 8.5 Dashboard design doc updates

`Coder` should update `docs/FIELDMESH_DASHBOARD_DESIGN.md`:
- Replace the "Phase 1: Google Sheets as Backend" section with a note pointing to this document.
- Update the architecture diagram: `Google Apps Script → Google Sheet` becomes `Supabase Edge Function → PostgreSQL`.
- Update the "Data Source" table: the CSV column list becomes the `readings` table long-format columns.
- Update the `sensorMetadata` object to use `sensor_id` enum values instead of numeric keys.
- Update the "Deployment" section: backend is now Supabase (free tier), not Apps Script.

---

## 9. Migration Plan (phased)

### Phase 1: Set up Supabase project and schema (no firmware changes)

- Create Supabase project.
- Run the SQL DDL (§5).
- Insert a `deployments` row and a `mothership_tokens` row manually.
- Create the Edge Function `ingest-fieldmesh` (§7) but do not deploy yet.
- **No firmware or dashboard changes.** Google Sheets remains live.

**Exit criteria:** Schema exists, Edge Function tested with a hand-crafted binary batch via `curl`.

### Phase 2: Create Edge Function that accepts JSON, CSV, and binary

- Deploy the Edge Function.
- Create Edge Function that accepts JSON payload (from `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`) and distributes across tables (`sync_sessions`, `mothership_status`, `mothership_config`, `nodes`, `readings`).
- Extend it to also accept `Content-Type: text/plain` (CSV) and parse CSV rows into the same `readings` table.
- This lets the **existing firmware** (CSV path) write to Supabase without any firmware change — just point `endpointUrl` at the Edge Function.
- Test with the real mothership sending CSV to the Edge Function.
- **Dashboard still reads from Google Sheets** in this phase.

**Exit criteria:** Mothership CSV uploads appear in Supabase `readings` table. Google Sheets and Supabase run in parallel.

### Phase 3: Update firmware to send JSON (with CSV fallback)

- Update firmware to build JSON payload with ArduinoJson (meta + status + readings), send as `application/json` to the Edge Function. See §6.7.
- `Coder` implements `getNewDataBinary()` and the `/snapshots.bin` logging path (§6.4) as a later optimization.
- Add `useBinaryUpload` flag to `TransmissionSettings` (default `true`).
- Set `binaryEndpointUrl` and `binaryAuthToken` via the web UI.
- Test JSON upload end-to-end: mothership → Edge Function → PostgreSQL (readings + status + config tables populated).
- CSV fallback remains available if `useBinaryUpload = false`.
- **Note:** The binary V2 format (§6.3) is a later optimization for large payloads where status reporting isn't needed.

**Exit criteria:** JSON uploads populate `readings`, `mothership_status`, `mothership_config`, and `nodes` tables. CSV fallback verified working.

### Phase 4: Update dashboard to query Supabase

- `Coder` implements the Supabase client and query hooks (§8).
- Replace Apps Script calls with Supabase queries.
- Add real-time subscriptions.
- Deploy dashboard to Vercel/Netlify with `VITE_SUPABASE_URL` and `VITE_SUPABASE_ANON_KEY`.
- **Google Sheets still receives data** (parallel run).

**Exit criteria:** Dashboard shows live data from Supabase. Real-time updates work.

### Phase 5: Cut over from Google Sheets to Supabase

- Disable the Google Sheets upload path in `TransmissionSettings` (set Google `endpointUrl` empty).
- Verify all data flows through Supabase only.
- Monitor for 1 week.

**Exit criteria:** One week of clean data in Supabase, no Google Sheets writes.

### Phase 6: Remove CSV path from firmware

- `Coder` removes `getNewData()` CSV path and `kUploadCSVHeader` (or keeps it as a download-only format for the web UI CSV export).
- Remove Google Cloud Function deployment.
- Update `docs/FIELDMESH_DASHBOARD_DESIGN.md` and `docs/concept_overview.md` to remove Google Sheets references.

**Exit criteria:** No Google Sheets or Apps Script references in firmware or docs.

---

## 10. Cost Analysis

### 10.1 Supabase free tier limits

| Resource | Free tier | Pro tier ($25/mo) |
|---|---|---|
| Database size | 500 MB | 8 GB |
| Storage | 1 GB | 100 GB |
| Monthly active users | 50,000 | 100,000 |
| Edge Function invocations | 500K/mo | 2M/mo (then $2 per 100K) |
| Real-time connections | 200 concurrent | 500 concurrent |
| Bandwidth | 5 GB | 250 GB |

### 10.2 Estimated usage

**Assumptions:** 30 nodes, 5 sensors each, 5-minute wake interval (12 reads/hour).

| Metric | Calculation | Daily | Monthly |
|---|---|---|---|
| Snapshots | 30 × 12 | 360 | ~10,800 |
| Readings (rows) | 360 × 5 | 1,800 | ~54,000 |
| Edge Function invocations | 1 per upload session (every 15 min) | 96 | ~2,880 |
| DB growth | 54K rows × ~80 bytes/row | ~4.3 MB | ~130 MB |

**Database size:** ~130 MB/month. Free tier (500 MB) holds ~3.5 months. Pro tier (8 GB) holds ~5 years.

**Edge Function invocations:** ~2,880/month. Well under the 500K free limit.

**Real-time connections:** 1–2 dashboard users. Well under 200.

### 10.3 When paid tier is needed

| Trigger | When |
|---|---|
| 500 MB DB limit | ~3.5 months of data (single deployment). At this point, either upgrade to Pro ($25/mo) or set up a retention policy (delete readings older than 90 days). |
| 50K MAU | Not relevant — this is a field monitoring system with a handful of dashboard users, not a consumer app. |
| 200 concurrent real-time | Not relevant for a small team. |

**Recommendation:** Start on free tier. Add a nightly retention job (Supabase scheduled function) that deletes `readings` older than 90 days once DB approaches 400 MB. Upgrade to Pro only if long-term historical retention is needed or a second deployment is added. *[Confirmed decision]*

### 10.4 LTE data cost comparison

| | CSV (current) | Binary (target) |
|---|---|---|
| Daily upload volume | ~1.30 MB | ~674 KB |
| Monthly LTE data | ~39 MB | ~20 MB |
| Annual LTE data | ~475 MB | ~247 MB |

On a typical 1 GB/month IoT SIM plan, binary leaves more headroom for retries, OTA, and status reporting.

---

## 11. Security Considerations

### 11.1 API key management on the mothership

- The mothership bearer token is stored in **NVS** (namespace `tx`, key `binary_auth_token`), not hardcoded in firmware.
- It is set via the mothership web UI (WiFi AP config page), entered once during deployment.
- The token is sent as `Authorization: Bearer <token>` in the HTTPS header — not in the URL query string (unlike the current `?token=xxx`).
- The `service_role` key is **never** on the mothership. The Edge Function uses it server-side only.

### 11.2 RLS policies

- **Mothership** (via Edge Function with `service_role`): can INSERT into `readings`, `sync_sessions`, and UPSERT into `nodes`. The `service_role` bypasses RLS, so the Edge Function is the trust boundary.
- **Dashboard** (anon key): can SELECT from `readings`, `nodes`, `sync_sessions`, `deployments`. Cannot INSERT/UPDATE/DELETE.
- **mothership_tokens**: anon cannot read or write. Only the Edge Function (service_role) reads it.

### 11.3 HTTPS/TLS

- Already in place: the A7670G uses the CCH* SSL/TLS API (`AT+CCHSTART`, `AT+CCHOPEN`, `AT+CCHSEND`) with SNI enabled (`AT+CSSLCFG="enableSNI",0,1`).
- The Supabase Edge Function URL is HTTPS-only. No change to modem SSL config.
- `authmode` is currently set to `0` (no server cert validation). *[Open question — see §12: should we enable cert validation for Supabase?]*

### 11.4 Anon key vs service role key

| Key | Where used | Risk if leaked |
|---|---|---|
| `anon` key | Dashboard frontend (embedded in JS bundle) | Low — only allows SELECT with RLS. Safe to expose. |
| `service_role` key | Edge Function env only | High — bypasses RLS. Must never be in firmware or frontend. |
| Mothership bearer token | Mothership NVS | Medium — allows INSERT via Edge Function only. Revocable via `mothership_tokens.revoked_at`. |

### 11.5 Token rotation

- To rotate a mothership token: insert a new row in `mothership_tokens`, set the old row's `revoked_at`, and update the mothership web UI with the new token.
- The Edge Function checks `revoked_at IS NULL` on every request, so revocation is immediate.

---

## 12. Open Questions

### Decisions needed from user

1. **Binary storage on mothership:** Should raw V2 snapshots be stored in a new `/snapshots.bin` file (binary append), or should `getNewDataBinary()` re-parse `/datalog.csv` back into V2 structs? The former is simpler and preserves exact wire bytes; the latter avoids a second LittleFS file but loses the original packing. *[Recommendation: `/snapshots.bin` — simpler, exact.]*

2. **ESP32 `String` and embedded nulls:** `ModemDriver::httpsPost()` takes `const String& payload`. Does the ESP32 Arduino `String` correctly report `length()` for buffers containing `0x00` bytes? If not, `httpsPost()` must be refactored to accept `const uint8_t* + size_t`. `Coder` should verify with a unit test before implementing the binary path.

3. **SSL certificate validation:** The modem currently uses `AT+CSSLCFG="authmode",0,0` (no server cert validation). Should we enable cert validation (`authmode=1`) for Supabase? This requires loading the Supabase root CA onto the modem. *[Recommendation: keep `authmode=0` for now — the A7670G CA bundle management is fragile, and the bearer token provides the auth layer. Flag as future hardening.]*

4. **Retention policy:** Should readings be retained indefinitely (upgrade to Pro at ~3.5 months) or pruned to 90 days (stay on free tier)? This depends on whether the user needs long-term historical analysis in the database vs. exporting to CSV/Parquet for archival.

5. **Single vs multi-mothership:** The schema supports multiple motherships via `mothership_tokens.mothership_id`. Is the initial deployment a single mothership, or should we provision multiple tokens from day one?

6. **Dashboard hosting:** The dashboard design doc mentions Vercel/Netlify. Supabase real-time requires a persistent WebSocket connection, which works on both. Confirm hosting choice.

### Alternative approaches considered

| Alternative | Why rejected |
|---|---|
| **Direct PostgREST insert (no Edge Function)** | PostgREST expects JSON, not binary. The mothership would have to JSON-encode readings, which is larger than CSV. The Edge Function lets us send raw binary. |
| **MQTT instead of HTTPS** | The A7670G supports MQTT, but Supabase does not offer managed MQTT. Would require an MQTT broker (e.g. HiveMQ Cloud) as an additional hop. More moving parts. |
| **InfluxDB / TimescaleDB** | Better for pure time-series, but Supabase (PostgreSQL) gives us RLS, REST, real-time, and a dashboard ecosystem for free. TimescaleDB extension can be added later if query performance on millions of rows becomes an issue. |
| **Keep Google Sheets, add a sync layer** | Does not solve the CSV overhead or query limitation. Just moves the problem. |

---

## Appendix A — V2 snapshot wire format reference

From `node/firmware/v2/shared/protocol.h`:

```
node_snapshot_v2_t  (48 bytes, packed, little-endian)
  offset 0:  char     command[16]       // "NODE_SNAPSHOT2"
  offset 16: char     nodeId[16]        // e.g. "ENV_6C0AA0"
  offset 32: uint32_t nodeTimestamp     // node RTC unix at capture
  offset 36: uint32_t seqNum            // snapshot sequence number
  offset 40: uint16_t sensorCount       // number of v2_reading_t entries
  offset 42: uint16_t qualityFlags      // QF_DROPPED etc.
  offset 44: uint16_t configVersion     // node config version
  offset 46: uint8_t  protocolVersion   // 2
  offset 47: uint8_t  reserved          // padding

v2_reading_t  (6 bytes, packed)
  offset 0: uint16_t sensorId           // SENSOR_ID_* constant
  offset 2: float    value              // IEEE 754 little-endian

Wire size = 48 + sensorCount × 6
Max sensorCount = 33  (ESP-NOW 250-byte limit: (250-48)/6 = 33)
```

## Appendix B — Sensor ID constants

From `node/firmware/v2/shared/protocol.h`:

```
SENSOR_ID_AIR_TEMP      1001    SENSOR_ID_SPECTRAL_555  1105
SENSOR_ID_AIR_RH        1002    SENSOR_ID_SPECTRAL_590  1106
SENSOR_ID_SPECTRAL_415  1101    SENSOR_ID_SPECTRAL_630  1107
SENSOR_ID_SPECTRAL_445  1102    SENSOR_ID_SPECTRAL_680  1108
SENSOR_ID_SPECTRAL_480  1103    SENSOR_ID_WIND_SPEED    1201
SENSOR_ID_SPECTRAL_515  1104    SENSOR_ID_WIND_DIR      1202
SENSOR_ID_PAR           1301    SENSOR_ID_SOIL1_VWC     2001
SENSOR_ID_SOIL2_VWC     2002    SENSOR_ID_SOIL1_TEMP    2003
SENSOR_ID_SOIL2_TEMP    2004    SENSOR_ID_AUX1          3001
SENSOR_ID_AUX2          3002    SENSOR_ID_BAT_V         4001
```

Reserved ranges:
- 1000–1999: Standard environmental (temp, humidity, spectral, wind, PAR)
- 2000–2999: Soil and analog sensors
- 3000–3999: Auxiliary inputs
- 4000–4999: System metrics (battery, internal temp)
- 5000–5999: Future port-based dynamic sensors