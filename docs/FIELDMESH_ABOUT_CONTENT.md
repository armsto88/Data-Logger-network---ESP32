# FieldMesh — About Section Content

**Date:** 2026-06-28
**Status:** Repository-backed content document for the FieldMesh web application About page
**Audience:** Frontend team implementing the About section
**Companion docs:**
- `docs/FIELDMESH_ABOUT_SECTION_DESIGN.md` — About page visual/interaction design
- `docs/FIELDMESH_DASHBOARD_DESIGN.md` — dashboard architecture
- `docs/FIELDMESH_UI_STYLE_GUIDE_2026.md` — design tokens and colour system
- `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` — JSON upload protocol spec
- `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md` — Supabase migration plan
- `docs/FIELDMESH_USER_ONBOARDING_BRIEF.md` — multi-tenant onboarding

---

## How to use this document

This document provides the **text content** for the FieldMesh About page at three levels of complexity, plus structural and visual recommendations. It is grounded entirely in the actual repository — firmware source, design notes, and bring-up logs — not assumptions.

**Status labels used throughout:**
- ✅ **Implemented** — confirmed working in firmware and/or verified on hardware
- 🟡 **Partial** — code exists but is incomplete, stubbed, or not yet validated end-to-end
- 🔵 **Planned** — documented in a design note but not yet implemented in firmware
- ⚪ **Architectural intention** — described in design notes as a target state, no code yet

---

## 1. Repository-backed system summary

**FieldMesh** is a battery-powered, low-cost environmental sensor network built on ESP32 microcontrollers. It is designed for ecological field research, environmental monitoring, and long-term deployment at remote sites without reliable internet or mains power.

The system has two hardware tiers:

1. **Sensor nodes** — small, self-contained, battery-powered devices deployed in the field. Each node wakes on a configurable interval (driven by a DS3231 real-time clock alarm), reads its attached sensors, and transmits a compact binary snapshot to the mothership over ESP-NOW (a low-power Wi-Fi protocol on channel 11). The node then powers off completely until the next alarm.

2. **Mothership (FieldHub)** — a central hub that stays on-site. It receives snapshots from all deployed nodes via ESP-NOW, logs them to local flash storage (LittleFS) as a CSV file, and periodically uploads accumulated data to the cloud over LTE (via a SIMCom A7670G Cat-1 cellular modem with SSL/TLS). It also hosts a captive Wi-Fi portal (`192.168.4.1`) for field configuration — pairing nodes, setting recording intervals, and starting deployments.

The cloud backend is currently **Google Sheets via Google Apps Script**. A migration to **Supabase** (PostgreSQL + Edge Functions + real-time subscriptions + row-level security) is planned and documented but not yet implemented. A multi-tenant model (user accounts, projects, per-device API keys, QR provisioning) is designed but not yet built.

The web dashboard is a **React + Vite** frontend that visualises sensor readings, system health, and node status. It currently polls the Google Apps Script backend every 60 seconds.

**What FieldMesh measures** (per the V2 snapshot protocol in `node/firmware/v2/shared/protocol.h`):
- Air temperature (°C) — SHT41
- Air relative humidity (%) — SHT41
- Spectral light, 8 channels from 415 nm to 680 nm (raw counts) — AS7341
- Wind speed (m/s) and direction (degrees) — ultrasonic anemometer (stub backend)
- Soil volumetric water content (m³/m³) for two probes — ADS1115 + VH-5 probes
- Soil temperature (°C) for two probes — ADS1115 + thermistor
- Battery voltage (V) — node ADC
- Two auxiliary I2C expansion ports (reserved for future sensors)

---

## 2. Confirmed current capabilities

These features are implemented in firmware and, where noted, verified on hardware per `mothership/docs/MOTHERSHIP_V1_BRINGUP_RESULTS_2026-06-19.md`.

### Hardware / power
- ✅ **Mothership power gating** — `PWR_HOLD` (GPIO26) holds the switched rail on after boot; releasing it powers the board off completely. Verified on hardware (Test 1).
- ✅ **Three wake sources on the mothership** — DS3231 RTC alarm, config button (latched), and USB service path. All three confirmed wake-from-off (Test 7).
- ✅ **DS3231 RTC alarm programming** — I2C on GPIO21/22; alarm fires, flag clears, re-arms. Phase-aligned alarms with 10-second pre-wake offset implemented (Test 3).
- ✅ **Mothership battery ADC** — GPIO34, 220 kΩ/100 kΩ divider, 12-bit ADC, 16-sample averaging. Reads match multimeter within ~10 mV (Test 5).
- ✅ **A7670G LTE modem power rail** — TPS63020 buck-boost with PWM soft-start on `4V_EN` (GPIO33); power-good on GPIO35. Verified (Test 8).
- ✅ **A7670G modem UART + AT commands** — Serial2 on GPIO17 (TX) / GPIO16 (RX) at 115200 baud through level shifters. AT handshake, IMEI, SIM detection, network registration all verified (Tests 9–13).
- ✅ **Node power hold** — `PWR_HOLD` on GPIO23 (node), active-high, asserted immediately at boot (`node/firmware/v2/src/main.cpp`).

### Firmware / sync pipeline
- ✅ **ESP-NOW sync protocol** — channel 11; mothership broadcasts `SYNC_WINDOW_OPEN` repeatedly (every 5 s) during the sync window; nodes flush queued snapshots on receipt. Full autonomous sync cycle proven on 2026-06-19: mothership woke at 20:54:50, node at 20:55:00, 17 snapshots (seq 2–18) received and logged.
- ✅ **Node discovery, pairing, deployment** — `DISCOVER_REQUEST` / `DISCOVERY_SCAN` / `PAIR_NODE` / `DEPLOY_NODE` message flow. Node appears in the Field UI Node Manager; user can set numeric ID, friendly name, wake interval, and deploy. Verified via web UI.
- ✅ **Node state persistence** — nodes store mothership MAC, deployed flag, wake interval, and RTC-synced flag in NVS. Deployed nodes resume sending data automatically after power loss if the RTC coin cell preserves valid time.
- ✅ **Mothership node registry persistence** — paired/deployed nodes, IDs, names, and metadata stored in NVS namespaces `paired_nodes` and `node_meta` (`mothership/firmware/v2/src/config/node_registry.cpp`).
- ✅ **V1 snapshot protocol** — `node_snapshot_t` (124 bytes, fixed layout) with `sensorPresent` bitmask. Received, decoded, and logged.
- ✅ **V2 snapshot protocol** — `node_snapshot_v2_t` (48-byte header + N × 6-byte `v2_reading_t` entries, variable length, max 33 readings). `decodeV2()` in `mothership/firmware/v2/src/storage/flash_logger.cpp` decodes and synthesises the V1 `sensorPresent` bitmask for CSV compatibility.
- ✅ **LittleFS CSV logging** — `/datalog.csv` with 25-column header. `logDecodedSnapshot()` writes one row per snapshot. Used as the primary local store (SD card has a PCB routing bug on V1; LittleFS is the working path).
- ✅ **Upload queue with cursor** — `UploadQueue` (`mothership/firmware/v2/src/storage/upload_queue.cpp`) tracks byte offset, rows uploaded, retry count, and next-attempt time in NVS namespace `tx`. A/B-slot data-file recovery with backup/temp/commit pattern.
- ✅ **JSON payload builder** — `mothership/firmware/v2/src/storage/json_payload.cpp` converts CSV rows to structured JSON `{meta, status, readings}` per `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`. Omits NaN fields; hex fields emitted as decimal integers.
- ✅ **Transmission settings** — `TransmissionSettings` struct persisted in NVS (`tx` namespace): endpoint URL, auth token, API key, site ID, deployment ID, upload interval, min battery, max bytes per session, max retries, manual upload flag, JSON upload flag (`mothership/firmware/v2/src/config/transmission_settings.cpp`).
- ✅ **Captive portal / Field UI** — WiFi AP `Logger001` on channel 11, web server at `192.168.4.1`. Routes: `/` (dashboard), `/nodes` (node manager), `/node-config` (per-node), `/upload` (transmission settings), `/download-csv`, `/shutdown`, `/discover-nodes`, `/set-wake-interval`, `/set-transmission`. Async-form pattern, captive portal redirect. Verified on hardware.
- ✅ **RTC time set via web UI** — user can set the mothership DS3231 time from the browser. Verified.
- ✅ **CSV download** — web UI serves the flash-stored `datalog.csv` with actual node sensor readings. Verified.
- ✅ **Node local queue** — NVS-backed circular byte-slab queue (`node/firmware/v2/src/storage/local_queue.cpp`) with A/B slots, FNV1a checksums, generation counters, and wraparound-safe read/write. Stores raw V2 wire-format records so snapshots survive node power loss between sync windows.

### Sensors (node firmware)
- ✅ **SHT41 air temperature + humidity** — `sensors_sht41.cpp`, I2C address 0x44 on PCA9548A mux channel 0. High precision, no heater. Labels: `AIR_TEMP`, `AIR_RH`.
- ✅ **AS7341 8-channel spectral sensor** — `sensors_par_as7343.cpp`, I2C address 0x39 on mux channel 1. Reads channels F1–F8 (415–680 nm). Labels: `SPECTRAL_415` through `SPECTRAL_680`. *(Note: the file is named `as7343` but uses the Adafruit AS7341 library and the AS7341 I2C address — the sensor is an AS7341.)*
- ✅ **ADS1115 soil moisture + temperature** — `soil_moist_temp.cpp`, I2C address 0x48 on the root bus (not muxed). Four channels: SOIL1 moisture, SOIL2 moisture, SOIL1 temp (thermistor), SOIL2 temp (thermistor). Supports both legacy polynomial + Steinhart-Hart calibration and CWT TH-A probe mode (compile-time flag `SOIL_CWT_THA_MODE`). Labels: `SOIL1_VWC`, `SOIL2_VWC`, `SOIL1_TEMP`, `SOIL2_TEMP`.
- ✅ **Sensor registry** — `g_sensors[]` array populated by `initSensors()` in `sensors.cpp`; modular backends register slots with label, type, and stable sensor ID. Per-backend I2C read budgets prevent a hung sensor from blocking the wake cycle.
- ✅ **Battery voltage reading** — node ADC on GPIO35, 220 kΩ/100 kΩ divider, scale factor 3.62, 16 samples. Reported as `SENSOR_ID_BAT_V` (4001) in snapshots. *(Note: battery voltage is a system health metric, not an environmental reading.)*

### Cloud / dashboard
- ✅ **Google Apps Script backend** — `doPost()` receives CSV (legacy) or JSON (current) from the mothership; `doGet()` endpoints serve the dashboard (`getData`, `getLatest`, `getAllLatest`, `getNodes`, `getSystemStatus`, `getConfig`).
- ✅ **Google Sheets data store** — single "Data" sheet, 25 columns matching the CSV header.
- ✅ **React + Vite dashboard** — polls Apps Script every 60 s; displays sensor cards, node status, charts (Recharts), system status.

---

## 3. Planned or partially implemented capabilities

### 🟡 Partial — code exists but incomplete or not fully validated
- 🟡 **Ultrasonic wind backend** — `sensors_ultrasonic_wind.cpp` is a stub. The node PCB has a full ultrasonic anemometer subsystem (40 kHz transducers, 22 V boost, analog mux, comparator, timer capture), but the firmware wind-speed measurement is not producing discriminative readings yet. Bring-up logs note feedthrough/noise dominance. A reed-switch cup-anemometer fallback is wired via `J52 / AUX WIND` (solder-jumper selectable, mutually exclusive with ultrasonic mode). See `node/docs/NODE-PCB-OVERVIEW.md`.
- 🟡 **AUX I2C expansion ports** — `sensors_aux_i2c.cpp` is a stub. Two ports are reserved in the protocol (`SENSOR_ID_AUX1` 3001, `SENSOR_ID_AUX2` 3002) and in the V2 snapshot, but no sensor backend is populated.
- 🟡 **SD card logging on the mothership** — the PCB has an SD card socket, but V1 has a routing bug: GPIO23 (MOSI) is routed to SD socket pin 8 (DAT1) instead of pin 3 (CMD). LittleFS flash is the working storage path. SD is planned for a PCB revision fix. See `MOTHERSHIP_V1_BRINGUP_RESULTS_2026-06-19.md` Test 4.
- 🟡 **LTE cloud upload end-to-end** — the modem driver, upload queue, and JSON payload builder are implemented and the modem is verified at the AT level, but a full over-the-air upload to Google Apps Script with a real antenna and active SIM has not been confirmed in the bring-up logs. The upload path code exists in `mothership/firmware/v2/src/main.cpp` `performModemUpload()`.

### 🔵 Planned — documented in design notes, not yet in firmware
- 🔵 **Supabase migration** — replace Google Sheets with PostgreSQL + Edge Functions + real-time subscriptions + RLS. Binary V2 upload (`application/octet-stream`) instead of CSV/JSON. Documented in `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md`. Schema: `deployments`, `nodes`, `readings` (long format), `sync_sessions`.
- 🔵 **Multi-tenant onboarding** — user accounts (Supabase Auth), projects, per-mothership API keys (`fm_xxxxxxxx`), QR-code provisioning, RLS-scoped data isolation. Documented in `docs/FIELDMESH_USER_ONBOARDING_BRIEF.md`.
- 🔵 **GPS / spatial location** — capture lat/lon per node via phone GPS (`navigator.geolocation.getCurrentPosition()`) or manual entry in the Field UI; store in NVS; include in JSON `status.nodes[]`; render on a Leaflet + OpenStreetMap dashboard map. Documented in `docs/FIELDMESH_SPATIAL_LOCATION_PLAN.md`. Four phases planned; Phase 1 (phone GPS + NVS) not yet implemented.
- 🔵 **New data fields** — `projectStarted` (mothership first-boot timestamp), `deployedSinceUnix` (per-node deployment timestamp), `latitude`/`longitude`, mothership `batVoltage` in status, `lastUploadResult` string. Documented in `docs/FIELDMESH_NEW_DATA_FIELDS_BRIEF.md`. Some fields have firmware support (e.g. `projectStarted` is set in NVS in `main.cpp`); the Apps Script backend and dashboard consumption are pending.
- 🔵 **Field UI redesign** — four plain-English pages (Hub Overview, Stations, Settings, Data Export) with terminology mapping (Node→Station, Mothership→Hub, Deploy→Activate, etc.). Documented in `docs/FIELDMESH_FIELD_UI_REDESIGN.md`. Current firmware still serves the developer-language routes.
- 🔵 **Real-time dashboard updates** — Supabase real-time subscriptions on the `readings` table to replace 60-second polling.
- 🔵 **Binary V2 upload** — send raw `node_snapshot_v2_t` wire format directly to a Supabase Edge Function instead of decoding to CSV then JSON. ~48% smaller payload on LTE.

### ⚪ Architectural intention — target state in design notes
- ⚪ **Node V2 PCB revision** — fixes for V1 bring-up issues (D8/D9 clamp diode pin swap, 22 V boost inductor saturation, decoupling upgrades). Checklist in `node/docs/NODE_HARDWARE_V2_CHECKLIST.md`; many items checked off, some still open (reverse-polarity protection, leakage current verification, LED strategy).
- ⚪ **Mothership always-on → power-gated transition** — the V1 firmware plan (`mothership/docs/MOTHERSHIP_V1_FIRMWARE_PLAN.md`) describes the target wake-gated architecture. The V2 firmware implements much of this (PWR_HOLD, wake-reason branching, RTC alarms), but the design note frames it as an ongoing transition.
- ⚪ **OTA firmware updates** — Supabase storage bucket `firmware-ota` noted for future node OTA; not implemented.
- ⚪ **Dashboard map overlays** — vegetation, tenure boundaries, catchments on the Leaflet map; noted as out-of-scope for Phase 4 but on the roadmap.

---

## 4. Level 1 — Simple overview

*(250–500 words. Plain language for non-technical visitors.)*

### What is FieldMesh?

FieldMesh is an environmental monitoring system that measures conditions in the field — air temperature, humidity, light, soil moisture, soil temperature, and wind — and brings that data back to you through a web dashboard.

It is built for ecologists, researchers, and land managers who need reliable, long-term data from remote sites where there is no mains power and no reliable internet.

### How it works

Small battery-powered sensor devices (we call them **nodes**) are placed around your study site. Each node wakes up on a schedule — for example, every 10 minutes — takes its readings, sends them wirelessly to a central **hub** on site, and then goes back to sleep to save battery.

The hub collects readings from all your nodes and stores them locally. On its own schedule — for example, every hour — the hub sends the accumulated data to the cloud over a cellular connection. From there, you view your data in a web dashboard: current readings, charts over time, node battery levels, and system health.

### What does it measure?

- 🌡 **Air temperature** — how warm or cool the air is
- 💧 **Humidity** — moisture in the air
- ☀ **Light** — eight colour bands of light, from violet to deep red
- 🌱 **Soil moisture** — how wet the soil is, for two separate probes
- 🌡 **Soil temperature** — how warm the soil is, for two separate probes
- 🌀 **Wind** — speed and direction *(in development)*

### Why trust the data?

- Every reading carries a **timestamp** from a precision clock on the node, so you know exactly when it was measured.
- Data is **stored locally** on the hub before it is uploaded. If the cellular signal drops, nothing is lost — the hub keeps the data and retries.
- Each node reports its **battery voltage**, so you can see which stations need attention.
- The system is designed to **recover automatically** after power loss — if a node's clock battery survives, it picks up where it left off.

### Who is it for?

Ecologists tracking microclimate, researchers monitoring soil conditions over a season, planners assessing a site, and anyone who needs trustworthy environmental data from places where conventional instruments and internet are not available.

---

## 5. Level 2 — How the system works

*(800–1500 words. For ecologists, field technicians, researchers.)*

### Sensor nodes

A sensor node is a small, self-contained device built around an ESP32 microcontroller. It carries:

- An **SHT41 sensor** for air temperature and relative humidity.
- An **AS7341 spectral sensor** that reads light in eight colour bands from 415 nm (violet) to 680 nm (deep red).
- An **ADS1115 analog-to-digital converter** connected to two VH-5 soil probes, measuring volumetric water content and soil temperature for each probe.
- An optional **ultrasonic anemometer** for wind speed and direction (in development).
- Two **auxiliary I2C ports** reserved for future sensors.
- A **DS3231 real-time clock** with a coin-cell backup so time survives power loss.
- A **battery** (Li-ion, charged via solar or USB) with voltage monitoring.

All I2C sensors share a single bus through a **PCA9548A multiplexer**, which lets the node talk to one sensor at a time without address conflicts.

#### Wake and sleep

The node is normally **fully powered off**. The DS3231 alarm wakes it on a configurable interval (1, 5, 10, 20, 30, or 60 minutes). On wake:

1. The node asserts a **power-hold** signal so the board stays alive.
2. It reads all attached sensors (each backend has a time budget so a hung sensor does not block the cycle).
3. It packages the readings into a compact **snapshot** — one packet per wake event containing all sensor values.
4. If a sync window is open (see below), it sends the snapshot to the mothership over **ESP-NOW**, a low-power Wi-Fi protocol on channel 11.
5. It re-arms the RTC alarm and releases the power-hold — the board powers off completely.

If the node cannot reach the mothership (e.g. the hub is asleep), it stores the snapshot in a **local queue** in non-volatile storage (NVS). The queue is a circular byte buffer with checksums and dual A/B slots for crash safety. On the next sync window, the node flushes all queued snapshots.

#### Battery

The node reports its battery voltage in every snapshot. This is a **system health metric** — it tells you whether the node needs charging or solar attention. It is not an environmental measurement.

### Mothership (FieldHub)

The mothership is the on-site coordinator. It is also built on an ESP32 and carries:

- A **DS3231 RTC** for accurate time.
- **LittleFS flash storage** for the local data log (`/datalog.csv`).
- A **SIMCom A7670G LTE Cat-1 modem** for cloud upload over cellular, with SSL/TLS encryption.
- A **Wi-Fi access point** (`Logger001`) for field configuration.
- A **power-gating architecture** — the board is off between sync windows and wakes on RTC alarm, config button, or USB.

#### What the mothership does

1. **Receives snapshots** from nodes during the sync window (ESP-NOW, channel 11).
2. **Decodes** each snapshot (V1 124-byte fixed format or V2 variable-length format) into a common `DecodedSnapshot` structure.
3. **Logs** the decoded reading as a CSV row in `/datalog.csv` (25 columns).
4. **Updates the node registry** — last-seen time, battery voltage, active flag.
5. **Sends a snapshot ACK** back to the node confirming durable storage.
6. On its own schedule, **uploads** accumulated CSV rows to the cloud as structured JSON.

#### Config portal

When you press the config button, the mothership wakes into **config mode**: it starts the Wi-Fi AP and serves a web portal at `192.168.4.1`. From your phone or laptop you can:

- See the current hub time.
- Set the global recording interval for nodes.
- Discover new nodes.
- Pair, name, deploy, stop, or unpair nodes.
- Set upload settings (endpoint, site ID, API key, upload interval, battery threshold).
- Download the CSV log.
- Shut down the hub (which arms the next RTC alarm and powers off).

### Communications

There are three communication legs:

1. **Sensor-to-node** — I2C over the mux. Each sensor backend reads its hardware and populates a slot in the sensor registry.
2. **Node-to-mothership** — ESP-NOW on Wi-Fi channel 11. The mothership broadcasts `SYNC_WINDOW_OPEN` repeatedly during the sync window; nodes that wake during the window hear the marker and flush their queued snapshots. The mothership sends a `SNAPSHOT_ACK` per snapshot confirming durable storage.
3. **Mothership-to-cloud** — HTTPS POST over LTE via the A7670G modem. The payload is structured JSON (`{meta, status, readings}`) carrying upload metadata, system status (fleet counts, upload queue, flash usage, node registry), and the batch of sensor readings since the last upload. SSL/TLS is handled by the modem's CCH* API with chunked sends.

> **Important distinction:** Node *recording* (waking and reading sensors) is independent of mothership *upload* (sending data to the cloud). Nodes record on their own interval; the mothership uploads on its own interval. A node can record many times between uploads.

### Dashboard

The web dashboard (React + Vite) shows:

- **Project overview** — current readings per node, system status.
- **Node status** — state (deployed/paired/unpaired), battery, last-seen, sensor count.
- **Charts** — time-series for each sensor, selectable by node and time range.
- **System health** — mothership battery, storage usage, upload queue depth, last upload result.
- **Export** — CSV download of the full data log.

The dashboard currently polls the Google Apps Script backend every 60 seconds. Real-time push updates are planned via Supabase subscriptions.

### Field workflow (deployment steps)

1. **Charge and power on** the mothership (battery or USB).
2. **Press the config button** — the hub wakes into config mode and serves the Wi-Fi AP `Logger001`.
3. **Connect your phone** to `Logger001` and open `192.168.4.1`.
4. **Set the hub time** via the web UI (or it inherits from the RTC if already set).
5. **Set the recording interval** (e.g. 10 minutes) and the upload interval (e.g. 1 hour).
6. **Find new stations** — press the pair button on each node (or power it on); tap "Discover Nodes" in the UI.
7. **Name and configure** each discovered node — set a numeric ID and a friendly name (e.g. "North Hedge 01").
8. **Deploy** each node — choose "Start / deploy"; the hub sends the current time and schedule to the node.
9. **Place the nodes** in the field at your study locations.
10. **Set upload settings** — enter the cloud endpoint URL, site ID, and API key (or scan a QR code in future).
11. **Start monitoring** — tap "Shut Down" (or "Start Monitoring" in the redesigned UI); the hub arms its RTC alarm and powers off.
12. **View data** — open the cloud dashboard; data appears as the hub completes its upload cycles.

---

## 6. Level 3 — Technical architecture

*(2000–4000 words. For developers, engineers, advanced researchers.)*

### Hardware architecture

#### Node

The node is an ESP32-WROOM-32D-based board with a power-gated architecture. Key subsystems:

| Subsystem | Component | Interface | Pin / Address | Source |
|---|---|---|---|---|
| MCU | ESP32-WROOM-32D | — | — | `platformio.ini` |
| RTC | DS3231 | I2C | SDA=18, SCL=19, INT=4 | `platformio.ini` |
| I2C mux | PCA9548A | I2C | 0x71 (node build) / 0x70 (protocol.h default) | `protocol.h`, `platformio.ini` |
| Air T/RH | SHT41 (Adafruit SHT4x) | I2C via mux ch0 | 0x44 | `sensors_sht41.cpp` |
| Spectral | AS7341 (Adafruit AS7341) | I2C via mux ch1 | 0x39 | `sensors_par_as7343.cpp` |
| Soil moisture/temp | ADS1115 | I2C root bus | 0x48 | `soil_moist_temp.cpp` |
| Ultrasonic wind | 40 kHz transducers + 22 V boost + analog mux + comparator | GPIO | RX_EN_N=4, TOF_EDGE=34, TX_22V_EN_N=5, TX_PWM=25 | `NODE-PCB-OVERVIEW.md` |
| Battery ADC | ESP32 ADC1 | GPIO35 | 220 kΩ/100 kΩ divider, scale 3.62 | `platformio.ini` |
| Power hold | PMOS gate | GPIO23 | active-high | `platformio.ini`, `main.cpp` |
| AUX I2C | 2 expansion ports | I2C | — | `sensors_aux_i2c.cpp` (stub) |

**Power architecture:** Three rails — `3V3_SYS` (digital), `5V_SYS` (analog/sensors), `22V_SYS` (ultrasonic TX, pulsed). The node is fully power-gated: `PWR_HOLD` (GPIO23) is asserted at boot and released to cut power. The DS3231 alarm on GPIO4 (RTC_INT_PIN) is the wake source. A RUN/KILL slide switch provides hard battery isolation for storage/servicing.

**Charging:** CN3163 (solar, ~0.5 A) and TP5100 (USB, ~1 A) chargers feed `BAT_BUS` directly. Dual-source (solar + USB) charging verified in bring-up.

**Known V1 hardware issues (fixed or pending in V2):** D8/D9 BAV99 clamp diode pin swap (caused 3V3-to-GND short, fixed by removal on V1), 22 V boost inductor saturation (V3 upgrades to 2.5 A I_sat molded inductor), MT3608 input cap upgraded to 100 µF. See `node/docs/NODE_HARDWARE_V2_CHECKLIST.md`.

#### Mothership

The mothership is an ESP32-WROOM-based power-gated hub. Pin assignments from `mothership/firmware/v2/src/system/pins.h`:

| Function | GPIO | Notes |
|---|---|---|
| PWR_HOLD | 26 | Assert HIGH at boot; LOW to power off |
| Config wake (latch sense) | 32 | Active LOW |
| Config clear (latch clear) | 25 | Pulse HIGH 20 ms |
| Config LED | 27 | Status indicator |
| 4V_EN (modem rail) | 33 | PWM soft-start to TPS63020 |
| Modem PWRKEY | 14 | NMOS gate, HIGH = press |
| Modem STATUS | 4 | HIGH = modem powered |
| Modem PG (power-good) | 35 | TPS63020 PG, input-only |
| Modem TX (ESP→modem) | 17 | Serial2 TX |
| Modem RX (ESP←modem) | 16 | Serial2 RX |
| Battery ADC | 34 | ADC1, input-only, 220 kΩ/100 kΩ divider |
| I2C SDA (DS3231) | 21 | |
| I2C SCL (DS3231) | 22 | |
| SD CS | 13 | *(SD has PCB routing bug on V1)* |
| SD SCK | 18 | |
| SD MISO | 19 | |
| SD MOSI | 23 | *(routed to wrong SD pin on V1)* |

**Wake sources:** DS3231 RTC alarm (INT/SQW), config button (via SN74LVC2G74 latch), USB service (VBUS_USB holds LOGIC high via SW10). SW9 is a hard-kill master switch outside normal wake logic. See `mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md`.

**Modem:** SIMCom A7670G-LABE, firmware A110B06A7670M7. Power sequence: PWM soft-start on 4V_EN (500 ms ramp) → wait for PG HIGH → pulse PWRKEY 1100 ms → wait STATUS HIGH → UART AT handshake. SSL/TLS via CCH* API with chunked CCHSEND (1024-byte chunks). Verified at AT level (Tests 9–13); full over-the-air upload pending antenna validation.

### Firmware architecture

#### Boot sequence (mothership)

From `mothership/firmware/v2/src/main.cpp`:

1. **Assert PWR_HOLD** (GPIO26 HIGH) — critical, must be first.
2. **Detect wake reason** — read CONFIG_WAKE (GPIO32, active LOW = config wake); check DS3231 alarm flag; check USB service.
3. **Branch:**
   - **Sync wake** (`handleSyncWake()`): init I2C + RTC → init ESP-NOW (no AP) → broadcast `SYNC_WINDOW_OPEN` every 5 s → receive and log snapshots → optional modem upload → re-arm RTC alarm → release PWR_HOLD.
   - **Config wake** (`handleConfigWake()`): init I2C + RTC + LittleFS → start WiFi AP + web server + captive portal + ESP-NOW → serve UI loop → on timeout or shutdown: re-arm alarm → release PWR_HOLD.
   - **Service wake** (`handleServiceWake()`): full runtime for flashing/diagnostics; powers off (no alarm arming).
4. **Re-arm RTC alarm** before power-down.
5. **Release PWR_HOLD** — board powers off.

A **bounded retry** mechanism (`boundedRetryAndShutdown()`) attempts to re-arm the rescue alarm up to 3 times if RTC init fails, so the board is not stranded without a wake source.

#### Boot sequence (node)

From `node/firmware/v2/src/main.cpp`:

1. **Assert PWR_HOLD** (GPIO23 HIGH).
2. **Initialise node identity** — auto-generate `ENV_XXYYZZ` from STA MAC (last 3 octets).
3. **Init I2C** (WireRtc on pins 18/19) + **PCA9548A mux**.
4. **Init RTC** (DS3231) — detect power-loss, alarm verification.
5. **Init sensors** — `initSensors()` calls each backend's `init()` (SHT41, AS7341, soil, wind, aux); each registers slots in `g_sensors[]`.
6. **Load config** from NVS — mothership MAC, deployed flag, wake interval, sync schedule, RTC-synced flag.
7. **Determine wake reason** — alarm wake (read sensors + send/queue) vs sync wake (flush queue) vs config/first-boot.
8. **Read sensors** → **build snapshot** → **send or queue** → **re-arm alarm** → **release PWR_HOLD**.

A **post-wake command window** (`POST_WAKE_WINDOW_MS`, default 1500 ms) keeps ESP-NOW alive after the node's HELLO so the mothership can push a `CONFIG_SNAPSHOT` before the node powers off.

#### Module interactions

```
Node firmware:
  main.cpp
    ├── sensors/sensors.cpp (registry: g_sensors[])
    │     ├── sensors_sht41.cpp (mux ch0, I2C 0x44)
    │     ├── sensors_par_as7343.cpp (mux ch1, I2C 0x39)
    │     ├── soil_moist_temp.cpp (root bus, I2C 0x48)
    │     ├── sensors_ultrasonic_wind.cpp (stub)
    │     └── sensors_aux_i2c.cpp (stub)
    ├── storage/local_queue.cpp (NVS circular byte-slab, A/B slots)
    ├── storage/node_config_store.cpp (NVS config persistence)
    ├── message_dispatch.cpp (ESP-NOW command routing)
    └── node_event_queue.cpp (FreeRTOS event queue)

Mothership firmware:
  main.cpp
    ├── system/power.cpp (PWR_HOLD, config latch, battery ADC)
    ├── system/wake_reason.h (wake-source detection)
    ├── time/rtc_alarm.h (DS3231 alarm programming)
    ├── comms/espnow_manager.cpp (ESP-NOW receive, snapshot queue, node registry)
    ├── comms/modem_driver.cpp (A7670G power, AT, SSL, upload)
    ├── storage/flash_logger.cpp (LittleFS CSV, V1/V2 decode)
    ├── storage/upload_queue.cpp (cursor, retry, purge, A/B recovery)
    ├── storage/json_payload.cpp (CSV → JSON {meta, status, readings})
    ├── config/config_server.cpp (WiFi AP, web server, captive portal)
    ├── config/node_registry.cpp (NVS node meta, desired config)
    └── config/transmission_settings.cpp (NVS upload settings)
```

### Node state model

Nodes transition through three states, managed by the mothership's node registry and persisted in NVS:

```
UNPAIRED ──pair──→ PAIRED ──deploy──→ DEPLOYED
   ↑                  │                    │
   │                  └──stop (keep)───────┘
   │                  │
   └──unpair (forget)─┘
```

| State | Meaning | Behaviour |
|---|---|---|
| `UNPAIRED` | Node is discovered but not associated | Advertises `DISCOVER_REQUEST`; appears in UI as "New" |
| `PAIRED` | Node is bound to this mothership but not actively recording | Remembers mothership MAC; can be redeployed without rediscovery |
| `DEPLOYED` | Node is actively recording and sending snapshots on its wake interval | RTC synced to mothership time; alarm armed; snapshots sent/queued each wake |

**Pending transitions:** The registry tracks `deployPending`, `stateChangePending`, and `pendingTargetState` for in-flight state changes that have been commanded but not yet confirmed by the node. The mothership retries pending transitions on subsequent sync windows.

**Recovery:** After power loss, a deployed node reloads its state from NVS. If the DS3231 coin cell preserved valid time, the node sees it is deployed + synced and resumes sending data without redeployment.

### Timing architecture

Three independent timing domains:

1. **Recording interval** (node wake interval) — set per-node at deployment (1, 5, 10, 20, 30, 60 minutes). Driven by DS3231 Alarm 1 on the node. The node wakes, reads sensors, and queues/sends a snapshot.

2. **Sync interval** (mothership upload + node flush interval) — set globally on the mothership. Driven by DS3231 Alarm 1 on the mothership. The mothership wakes, opens the sync window, receives node snapshots, optionally uploads to cloud, and re-arms. The sync interval is a multiple of the recording interval (`kSyncFillK = 18` in `config_server.cpp`), so the sync window aligns with node wake cycles.

3. **Upload interval** (cloud upload cadence) — configurable in `TransmissionSettings.uploadIntervalMin`. Can be 0 (upload every sync wake) or a multiple of the sync interval. The upload queue tracks a cursor and only uploads rows since the last successful upload.

**Phase alignment:** The mothership stores a `syncPhaseUnix` anchor in NVS (A/B slots with CRC). Alarms are phase-aligned so all motherships and nodes in a fleet wake on the same grid. The mothership wakes 10 seconds before the node sync alarm (`SYNC_PRE_WAKE_SEC` offset, though currently 0 in the node build) to broadcast `SYNC_WINDOW_OPEN` before nodes wake.

**RTC anchors:** `SyncAnchorRecord` in `config_server.cpp` stores magic, version, generation, phaseUnix, intervalMin, mode, and CRC. Validated on read; falls back to defaults if corrupted.

### Data flow

```
Sensor hardware (SHT41 / AS7341 / ADS1115 / wind / aux)
    ↓ I2C via PCA9548A mux
Node sensor backends (sensors_*.cpp)
    ↓ populate g_sensors[] registry
Node main loop
    ↓ readSensor() per slot → build snapshot
    ↓ node_snapshot_v2_t (48B header + N×6B readings)
    ↓ ESP-NOW (WiFi ch11) — or local_queue.cpp if no sync window
Mothership ESP-NOW callback (espnow_manager.cpp)
    ↓ enqueue raw snapshot struct (SnapEntry, 128-capacity ring buffer)
Mothership main loop
    ↓ dequeue → decodeV2() → DecodedSnapshot
    ↓ logDecodedSnapshot() → /datalog.csv (LittleFS, 25 columns)
    ↓ sendSnapshotAck() → node (persisted flag)
Upload cycle (performModemUpload in main.cpp)
    ↓ UploadQueue::getNewData() reads CSV rows from cursor
    ↓ json_payload.cpp → JSON {meta, status, readings}
    ↓ ModemDriver::httpsPost() — SSL/TLS via CCH* API, chunked CCHSEND
Google Apps Script (doPost → appendSensorData / ingest)
    ↓
Google Sheet (Data sheet, 25 columns)
    ↓ doGet?action=getData / getAllLatest (polls every 60s)
FieldMesh Dashboard (React + Vite)
```

### Protocol architecture

#### Message types (from `node/firmware/v2/shared/protocol.h`)

| Message | Direction | Struct | Purpose |
|---|---|---|---|
| `DISCOVER_REQUEST` | Node→broadcast | `discovery_message_t` | Node announces itself |
| `DISCOVERY_SCAN` / `DISCOVER_RESPONSE` | Mothership→broadcast | `discovery_response_t` | Mothership acknowledges |
| `PAIR_NODE` | Mothership→node | `pairing_command_t` | Bind node to mothership |
| `PAIRING_RESPONSE` | Mothership→node | `pair_response_t` | Confirm pairing |
| `DEPLOY_NODE` | Mothership→node | `deployment_command_t` | Deploy with time + schedule + config version |
| `DEPLOY_ACK` | Node→mothership | `deployment_ack_message_t` | Confirm deployment applied |
| `SET_SCHEDULE` | Mothership→node | `schedule_command_message_t` | Set wake interval |
| `SET_SYNC_SCHED` | Mothership→node | `sync_schedule_command_message_t` | Set sync interval + phase anchor |
| `NODE_HELLO` | Node→mothership | `node_hello_message_t` | Start of wake cycle; carries config version, queue depth |
| `NODE_SNAPSHOT` (V1) | Node→mothership | `node_snapshot_t` (124B) | Fixed-layout snapshot |
| `NODE_SNAPSHOT2` (V2) | Node→mothership | `node_snapshot_v2_t` (48B + N×6B) | Variable-length key-value snapshot |
| `SNAPSHOT_ACK` | Mothership→node | `snapshot_ack_t` | Confirm durable storage |
| `CONFIG_SNAPSHOT` | Mothership→node | `config_snapshot_message_t` | Push new config version |
| `CONFIG_ACK` | Node→mothership | `config_apply_ack_message_t` | Confirm config applied |
| `REQUEST_TIME` / `TIME_SYNC` | Node↔mothership | `time_sync_request_t` / `time_sync_response_t` | Time synchronisation |
| `UNPAIR_NODE` | Mothership→node | `unpair_command_t` | Forget mothership |
| `NODE_STATUS` | Node→mothership | `node_status_message_t` | Async status push (rescue/recovery) |

#### V2 wire format

```
node_snapshot_v2_t (48 bytes, packed):
  command[16]        "NODE_SNAPSHOT2"
  nodeId[16]         e.g. "ENV_6C0AA0"
  nodeTimestamp      uint32  (node RTC unix)
  seqNum             uint32  (snapshot sequence number)
  sensorCount        uint16  (number of v2_reading_t entries)
  qualityFlags       uint16
  configVersion      uint16
  protocolVersion    uint8   (2)
  reserved           uint8

v2_reading_t (6 bytes, packed):
  sensorId           uint16  (SENSOR_ID_* constant)
  value              float   (sensor reading)

Total wire size = 48 + sensorCount × 6
Max sensorCount = 33 (ESP-NOW 250-byte limit: (250-48)/6 = 33.67)
```

#### Sensor IDs

| Range | Category | Examples |
|---|---|---|
| 1000–1999 | Standard environmental | 1001 AIR_TEMP, 1002 AIR_RH, 1101–1108 SPECTRAL_415–680, 1201 WIND_SPEED, 1202 WIND_DIR, 1301 PAR |
| 2000–2999 | Soil / analog | 2001 SOIL1_VWC, 2002 SOIL2_VWC, 2003 SOIL1_TEMP, 2004 SOIL2_TEMP |
| 3000–3999 | Auxiliary | 3001 AUX1, 3002 AUX2 |
| 4000–4999 | System metrics | 4001 BAT_V |
| 5000–5999 | Future port-based dynamic sensors | reserved |

#### ACK behaviour

The mothership sends a `SNAPSHOT_ACK` per received snapshot with a `persisted` flag (1 = durably logged to LittleFS, 0 = not). The node's `NODE_REQUIRE_DURABLE_SNAPSHOT_ACK` compile flag (default 0) controls whether the node waits for a durable ACK before dropping the snapshot from its local queue. In link-layer compatibility mode (default), the node trusts the ESP-NOW link-layer ACK and keeps the snapshot until the next sync window confirms it was received.

### Local web interface (captive portal)

The Field UI is served by `mothership/firmware/v2/src/config/config_server.cpp` using the Arduino `WebServer` library on port 80 with a `DNSServer` for captive portal redirect.

**Current routes:**

| Route | Method | Handler | Purpose |
|---|---|---|---|
| `/` | GET | `handleRoot` | Dashboard: RTC time, wake interval, system status |
| `/nodes` | GET | `handleNodesPage` | Node Manager: list with state chips + time-health pills |
| `/node-config` | GET/POST | `handleNodeConfigForm` | Per-node: set ID, name, deploy/stop/unpair |
| `/upload` | GET/POST | `handleUploadSettings` | Transmission settings |
| `/download-csv` | GET | `handleDownloadCSV` | CSV log download |
| `/shutdown` | POST | `handleShutdown` | Shut down hub (arm alarm + power off) |
| `/discover-nodes` | POST | — | Trigger discovery scan |
| `/set-wake-interval` | POST | — | Set global recording interval |
| `/set-transmission` | POST | — | Save upload settings |

**Architecture:** HTML/CSS/JS served from PROGMEM (inline, no external resources, no internet required). Async-form pattern: POST to action endpoint, redirect back to page. WiFi AP `Logger001` on channel 11, password `logger123` (configurable). Captive portal redirect via DNS.

**Planned redesign** (`docs/FIELDMESH_FIELD_UI_REDESIGN.md`): four pages (Hub Overview, Stations, Settings, Data Export) with plain-English terminology mapping (Node→Station, Mothership→Hub, Deploy→Activate, Paired→Connected, Unpaired→New, Sync window→Collection round, Wake interval→Recording interval, Sync interval→Upload interval). All mechanism details (ESP-NOW, MAC, flash, config versions) hidden in a collapsed About/Advanced section.

### Cloud architecture

#### Current: Google Apps Script + Sheets

- **Ingest:** `doPost()` receives CSV (legacy) or JSON (current) from the mothership. JSON payload `{meta, status, readings}` per `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`.
- **Storage:** Google Sheet with "Data" (25 columns), "Nodes", "Status" sheets.
- **Query:** `doGet()` endpoints — `getData`, `getLatest`, `getAllLatest`, `getNodes`, `getSystemStatus`, `getConfig`. `getAllLatest` does a full-range scan + JS filter (O(n)).
- **Auth:** `?token=xxx` query param on the URL (weak, shared, visible in modem AT logs).
- **Limitations:** 20,000 reads/day quota, 6-min execution limit, no real-time, no schema enforcement, no RLS, slow past ~100k rows.

#### Planned: Supabase

- **Ingest:** Supabase Edge Function (Deno/TypeScript) decodes V2 binary or JSON, batch-inserts into PostgreSQL.
- **Storage:** PostgreSQL tables — `deployments`, `nodes`, `readings` (long format: one row per sensor reading), `sync_sessions`.
- **Query:** PostgREST auto-generated REST API; SQL queries with indexes.
- **Real-time:** Supabase real-time subscriptions on `readings` table; dashboard updates instantly without polling.
- **Auth:** Per-mothership API key (`fm_xxxxxxxx`) validated by the Edge Function; `service_role` key used internally only.
- **RLS:** Mothership role INSERT-only; dashboard anon/authenticated role SELECT-only, scoped to `project_id`.
- **Payload:** Binary V2 (`application/octet-stream`) — ~48% smaller than CSV on LTE.

#### Planned: Multi-tenant

- **Users:** Supabase Auth (email/password or magic link).
- **Projects:** User-created, owned by `auth.uid()`.
- **Motherships:** Registered to a project; MAC address or QR scan.
- **API keys:** Per-device, hashed in `api_keys` table, revocable, shown once.
- **Provisioning:** QR code (recommended) encoding endpoint + API key; short code (fallback); pre-provisioned (future fleet-scale).
- **Data routing:** API key → mothership → project → user; every inserted row stamped with `project_id` for RLS.

### Security model

#### Current limitations
- **Shared token:** `?token=xxx` in the URL query string. Visible in modem AT command logs and Apps Script deployment history. Not revocable per-device.
- **No user accounts:** One shared dashboard URL; anyone with the URL sees all data.
- **No data isolation:** No multi-tenant separation.
- **No encryption at rest on the mothership:** LittleFS CSV is plaintext.
- **ESP-NOW unencrypted:** `peer.encrypt = false` in all peer registrations. ESP-NOW payloads are not encrypted on the radio link.

#### Planned improvements
- **Per-device API keys:** `fm_xxxxxxxx` format, hashed in database, revocable, validated by Edge Function.
- **Row-level security:** PostgreSQL RLS policies scoped to `project_id` → `user_id`.
- **Supabase Auth:** User accounts with email/password or magic link.
- **QR provisioning:** Zero-typing field setup; endpoint + key encoded in a single QR string.
- **ESP-NOW encryption:** Not yet planned but noted as a future consideration for sensitive deployments.

### Reliability and failure handling

| Failure mode | Behaviour | Source |
|---|---|---|
| **Node offline (missed sync window)** | Snapshots queue in NVS local_queue (circular byte-slab, A/B slots, FNV1a checksum). Flushed on next sync window. Queue drops oldest if full. | `local_queue.cpp` |
| **Mothership upload fails** | `UploadQueue.incrementRetryCount()` with cooldown (`nextAttemptUnix`). Retry count capped at `maxRetriesPerWindow` (default 3). Cursor not advanced; rows retried next cycle. | `upload_queue.cpp` |
| **Storage full (LittleFS)** | `flashUsagePct` reported in JSON status. No automatic purge implemented yet; manual CSV download + clear is the current path. Purge after confirmed upload is planned. | `json_payload.cpp`, `upload_queue.cpp` |
| **RTC unset / power lost** | Node detects `RTC_LOST_POWER`; enters recovery mode. Mothership `boundedRetryAndShutdown()` re-arms rescue alarm up to 3 times. `rtcTimeValid()` check before relying on timestamps. | `main.cpp` (both), `rtc_alarm.h` |
| **Sensor fail (I2C hang)** | Per-backend I2C ping before full read; `WireRtc.setTimeOut(2000)` per-transaction timeout. Failed reads produce NaN values, logged as "nan" in CSV and omitted in JSON. `sensorPresent` bitmask indicates which sensors had valid reads. | `sensors.cpp`, `flash_logger.cpp` |
| **Modem power-on fail** | `performModemUpload()` logs failure, increments retry count, graceful shutdown, re-arms alarm. | `main.cpp` |
| **Network registration timeout** | 60-second timeout; skips upload, increments retry, graceful shutdown. | `main.cpp` |
| **Session watchdog** | `kSyncSessionLimitMs` (180 s) — forces shutdown if sync session exceeds limit. | `main.cpp` |
| **Data file corruption** | `UploadQueue::recoverDataFile()` handles data+temp, data+backup, backup-only, and temp-only states with rename/restore logic. | `upload_queue.cpp` |
| **NVS corruption** | `validateBlob()` in local_queue checks magic, version, layout ID, and checksum. A/B slot generation comparison picks the newer valid slot. Node registry uses fixed-buffer NVS reads to avoid stack overflow from malformed length metadata. | `local_queue.cpp`, `node_registry.cpp` |
| **Stale node (not seen)** | `STALE_MISS_THRESHOLD = 3` missed sync windows; `STALE_MIN_AGE_MS = 24h`. Mothership can send a stale-assist `TIME_SYNC` to nudge a stale node. | `espnow_manager.cpp` |

### Power management

#### Node
- **Sleep strategy:** Full power cut via `PWR_HOLD` release. The board is completely off between wake cycles; only the DS3231 (coin-cell backed) and the alarm interrupt path are active.
- **Battery monitoring:** ADC on GPIO35, 220 kΩ/100 kΩ divider, 16-sample average, scale 3.62. Reported as `SENSOR_ID_BAT_V` (4001) in every snapshot.
- **Charging:** Solar (CN3163, ~0.5 A) and USB (TP5100, ~1 A); dual-source supported.

#### Mothership
- **Sleep strategy:** Full power cut via `PWR_HOLD` (GPIO26) release. Three wake sources: RTC alarm, config button, USB service. Board is off between sync windows.
- **Battery monitoring:** ADC on GPIO34, 220 kΩ/100 kΩ divider, 12-bit ADC, 16-sample average. Reported in JSON status as `batVoltage`.
- **Upload thresholds:** `TransmissionSettings.minBatteryMv` (default 3700 mV) — the mothership skips upload if battery is below this threshold. `maxBytesPerSession` (default 96 KB) caps upload size per session.
- **Modem power:** TPS63020 buck-boost with PWM soft-start (500 ms ramp) on `4V_EN` (GPIO33). Power-good on GPIO35. Modem is powered only during upload, then shut down.
- **Solar:** Planned but not yet a confirmed hardware feature on the mothership V1.

### Scalability

| Dimension | Current | Scaling path |
|---|---|---|
| **Nodes per mothership** | Tested with 1; designed for multiple (sync window 120 s, repeated `SYNC_WINDOW_OPEN` every 5 s, early shutdown when all deployed nodes synced) | More nodes = longer sync window; ESP-NOW practical limit ~20–30 nodes per hub at 10-min intervals |
| **Motherships per deployment** | One | Supabase multi-tenant: many motherships per project, many projects per user |
| **Projects / users** | One shared dashboard | Supabase Auth + RLS: unlimited users, each sees only their own projects |
| **Data volume** | ~150 bytes/row CSV; ~1.3 MB/day at 30 nodes × 12 reads/hr | Binary V2: ~674 KB/day (48% smaller). PostgreSQL handles millions of rows with indexes. |
| **Deployment duration** | Battery-dependent; solar extends indefinitely | Node power-cut architecture minimises idle drain; coin-cell RTC backup preserves time across main battery depletion |
| **ESP-NOW range** | Verified: 1 m ~98.5% ACK, 30 m ~99.7% ACK, 100 m obstructed ~84–87% ACK | Mesh/repeater not implemented; range is line-of-sight dependent |

---

## 7. Recommended diagrams

Ten diagrams for the About page, with audience, labels, animation, mobile fallback, and accessibility notes.

### Diagram 1 — System overview (animated)
- **What it shows:** Sensors → Node → Mothership → Cloud → Dashboard, left to right.
- **Audience:** Layer 1 (all)
- **Labels:** "Sensors", "Node", "Mothership", "Cloud", "Dashboard"
- **Animation:** Auto-plays on load, loops every 15 s. Sensor pulse → data dots into node → radio arc to mothership → cellular tower to cloud → dots to dashboard chart. (Per `FIELDMESH_ABOUT_SECTION_DESIGN.md` §3.3.)
- **Mobile fallback:** Stages stack vertically; data dots travel downward.
- **Accessibility:** `prefers-reduced-motion` disables animation; static SVG with labels. ARIA labels on each stage. Text alternative: "FieldMesh measures environmental data at sensor nodes, sends it to a mothership hub over radio, which uploads it to the cloud over cellular for display in a web dashboard."

### Diagram 2 — Node wake/sleep cycle
- **What it shows:** RTC alarm fires → PWR_HOLD assert → sensor reads → snapshot build → ESP-NOW send (or queue) → re-arm alarm → PWR_HOLD release → power off → (loop).
- **Audience:** Layer 2
- **Labels:** "RTC alarm", "Power on", "Read sensors", "Build snapshot", "Send / Queue", "Re-arm alarm", "Power off"
- **Animation:** Circular flow with a moving highlight around the cycle. Click a step for a popover explanation.
- **Mobile fallback:** Vertical timeline list instead of circle.
- **Accessibility:** Ordered list fallback with `aria-current` on the active step.

### Diagram 3 — Node hardware block diagram
- **What it shows:** ESP32 in centre; DS3231, PCA9548A mux, SHT41 (ch0), AS7341 (ch1), ADS1115 (root bus), battery ADC, PWR_HOLD, ultrasonic subsystem, AUX ports as connected blocks. I2C bus lines shown.
- **Audience:** Layer 3
- **Labels:** Component names, I2C addresses, GPIO pins, mux channels.
- **Animation:** None (static). Hover a block to highlight its I2C address and pin.
- **Mobile fallback:** Collapsible accordion of blocks; tap to expand pin details.
- **Accessibility:** Table fallback with columns: Component, Interface, Address/Pin, Purpose.

### Diagram 4 — Mothership hardware block diagram
- **What it shows:** ESP32 centre; DS3231, LittleFS flash, A7670G modem (UART2, PWRKEY, 4V_EN, STATUS, PG), battery ADC, PWR_HOLD, config latch, WiFi AP. Wake-source OR gate (RTC + button + USB).
- **Audience:** Layer 3
- **Labels:** GPIO numbers, signal names, wake-source labels.
- **Animation:** None. Hover to highlight a subsystem's pins.
- **Mobile fallback:** Accordion.
- **Accessibility:** Table fallback.

### Diagram 5 — Data flow pipeline
- **What it shows:** Full pipeline: sensor → V2 snapshot (hex) → ESP-NOW → mothership decode → CSV log → JSON payload → LTE POST → Apps Script → Sheet → dashboard poll. With byte sizes at each stage.
- **Audience:** Layer 2–3
- **Labels:** Format names, byte sizes (48B + N×6B, 124B V1, ~150B CSV, JSON {meta,status,readings}).
- **Animation:** Data dots flowing through the pipeline; click a stage to see the format at that stage.
- **Mobile fallback:** Vertical stack with connecting lines.
- **Accessibility:** Ordered list with format and size at each step.

### Diagram 6 — V2 snapshot wire format (hex decoder)
- **What it shows:** A byte-by-byte breakdown of `node_snapshot_v2_t`: 16B command, 16B nodeId, 4B timestamp, 4B seqNum, 2B sensorCount, 2B qualityFlags, 2B configVersion, 1B protocolVersion, 1B reserved, then N × 6B readings (2B sensorId + 4B float).
- **Audience:** Layer 3
- **Labels:** Field names, byte offsets, data types. A sample hex dump alongside.
- **Animation:** Click a byte range to highlight the corresponding struct field. An interactive hex decoder where the user pastes hex and sees decoded fields.
- **Mobile fallback:** Static table with byte offsets; no interactive decoder.
- **Accessibility:** Table with headers: Offset, Size, Field, Type, Example Value.

### Diagram 7 — Node state machine
- **What it shows:** UNPAIRED → PAIRED → DEPLOYED with transitions: pair, deploy, stop (keep paired), unpair (forget). Pending transitions shown as dashed arrows.
- **Audience:** Layer 2–3
- **Labels:** State names, transition commands, UI action labels (Start, Stop, Unpair).
- **Animation:** Click a transition to see the message sequence (e.g. DEPLOY_NODE → DEPLOY_ACK).
- **Mobile fallback:** Static state diagram as a table of transitions.
- **Accessibility:** Table: From State, To State, Trigger, Messages.

### Diagram 8 — Sync window timeline
- **What it shows:** A timeline showing mothership pre-wake (10 s before), sync window open (120 s), repeated `SYNC_WINDOW_OPEN` broadcasts (every 5 s), node wake + flush, snapshot ACKs, early shutdown, re-arm, power off. Phase alignment across multiple cycles.
- **Audience:** Layer 2–3
- **Labels:** Time markers, message types, wake events.
- **Animation:** Play button animates a single sync cycle in real-time (compressed).
- **Mobile fallback:** Vertical timeline.
- **Accessibility:** Ordered list with timestamps and events.

### Diagram 9 — Cloud architecture (current vs planned)
- **What it shows:** Side-by-side comparison. Left: Google Apps Script + Sheets (CSV/JSON, polling, shared token). Right: Supabase (Edge Function, PostgreSQL, real-time, RLS, API keys, multi-tenant).
- **Audience:** Layer 3
- **Labels:** Component names, protocol, auth method, query method.
- **Animation:** None. Toggle between "Current" and "Planned" tabs.
- **Mobile fallback:** Stacked cards (current above, planned below).
- **Accessibility:** Two tables with a summary of differences.

### Diagram 10 — Field deployment workflow (1–12 steps)
- **What it shows:** The 12-step field workflow from §5 as a numbered horizontal stepper with icons.
- **Audience:** Layer 1–2
- **Labels:** Step numbers, short action labels (Power on hub, Connect WiFi, Set time, Set intervals, Find stations, Name stations, Deploy, Place in field, Set upload, Start monitoring, View data).
- **Animation:** Progress bar fills as user scrolls; each step card animates in.
- **Mobile fallback:** Vertical numbered list with icons.
- **Accessibility:** Ordered list with `aria-label` per step.

---

## 8. Recommended About-page layout

### Overall structure

A single scrollable page with a **sticky layer-switcher** at the top (per `FIELDMESH_ABOUT_SECTION_DESIGN.md`). Three tabs plus an optional simulator:

```
┌──────────────────────────────────────────────────────┐
│  fieldMesh — About                           [≡]     │
│                                                      │
│  ┌──────────┐ ┌───────────┐ ┌────────────┐          │
│  │ What is  │ │ How does  │ │ Show me    │          │
│  │ this?    │ │ it work?  │ │ everything │          │
│  └──────────┘ └───────────┘ └────────────┘          │
│                                                      │
│  [Active layer content scrolls here]                 │
│                                                      │
│  [▶ Run a sync cycle]  (floating, bottom-right)       │
└──────────────────────────────────────────────────────┘
```

### Desktop (≥1024 px)
- Sticky tab bar at top, full width.
- Layer content in a max-width 1200 px centred column.
- Diagrams full-width within the column.
- Two-column layout for sensor grid (3 × 2 cards) and feature lists.
- Mini-simulator as an embedded widget in the right rail or as a modal.

### Tablet (768–1023 px)
- Sticky tab bar at top.
- Single-column content.
- Sensor grid 2 × 3.
- Diagrams scale to container width.
- Mini-simulator as a modal on tap.

### Mobile (<768 px)
- Sticky tab bar, condensed (icons + short labels or a dropdown).
- Single-column content.
- Sensor grid 1 × 6 or 2 × 3.
- Diagrams: horizontal diagrams stack vertically; animations replaced with static labelled SVG.
- Mini-simulator: simplified auto-running version; full controls in a modal.
- Floating "Run sync cycle" button bottom-right, above any nav.

### Progressive disclosure pattern
- Layer 1 is default open.
- Each layer ends with a "Go deeper" button that switches to the next layer and scrolls to the relevant section.
- Advanced details (MAC addresses, firmware build strings, config versions, protocol hex) are in collapsed `<details>` elements within Layer 3.
- A floating "▶ Run a sync cycle" button is always visible and opens the mini-simulator.

### Status badges
Throughout all layers, use the status badges from §2–3 to distinguish:
- ✅ Implemented (sage green dot)
- 🟡 Partial (amber dot)
- 🔵 Planned (blue dot)
- ⚪ Architectural intention (grey dot)

This keeps the About page honest about what exists today vs what is on the roadmap.

---

## 9. Suggested headings and microcopy

### Tab labels
- **Layer 1:** "What is this?"
- **Layer 2:** "How does it work?"
- **Layer 3:** "Show me everything"

### Section headings

**Layer 1:**
- "FieldMesh in 30 seconds"
- "What does it measure?"
- "Why trust the data?"
- "Who is it for?"

**Layer 2:**
- "Sensor nodes"
- "The hub (mothership)"
- "How data moves"
- "The dashboard"
- "Setting up in the field"
- "What's working today" *(with status badges)*
- "What's coming" *(with status badges)*

**Layer 3:**
- "Hardware architecture"
- "Firmware architecture"
- "Node state model"
- "Timing architecture"
- "Data flow"
- "Protocol architecture"
- "Local web interface"
- "Cloud architecture"
- "Security model"
- "Reliability and failure handling"
- "Power management"
- "Scalability"

### Button text
- "Run a sync cycle" — simulator launch
- "Tell me more →" — Layer 1 → Layer 2
- "Show me the raw protocol →" — Layer 2 → Layer 3
- "Download the protocol spec" — link to `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`
- "View on GitHub" — link to the repository
- "Back to top ↑"

### Callouts
- **Info callout (blue):** "Battery voltage is a system health metric, not an environmental reading. It tells you when a node needs charging."
- **Warning callout (amber):** "Wind measurement is in development. The ultrasonic anemometer hardware exists on the node PCB, but the firmware does not yet produce reliable wind readings."
- **Success callout (sage):** "Data is stored locally on the hub before upload. If the cellular signal drops, nothing is lost."
- **Neutral callout (grey):** "The cloud backend is currently Google Sheets. A migration to Supabase (PostgreSQL) is planned for production scale."

### Microcopy for the "Why trust it?" card (Layer 1)
- "Every reading has a timestamp from a precision real-time clock on the node, backed by a coin cell so time survives power loss."
- "Data is stored locally on the hub before it is uploaded. If the cellular signal drops, the hub keeps the data and retries on the next cycle."
- "Each node reports its battery voltage in every reading, so you can see which stations need attention before they go dark."
- "After a power cut, a deployed node with a working clock picks up where it left off — no redeployment needed."

### Microcopy for sensor cards (Layer 1)
- **Air temperature:** "How warm or cool the air is. Measured by the SHT41 sensor."
- **Humidity:** "Moisture in the air, as relative humidity. Measured by the SHT41 sensor."
- **Light (spectral):** "Light broken into eight colour bands, from violet (415 nm) to deep red (680 nm). Measured by the AS7341 sensor."
- **Soil moisture:** "How wet the soil is, as volumetric water content. Two probes, measured via the ADS1115 analog converter."
- **Soil temperature:** "How warm the soil is. Two probes, measured via thermistors on the ADS1115."
- **Wind:** "Wind speed and direction. In development — the ultrasonic anemometer hardware is on the node board."

---

## 10. Technical claims requiring confirmation

Before publishing, verify the following:

1. **AS7341 vs AS7343:** The firmware file is named `sensors_par_as7343.cpp` but uses the Adafruit AS7341 library and the AS7341 I2C address (0x39). Confirm which physical sensor is populated on the node PCB. The About page should say "AS7341" unless the board actually has an AS7343.

2. **Full over-the-air LTE upload:** The modem is verified at the AT level (Tests 9–13), but a complete HTTPS POST to Google Apps Script with a real antenna and active SIM has not been confirmed in the bring-up logs. Confirm whether `performModemUpload()` has successfully uploaded data to the cloud end-to-end.

3. **SD card on mothership V1:** The SD card has a known PCB routing bug (GPIO23 MOSI → pin 8 instead of pin 3). LittleFS is the working storage. Confirm whether the SD card fix has been applied (bodge wire or new PCB revision) and whether SD is now functional.

4. **Node V2 PCB status:** The `NODE_HARDWARE_V2_CHECKLIST.md` has many items checked off but several still open (reverse-polarity protection, leakage current, LED strategy, decoupling review). Confirm which PCB revision is currently in use and whether V2/V3 boards are in field testing.

5. **Multi-node sync:** The autonomous sync pipeline was proven with 1 node (17 snapshots, seq 2–18). Confirm whether multi-node sync (2+ nodes in the same window) has been tested and whether the 120-second window + early-shutdown logic handles concurrent node flushes correctly.

6. **`projectStarted` field:** `g_projectStartedUnix` is set in `main.cpp` NVS, but confirm whether it is included in the JSON `status` payload and whether the Apps Script backend writes it to the Status sheet.

7. **`deployedSinceUnix` field:** Documented in `FIELDMESH_NEW_DATA_FIELDS_BRIEF.md` as per-node in NVS. Confirm whether the mothership firmware actually stores and emits this per node in `status.nodes[]`.

8. **Mothership battery in JSON status:** `FIELDMESH_NEW_DATA_FIELDS_BRIEF.md` says `batVoltage` is now in the Status sheet. Confirm whether `buildStatusJson()` in `config_server.cpp` includes the mothership battery voltage (the Field UI redesign doc flags this as an open question in §12).

9. **`lastUploadResult` field:** Documented as a new Status sheet field. Confirm whether the firmware populates this (success/failed/pending) after each upload attempt.

10. **Solar charging on the mothership:** The node has verified solar charging (CN3163). Confirm whether the mothership V1 PCB has a solar charging path or relies on USB/battery only.

11. **ESP-NOW encryption:** All peer registrations use `peer.encrypt = false`. Confirm whether this is acceptable for the target deployments or whether encrypted ESP-NOW is planned.

12. **Dashboard polling interval:** The dashboard design doc says 60-second polling. Confirm the actual implemented interval in the React frontend.

13. **CWT TH-A soil probe mode:** `soil_moist_temp.cpp` has a compile-time flag `SOIL_CWT_THA_MODE` that switches between legacy polynomial calibration and CWT TH-A probe conversion. Confirm which mode is active in the current build and which soil probes are in use.

14. **Mux address:** `protocol.h` defaults `MUX_ADDR` to 0x70, but the node `platformio.ini` overrides it to 0x71. Confirm which address the populated PCA9548A uses on the current node board.

---

## 11. Evidence files

All files read as evidence for this document:

### Firmware source
- `node/firmware/v2/shared/protocol.h` — V2 snapshot protocol, sensor IDs, message types, wire format
- `node/firmware/v2/platformio.ini` — build flags, pin assignments, sensor config
- `node/firmware/v2/src/main.cpp` — node main loop, wake/sleep, power hold, identity
- `node/firmware/v2/src/sensors/sensors.cpp` — sensor registry, backend init, read budgets
- `node/firmware/v2/src/sensors/sensors_sht41.cpp` — SHT41 air temp/humidity backend
- `node/firmware/v2/src/sensors/sensors_par_as7343.cpp` — AS7341 spectral backend
- `node/firmware/v2/src/sensors/soil_moist_temp.cpp` — ADS1115 soil moisture/temp backend
- `node/firmware/v2/src/storage/local_queue.cpp` — NVS-backed circular byte-slab queue
- `mothership/firmware/v2/src/main.cpp` — mothership main, wake-reason branching, sync wake, upload
- `mothership/firmware/v2/src/config/config_server.cpp` — captive portal, web server, sync globals
- `mothership/firmware/v2/src/config/node_registry.cpp` — node registry, NVS meta, desired config
- `mothership/firmware/v2/src/config/transmission_settings.cpp` — upload settings, NVS persistence
- `mothership/firmware/v2/src/comms/modem_driver.cpp` — A7670G LTE modem driver
- `mothership/firmware/v2/src/comms/espnow_manager.cpp` — ESP-NOW sync, snapshot queue, node tracking
- `mothership/firmware/v2/src/storage/upload_queue.cpp` — upload cursor, retry, A/B recovery
- `mothership/firmware/v2/src/storage/flash_logger.cpp` — LittleFS CSV logging, V1/V2 decode
- `mothership/firmware/v2/src/storage/json_payload.cpp` — CSV → JSON payload builder
- `mothership/firmware/v2/src/system/power.cpp` — power management, battery ADC, config latch
- `mothership/firmware/v2/src/system/pins.h` — pin assignments, ADC config, sync defaults
- `mothership/firmware/v2/src/time/rtc_alarm.h` — RTC alarm management

### Documentation
- `docs/concept_overview.md` — system concept, node lifecycle, data format, sensor backends
- `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` — JSON upload protocol spec
- `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md` — Supabase migration plan, schema, RLS
- `docs/FIELDMESH_USER_ONBOARDING_BRIEF.md` — multi-tenant onboarding, provisioning
- `docs/FIELDMESH_FIELD_UI_REDESIGN.md` — Field UI redesign, terminology mapping
- `docs/FIELDMESH_DASHBOARD_DESIGN.md` — dashboard architecture, backend, frontend
- `docs/FIELDMESH_UI_STYLE_GUIDE_2026.md` — design tokens, colour system, principles
- `docs/FIELDMESH_SPATIAL_LOCATION_PLAN.md` — GPS/location plan, phases
- `docs/FIELDMESH_NEW_DATA_FIELDS_BRIEF.md` — new data fields, dashboard usage
- `docs/FIELDMESH_ABOUT_SECTION_DESIGN.md` — About page visual/interaction design
- `docs/FIRMWARE_AND_HARDWARE_NOTES.md` — PCB bring-up, flashing, firmware architecture
- `node/docs/NODE-PCB-OVERVIEW.md` — node PCB, ultrasonic anemometer subsystem
- `node/docs/NODE_HARDWARE_V2_CHECKLIST.md` — V2 hardware checklist
- `mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md` — power/wake architecture
- `mothership/docs/MOTHERSHIP_V1_FIRMWARE_PLAN.md` — wake-gated firmware plan
- `mothership/docs/MOTHERSHIP_V1_BRINGUP_RESULTS_2026-06-19.md` — hardware bring-up results

---

*This document is grounded in the repository as of 2026-06-28. Firmware and hardware are under active development; capabilities marked as partial or planned should be re-verified before publication.*