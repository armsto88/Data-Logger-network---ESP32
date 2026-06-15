# FieldMesh — App & Backend Integration Brief

**Date:** 2026-06-12
**Product:** FieldMesh
**Tagline:** From field sensor to ecological insight.
**Purpose:** Offline-first platform for deploying, configuring, monitoring and analysing distributed environmental sensor networks
**Audience:** App/backend team, product team, firmware team
**Context:** The mothership hardware (ESP32-WROOM + A7670G LTE modem) is in production. The WiFi AP interface and BLE command set are fully implemented in firmware. The LTE modem hardware is designed but the firmware driver and cloud backend do not yet exist.

---

## 1. What the Mothership Does Today

The mothership is a central data concentrator for a fleet of ESP32 sensor nodes. It has **two live interfaces** right now:

### 1.1 WiFi Access Point (AP) — Web UI

| Parameter | Value |
|-----------|-------|
| SSID | `Logger001` (configurable) |
| Password | `logger123` |
| IP | `192.168.4.1` |
| Channel | 11 (shared with ESP-NOW) |
| Max clients | 4 |
| Captive portal | Yes (DNS redirect to `192.168.4.1`) |
| Authentication | None (password on AP only) |

**HTTP endpoints currently served:**

| Method | Route | Function |
|--------|-------|----------|
| GET | `/` | Dashboard: RTC time, fleet KPIs, discover button, CSV download, schedules, SD card status |
| GET | `/ui-status` | JSON status API (device info, fleet counts, schedule config) |
| GET | `/download-csv` | Stream entire `datalog.csv` as attachment |
| GET | `/nodes` | Node manager list page |
| GET | `/node-config?node_id=X` | Per-node configuration form |
| POST | `/node-config` | Save per-node config (deploy/revert/unpair/rename) |
| POST | `/set-time` | Set DS3231 RTC |
| POST | `/discover-nodes` | Send 3× ESP-NOW discovery broadcasts |
| POST | `/set-wake-interval` | Set fleet-global wake interval |
| POST | `/set-sync-mode` | Set sync mode (interval/daily) |
| POST | `/set-sync-time` | Set daily sync HH:MM |
| POST | `/revert-node` | Revert deployed→paired |

**`/ui-status` JSON response (current schema):**

```json
{
  "deviceId": "001",
  "firmwareVersion": "v1.0.0",
  "firmwareBuild": "<date> <time>",
  "rtcUnix": 1777028400,
  "apEnabled": true,
  "espnowChannel": 11,
  "wakeIntervalMinutes": 5,
  "syncIntervalMinutes": 90,
  "syncDailyTime": "06:00",
  "syncMode": "interval",
  "nextSyncLocal": "11:02:05 24-04-2026",
  "fleet": {
    "total": 3,
    "unpaired": 0,
    "paired": 1,
    "deployed": 2,
    "pending": 0
  }
}
```

### 1.2 BLE GATT Service — Native App Interface

| UUID | Type | Purpose |
|------|------|---------|
| `6f880001-2d0f-4aa0-8f9e-8f8b7e5a0001` | Service | Microclimate service |
| `6f880002-...` | Write characteristic | Request channel |
| `6f880003-...` | Notify characteristic | Response channel |
| `6f880004-...` | Notify characteristic | Status/telemetry stream |

**Device name:** `Microclimate-001`

**BLE command set (10 commands, all implemented):**

| Command | Request Payload | Response |
|---------|----------------|----------|
| `get_status` | `{}` | Mothership + fleet + schedule status |
| `set_time` | `{"timestampUnix": N}` | Ack |
| `discover_nodes` | `{}` | Discovery results stream |
| `set_wake_interval` | `{"intervalMinutes": N}` | Ack |
| `set_sync_interval` | `{"syncIntervalMinutes": N, "phaseUnix": N}` | Ack |
| `list_nodes` | `{}` | Full node list with sensors, queue, health |
| `node_config_apply` | `{"nodeId", "friendlyName", "targetState", "wakeIntervalMin", "syncIntervalMin", "phaseUnix"}` | Ack |
| `node_revert` | `{"nodeId"}` | Ack |
| `node_unpair` | `{"nodeId"}` | Ack |
| `export_csv_request` | `{}` | Chunked CSV with metadata |

**BLE request envelope:**

```json
{
  "command": "discover_nodes",
  "correlationId": "d428f3d7-...",
  "timestampUnix": 1775941500,
  "payload": {}
}
```

**Chunked framing:** 8-byte header (4B messageId, 1B chunkIndex, 1B chunkCount, 2B payloadLength), 160B payload per chunk.

**Stale rejection:** Commands with `timestampUnix` > ±5 minutes from current time are rejected.

---

## 2. Sensor Data Schema

### 2.1 Wire Format (ESP-NOW, 124 bytes packed)

```c
typedef struct __attribute__((packed)) node_snapshot {
    char     command[16];       // "NODE_SNAPSHOT"
    char     nodeId[16];       // e.g. "ENV_94E38C"
    uint32_t nodeTimestamp;     // node RTC unix at capture
    uint32_t seqNum;            // snapshot sequence number
    uint16_t sensorPresent;     // bitmask of valid channels
    uint16_t qualityFlags;      // 0=clean, 0x0001=queue overflow
    uint16_t configVersion;
    uint8_t  _pad[2];
    float    batVoltage;        // V
    float    airTemp;           // °C
    float    airHumidity;       // % RH
    float    spectral[8];       // raw counts, 415–680 nm
    float    windSpeed;         // m/s
    float    windDir;           // degrees 0–360
    float    soil1Vwc;          // m³/m³
    float    soil1Temp;         // °C
    float    soil2Vwc;
    float    soil2Temp;
    float    aux1;
    float    aux2;
} node_snapshot_t;
```

**Sensor present bitmask:**

| Bit | Sensor |
|-----|--------|
| 0 | Air temperature |
| 1 | Air humidity |
| 2 | Spectral (all 8 channels) |
| 3 | Wind (speed + direction) |
| 4 | Soil 1 (VWC + temp) |
| 5 | Soil 2 (VWC + temp) |
| 6 | AUX 1 |
| 7 | AUX 2 |
| 8 | Battery voltage |

### 2.2 CSV Format (SD Card)

```
ms_datetime,ms_sync_unix,node_id,node_name,node_mac,fw_id,node_datetime,node_unix,
bat_v,air_temp_c,air_hum_pct,
spectral_415nm,spectral_445nm,spectral_480nm,spectral_515nm,
spectral_555nm,spectral_590nm,spectral_630nm,spectral_680nm,
wind_speed_ms,wind_dir_deg,
soil1_vwc,soil1_temp_c,soil2_vwc,soil2_temp_c,
aux1,aux2,
sensor_present,quality_flags,seq_num
```

One row per node per wake cycle. The mothership adds its own timestamp (`ms_datetime`, `ms_sync_unix`) at ingest time.

---

## 3. Node Fleet Management

### 3.1 Node Lifecycle States

```
DISCOVERED → PAIRED → DEPLOYED → (revert) → PAIRED
                                    ↓
                              (unpair) → removed
```

### 3.2 ESP-NOW Command Protocol

**Mothership → Node commands:**

| Command | Purpose | Payload |
|---------|---------|---------|
| `DISCOVERY_SCAN` | Trigger node discovery | Broadcast |
| `PAIR_NODE` | Pair a specific node | nodeId, mothership_id |
| `DEPLOY_NODE` | Deploy with RTC + config | nodeId, RTC time, wakeInterval, syncInterval, syncPhase |
| `TIME_SYNC` | Sync node RTC | Unix timestamp |
| `UNPAIR_NODE` | Remove pairing | nodeId |
| `SET_SCHEDULE` | Set wake interval | wakeIntervalMin (broadcast) |
| `SET_SYNC_SCHED` | Set sync schedule | syncInterval, phase (broadcast) |
| `SYNC_WINDOW_OPEN` | Gate node queue flush | Marker (broadcast) |
| `CONFIG_SNAPSHOT` | Push config to node | configVersion, wakeInterval, syncInterval, phase |

**Node → Mothership messages:**

| Message | Purpose | Key Fields |
|---------|---------|------------|
| `NODE_SNAPSHOT` | All sensor data | 124-byte struct (see §2.1) |
| `NODE_HELLO` | Wake announcement | nodeId, configVersion, queueDepth, rtcUnix |
| `NODE_STATUS` | Async state push | nodeId, state, rtcSynced, deployed, rescueMode |
| `DISCOVER_REQUEST` | Discovery response | nodeId, nodeType |
| `DEPLOY_ACK` | Deployment confirmed | nodeId |
| `CONFIG_ACK` | Config applied | appliedVersion |

### 3.3 Scheduling

**Two sync modes:**
- **Interval mode:** Sync every `wakeMin × 18` minutes, aligned to phase anchor
- **Daily mode:** Sync once per day at configured HH:MM

**Allowed wake intervals:** 1, 5, 10, 20, 30, 60 minutes

**Per-node config (mothership-side `NodeDesiredConfig`):**

```c
struct NodeDesiredConfig {
    uint16_t configVersion;
    uint8_t  wakeIntervalMin;
    uint16_t syncIntervalMin;
    uint32_t syncPhaseUnix;
};
```

### 3.4 Node Identity

- **Firmware ID:** Derived from MAC (e.g. `ENV_94E38C`)
- **User numeric ID:** 3-digit, stored in NVS (e.g. `001`)
- **Friendly name:** Up to 32 chars, stored in NVS (e.g. "North Hedge 01")
- **Notes:** Up to 180 chars, stored in NVS

---

## 4. LTE Backhaul (Hardware Ready, Firmware/Backend Not Built)

### 4.1 Modem Hardware

| Component | Details |
|-----------|---------|
| Modem | SIMCom A7670G (LTE Cat-1, global bands, B28 for Australia) |
| Interface | UART2 via 1.8V level shifters (SN74LVC1T45) |
| Power | TPS63020 buck-boost → ~3.9V dedicated rail, 2A+ peak |
| Control | GPIO33=4V_EN, GPIO35=PGOOD, GPIO14=PWRKEY, GPIO4=STATUS |
| Antenna | u.FL/IPEX connector → internal FPC or external SMA |
| SIM | Direct wiring with 22Ω series resistors, ESD protection |

### 4.2 Planned Upload Architecture

```
Nodes → ESP-NOW → Mothership → SD card (authoritative)
                                    ↓
                              LTE modem → HTTPS file upload → Cloud backend
```

**Key design rules:**
- SD card is always the authoritative local record
- LTE upload is additive — failures never block local ingest
- Upload cursor tracks byte offset, last successful newline, timestamps, retry eligibility
- Modem stays OFF most of the time; powers on at scheduled intervals
- First-pass transport: HTTPS file upload (CSV-centric)
- Later option: MQTT for summaries/alerts

### 4.3 Planned Transmission Settings Schema

```json
{
  "enabled": true,
  "transportType": "https_file",
  "endpointUrl": "https://example.invalid/upload",
  "authMode": "token",
  "siteId": "site-001",
  "deploymentId": "deploy-001",
  "uploadPolicy": "scheduled_batch",
  "uploadIntervalMin": 360,
  "uploadPhaseUnix": 1767225600,
  "minBatteryMv": 3700,
  "maxBytesPerSession": 262144,
  "maxRetriesPerWindow": 3,
  "allowManualUpload": true
}
```

### 4.4 Modem Control State Machine (Planned)

```
Off → Rail Enabled → Rail Stable → Boot Requested → Status High →
UART Ready → Registered → Transport Open → Upload Active →
Graceful Shutdown → Rail Disabled
```

Plus a `Recovery` branch for modem hangs.

---

## 5. What the App/Backend Team Needs to Deliver

### 5.1 Desktop + Mobile Application

**Must support three connection modes:**
1. **Local (WiFi AP):** Connect to mothership AP at `192.168.4.1`, use HTTP API
2. **Local (BLE):** Connect via BLE GATT service, use command protocol
3. **Remote (Cloud):** Connect to cloud backend, view synced data from any deployment

**Feature parity with current web UI (minimum):**
- Dashboard: mothership identity, RTC, fleet counts, SD card status
- Node list: search, filter by state, per-node detail
- Discovery: trigger and stream results
- Deploy/pair/revert/unpair with confirmation
- Schedule control: wake interval, sync mode, sync time
- RTC time set (from device clock or manual entry)
- CSV data export and share
- Command history with correlation IDs

**Extended features for desktop/mobile app:**
- Multi-mothership management (switch between deployments)
- Data visualization: time-series charts per sensor per node
- Fleet health dashboard: battery trends, missed syncs, queue pressure
- Offline operation: cache last-known state, queue commands when disconnected
- Push notifications (via cloud) for alerts: node offline, battery low, data gap
- Map view: node positions on site map (if coordinates configured)
- Sensor calibration interface: per-node offset entry
- Firmware update staging (future)

### 5.2 Cloud Backend

**Must receive:**
- CSV data uploads from motherships via HTTPS
- Transmission settings per deployment (site ID, auth token, upload schedule)

**Must provide:**
- Data ingestion API: accept CSV uploads, parse, store
- Time-series database: store all sensor readings with metadata
- Deployment management: site registration, mothership registration, node registry
- Data query API: time-range queries, per-node, per-sensor-type
- User authentication: multi-user access to deployment data
- Dashboard API: fleet status, latest readings, health summaries
- Alert rules engine: configurable thresholds per sensor per node
- Data export API: CSV/JSON download, date range, node filter

**Recommended architecture:**
- API gateway → ingestion service → time-series DB (InfluxDB/TimescaleDB)
- Relational DB (PostgreSQL) for deployment metadata, users, node registry
- Object storage (S3) for raw CSV archives
- Alert service with configurable rules and notification channels (email, push, webhook)

### 5.3 Data Sync Contract

**Mothership → Cloud upload:**
- Transport: HTTPS POST with multipart file upload
- Auth: Bearer token per deployment
- Payload: CSV file chunk (bounded by `maxBytesPerSession`)
- Cursor: Byte offset in `datalog.csv`, persisted locally on SD
- Retry: Up to `maxRetriesPerWindow` per upload window
- Idempotency: Server deduplicates by (siteId, nodeId, seqNum)

**Cloud → App delivery:**
- REST API with JSON responses
- WebSocket or SSE for real-time updates (optional)
- Pagination for large queries
- Date-range and node-ID filters

---

## 6. Existing Schemas and Contracts (Reference)

The following schemas are already defined in the codebase and must be treated as the source of truth:

### 6.1 BLE Command Schemas (from `docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md`)

**Full command catalog with request/result payloads is defined in §9.4 of that document.** Key schemas:

- `get_status` result: mothership info + fleet summary + schedule
- `list_nodes` result: per-node detail with sensors array, queue stats, health
- `node_config_apply` request: nodeId, friendlyName, targetState, wake/sync config
- `export_csv_request`: range, nodeIds, sensorTypes, format
- `node_telemetry_event`: real-time sensor stream (nodeId, sensorId, value, timestamp, qualityFlags)

### 6.2 Error Schema

```json
{
  "correlationId": "...",
  "ok": false,
  "error": {
    "code": "NODE_NOT_FOUND",
    "category": "validation",
    "retryable": false,
    "message": "Node ENV_001 does not exist"
  }
}
```

Error codes: `INVALID_COMMAND`, `INVALID_PAYLOAD`, `UNAUTHORIZED`, `BUSY`, `TIMEOUT`, `NODE_NOT_FOUND`, `STORAGE_ERROR`, `RTC_ERROR`, `EXPORT_ERROR`, `INTERNAL_ERROR`

### 6.3 Node State Enums

- **Node state:** `discovered`, `paired`, `deployed`, `reverted`, `offline`
- **Health severity:** `ok`, `warning`, `critical`
- **RTC health:** `ok`, `uncertain`, `invalid`

### 6.4 App Local Storage Schema (SQLite)

Tables already defined: `mothership_snapshot`, `nodes`, `command_log`, `event_log`, `decisions_log`, `risk_register`, `open_issues`, `export_files`

---

## 7. Constraints and Open Questions

### Constraints
- ESP32 has limited RAM (~320KB usable) and flash (4MB)
- BLE MTU is typically 23 bytes (negotiable up to 512 on some platforms)
- WiFi AP supports max 4 simultaneous clients
- ESP-NOW max payload is 250 bytes per packet
- SD card write speed limits burst ingest rate
- LTE modem draws 2A+ peaks — must be power-gated
- No user authentication exists on the current AP or BLE interface
- The mothership currently runs 24/7 (no sleep/wake power gating yet)

### Open Questions for App/Backend Team
1. **Authentication model:** How should users authenticate to the cloud backend? Per-deployment token? User accounts with role-based access?
2. **Data retention policy:** How long should raw sensor data be retained in the cloud? What aggregation/downsampling strategy for historical data?
3. **Multi-tenancy:** Can one cloud instance serve multiple independent deployments? How are they isolated?
4. **Real-time requirements:** Does the app need real-time data streaming from the cloud, or is periodic polling sufficient?
5. **Offline-first priority:** Should the app be designed as offline-first (local DB, sync when connected) or cloud-first (API calls, cache when offline)?
6. **Desktop vs mobile priority:** Which platform ships first? The BLE interface is mobile-only; the HTTP API works on both.
7. **Alert delivery:** Should alerts be push notifications (requires cloud), local-only (app polls mothership), or both?
8. **CSV vs structured API:** Should the cloud backend parse and normalize CSV, or store raw files and parse on query?
9. **Firmware update channel:** Should the app/backend support OTA firmware updates for motherships and nodes?
10. **Map/GIS integration:** Do users need to see node positions on a map? If so, where are coordinates stored and managed?

---

## 8. Key Source Files (For Reference)

| File | Content |
|------|---------|
| `mothership/firmware/src/main.cpp` | Full mothership firmware: AP, HTTP server, BLE, ESP-NOW, SD logging |
| `mothership/firmware/src/comms/espnow_manager.cpp` | ESP-NOW receive, node registry, sync window logic |
| `mothership/firmware/src/storage/sd_manager.cpp` | CSV write, header management |
| `mothership/firmware/src/ble/ble_manager.cpp` | BLE GATT service, chunked framing |
| `node/firmware/shared/protocol.h` | All wire protocol structs (node_snapshot_t, etc.) |
| `node/firmware/shared/sensors.h` | Sensor registry, sensorId assignments |
| `docs/NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md` | Complete native app spec with all JSON schemas |
| `docs/NATIVE_APP_INTEGRATION_V2.md` | BLE integration design |
| `docs/NATIVE_APP_INTEGRATION_STATUS_2026-04-13.md` | Integration status (Phase A-C done) |
| `mothership/docs/MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md` | LTE modem hardware + upload architecture |
| `mothership/docs/MOTHERSHIP_A7670G_MODEM_BRINGUP_NOTES.md` | Modem bring-up AT commands |

---

## 9. FieldMesh Product Model

> **FieldMesh is not a real-time IoT dashboard. It is an offline-first environmental monitoring system that provides immediate local control, periodic remote visibility and eventually reliable deferred remote configuration.**

FieldMesh is organised around four layers:

```
FieldMesh App
    ↓
Site and deployment
    ↓
Mothership gateway
    ↓
Monitoring positions and sensor nodes
```

The monitoring position belongs to the cloud data model, while the physical node ID remains the hardware identity.

Example:
```
Monitoring position
Under Panel — Row 4 East

Current hardware
ENV_A213F2

Previous hardware
ENV_94E38C
```

This prevents hardware replacement from breaking a long-term ecological time series.

---

## 10. FieldMesh Operating Modes

### 10.1 Field Mode

The user is physically near the mothership and connected through Bluetooth or the mothership Wi-Fi access point.

This is the **live operational mode**. It supports immediate node discovery, pairing, deployment, schedule changes, troubleshooting and local data export.

### 10.2 Cloud Mode

The user is connected remotely through the FieldMesh backend.

This mode provides:
- The most recently uploaded mothership and node status
- Historical environmental data
- Fleet health information
- Alerts
- Data exports
- Configuration preparation

Because the LTE modem is normally off, the app must clearly communicate that this information reflects the **last cloud synchronisation**, not necessarily the current physical state.

### 10.3 Deferred Control Mode

A user makes a configuration change remotely, but the mothership is offline.

The command is stored in the cloud and collected during the mothership's next LTE session. The mothership may then need to wait for the relevant node to wake before applying it.

A remote configuration change passes through these stages:

```
Draft → Queued in cloud → Downloaded by mothership → Waiting for node → Delivered to node → Confirmed by node
```

The interface must never simply show **"Saved"** when a command has only been queued.

---

## 11. Two Forms of Delayed Control

This distinction is critical for the FieldMesh interface.

### 11.1 Local delayed control

The app sends the request immediately to the mothership through BLE or Wi-Fi, but the mothership may need to wait for the sleeping node's next `NODE_HELLO`.

```
FieldMesh → Mothership: immediate
Mothership → Sleeping node: next wake cycle
Node → Mothership confirmation: same contact window
```

This is already supported by the current configuration-version system.

### 11.2 Remote delayed control

The request first waits for the mothership's next LTE session, then may wait again for the node's next wake.

```
FieldMesh Cloud
→ waiting for LTE session
→ downloaded by mothership
→ waiting for node wake
→ delivered to node
→ confirmed by node
→ confirmation uploaded at a later LTE session
```

This is not yet implemented and should remain a Release 3 feature.

---

## 12. FieldMesh Core Concepts

### 12.1 Four Timestamps

| Timestamp | Meaning |
|-----------|----------|
| **Last node measurement** | When the sensor values were recorded by the node |
| **Last node report** | When the node last successfully communicated with the mothership |
| **Last cloud synchronisation** | When the mothership last uploaded information through LTE |
| **Next expected event** | When the next node wake, sync, or LTE upload is expected |

The fourth timestamp allows FieldMesh to distinguish normal sleeping from abnormal absence. A node with a 60-minute wake interval should not receive a warning after ten minutes without contact.

Example display:
```
Latest measurement:        10:00
Reported to mothership:    10:02
Uploaded to FieldMesh:     12:00
Next expected node wake:   14:05
Next expected LTE upload:  18:00
```

### 12.2 Four-Axis Health Model

A single red/orange/green dot is insufficient. FieldMesh represents four independent health areas:

| Status area | What it describes |
|-------------|-------------------|
| **Node health** | Battery, RTC and sensor condition |
| **Local communication** | Node-to-mothership reporting |
| **Gateway health** | Mothership storage, RTC, power and firmware |
| **Cloud delivery** | LTE and backend synchronisation |

Example display:
```
Node health:          Healthy
Local communication:  Delayed
Gateway health:       Healthy
Cloud delivery:       Current
```

This avoids placing SD-card problems or mothership RTC problems under "cloud connection."

### 12.3 Monitoring Positions vs Hardware Nodes

A monitoring position should be conceptually separate from the physical node. This allows a failed node to be replaced without breaking the historical time series.

**Current firmware status:** The data model ties all records to the node's firmware ID (`nodeId` derived from MAC). There is no "location" abstraction in firmware. This would need to be designed at the cloud/backend level. See §18 Q10 for details.

### 12.4 Cloud Data Freshness Communication

The app must always distinguish between data collected locally, data uploaded to the cloud, and data not yet uploaded.

When cloud data is delayed, the app should communicate:

> Cloud data is delayed. This does not necessarily mean field data has been lost.

Example display:
```
Last node report to mothership:  14 minutes ago
Last LTE upload:                 12 hours ago
Local data awaiting upload:      Approximately 1.8 MB
```

### 12.5 Effective vs Desired Configuration

FieldMesh should always show both what the node is currently running and what the user wants it to run:

```
Wake interval

Effective: 5 minutes
Desired:   10 minutes
Status:    Waiting for node contact
```

### 12.6 Three Data Layers

| Layer | Meaning |
|-------|---------|
| **Raw** | Exact values uploaded from the mothership CSV |
| **Quality-controlled** | Raw data with validity and quality annotations |
| **Derived** | Calibrated, aggregated or calculated values |

Raw values must never be edited. Calibration and cleaning should produce derived values rather than overwrite the original measurements.

### 12.7 Terminology: Local vs Remote

FieldMesh uses these terms consistently:

- **Local BLE** — requires physical proximity to the mothership
- **Local Wi-Fi** — requires connection to the mothership access point
- **Remote cloud** — requires internet connectivity to the FieldMesh backend

"Remote" is reserved exclusively for cloud access. Local interfaces (BLE, Wi-Fi AP) are never described as "remote" even when the user is not physically touching the device.

---

## 13. FieldMesh User Groups

### 13.1 Field technician

Their priority is completing work correctly before leaving the site.

Key use cases:
- Connect to a mothership
- Commission a gateway
- Discover nodes
- Identify physical nodes
- Pair and deploy nodes
- Verify first measurements
- Check sensor presence
- Diagnose communication problems
- Replace failed hardware
- Download local data
- Complete a deployment checklist

This user primarily works in **Field Mode**.

### 13.2 Researcher or ecologist

Their priority is understanding environmental measurements and data quality.

Key use cases:
- View current and historical measurements
- Compare monitoring positions
- Compare treatment and reference areas
- Identify missing intervals
- Add annotations
- Review calibration history
- Export raw and derived data
- Compare deployments and seasons

This user primarily works in **Cloud Mode**.

### 13.3 Fleet or operations manager

Their priority is determining which sites need intervention.

Key use cases:
- Review all deployments
- Find overdue uploads
- Find nodes missing expected contacts
- Monitor battery trends
- Review storage and queue risks
- Create maintenance tasks
- Review unresolved alerts
- Track replacement hardware
- Review deployment readiness

### 13.4 Administrator

Their priority is managing access and infrastructure.

Key use cases:
- Create organisations and sites
- Register motherships
- Assign users and roles
- Manage deployment tokens
- Configure data retention
- Review command audits
- Revoke device access
- Manage alert delivery

---

## 14. FieldMesh Workflows

### Workflow A: Prepare and commission

```
Create site
→ Create deployment
→ Register mothership
→ Connect locally
→ Verify RTC and storage
→ Configure LTE
→ Discover nodes
→ Assign monitoring positions
→ Deploy nodes
→ Verify first readings
→ Run readiness check
```

The result should be a formal deployment state with two levels:

**Field-ready** (available in Release 1):
```
Deployment readiness

Gateway                         Ready
SD storage                      Ready
Expected nodes                  8 of 8
Nodes reporting                 8 of 8
RTC synchronisation             8 of 8
Expected sensors                8 of 8
Node battery                    Acceptable

Field status: Ready
```

**Cloud-ready** (added in Release 2):
```
Cloud readiness

Modem registered                Verified
Backend authenticated            Verified
Status summary uploaded          Verified
First CSV upload                 Verified
Upload cursor working            Verified
Next LTE session scheduled      Known

Cloud status: Ready
```

This prevents a perfectly usable local deployment being labelled incomplete because cloud functionality has not shipped.

### Workflow B: Observe deployment health

The remote site overview should answer four questions immediately:

1. Is the mothership uploading?
2. Were nodes reporting when the mothership last uploaded?
3. Is data accumulating safely?
4. Does anything require a field visit?

A deployment card could show:

```
North Array 2026

Last cloud sync          42 minutes ago
Next expected sync       In 3 hours 18 minutes
Nodes healthy            7 of 8
Nodes delayed            1
Upload backlog           420 KB
Storage                  Healthy
Field visit              Not currently required
```

### Workflow C: Diagnose a problem

The app should diagnose by layer.

**Layer 1 — Cloud connection:** LTE session overdue, authentication failure, upload rejected, upload cursor error, modem registration failure.

**Layer 2 — Gateway:** Mothership possibly powered down, RTC invalid, SD card unavailable, SD write failure, recent mothership restart, runtime telemetry rebuilding.

**Layer 3 — Node communication:** Node missed expected wake cycles, configuration acknowledgement missing, queue depth increasing, synchronisation stale, ESP-NOW communication unreliable.

**Layer 4 — Sensor data:** Sensor missing, reading implausible, reading unchanged, battery low, queue overflow caused lost measurements.

The interface should guide the user through this hierarchy rather than only showing a generic red status.

### Workflow D: Configure and maintain

Configuration actions should show their actual state.

**Local command lifecycle:**
```
Draft → Queued locally → Stored by mothership → Waiting for node → Sent to node → Applied by node → Confirmed
```

**Remote command lifecycle:**
```
Draft → Queued in cloud → Downloaded by mothership → Waiting for node → Applied by node → Confirmation awaiting cloud upload → Confirmed in FieldMesh
```

### Workflow E: Analyse and export

A chart should allow users to enable overlays for: missing measurements, queue-overflow periods, upload gaps, node replacements, calibration changes, configuration changes, maintenance visits, quality flags.

---

## 15. FieldMesh Status Model

### 15.1 Four-axis health display

| Area | Meaning |
|------|---------|
| Node health | Battery, RTC and sensor condition |
| Local communication | Node-to-mothership communication |
| Gateway health | Mothership storage, RTC, power and firmware |
| Cloud delivery | LTE and backend synchronisation |

### 15.2 Specific status language

FieldMesh should avoid generic "offline" and use specific language:

| Status | Meaning |
|--------|---------|
| Cloud data current | Recent successful LTE upload |
| Cloud data delayed | LTE upload overdue but within tolerance |
| Gateway contact overdue | No recent mothership cloud contact |
| Node report delayed | Missed expected reports but within tolerance |
| Node state unknown | No recent information from any path |
| Node communication failure suspected | Node has missed multiple expected cycles |
| Recent gateway restart | Mothership rebooted; telemetry rebuilding |
| Waiting for fresh telemetry | Post-restart grace period |

"Offline" should only appear when the system actually has enough information to establish that something is unavailable.

---

## 16. FieldMesh Navigation

### 16.1 Mobile Field Mode

```
Connect
Overview
Nodes
Data
Activity
```

The first screen should help the technician connect by: previously paired mothership, BLE scan, Wi-Fi AP, QR or pairing code in future.

### 16.2 Desktop or Cloud Mode

```
Sites
Fleet
Data
Alerts
Activity
Administration
```

Within a deployment:

```
Overview
Map
Gateway
Nodes
Data
Maintenance
Settings
```

The interface should change emphasis according to connection mode without becoming a completely different app.

A persistent connection banner should appear at the top:

```
Cloud mode
Last synchronised 46 minutes ago
```

or:

```
Field mode — Bluetooth
Connected directly to Gateway North 01
```

---

## 17. Limitations as Product Rules

### LTE is intermittent

FieldMesh should:
- Show last and expected next LTE sync
- Show cloud-data age
- Queue remote commands
- Never promise immediate remote execution
- Allow local operation without cloud access

### Nodes sleep between wake cycles

FieldMesh should:
- Show the expected next node contact
- Treat short absences as normal
- Base warnings on missed expected cycles
- Show configurations as pending until acknowledged

### The SD card is authoritative

FieldMesh should distinguish:
- Data collected locally
- Data uploaded to the cloud
- Data not yet uploaded
- Data believed to be missing locally

### Mothership restarts create a temporary knowledge gap

Most runtime telemetry is not persisted. After reboot, FieldMesh should not immediately mark nodes as failed.

It should display:
```
Gateway restarted recently

Some node telemetry is being reconstructed.
Status will update as nodes complete their next wake cycles.
```

The reconstruction period should be based on the longest configured node wake interval.

### Queue capacity creates a measurable data-loss risk

A node stores 24 snapshots. Approximate maximum unsynchronised duration:

| Wake interval | Queue coverage |
|---------------|----------------|
| 1 minute | 24 minutes |
| 5 minutes | 2 hours |
| 10 minutes | 4 hours |
| 20 minutes | 8 hours |
| 30 minutes | 12 hours |
| 60 minutes | 24 hours |

FieldMesh should calculate this dynamically:
```
Queue depth             19 of 24
Estimated capacity      25 minutes remaining
Data-loss risk          High
```

### A missed sync does not immediately mean lost data

The node retains measurements for the next sync window, subject to queue capacity. The app should distinguish:

```
Cloud delay
Local sync delay
Local queue approaching capacity
Confirmed dropped records
```

### Pending state changes may be lost on mothership restart

Desired configuration persists, but some pending state transitions do not. Until this is improved in firmware, FieldMesh should warn users when a deployment or unpair operation has not been confirmed:
```
Deployment request is not yet confirmed.

Avoid restarting the mothership until the node acknowledges this change.
```

### Configuration has no rollback

Configuration versions only increase. "Rollback" in FieldMesh should therefore mean: create a new version containing the previous settings. It should not imply that the firmware supports decrementing to an earlier version.

### BLE has limited transfer capacity

BLE is appropriate for: status, node lists, configuration, small diagnostic responses, small exports. Large CSV downloads should preferably use: local Wi-Fi, cloud export, or direct SD-card access.

### Wi-Fi AP supports few clients

FieldMesh should avoid encouraging several users to control one mothership simultaneously. Configuration changes should include correlation IDs and an activity log so conflicting actions can be understood.

### Local authentication must be improved before public release

Current state:
- Shared hardcoded Wi-Fi password
- No BLE pairing
- No BLE encryption
- No command authorisation
- Timestamp validation provides replay resistance, not authentication

Before a public FieldMesh Local release, the following are required at minimum:
- Unique device setup code
- Changeable Wi-Fi credential
- BLE secure pairing and bonding
- First-owner enrolment
- Local session authentication
- Reset/recovery procedure
- Confirmation for destructive operations

Remote unpair should remain disabled until command authentication and acknowledgement are reliable.

---

## 18. Repository Manager Answers

The following questions affect whether the remote workflows are technically possible. Answers are based on the current firmware codebase as of 2026-06-12.

### Original 20 Questions

**Q1: During each LTE session, can the mothership both upload data and download pending commands?**

Not yet. The LTE subsystem is not implemented in firmware. The hardware (A7670G modem, TPS63020 power rail, level shifters) is designed on the PCB, but no modem driver, upload queue, or command-download logic exists in code. The first-pass design is HTTPS file upload (CSV-centric). Bidirectional LTE would need a separate design.

**Q2: Can the LTE session upload a compact status summary?**

The mothership has all the data, but no code currently serializes it for LTE upload. The `NodeInfo` struct stores `lastReportedBatV`, `lastReportedQueueDepth`, `configVersionApplied`, `lastNodeTimestamp`, `syncStale`, `staleMissCount`, `wakeIntervalMin`, `inferredWakeIntervalMin`. The `node_snapshot_t` wire format carries `sensorPresent` (bitmask) and `qualityFlags`. A compact JSON status summary would need to be designed and wired into the LTE upload path.

**Q3: Does the mothership retain the last known telemetry for every node after restart?**

Partially. The mothership persists node identity, state, and battery voltage to NVS. However, most telemetry fields (queue depth, sensor-present bitmask, quality flags, last sensor values, config version applied) are not persisted and reset to defaults on reboot. After a mothership restart, the system loses: last sensor readings, queue depth, sensor-present bitmask, quality flags, config version applied, inferred wake interval, stale state. Only MAC, nodeId, state, and battery voltage survive.

**Q4: When does a node accept configuration?**

On every contact with the mothership. The pull-handshake design means the node sends `NODE_HELLO` at the start of every wake cycle, and the mothership responds with `CONFIG_SNAPSHOT` if it has a newer version. The node keeps ESP-NOW alive for 1500ms (`POST_WAKE_WINDOW_MS`) after HELLO specifically to receive `CONFIG_SNAPSHOT` before power cut. Deferred control mode is architecturally supported.

**Q5: Can the mothership confirm separately that it downloaded a cloud command, sent the command to the node, and the node applied the command?**

Partially. The local two-stage chain works: the mothership can confirm it sent a command to a node (ESP-NOW send callback) and that the node applied it (`CONFIG_ACK` / `DEPLOY_ACK`). The cloud-to-mothership stage does not exist yet because LTE is not implemented.

**Q6: Can the cloud estimate how many bytes or rows are awaiting upload from the SD card?**

Not currently. The mothership has `getCSVStats()` that counts data records, but this information is only available locally. No upload cursor or byte-offset tracking exists. The LTE backhaul concept describes the intended upload cursor design but it is not implemented.

**Q7: Will LTE connections occur on a fixed schedule, or can the interval vary?**

The planned design supports variable intervals. The transmission settings schema includes `uploadIntervalMin`, `uploadPhaseUnix`, `minBatteryMv`, and `maxRetriesPerWindow`. However, no firmware logic evaluates battery voltage, error count, or deployment settings to decide whether to power on the modem.

**Q8: Should remote commands expire if they are not applied within a defined time?**

No expiration mechanism exists. The mothership stores `NodeDesiredConfig` in NVS with no TTL. Pending state changes have `pendingSinceMs` timestamps but no expiry logic. `processPendingStateCommand()` retries indefinitely with a 5-second throttle. Command expiry is important for FieldMesh deferred control mode and should be designed.

**Q9: Should the cloud initially be read-only, or is remote configuration required in the first backend release?**

This is a product decision, not a firmware constraint. The firmware architecture supports deferred remote configuration. Recommendation: start with read-only cloud in Release 2, add deferred control in Release 3.

**Q10: Can a monitoring location exist independently from a hardware node?**

Not in firmware. The current data model ties all records to the node's firmware ID (`nodeId` derived from MAC). There is no "location" abstraction in firmware. The cloud/backend should introduce a `monitoring_position` entity that maps to one or more hardware nodes over time.

**Q11: What is the current node state model and how does it persist across mothership restarts?**

Three states: `UNPAIRED → PAIRED → DEPLOYED`. Persisted to NVS with MAC, nodeId, nodeType, state, and battery voltage. Most runtime fields are not persisted and reset on reboot. After a mothership restart, the system loses: `configVersionApplied`, `lastReportedQueueDepth`, `inferredWakeIntervalMin`, `lastNodeTimestamp`, `syncStale`, `staleMissCount`, `lastTimeSyncMs`, `lastConfigPushMs`, all pending state tracking.

**Q12: What telemetry fields does the mothership currently store per node?**

The `NodeInfo` struct stores these per-node fields in RAM:

| Field | Type | Persisted to NVS? |
|-------|------|-------------------|
| `mac[6]` | uint8_t | ✅ Yes |
| `nodeId` | String | ✅ Yes |
| `nodeType` | String | ✅ Yes |
| `state` | NodeState | ✅ Yes |
| `lastReportedBatV` | float | ✅ Yes |
| `lastSeen` | uint32_t | ❌ |
| `isActive` | bool | ❌ |
| `wakeIntervalMin` | uint8_t | ❌ |
| `lastReportedQueueDepth` | uint8_t | ❌ |
| `inferredWakeIntervalMin` | uint8_t | ❌ |
| `lastNodeTimestamp` | uint32_t | ❌ |
| `configVersionApplied` | uint16_t | ❌ |
| `lastConfigPushMs` | uint32_t | ❌ |
| `lastTimeSyncMs` | uint32_t | ❌ |
| `deployPending` | bool | ❌ |
| `stateChangePending` | bool | ❌ |
| `pendingTargetState` | NodePendingState | ❌ |
| `pendingSinceMs` | uint32_t | ❌ |
| `pendingLastAttemptMs` | uint32_t | ❌ |
| `lastStateAppliedMs` | uint32_t | ❌ |
| `lastAppliedTargetState` | NodePendingState | ❌ |
| `syncStale` | bool | ❌ |
| `staleMissCount` | uint8_t | ❌ |
| `lastStaleAssistMs` | uint32_t | ❌ |
| `userId` | String | ✅ (separate NVS `node_meta`) |
| `name` | String | ✅ (separate NVS `node_meta`) |

**Q13: How does the ESP-NOW sync window work?**

Nodes flush their queues when they receive a `SYNC_WINDOW_OPEN` broadcast from the mothership, or when their DS3231 Alarm 2 fires at the scheduled sync time. If a node misses the marker, it won't flush that cycle. The stale-sync recovery mechanism provides a fallback after 24 hours.

**Q14: What is the current upload cursor / SD card management approach?**

No upload cursor exists. The SD card is append-only. `datalog.csv` grows indefinitely. There is no rotation, compaction, or upload-tracking mechanism.

**Q15: How does the mothership handle node configuration versioning?**

The mothership uses a `NodeDesiredConfig` struct per node, persisted in NVS. When a node's `NODE_HELLO` reports a `configVersion` lower than the desired version, the mothership pushes a `CONFIG_SNAPSHOT`. The node applies it and returns `CONFIG_ACK`. Version is a monotonically increasing `uint16_t` — no rollback mechanism.

**Q16: What happens when a node misses a sync window?**

The node keeps its queued data locally and waits for the next sync window. The mothership detects the missed sync via stale inference and attempts proactive assistance. The `local_queue` capacity is 24 snapshots. When the queue overflows, oldest records are dropped with `QF_DROPPED` flag set.

**Q17: Can the mothership store pending commands for nodes that are asleep?**

Yes. The `NodeDesiredConfig` + pending state system is specifically designed for this. The mothership stores desired config in NVS and pushes it when the node next contacts. Pending commands survive mothership restart for `NodeDesiredConfig` (NVS-persisted) but not for `stateChangePending` (RAM-only, lost on reboot).

**Q18: What is the current authentication model on the AP and BLE interfaces?**

Minimal. The WiFi AP uses a hardcoded password (`logger123`). BLE has no authentication — no pairing, no bonding, no encryption. The only security measure is a stale-rejection guard: BLE commands with `timestampUnix` > ±5 minutes from current time are rejected.

**Q19: How large is a typical datalog.csv row and how fast does the file grow?**

A typical CSV row is approximately 300–400 bytes.

| Nodes | Wake interval | Growth rate |
|-------|---------------|-------------|
| 1 | 5 min | ~115 KB/day |
| 10 | 5 min | ~1.15 MB/day |
| 10 | 1 min | ~5.76 MB/day |

**Q20: What is the current firmware version reporting mechanism?**

Firmware version is a compile-time `#define` (`FW_VERSION "v1.0.0"`) reported via the `/ui-status` JSON endpoint, BLE status, and serial boot log. Node firmware reports only a build timestamp (`FW_BUILD`), not a semantic version. Node firmware version is not reported to the mothership.

### Follow-Up Questions (2026-06-12)

**Q21: Does the mothership itself measure or receive its supply voltage?**

The PCB hardware provides a battery voltage divider on `VOLT` → `VOLT_ESP` (GPIO34, ADC1), but the mothership firmware **does not currently read it**. There is no ADC code in the mothership firmware. Without this, FieldMesh cannot distinguish LTE power suppression from other upload failures. The node firmware does read battery voltage, so a pattern exists to port.

**Recommendation:** Implement `analogRead(GPIO34)` with the documented divider scale factor. Include `batVoltage` in the mothership status JSON. The LTE upload planner should check battery voltage before enabling the modem rail.

**Q22: Can the app determine that the mothership recently rebooted?**

Partially. The mothership exposes `firmwareVersion` and `firmwareBuild` in its BLE status JSON, but there is **no boot counter, no boot timestamp persisted to NVS, and no reset reason** exposed to the app. No `esp_reset_reason()` call exists anywhere in mothership firmware.

**Recommendation:** Add a `bootCount` (persisted in NVS), a `lastBootUnix` (written at boot from RTC), and `esp_reset_reason()` to the status JSON. This would let the app detect "mothership just rebooted" and explain missing runtime telemetry.

**Q23: Can NODE_HELLO be extended to report node firmware semantic version?**

Not currently. The `node_hello_message_t` struct contains `nodeId`, `nodeType`, `configVersion`, `wakeIntervalMin`, `queueDepth`, and `rtcUnix` — but no firmware version field. The node firmware defines only `FW_BUILD` as a compile timestamp, no semantic version string. The struct has room for a `uint16_t fwVersion` or a compact semver encoding.

**Recommendation:** Add a `uint16_t fwSemanticVersion` field to `node_hello_message_t` (e.g., major:minor:patch packed as 5:5:6 bits). Also add a `#define FW_SEMANTIC_VERSION` to the node firmware. The mothership should track `fwSemanticVersion` per node for fleet version visibility.

**Q24: Can node queue capacity vary in future, or is 24 a fixed protocol assumption?**

The capacity is currently hardcoded as `kCapacity = 24` in `local_queue.cpp`, but it is **not a protocol-level constant**. The queue depth is reported at runtime via `NODE_HELLO.queueDepth` (uint8_t), and the mothership reads it dynamically. However, the NVS blob size is statically compiled (`sizeof(QueueBlob) < 4000`), so changing capacity requires a firmware update and invalidates existing NVS data. At 124 bytes per snapshot, 24 is near the practical max for a single NVS key.

**Recommendation:** Treat 24 as a firmware-internal constant, not a protocol guarantee. The `queueDepth` field in NODE_HELLO already communicates the actual depth at runtime. If capacity must grow, consider SD-backed queuing or a multi-key NVS scheme.

**Additional recommendation:** Add a `queueCapacity` field to `NODE_HELLO` alongside the existing `queueDepth`. Without this, FieldMesh cannot reliably calculate queue percentage or time until overflow. The struct has room for a `uint8_t queueCapacity` field. Alternatively, report queue capacity in node capability metadata exchanged during discovery or pairing.

**Q25: Should every uploaded CSV chunk include a mothership boot ID and upload-session ID?**

Not currently implemented. The CSV rows contain `ms_sync_unix` and `seq_num`, but there is no boot ID, no upload-session ID, and no mothership-side record identity. Without a boot ID, the cloud cannot distinguish data from two different boots that happen to have overlapping `ms_sync_unix` ranges (e.g., after RTC reset). Without a session ID, partial upload retries cannot be deduplicated.

**Recommendation:** Add a `boot_id` column to the CSV (e.g., `uint32_t` derived from boot count + RTC unix at boot). Add an `upload_session_id` to the LTE upload metadata (generated at modem session start). This improves diagnostics and deduplication with minimal overhead.

**Additional recommendation for deduplication:** The proposed cloud deduplication key `siteId + nodeId + seqNum` is only safe if `seqNum` never resets. If it resets after a node reboot, a later legitimate record could collide with an earlier one. A mothership `boot_id` does not fully solve this because the sequence number originates from the node. FieldMesh needs both a `node_boot_id` (persisted incrementing counter included in `NODE_HELLO`, `NODE_SNAPSHOT` and the CSV) and a `mothership_boot_id`. A safer reading identity would be: `siteId + nodeId + nodeBootId + seqNum`.

**Q26: How should uint16_t configuration-version rollover be handled?**

The current code handles rollover by wrapping to 1 when the version reaches `0xFFFF`. This is implemented consistently across multiple locations. However, the comparison logic has a bug: `handleNodeHello()` uses `desired.configVersion <= hello.configVersion` to decide whether to push config. This means:

- When `desired == hello`, it correctly skips (node already has this version).
- When `desired < hello`, it also skips. After rollover from 65535→1, if a node is at v65535 and the desired version is v1, the comparison `1 <= 65535` evaluates to true, so the push is skipped. The node never receives the new config.

This must be verified directly in the code and covered with tests for: 65534→65535, 65535→1, 1→2.

**Recommendation:** The cleanest solution is to change `configVersion` to `uint32_t`. It adds only two bytes per config record and makes rollover effectively irrelevant (4.3 billion increments). If `uint16_t` remains, use proper serial-number arithmetic (modular comparison) rather than a simple greater-than or less-than comparison. The wrap-to-1 scheme should also be changed to wrap-to-0-reserved: use 0 as "no config" and 1–65535 as the valid range, with rollover from 65535→1.

**Q27: Should destructive commands such as unpair require direct local presence?**

Currently, unpair does NOT require local presence. The BLE command router and web UI both allow unpair from local interfaces (BLE and Wi-Fi AP). The UNPAIR_NODE command is sent over ESP-NOW (best-effort, burst 3x), and the local registry is updated immediately regardless of delivery confirmation. For the current BLE-only path, physical proximity is implicitly required (BLE range ~10m). For future remote cloud paths, consider requiring a confirmation step.

**Recommendation:** For remote unpair, require either (a) the node must ACK the UNPAIR within a timeout, or (b) a "force unpair after N missed sync windows" policy. Document that unpair is a two-phase operation: local state changes immediately, remote state requires node contact. Strongly restrict or prevent remote unpair in the first cloud release.

**Q28: What happens when two cloud users queue conflicting configuration changes before the next LTE session?**

Not currently addressed. There is no cloud/LTE command path yet. The current system uses "last write wins" with a monotonically increasing `configVersion`. There is no conflict detection, queuing, or merge logic. When two users queue changes, the second write overwrites the first.

**Recommendation:** For the first LTE pass, adopt "last write wins" with a timestamp-based conflict note in the upload metadata. For a more robust solution, consider: (a) a command queue with per-command IDs and status tracking, (b) a "desired config" diff/merge layer that preserves both users' intent for non-overlapping fields, or (c) optimistic concurrency with a config-version check before applying cloud commands.

**Q29: Should the LTE session download commands before or after uploading telemetry?**

Not yet decided. The LTE backhaul concept document describes the upload flow in detail but does not specify a command-download phase.

**Recommendation:** A more reliable LTE session sequence would be:

1. Register and authenticate
2. Upload lightweight gateway/node status
3. Download pending commands
4. Validate and persist commands
5. Acknowledge command receipt
6. Upload CSV data
7. Upload any command results and final session status
8. Shut down modem

This lets the backend evaluate commands against current state (from step 2) while ensuring command receipt is tracked (step 5) before the modem switches off. Configuration changes cannot alter measurements already collected on the SD card, so uploading status before downloading commands does not create inconsistency.

**Q30: Can the mothership provide a reset reason, SD-card free capacity and current CSV file size in its status summary?**

Partially. The status JSON already includes SD card free capacity and CSV file size via the web UI's `buildDataStatusSectionHtml()`, but these are **not exposed in the BLE status JSON** (`buildBleStatusDataJson()`). Reset reason is not provided anywhere. No `esp_reset_reason()` call exists in mothership firmware.

**Recommendation:** Add to `buildBleStatusDataJson()`: `sdTotalBytes`, `sdFreeBytes`, `csvFileSize`, `csvRecordCount`, `resetReason` (from `esp_reset_reason()`), and `bootCount` (once implemented). This gives the app full visibility into storage health and boot history.

**Q31: What is the site-timezone model?**

Not defined. There is no timezone handling anywhere in the codebase. All timestamps are stored as UTC (unix epoch from DS3231 RTC). The web UI displays times using `gmtime()` (UTC formatting). There is no timezone offset, no IANA timezone name, and no daylight-saving logic. The `gSyncDailyHour` is described as "local daily sync trigger time" but there is no mechanism to convert between UTC and local time.

**Recommendation:** Store all data in UTC (already done). Add a `siteTimezone` field to the mothership configuration (IANA timezone name, e.g., `"Australia/Sydney"`). The firmware doesn't need full TZ logic — instead, the app/cloud can convert UTC timestamps to local time for display. For the daily sync schedule, store the target hour in UTC and let the app convert the user's local-time input to UTC when configuring.

**DST decision required:** Converting a local daily sync time into UTC when configuring the mothership works only if the schedule is intended to remain fixed in UTC. It will shift by one hour locally when daylight-saving time changes. FieldMesh must explicitly choose between:

- **Fixed UTC schedule:** Always run at 05:00 UTC. The displayed local time may change seasonally. Simpler firmware; no DST logic needed.
- **Fixed local-time schedule:** Always run at 06:00 Europe/Berlin. This requires FieldMesh or the backend to issue a corrected UTC schedule when the UTC offset changes, unless timezone handling is added to firmware.

For ecological monitoring, a fixed local-time schedule will often be more intuitive. This is a product decision that should be settled before Release 2.

**Q32: Should the mothership use file rotation before cloud integration?**

Not currently implemented. The mothership writes all data to a single `/datalog.csv` file with append. The LTE backhaul concept explicitly defers file rotation.

**Recommendation:** Implement **daily file rotation** before cloud integration. Name files by UTC date: `/data/2026-06-12.csv`. Rationale: (a) each day's file is a natural upload unit — upload the oldest complete file, then mark it uploaded; (b) corruption is isolated to one day; (c) the upload cursor becomes a simple file list rather than a byte offset; (d) daily files are manageable (~0.9–1.2 MB for 10 nodes at 5-min intervals) for a single LTE session. Writing each record into two files would create unnecessary complexity and additional SD writes. A cleaner model is: the current physical file is `/data/2026-06-12.csv`, and the legacy `/download-csv` endpoint streams or concatenates the required daily files. The API preserves backward compatibility without maintaining duplicate data files.

---

## 19. Backend Entity Model

The backend should explicitly introduce these entities:

```
organisation
user
role
site
deployment
mothership
monitoring_position
hardware_node
node_position_assignment
sensor_channel
raw_reading
derived_reading
calibration_record
configuration_desired
configuration_applied
command
command_event
alert_rule
alert_event
maintenance_event
annotation
upload_session
upload_cursor
```

The especially important relationship is:

```
monitoring_position
    ↓ assigned during a date range
hardware_node
```

A simplified assignment record:
```json
{
  "monitoringPositionId": "position-row4-east",
  "nodeId": "ENV_A213F2",
  "assignedFrom": "2026-06-16T09:30:00Z",
  "assignedUntil": null
}
```

---

## 20. FieldMesh MVP Roadmap

### Release 1 — FieldMesh Local

The first genuinely deliverable app. No cloud dependency required.

- BLE connection
- Wi-Fi connection
- Gateway status
- Discovery
- Pairing
- Deployment
- Configuration
- Node last report
- Queue depth
- Sensor presence
- RTC management
- CSV export
- Deployment readiness check
- Local activity history

### Release 2 — FieldMesh Cloud

Read-only from the device-control perspective.

- User accounts
- Organisations
- Sites
- Deployments
- LTE ingestion
- Raw CSV archive
- Normalised sensor data
- Gateway status summaries
- Historical charts
- Fleet health
- Alerts
- Data exports
- Monitoring positions

### Release 3 — FieldMesh Remote Control

- Cloud command queue
- HTTPS command download
- Command receipt acknowledgement
- Desired-versus-applied configuration
- Command TTL
- Cancellation
- Conflict handling
- Node acknowledgement upload
- Remote schedule changes

### Release 4 — Ecological Intelligence

- Data annotations
- Calibration management
- Treatment/reference comparisons
- Site maps and orthomosaics
- Seasonal and diel visualisations
- Environmental threshold rules
- Automated reports
- Deployment comparisons
- Data-quality scoring

### Features deferred beyond early MVP

These are useful but should not complicate the first releases:

- Firmware OTA
- MQTT live streaming
- Real-time WebSockets
- Complex role hierarchies
- Automated data cleaning
- Predictive battery estimates
- Drone orthomosaic integration
- Advanced GIS analysis
- Custom report builder
- Remote unpair or destructive commands
- Automatic adaptive LTE scheduling

The early product should be extremely reliable at:

1. Deploying hardware correctly.
2. Showing what the system actually knows.
3. Revealing how old that knowledge is.
4. Protecting and visualising environmental data.

---

## 21. Firmware Prerequisites for FieldMesh Cloud

Release 2 needs more than only an LTE CSV uploader. The following firmware work is required before cloud monitoring can deliver its promised features:

1. **Gateway battery ADC measurement** — implement `analogRead(GPIO34)` with documented divider scale factor; include `batVoltage` in status JSON
2. **Boot count, boot timestamp and reset reason** — persist `bootCount` in NVS, write `lastBootUnix` from RTC at boot, call `esp_reset_reason()` and expose in status JSON
3. **Compact LTE status-summary serializer** — JSON payload containing per-node: last report time, battery, queue depth, sensor-present bitmask, config version, RTC state, quality flags; plus gateway: boot count, reset reason, SD capacity, CSV stats
4. **Upload cursor and session tracking** — byte offset in datalog.csv persisted to SD or NVS; session ID generated at modem session start; upload outcome recorded
5. **Daily file rotation** — rotate CSV into `/data/YYYY-MM-DD.csv`; legacy `/download-csv` endpoint streams or concatenates daily files
6. **SD capacity and CSV statistics in BLE/API responses** — add `sdTotalBytes`, `sdFreeBytes`, `csvFileSize`, `csvRecordCount` to `buildBleStatusDataJson()`
7. **Node semantic firmware version** — add `uint16_t fwSemanticVersion` to `node_hello_message_t`; add `#define FW_SEMANTIC_VERSION` to node firmware; mothership tracks per node
8. **Node boot ID** — persisted incrementing counter in node NVS; included in `NODE_HELLO`, `NODE_SNAPSHOT` and CSV; used for cloud deduplication alongside `seqNum`
9. **Queue capacity reporting** — add `uint8_t queueCapacity` to `NODE_HELLO` alongside `queueDepth`
10. **Secure local enrolment** — unique device setup code, changeable Wi-Fi credential, BLE secure pairing and bonding, first-owner enrolment, local session authentication
11. **Site/deployment identity storage** — `siteId` and `deploymentId` fields in mothership NVS; included in LTE status summary and CSV rows
12. **LTE session outcome and error reporting** — record modem state transitions, registration outcome, upload success/failure, bytes transferred, session duration; persist to SD for diagnostics

Without the compact status summary, the cloud cannot provide many of the promised fleet-health features from CSV uploads alone.

---

## 22. Ecological Branding Direction

FieldMesh could use a visual identity based on:
- Interconnected root, fungal or leaf-vein structures
- Sensor nodes represented as points within a natural network
- Muted forest, soil, moss and water tones
- Clear scientific charts rather than heavily decorative screens
- A logo that combines a leaf structure with a connected-node network

Possible taglines:
- **Environmental sensing, connected**
- **From field sensor to ecological insight**
- **Connected monitoring for changing environments**
- **Field networks. Reliable data. Better decisions.**
- **Understand the environment, from every node**

Preferred combination:

> **FieldMesh**
> **From field sensor to ecological insight.**

---

## 23. Next Step

The brainstorming phase is now largely complete. The next useful document should be a **FieldMesh Release 1 Product Requirements Document**, containing:

- Exact screens
- User actions
- UI states
- BLE/Wi-Fi command mapping
- Failure states
- Acceptance criteria
- Firmware gaps
- Features explicitly excluded from Release 1

This would convert this broad integration brief into something an app developer can implement without making product decisions along the way.

---

*Sections 1–8 contain facts derived from the current codebase and design documents. Sections 9–23 contain product direction, design recommendations, and firmware Q&A for the FieldMesh app and backend team.*