# FieldMesh — Mothership V2 & Node V2 Firmware Flow Summary

**Date:** 2026-07-03
**Purpose:** Generic, repository-grounded description of how the mothership and sensor nodes operate end-to-end. Intended as source material for an LLM generating About-page copy, website content, or documentation. No code is required to interpret this document — it describes behaviour, not implementation.

**Companion docs:**
- `docs/FIELDMESH_ABOUT_CONTENT.md` — existing About-page content (predecessor)
- `docs/concept_overview.md` — user-facing system overview
- `docs/FIELDMESH_COORDINATED_SYNC_PROTOCOL.md` — sync protocol reference
- `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` — JSON upload protocol spec

---

## 1. System at a glance

FieldMesh is a battery-powered, low-cost environmental sensor network built on ESP32 microcontrollers. It is designed for long-term ecological and environmental monitoring at remote sites without mains power or reliable internet.

There are two hardware tiers:

1. **Sensor nodes** — small, self-contained, battery-powered field devices. Each node sleeps almost all the time, wakes on a DS3231 real-time-clock alarm, samples its attached sensors, queues the reading locally, and — during a scheduled sync window — transmits a compact binary snapshot to the mothership over ESP-NOW (a low-power Wi-Fi protocol on channel 11). The node then powers off completely until the next alarm.

2. **Mothership (FieldHub)** — a central on-site hub. It wakes on its own RTC alarm, opens a coordinated sync window, collects snapshots from all deployed nodes, logs them to local flash storage (LittleFS) as a CSV file, and periodically uploads accumulated data to the cloud over LTE (via a SIMCom A7670G Cat-1 cellular modem with SSL/TLS). It also hosts a captive Wi-Fi portal for field configuration — pairing nodes, setting recording intervals, and starting deployments.

The cloud backend is currently **Google Sheets via Google Apps Script**, with a documented migration path to **Supabase** (PostgreSQL + Edge Functions + row-level security). A web dashboard (React + Vite) visualises readings, system health, and node status.

---

## 2. Node V2 firmware flow

### 2.1 Boot sequence

1. **Assert power hold** — the very first action is to drive the `PWR_HOLD` pin high. This latches the power rail on so the board does not lose power mid-boot. Without this, the board would power off immediately.
2. **Initialise serial, watchdog, NVS** — a hardware-timer watchdog is armed so an I2C bus stall or sensor hang reboots the node instead of leaving it "on" and draining the battery.
3. **Generate node identity** — by default the node ID is derived from the last three bytes of its Wi-Fi MAC address (e.g. `ENV_A3F2B1`), so each board is unique without per-device firmware edits.
4. **Load persistent config from NVS** — the node reloads its state: mothership MAC, deployed flag, wake interval, sync interval, sync phase, last time-sync timestamp, applied config version, and recording-paused (standby) flag.
5. **Initialise I2C bus and RTC** — a single I2C bus serves the DS3231 RTC, a PCA9548A multiplexer, and an ADS1115 ADC. If the RTC lost power (coin cell dead), the node marks itself unsynced and disables alarms until a time sync is received.
6. **Re-arm alarms (if deployed and synced)** — if the node was deployed and the RTC is valid, it re-arms both the recording alarm (A1) and the sync alarm (A2) from stored intervals, so a reboot can never leave the node without a future wake.
7. **Initialise sensors** — the sensor registry auto-detects self-identifying I2C sensors (SHT41, AS7343 spectral) and gates passive sensors (soil, wind, AUX) against an operator-configured sensor mask.
8. **Rescue-mode check** — if the node has rebooted three times within 20 seconds (a deliberate user gesture), it wipes its config to UNPAIRED, turns the radio on, and listens for re-pairing for 15 minutes.
9. **Shut down radio if deployed** — in deployed mode, Wi-Fi/ESP-NOW is kept off except during sync windows to save power.

### 2.2 Node states

A node is always in one of three states:

| State | Meaning |
|---|---|
| **UNPAIRED** | No mothership MAC known. Radio listens for pairing. No alarms armed. |
| **PAIRED** | Mothership MAC known but not yet deployed. Radio listens; no sampling. |
| **DEPLOYED** | Bound to a mothership, RTC synced, alarms armed. Samples on A1, syncs on A2. |

A fourth implicit mode — **standby** — is a DEPLOYED node with `recordingPaused = true`: it keeps its sync check-ins (A2) but does not arm the recording alarm (A1) and takes no samples.

### 2.3 Data wake (Alarm 1 — recording)

When the DS3231 Alarm 1 fires:

1. The node clears the A1 flag.
2. If recording is paused (standby), the wake is ignored — no sampling.
3. Otherwise, the node reads all registered sensors into a V2 snapshot structure:
   - Battery voltage (dedicated ADC read with divider scaling)
   - Air temperature and humidity (SHT41)
   - 8-channel spectral light 415–680 nm (AS7343)
   - Wind speed/direction (ultrasonic anemometer or reed-cup anemometer, mask-gated)
   - Soil volumetric water content and temperature for two probes (ADS1115 + VH-5)
   - Two auxiliary I2C expansion channels (reserved)
4. The snapshot is **enqueued locally** (not transmitted yet). The radio stays off during a data wake — sampling is silent and low-power.
5. If the last time sync is older than 24 hours, a **stale-sync recovery** runs: the node briefly brings up ESP-NOW, sends a HELLO and a time-sync request, and flushes its queue if it hears a sync marker.
6. The node re-arms A1 (next recording) and A2 (next sync) **before** the hang-prone sensor work is done, so a stall can never leave the DS3231 without a future alarm.
7. The node releases `PWR_HOLD` and powers off completely.

### 2.4 Sync wake (Alarm 2 — coordinated sync)

When the DS3231 Alarm 2 fires at the scheduled sync slot:

1. The node clears the A2 flag and brings up ESP-NOW.
2. It listens for a `SYNC_SESSION_OPEN` broadcast from the mothership (or a legacy `SYNC_WINDOW_OPEN` marker for backward compatibility).
3. On hearing the session open, the node waits a deterministic, per-node jitter delay (hash of node ID + session ID) to spread HELLO responses, then sends a `NODE_HELLO` carrying its node ID, config version, wake interval, and local queue depth.
4. The mothership responds with per-node `NODE_CONFIG` (declarative desired state: recording interval, target state, sensor mask, monotonic config version) and a `SET_SYNC_SCHED` (sync interval + phase anchor).
5. The mothership sends a `DUMP_GRANT` giving the node a time-limited window and a record quota to transmit queued snapshots.
6. The node flushes its local queue to the mothership, one snapshot at a time, each requiring a **durable `SNAPSHOT_ACK`** (persisted flag) before the record is popped. Lost ACKs trigger bounded retries with per-node backoff.
7. When the queue is drained (or the grant window expires), the node sends `DUMP_DONE` with the count sent and remaining.
8. The mothership sends `SYNC_RELEASE` carrying the mothership's current Unix time, the active sync interval, and the phase anchor. The node applies this (updating its RTC, sync interval, and phase), persists config, and ACKs with `RELEASE_ACK`.
9. The node re-arms A1 and A2 from the freshly applied schedule, shuts down ESP-NOW, and releases `PWR_HOLD`.

### 2.5 Pairing and deployment flow

- **Discovery** — an UNPAIRED node can broadcast a `DISCOVER_REQUEST`; the mothership answers with a `DISCOVERY_SCAN`. Discovery is for visibility only — it does not bind.
- **Pairing** — the operator triggers `PAIR_NODE` from the Field UI. The node stores the mothership MAC, clears its queue, and moves to PAIRED. It does not sample until deployed.
- **Deployment** — the operator triggers `DEPLOY_NODE` with a timestamp, wake interval, sync interval, and sync phase. The node sets its RTC, marks itself DEPLOYED, arms both alarms, and begins autonomous sampling. A `DEPLOY_ACK` confirms to the mothership.
- **Config updates** — `NODE_CONFIG` delivers a new recording interval, target state (DEPLOYED / STANDBY / UNPAIRED), or sensor mask. The node applies only a strictly newer config version and ACKs with `CONFIG_ACK`. An UNPAIRED ACK is positive confirmation the node has wiped, so the mothership removes it from the registry.
- **Unpair** — `UNPAIR_NODE` wipes the mothership MAC, disables alarms, clears the queue, and holds the radio on for 15 minutes for re-pairing before auto power-off.

### 2.6 Robustness features

- **NVS is the source of truth** — all state survives hard power cuts; RTC RAM is not trusted.
- **Hardware watchdog** — a 120-second one-shot timer ISR reboots the node if the main loop or a blocking I2C call hangs.
- **Pre-emptive alarm re-arm** — alarms are re-armed before any hang-prone work, so a stall never strands a node.
- **Bounded retry on alarm arm failure** — up to 3 attempts with I2C re-init; if all fail, the node stays awake and signals a fault rather than sleeping without an alarm.
- **Stale-sync recovery** — if no time sync has been received in 24 hours, the node opportunistically seeks one during a data wake.
- **Rescue mode** — three rapid boots (a deliberate user gesture) wipe config and listen for re-pairing, recovering a node that has lost its mothership or corrupted its config.

---

## 3. Mothership V2 firmware flow

### 3.1 Boot sequence

1. **Assert `PWR_HOLD`** (GPIO26) — critical first action; latches the switched rail on.
2. **Initialise serial, RTC, rescue alarm** — a conservative fallback RTC alarm is armed before any long-running work, so a crash mid-session still wakes the board later.
3. **Record boot diagnostics** — reset reason and a monotonic boot counter are stored in NVS. Repeated BROWNOUT/WDT resets are an early-warning signal for power or firmware problems.
4. **Record project-start timestamp** — the first boot with a valid RTC time is stored once in NVS and never overwritten, giving a permanent "project started" anchor for the dashboard.
5. **Detect wake reason** — three independent wake sources are classified:
   - **RTC alarm** → sync wake
   - **Config button** (latched) → config wake
   - **USB service** → service wake
   Config wins if both the button latch and an RTC alarm are active.
6. **Branch to the appropriate handler.**

### 3.2 Sync wake (the main operational cycle)

This is the heart of the mothership. On every RTC alarm:

1. **Load schedule from NVS** — wake interval, sync mode (interval or daily), daily sync time, and the active sync anchor (interval + phase).
2. **Detect schedule transitions** — if the operator changed the wake interval during config mode, the persisted anchor still holds the *old* sync interval that the sleeping fleet's A2 alarms are aligned to. The mothership must wake at the *old* schedule to meet the nodes, hand them the *new* schedule during the window, then re-anchor to the new schedule. Three "legacy rendezvous" grace cycles are persisted so a node that misses the first handover is not stranded.
3. **Initialise storage** — SD card (if present) and LittleFS flash. Flash is the working primary store; SD is secondary. The upload queue is initialised and emergency-purged if over 80% full.
4. **Load paired nodes** — the node registry (MACs, IDs, names, states, expected sensor masks, last-seen timestamps, battery voltages) is loaded from NVS.
5. **Initialise ESP-NOW in sync-only mode** — listen on channel 11; snapshot queue depth 32.
6. **Build per-node `NODE_CONFIG` broadcasts** — each deployed node gets a declarative config frame carrying its desired recording interval, target state, sensor mask, and monotonic config version. Re-broadcasting every cadence tick is idempotent because nodes apply only a strictly newer version.
7. **Run the coordinated sync window** (see §3.3).
8. **Persist node state** — freshly reported battery voltages, last-seen times, and config-version convergence are saved to NVS.
9. **Process `CONFIG_ACK`s** — an UNPAIRED ACK confirms a node has wiped and is removed from the registry; a DEPLOYED/STANDBY ACK confirms convergence and clears the pending-change flag for the UI.
10. **Shut down ESP-NOW** before any file-system work touches the upload queue.
11. **LTE upload phase** (conditional on transmission settings — see §3.4).
12. **Re-arm the RTC alarm** according to the configured schedule mode (interval or daily), honouring any schedule transition and legacy grace cycles.
13. **Verify the alarm is set** before power-down; on failure, run bounded recovery (3 attempts with I2C re-init).
14. **Release `PWR_HOLD`** — the board powers off completely.

A hard 5-minute session watchdog caps the entire sync wake. If exceeded at any checkpoint, the mothership forces alarm re-arm and shutdown rather than running the battery down.

### 3.3 Coordinated sync window

The sync window is a structured multi-phase rendezvous:

1. **Rendezvous / join window (~12 s)** — the mothership broadcasts `SYNC_SESSION_OPEN` (with session ID, join window, session window) every second, plus `NODE_CONFIG` bursts and `SET_SYNC_SCHED` (active sync interval + phase) every 6 seconds. Nodes respond with `NODE_HELLO`. The mothership collects HELLOs and builds a responder roster, authorising only registered, deployed nodes with matching MAC and node ID.

2. **Empty-node release** — nodes reporting queue depth 0 are clock-synchronised and released immediately with `SYNC_RELEASE` (carrying mothership Unix time, sync interval, phase, and legacy grace cycles). The node ACKs with `RELEASE_ACK`.

3. **Grant round-robin** — for nodes with queued data, the mothership sends `DUMP_GRANT` frames one at a time, each with a grant ID, record quota (default 4), and a ~9-second window. The node flushes snapshots within the grant and replies with `DUMP_DONE` (sent count, remaining count, status). The mothership advances to the next node, re-granting the same node if it still has data, until the grant budget is exhausted or all nodes are drained.

4. **Final release** — every responder is released individually, even if backlog remains. The node keeps unsent records but still receives time and the active schedule, so the next sync window picks up where this one left off.

5. **Snapshot processing** — every received snapshot is decoded (V1 or V2), logged to flash CSV, and ACKed with `SNAPSHOT_ACK` (persisted flag). The node's registry entry is updated with last-seen time, battery voltage, last node timestamp, and applied config version. Configured-sensor fault detection flags a sensor after two consecutive missing channels, so a single transient read does not flap the dashboard.

### 3.4 LTE upload phase

After the sync window closes and ESP-NOW is shut down, the mothership may upload accumulated data to the cloud. This is entirely conditional on transmission settings and never blocks the sync wake from completing — local logging is always primary.

1. **Policy check** — `uploadIntervalMin = 0` means upload every wake; otherwise the mothership counts how many sync wakes to skip. A wake counter in NVS tracks this.
2. **Battery check** — resting battery voltage is sampled *before* the modem rail comes up (the A7670G draws amp-level current during TX and sags the rail). If below `minBatteryMv`, upload is skipped.
3. **Retry check** — if max retries per window is exceeded, upload is skipped for this wake.
4. **Modem power-on** — the A7670G is powered via a TPS63020 buck-boost with PWM soft-start. The modem is initialised over UART (Serial2 at 115200 baud).
5. **Network registration** — waits up to 60 seconds for cellular network registration. On failure, the retry counter is incremented with a cooldown and the modem is gracefully shut down.
6. **JSON upload path** (preferred when `useJsonUpload` is true):
   - The mothership builds a **status context** once per session: resting battery, flash usage, fleet counts (total/deployed/paired/unpaired/paused/pending), sync schedule, firmware version, modem diagnostics (signal quality, IMEI, registration time), boot diagnostics (reset reason, boot count, free heap), and a per-node status JSON.
   - Data is chunked (~16 KB per POST, ~100–130 rows). The first POST carries the full status object; subsequent chunks carry readings only.
   - Each chunk is POSTed as a JSON array with Bearer auth (Supabase) or query-param auth (Google Apps Script).
   - On HTTP 200, the upload cursor is advanced and purged. On 400/401 (non-retryable), the cursor is not advanced and the retry counter is not incremented. On 429/5xx/transport error, the retry counter is incremented with backoff.
   - A CSV fallback path exists for chunks where the JSON builder fails (e.g. heap exhaustion).
7. **Emergency purge** — if flash is over 80% full, the oldest uploaded data is purged regardless of upload success.
8. **Graceful modem shutdown** — the modem is properly powered down before the mothership releases `PWR_HOLD`.

### 3.5 Config wake (config button)

When the config button is pressed (latched by hardware):

1. The mothership brings up Wi-Fi AP (`Logger001` on channel 11) and a web server at `192.168.4.1`.
2. The **Field UI** captive portal is served: dashboard, node manager, per-node config, transmission settings, CSV download, time set, discover nodes, set wake interval, shutdown.
3. The operator can pair/discover nodes, set recording intervals, configure transmission settings, set the RTC time, and start deployments.
4. On shutdown (web UI button or 30-minute timeout), the mothership saves NVS, re-arms the RTC alarm (honouring schedule transitions so it wakes at the *old* schedule to meet the fleet), and releases `PWR_HOLD`.

### 3.6 Service wake (USB)

If the board powers on with USB connected and no config button or RTC alarm, it simply logs the condition and powers off. This prevents accidental idle-on from USB powering the rail.

---

## 4. Data flow end-to-end

```
Sensor → Node ADC/I2C → V2 snapshot (local queue) → ESP-NOW → Mothership
    → LittleFS CSV (datalog.csv) → Upload queue (cursor-tracked)
    → JSON payload builder → HTTPS POST over LTE → Cloud backend
    → Web dashboard
```

- **Node local queue** — snapshots are stored in a ring/linear queue in NVS-backed storage. A snapshot is only popped after a durable `SNAPSHOT_ACK` (persisted flag) from the mothership. Lost ACKs retain the record for the next sync window.
- **Mothership flash logger** — `logDecodedSnapshot()` writes one CSV row per snapshot to `/datalog.csv` on LittleFS, with a 25-column header. V2 snapshots are decoded and the V1 `sensorPresent` bitmask is synthesised for CSV compatibility.
- **Upload queue** — `UploadQueue` tracks a byte cursor, rows uploaded, retry count, and next-attempt time in NVS. It reads new data from the CSV file without re-reading already-uploaded rows. A/B-slot recovery with backup/temp/commit pattern protects against corruption.
- **JSON payload** — `buildJsonUpload()` converts CSV rows to structured JSON `{meta, status, readings}`, omitting NaN fields. The status object is sent once per session on the first POST.

---

## 5. What FieldMesh measures

Per the V2 snapshot protocol, each node can report:

| Channel | Sensor | Unit |
|---|---|---|
| Air temperature | SHT41 | °C |
| Air relative humidity | SHT41 | % |
| Spectral light (8 channels) | AS7343 | raw counts at 415/445/480/515/555/590/630/680 nm |
| Wind speed | Ultrasonic anemometer or reed-cup | m/s |
| Wind direction | Ultrasonic anemometer | degrees |
| Soil volumetric water content (×2) | ADS1115 + VH-5 probes | m³/m³ |
| Soil temperature (×2) | ADS1115 + thermistor | °C |
| Battery voltage | Node ADC | V |
| Auxiliary I2C (×2) | Expansion ports | reserved |

Passive sensors (soil, wind, AUX) are gated by an operator-configured sensor mask; self-identifying I2C sensors (SHT41, AS7343) are always auto-detected.

Each node carries three bitmasks that let the dashboard distinguish a faulty sensor from a paused node:

- **Configured mask** — what the operator has told the node to capture.
- **Reported mask** — what the node actually reported in the latest snapshot (a channel is only present on a successful read).
- **Fault mask** — a channel that's configured but missing for two or more consecutive uploads, flagged as faulty rather than just showing a gap in the data.

The AS7343 spectral sensor also reports five metadata channels alongside the 8 spectral bands: broadband clear, near-infrared, acquisition gain, integration time, and a saturation flag. Gain and integration time enable a calibration-free "basic counts" normalisation so spectral readings are comparable across different acquisition settings; saturated snapshots can be flagged and excluded from derived vegetation-index or canopy metrics.

---

## 6. Power and wake architecture

Both boards use the same fundamental pattern:

- **`PWR_HOLD` pin** — a GPIO that drives a FET gate to latch the switched rail on. Asserted at the very first instruction of boot. Released at the end of a wake cycle to power the board off completely. This is the only wake path: the DS3231 alarm drives the FET gate to switch VSYS on.
- **DS3231 RTC** — the sole wake source. Alarm 1 (recording, second-resolution) and Alarm 2 (sync, minute-resolution) are independently armed and verified. The RTC is backed by a coin cell so time survives main power loss.
- **No deep-sleep fallback** — the firmware explicitly does not use ESP32 deep sleep, because the wake path is the RTC-driven FET, not a GPIO interrupt. If `PWR_HOLD` release fails to cut power (hardware fault), the board spins with the hold released rather than sleeping, and the watchdog eventually reboots it.
- **Three mothership wake sources** — RTC alarm (sync), config button (latched by a SN74LVC2G74 D-flip-flop), and USB service path.
- **Rescue alarm** — a conservative fallback alarm armed at boot before any long-running work, so a crash mid-session still wakes the board.

---

## 7. Sync scheduling model

The fleet operates on a **phase-aligned interval schedule**:

- The mothership owns a **sync anchor**: an interval (in minutes) and a phase (a Unix timestamp marking a slot boundary). All sync wakes land on `phase + N × interval`.
- Nodes receive the active schedule via `SET_SYNC_SCHED` during each sync window and arm their A2 alarm to the next slot boundary.
- The mothership re-arms its own alarm to the same phase, so both sides converge on identical slot boundaries.
- **Schedule transitions** are handled gracefully: when the operator changes the wake interval in config mode, the mothership keeps waking on the *old* schedule for up to three grace cycles (legacy rendezvous) while handing nodes the *new* schedule, so no node is stranded on an orphaned cadence.
- **Daily mode** is a special case where the sync interval is 0 and the phase carries a daily HH:MM anchor; the mothership arms a daily alarm at that time.

---

## 8. Cloud and dashboard

The cloud tier is where field telemetry becomes visible and manageable. It has two layers: the **backend** that ingests, stores, and serves data, and the **frontend dashboard** that users interact with.

### 8.1 Backend

Two backends coexist behind a **data-provider boundary** — an abstraction layer that lets the frontend read from either backend without rewriting pages, enabling a staged, rollback-safe migration.

**Legacy — Google Apps Script / Google Sheets**

- The original API layer (`Code.gs`) reads and writes from Google Sheets.
- The mothership POSTs CSV or JSON with an auth token; the script appends rows to a sheet.
- Retained for rollback safety during the Supabase cutover.

**Current — Supabase (PostgreSQL + Edge Functions)**

- **Multi-tenant Postgres schema** with a `projects → motherships → nodes → readings` hierarchy. Every tenant-owned table carries an explicit `project_id` for isolation.
- **Row-Level Security (RLS)** policies on all browser-accessible tables, tested via `rls.test.sql`. This means a user can only ever read or write data belonging to their own project, enforced at the database level — not just in application code.
- **Device authentication** using hashed API keys (`device_api_keys` + `private.device_api_key_secrets`). Secrets are never exposed to the browser. Credential lifecycle events (issuance, rotation, revocation) are audited in `device_credential_events`.
- **Edge Functions (Deno):**
  - `ingest-fieldmesh` — the authenticated telemetry ingestion endpoint. Validates Bearer tokens, enforces payload size limits, normalises payloads, and writes readings with idempotent, retry-safe semantics (so a duplicate upload from a mothership retry does not create duplicate rows).
  - `issue-device-key` — issues and rotates device API keys.
- **Version-controlled migrations** for schema evolution, including modem diagnostics, pause/battery health, sensor mask support, and AS7341 spectral raw channels.
- **Project sharing** — a `project_members` table lets a project owner grant read-only access to other users (e.g. a colleague or client viewing a site's data) without exposing device credentials or allowing writes. Row-level security policies ensure a shared viewer can see a project's nodes, readings, status, config, and motherships — and nothing else.
- **Realtime** delivery of telemetry updates to connected dashboard clients — when a mothership uploads, any open dashboard sees the new readings without polling.

The JSON upload protocol emitted by the mothership firmware is designed for this backend: Bearer auth, JSON array body, no query params, status object on the first POST of each session.

### 8.2 Frontend dashboard

A single-page React application built with **Vite**, **Tailwind CSS**, and **TanStack Query**.

**Architecture:**

- **React 18 + React Router** with authenticated and project-scoped routes. Users log in, select a project, then access dashboards, charts, node management, configuration, and mothership device pages.
- **Lazy-loaded pages** (Charts, Nodes, Config, Motherships, Settings, About) with Suspense fallbacks and an ErrorBoundary for resilient navigation — so a failure in one page does not take down the whole app.
- **Data-provider boundary** — the app reads from either the legacy Google Apps Script backend or the new Supabase backend without rewriting pages, enabling a staged, rollback-safe migration.
- **Supabase Auth** integration via an AuthGate and AuthProvider, with login, signup, and password reset flows.
- Full attention to **loading, empty, stale, error, offline, and permission states** across features — every data view handles the case where data is not yet loaded, not available, or the user lacks access.

**Visualisation and interaction:**

- **Recharts** for time-series visualisation of sensor telemetry, with expensive chart bundles kept out of the main chunk for fast initial load.
- **Leaflet / React-Leaflet** for geospatial display of deployed nodes on a map.
- **Framer Motion** for transitions and a nature-inspired design language defined in a dedicated UI style guide.
- **QR code generation** (`qrcode.react`) for on-device provisioning flows — a node or mothership can be provisioned by scanning a QR code that carries its credentials.

**What the dashboard shows:**

- Sensor readings (time-series charts and map views)
- Node status (deployed / paired / unpaired / paused / pending state changes)
- Battery voltages (per-node, over time)
- Sync schedule (next sync time, mode, interval)
- Flash usage and upload queue depth
- Modem diagnostics (signal quality, registration time, IMEI)
- Boot diagnostics (reset reason, boot count, free heap)
- Per-node status (last seen, battery, config version, sensor fault mask)
- Mothership device pages (per-device detail and configuration)

### 8.3 Status object

Every mothership upload session carries a rich **status context** sent once on the first POST of the session. This is what populates the dashboard's system-health views:

- Fleet counts: total, deployed, paired, unpaired, paused, pending state changes
- Battery: resting voltage (sampled before modem power-on) and loaded voltage (sampled during TX) — the sag between the two is a battery/regulator health signal
- Flash usage: total bytes, used bytes, percentage
- Sync schedule: next sync time (ISO local), mode (interval or daily), wake interval, sync interval
- Firmware version and build timestamp
- Modem diagnostics: signal strength (RSSI, RSRP, RSRQ), SIM identity (ICCID), carrier/operator, network type (LTE vs. 2G), registration time, IMEI
- Boot diagnostics: reset reason, monotonic boot count, free heap, minimum free heap, snapshot queue drops — so you can tell the difference between "the sensor stopped working" and "the modem lost signal"
- Per-node status: last seen, last reported battery, config version applied, sensor fault mask (configured vs. reported vs. missing-for-two-uploads)
- Node paused/standby count across the fleet — so you can tell which sensors are intentionally not recording versus which have gone silent
- Battery voltage under load (sampled during modem TX) — a much better indicator of real battery health than a resting voltage reading alone
- AS7341 spectral metadata: broadband clear, near-infrared, acquisition gain, integration time, and a saturation flag — enabling calibration-free "basic counts" normalisation and quality filtering of spectral readings
- Upload cursor: pending rows, rows uploaded, retry count, last upload Unix time, last result (success / failed / pending)
- Project-start timestamp (first-ever boot, stored once, never overwritten)

### 8.4 Tech stack summary

| Layer | Technologies |
|---|---|
| Frontend | React 18, Vite, Tailwind CSS, TanStack Query, Recharts, React-Leaflet, Framer Motion, React Router, Lucide |
| Backend (current) | Supabase (Postgres, Auth, RLS, Realtime, Edge Functions / Deno) |
| Backend (legacy) | Google Apps Script, Google Sheets |
| Testing | Vitest, Testing Library, Supabase SQL tests |
| Device | ESP32 firmware, JSON telemetry over HTTPS, QR-provisioned credentials |

---

## 9. Design principles

- **Local-first** — all data is logged locally before any upload. Upload failure never causes data loss; the upload queue is cursor-tracked and retryable.
- **Power-off is the default** — both boards spend almost all their time fully powered off. The only active time is the brief wake cycle (seconds for nodes, minutes for the mothership).
- **NVS is the source of truth** — all persistent state survives hard power cuts. RTC RAM is not trusted.
- **Durable ACKs** — a snapshot is only popped from the node's queue after the mothership confirms it was persisted to flash, not just received over the air.
- **Bounded recovery** — every critical operation (alarm arm, RTC init) has a bounded retry with a defined fallback, so a transient I2C failure never strands a device.
- **Coordinated, not contention-based** — the sync window is a structured grant-based protocol, not a free-for-all. The mothership schedules each node's transmit window to avoid collisions and ensure fair queue draining.
- **Backward compatibility** — V1 snapshots, legacy sync markers, and rolling-upgrade fallbacks are all handled, so a mixed-version fleet still operates.