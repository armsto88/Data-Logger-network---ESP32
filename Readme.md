# ESP32 Sensor Network – Mothership + Nodes

A small **ESP-NOW**–based sensor network for environmental data logging, designed to survive field power cuts and resume logging automatically, with explicit **time sync** between mothership and nodes.

---

## System Overview

The system is built around:

- **Mothership (ESP32-S3):**
  - Runs a Wi-Fi access point (`Logger001`)
  - Exposes a web UI dashboard
  - Manages node discovery, pairing, deployment & unpairing
  - Stores incoming sensor data to CSV on an SD card
  - Keeps time via a DS3231 RTC
  - Periodically sends **TIME_SYNC** messages to nodes
  - Persists node state (paired / deployed, IDs, names, wake interval) in NVS
  - Tracks **time health** per node (how fresh the last TIME_SYNC is)

- **Sensor Nodes (ESP32-C3 Mini, one or more):**
  - Measure environmental variables (e.g. air temperature)
  - Communicate with the mothership using ESP-NOW
  - Use a DS3231 RTC + Alarm 1 to drive their sampling interval
  - Persist state in NVS:
    - mothership MAC
    - deployed flag
    - wake interval
    - RTC sync flag
    - last time sync (unix timestamp)
  - Automatically request time sync (`REQUEST_TIME`) when needed
  - Designed to resume operation automatically after a power cut (with RTC coin cell)

---

## Features

- **ESP-NOW “mesh-ish” communication**
  - **Data:** node → mothership (`sensor_data_message_t`)
  - **Control:** mothership → node (`PAIR_NODE`, `DEPLOY_NODE`, `SET_SCHEDULE`, `UNPAIR_NODE`, `TIME_SYNC`)

- **DS3231 RTC integration (both sides)**
  - Accurate timestamps on measurements
  - Configurable wake/sampling intervals via **Alarm 1** on the node DS3231
  - Mothership DS3231 is the **time authority**; nodes sync to it

- **TIME_SYNC protocol**
  - Nodes send `REQUEST_TIME` when:
    - They’re bound to a mothership but `rtcSynced == false`, or
    - More than 24 h has passed since their last time sync
  - Mothership responds with `TIME_SYNC` carrying DS3231 time
  - Mothership can also broadcast fleet-wide time sync periodically
  - UI shows “Fresh / OK / Stale / Unknown” time health per node

- **CSV logging to SD card (mothership)**
  - Single `datalog.csv` with rows:
    ```
    timestamp,node_id,node_name,mac,sensor_type,value
    ```
  - Periodic mothership heartbeats:
    ```
    timestamp,MOTHERSHIP,<mac>,STATUS,ACTIVE
    ```
  - Optional TIME_SYNC fleet events logged as `TIME_SYNC_FLEET`

- **Web UI dashboard (from mothership)**
  - View live RTC time (ticking in the browser)
  - Set **global wake interval** (1–60 minutes) → broadcasts `SET_SCHEDULE` to paired/deployed nodes
  - Node Manager:
    - See all nodes with state chips (Unpaired / Paired / Deployed)
    - Per-node “time health” (Fresh / OK / Stale / Unknown)
    - Configure & Start (ID, name, interval, Start/Stop/Unpair)
  - Node discovery (“Discover Nodes” button)
  - Download CSV log

- **Persistent state in NVS**
  - **Mothership:**
    - Paired/deployed nodes (`paired_nodes` namespace)
    - Node metadata (`node_meta` namespace: `id_<firmwareId>`, `name_<firmwareId>`)
    - Global wake interval (`ui` namespace)
  - **Nodes:**
    - Node state enum
    - `rtcSynced`, `deployedFlag`
    - `g_intervalMin` (wake interval)
    - `mothershipMAC`
    - `lastTimeSyncUnix`

- **Power-loss resilience**
  - After a hard power cut, both sides reload their state from NVS
  - If the node RTC still has valid time (coin cell present), **deployed nodes resume sending data** without manual intervention
  - If RTC power is lost, nodes fall back to a safe state and ask for fresh time

---

## High-Level Architecture

```
+---------------------------+               +------------------------------+
|       Sensor Node(s)      |  ESP-NOW      |          Mothership          |
|       (ESP32-C3 Mini)     | <-----------> |       (ESP32-S3, AP)         |
+---------------------------+               +------------------------------+
  - Firmware ID (e.g. TEMP_001)              - Wi-Fi AP "Logger001"
  - DS3231 RTC + Alarm 1                      - DS3231 RTC
  - RTC INT pin → FET / wake (hardware)       - SD card (datalog.csv)
  - NVS   (MAC, deployedFlag, etc.)          - ESP-NOW manager
  - Packets:                                  - Web UI (HTTP server)
      DISCOVER_REQUEST                        - Node Manager + TIME_SYNC
      PAIRING_REQUEST
      REQUEST_TIME
      SENSOR_DATA
                                             Control packets:
                                              DISCOVER_RESPONSE / SCAN
                                              PAIR_NODE / PAIRING_RESPONSE
                                              DEPLOY_NODE
                                              SET_SCHEDULE
                                              UNPAIR_NODE
                                              TIME_SYNC (+ fleet broadcast)
```

(Current implementation polls the DS3231 Alarm 1 flag in firmware; no GPIO wiring to INT is required yet, but the design is ready for INT→FET / wake pin.)

---

### Node “Belonging” States

At a high level, nodes are in one of three effective states:

- **Unpaired**
    - No mothership MAC known.
    - Node periodically sends DISCOVER_REQUEST and PAIRING_REQUEST broadcasts.

- **Paired / Bound**
    - Mothership MAC known and stored in NVS.
    - Node is “owned” but not yet deployed.
    - RTC may or may not be synced (`rtcSynced` flag).

- **Deployed**
    - Mothership MAC known.
    - `deployedFlag == true`.
    - RTC has been synced from a DEPLOY_NODE or TIME_SYNC message.
    - Node arms the DS3231 Alarm 1 based on `g_intervalMin` and sends data on each alarm.

On the node, the effective state is derived from:
```c
bool hasMothershipMAC();  // derived from stored MAC in NVS
bool rtcSynced;           // true once time is set via DEPLOY or TIME_SYNC
bool deployedFlag;        // persisted "this node is deployed" flag

enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + deployed flag set
};
```
The raw NodeState enum is also stored in NVS for debugging, but runtime behaviour is ultimately driven by `hasMothershipMAC()`, `deployedFlag`, `rtcSynced`, and the DS3231 alarm.

On the mothership, each node is tracked as:
```c++
struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;         // firmware ID (e.g. "TEMP_001")
  String    nodeType;       // e.g. "AIR_TEMP"
  uint32_t  lastSeen;       // millis() of last packet
  bool      isActive;       // auto-false after 5 min silence
  NodeState state;          // UNPAIRED / PAIRED / DEPLOYED
  uint8_t   channel;

  // User-facing meta (from NVS "node_meta")
  String    userId;         // numeric ID, e.g. "001"
  String    name;           // friendly name, e.g. "North Hedge 01"

  // Time sync health
  uint32_t  lastTimeSyncMs; // millis() when last TIME_SYNC was sent
};
```
The Node Manager page uses `lastTimeSyncMs` to show a small “time health” pill per node:
- Fresh: < 6 h since last TIME_SYNC
- OK: 6–24 h
- Stale: > 24 h
- Unknown: no TIME_SYNC yet

---

### State Transitions

**Discovery → Unpaired → Paired**
- Node boots with no mothership MAC → STATE_UNPAIRED.
- Node periodically broadcasts DISCOVER_REQUEST.
- Mothership receives it, calls `registerNode(...)`, and replies with DISCOVER_RESPONSE.
- Node appears in Node Manager as Unpaired.
- In web UI: click node, set ID/name, choose Start/deploy.
- Mothership calls `pairNode(nodeId)`:
    - Sends PAIR_NODE and PAIRING_RESPONSE.
    - Sets state = PAIRED and persists.
- Node stores mothershipMAC and clears deployedFlag + rtcSynced → STATE_PAIRED.

**Paired → Deployed**
- Node is Paired (has mothershipMAC).
- In UI: Configure & Start, set ID/name/interval, choose Start.
- Mothership:
    - Broadcasts SET_SCHEDULE (interval) to all paired/deployed.
    - Sends DEPLOY_NODE with DS3231 time and mothership ID.
- Node on DEPLOY_NODE:
    - Sets DS3231, rtcSynced = true, deployedFlag = true, g_intervalMin, lastTimeSyncUnix
    - Arms DS3231 Alarm 1, sends immediate reading (optionally) → STATE_DEPLOYED.

**Deployed → Paired (Stop)**
- Node is Deployed.
- In UI: Configure & Start, choose Stop/keep paired.
- Mothership updates state = PAIRED.
- Node may clear deployedFlag (policy), becoming STATE_PAIRED.

**Unpair**
- In UI: Configure & Start, choose Unpair/forget.
- Mothership sends UNPAIR_NODE, removes node from ESP-NOW & NVS.
- Node clears mothershipMAC, rtcSynced, deployedFlag, lastTimeSyncUnix → STATE_UNPAIRED.

---

### Time Sync Behaviour

#### Node-initiated TIME_SYNC
- If `hasMothershipMAC()` && `!rtcSynced`:
    - Send REQUEST_TIME every ~30 s until TIME_SYNC.
- If `rtcSynced == true`:
    - If >24 h since lastTimeSyncUnix, send another REQUEST_TIME (max once every 30 s).

When TIME_SYNC is received:
- Node sets DS3231, rtcSynced = true, lastTimeSyncUnix.
- Persists all to NVS.
- If STATE_DEPLOYED and has interval, re-arms DS3231 alarm.

#### Fleet-wide TIME_SYNC

On mothership:
- `espnow_loop()` calls `broadcastTimeSyncIfDue(false)`:
    - If >24 h since last fleet sync, broadcast TIME_SYNC to all PAIRED/DEPLOYED.
    - Log event and CSV row.
- `broadcastTimeSyncIfDue(true)` = force immediate fleet sync (e.g. GUI button).

---

## Power-Loss Behaviour

### Normal Power Cut (RTC coin cell OK)
- NVS state retained, node DS3231 keeps time.
- On reboot:
    - Mothership loads paired nodes from NVS, restores state.
    - Node loads state: often STATE_DEPLOYED, deployedFlag = true, rtcSynced = true, interval, mothershipMAC, lastTimeSyncUnix.
    - If `rtc.lostPower() == false`, stays STATE_DEPLOYED, resumes alarm-driven sends.

### RTC Lost Power (no coin cell / dead cell)
- If DS3231 lost backup:
    - `rtc.lostPower() == true` at boot.
    - Node marks `rtcSynced = false`, clears deployedFlag, persists.
    - Remembers mothershipMAC.
    - Effective state: STATE_PAIRED.
    - Starts REQUEST_TIME packets for new TIME_SYNC.

Policy: RTC lost power → node drops to PAIRED, expects fresh DEPLOY.

---

## Node Firmware & Hardware

### Example Node Hardware

- **MCU**: ESP32-C3 Mini
- **RTC**: DS3231 (I²C)
- **INT/SQW**: Not required currently; designed for alarm INT in future.
- **Sensor**: Dummy temp for now (DS18B20, BME280, etc. possible)

### Boot Lifecycle

- NVS init: `nvs_flash_init()`, handle error cases.
- Load config: `state`, `rtcSynced`, `deployedFlag`, `interval`, `mothershipMAC`, `lastTimeSyncUnix`
- RTC init: `Wire.begin(...)` `rtc.begin()`
    - If `rtc.lostPower()`:
        - `rtcSynced = false`, `deployedFlag = false`, `lastTimeSyncUnix = 0`, persist.
    - Normalise DS3231 Alarm 1 flag.
- ESP-NOW init: station mode, `esp_now_init()`, callbacks, add peer(s).
- Initial state log: Print MAC, bootCount, state, RTC synced.
- **Main Loop:**
    - If bound but unsynced → REQUEST_TIME.
    - If bound & synced & >24h since last sync → REQUEST_TIME.
    - Poll DS3231 Alarm 1 flag:
        - If set:
            - handle alarm: log, if deployed & synced & paired → sendSensorData.
            - Re-arm alarm, clear A1F.

---

## Mothership Firmware & Hardware

### Example Hardware

- **MCU**: ESP32-S3 dev module
- **RTC**: DS3231 (I²C)
- **SD**: microSD with `datalog.csv`
- **Wi-Fi**: Soft AP (SSID `Logger001`, password `logger123`)

### On Boot

- `setupRTC()`: init DS3231, log time.
- `setupSD()`: mount SD, check CSV exists.
- Configure Wi-Fi: AP/STA mode.
- `setupESPNOW`: init, register callbacks, add peers, load NVS paired nodes.
- Start HTTP server (variety of routes), log boot/fleet summary.

### ESP-NOW Packet Handling

- `sensor_data_message_t`: Promote node to DEPLOYED, log to CSV.
- `discovery_message_t`: `registerNode(...)`, DISCOVER_RESPONSE.
- `pairing_request_t`: Determine state, `registerNode(...)`, PAIRING_RESPONSE.
- `time_sync_request_t`: Log, `sendTimeSync` with DS3231 time.

---

## Web UI Dashboard

- **Access**
    - Connect to `Logger001` (default: `logger123`)
    - Open [http://192.168.4.1/](http://192.168.4.1/)
- **Main Page**
    - Shows live DS3231 time
    - Set RTC time from browser
    - Set global wake interval (broadcasts via ESP-NOW)
    - Download logs
    - Node discovery & fleet overview
- **Node Manager** (`/nodes`)
    - List all nodes: ID, firmware, friendly name, “time health” pill, state chip.
- **Configure & Start** (`/node-config`)
    - Set/see: firmware ID, state, node ID, name, interval
    - Actions: Start/Deploy, Stop/keep paired, Unpair/forget
    - Shows broadcast/command results

---

## Build & Flash

### Mothership

    Board: ESP32-S3 Dev Module
    Project dir: mothership/

    pio run -t upload
    pio device monitor

Then:

- Connect to Logger001
- Open [http://192.168.4.1/](http://192.168.4.1/)

### Node(s)

    Board: ESP32-C3 Mini
    Project dir: e.g. nodes/air-temperature-node/

    pio run -t upload
    pio device monitor

On first boot you should see logs like:
```
STATE_UNPAIRED ...
📡 Discovery request sent
⏰ Bound but RTC unsynced → requesting initial TIME_SYNC
⏰ Time sync request sent
```
Once mothership is up and “Discover Nodes” is run, node will appear in /nodes.

---

## Current Status & Next Steps

**Current**
- ✅ Robust node state model (Unpaired / Paired / Deployed)
- ✅ End-to-end Pair / Deploy / Stop / Unpair flows via web UI
- ✅ CSV logging to SD card with node ID + friendly name
- ✅ Web UI for discovery, control, and RTC management
- ✅ NVS persistence on both mothership and nodes
- ✅ Recovery from full power cuts (with RTC coin cell present)
- ✅ Explicit REQUEST_TIME / TIME_SYNC handshake per node
- ✅ Fleet-wide periodic TIME_SYNC
- ✅ Per-node time-health indicators in the Node Manager

**Future**
- Wire DS3231 INT → FET / wake pin; enable true alarm-driven deep sleep
- Real sensor integrations (DS18B20, BME280, soil moisture, PAR, etc.)
- Per-node wake intervals instead of global broadcast
- OTA firmware updates (at least for mothership)
- Diagnostic charts in web UI (per-node sparkline)
- Data ingestion helpers for R/Python pipelines
