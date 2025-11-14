# ESP32 Sensor Network – Mothership + Nodes

A small **ESP-NOW**–based sensor network for environmental data logging.

---

## System Overview

The system is built around:

- **Mothership (ESP32-S3):**
  - Runs a Wi-Fi access point (`Logger001`)
  - Exposes a web UI dashboard
  - Manages node discovery, pairing and deployment
  - Stores incoming sensor data to CSV on an SD card
  - Keeps time via a DS3231 RTC

- **Sensor Nodes (ESP32-C3 Mini, one or more):**
  - Measure environmental variables (e.g., air temperature)
  - Communicate with the mothership using ESP-NOW
  - Wake on an RTC alarm interval
  - Send sensor readings when deployed

---

## Features

- **ESP-NOW mesh** style communication (unidirectional: node → mothership data; control: mothership → nodes)
- **DS3231 RTC integration** (both mothership & nodes):
  - Accurate timestamps on measurements
  - Configurable wake/sampling intervals via DS3231 Alarm 1
- **CSV logging to SD card** (mothership):
  - Single `datalog.csv` (timestamp, node ID, MAC, sensor type, value)
  - Periodic “MOTHERSHIP STATUS” heartbeats
- **Web UI dashboard** (from mothership):
  - View RTC time
  - Node discovery, pairing, deployment & unpairing
  - Set global wake interval (1–60 mins)
  - Download CSV log
- **Persistent state in NVS** (both sides):
  - Mothership remembers paired/deployed nodes across reboots
  - Nodes remember their mothership, wake interval & RTC sync status

---

## High-Level Architecture

```text
+---------------------+                     +--------------------------+
|   Sensor Node(s)    |  ESP-NOW (channel)  |        Mothership        |
|  (ESP32-C3 Mini)    | <-----------------> |      (ESP32-S3, AP)      |
+---------------------+                     +--------------------------+
  - NODE_ID, NODE_TYPE                         - Wi-Fi AP "Logger001"
  - DS3231 RTC                                  - DS3231 RTC
  - RTC INT pin wake (future deep-sleep)        - SD card (datalog.csv)
  - ESP-NOW: DISCOVER_REQUEST                   - ESP-NOW manager
             SENSOR_DATA                        - Web UI (HTTP server)
             REQUEST_TIME / TIME_SYNC           - Pair / Deploy / Unpair
             SET_SCHEDULE
```

---

### Node "Belonging" States

- **Unpaired**: No mothership MAC known
- **Paired / Bound**: Mothership MAC known, RTC not yet synced
- **Deployed**: Mothership MAC known & RTC synced — node sends data on each wake

**Node effective state:**
- `mothershipMAC` (all zeros = not bound)
- `rtcSynced` (true once deployment time is received)

```cpp
enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, RTC not synced
  STATE_DEPLOYED = 2    // has mothership MAC + RTC synced
};
```

**Mothership tracks each node as:**
```cpp
struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;
  String    nodeType;
  uint32_t  lastSeen;
  bool      isActive;
  NodeState state; // UNPAIRED / PAIRED / DEPLOYED
  uint8_t   channel;
};
```

---

### State Transitions

#### Discovery → Unpaired → Paired:
1. Node broadcasts `DISCOVER_REQUEST`
2. Mothership replies with `DISCOVER_RESPONSE`
3. Node appears in “Unpaired Nodes”
4. User selects node(s) → _Pair Selected Nodes_
5. Mothership sends `PAIR_NODE` + `PAIRING_RESPONSE`
6. Node saves mothershipMAC, sets `STATE_PAIRED`, `rtcSynced=false`

#### Paired → Deployed:
1. User selects paired nodes → _Deploy Selected Nodes_
2. Mothership sends `DEPLOY_NODE` with RTC time
3. Node sets DS3231 time, `rtcSynced=true`, state = `STATE_DEPLOYED`
4. Node starts sending sensor data

#### Deployed → Paired (Revert):
1. User clicks _Revert to Paired_
2. Mothership sets state to `PAIRED`, sends `PAIR_NODE`
3. Node keeps mothershipMAC, sets `rtcSynced=false`, `STATE_PAIRED`  
   (Node stops sending data)

#### Unpair:
1. User selects nodes in _Unpair Nodes_
2. Mothership sends `UNPAIR_NODE`, removes from registry
3. Node clears mothershipMAC, sets `rtcSynced=false`, `STATE_UNPAIRED`;  
   node resumes discovery broadcasts

---

## Node Firmware & Hardware

**Example Node Hardware:**
- **MCU:** ESP32-C3 Mini
- **RTC:** DS3231 (I²C: SDA=8, SCL=9, INT=3)
- **Sensor:** Dummy temperature (easily swapped for another sensor)

**Lifecycle:**
1. Load persisted config from NVS (`mothershipMAC`, `rtcSynced`, wake interval)
2. Initialise I²C + DS3231  
   (if `rtc.lostPower()` → `rtcSynced=false`)
3. Start ESP-NOW; add:
   - Broadcast peer (`FF:FF:FF:FF:FF:FF`)
   - Pre-configured mothership (if known)

**In loop():**
- If **UNPAIRED**: Periodically send `DISCOVER_REQUEST`, `PAIRING_REQUEST`
- If **PAIRED**: Log waiting for DEPLOY; if RTC unsynced, send `REQUEST_TIME` occasionally
- If **DEPLOYED**: On interval tick:
    - Read sensor
    - Send data
    - Log status

**Node message types:**
- _To mothership:_ `DISCOVER_REQUEST`, `PAIRING_REQUEST`, `REQUEST_TIME`, `sensor_data_message_t`
- _From mothership:_ `DISCOVER_RESPONSE`, `PAIR_NODE`, `PAIRING_RESPONSE`,  
  `DEPLOY_NODE`, `UNPAIR_NODE`, `SET_SCHEDULE`, `TIME_SYNC`

---

## Mothership Firmware & Hardware

**Example Mothership Hardware:**
- **MCU:** ESP32-S3
- **RTC:** DS3231 (I²C)
- **SD:** datalog.csv (SPI)
- **Wi-Fi:** Soft AP (`Logger001`, pw: `logger123`)

**On Boot:**
- Initialise RTC, SD card, Wi-Fi AP (+ web server)
- Start ESP-NOW, add broadcast & known peers
- Load previous pair/deploy state from NVS
- Print current RTC time & wake interval  
  (start auto-discovery)

**Main tasks in loop():**
- Handle HTTP web UI
- Auto discovery & node liveness via `espnow_loop`:
    - Send `DISCOVERY_SCAN` every 30s
    - Mark inactive nodes
- Log `MOTHERSHIP,STATUS,ACTIVE` every 60s

**On ESP-NOW packets:**
- **sensor_data_message_t:** Ensure node is `DEPLOYED`, log CSV record
- **DISCOVER_REQUEST:** Register/refresh node, send `DISCOVER_RESPONSE`
- **PAIRING_REQUEST:** Send `PAIRING_RESPONSE` reflecting current state
- **REQUEST_TIME:** Send current time via `TIME_SYNC`
- Other control: actioned via Web UI

---

## Web UI Dashboard

- Served at: [http://192.168.4.1/](http://192.168.4.1/) (connect to Logger001)

### Dashboard Sections

| Section                   | Actions                                                                      |
|---------------------------|------------------------------------------------------------------------------|
| **Wake Interval**         | Dropdown: 1, 5, 10, 20, 30, 60 mins; Broadcast to all nodes                  |
| **Current RTC Time**      | Shows DS3231 time; live browser-ticking                                      |
| **Data Logging**          | Logging status, Download CSV button                                          |
| **Node Discovery & Stats**| Mothership MAC, node counts, Discover Nodes button                           |
| **Unpaired Nodes**        | List: Pair selected nodes                                                    |
| **Paired (Ready to Deploy)** | List: Deploy nodes {"RTC not synced"}                                     |
| **Unpair Nodes**          | List: Unpair selected nodes                                                  |
| **Deployed (Active)**     | List: Revert to Paired button                                                |
| **RTC Settings Panel**    | Manual DS3231 setting, auto-detect button                                    |
| **Footer**                | Sticky bar: Refresh, CSV Download (auto-refreshes ~10s)                      |

<details>
<summary>Dashboard UI Preview (placeholders):</summary>

- ![Dashboard overview](docs/img/dashboard-overview.png)
- ![Node lists](docs/img/dashboard-nodes.png)
- ![RTC settings](docs/img/dashboard-rtc-settings.png)
- ![Wake interval section](docs/img/ui-wake-interval.png)
- ![RTC time section](docs/img/ui-rtc-time.png)
- ![Data logging section](docs/img/ui-data-logging.png)
- ![Discovery & stats](docs/img/ui-discovery-stats.png)
- ![Unpaired nodes](docs/img/ui-unpaired.png)
- ![Paired nodes](docs/img/ui-paired.png)
- ![Unpair nodes](docs/img/ui-unpair.png)
- ![Deployed nodes](docs/img/ui-deployed.png)
- ![RTC settings panel](docs/img/ui-rtc-settings-panel.png)
</details>

---

## Build & Flash

_Note: Assumes PlatformIO, but can adapt for Arduino IDE._

### Mothership

- **Board:** ESP32-S3 dev module
- **Project:** `mothership/`
- **Key Files:**
  - `src/main.cpp`        (Web UI + ESP-NOW + RTC + SD)
  - `src/espnow_manager.cpp`
  - `src/rtc_manager.cpp`
  - `src/sd_manager.cpp`
  - `include/protocol.h`  (message defs, pins, channel)

**Flash & Monitor:**
```sh
pio run -t upload
pio device monitor
```
Connect to Wi-Fi `Logger001`, open `http://192.168.4.1/`.

---

### Node(s)

- **Board:** ESP32-C3 Mini
- **Project:** e.g., `nodes/air-temperature-node/`
- **Key Files:**
  - `src/main.cpp`       (node logic)
  - `include/protocol.h` (shared)
  - `rtc_manager.h`      (optional)

**Flash & Monitor:**
```sh
pio run -t upload
pio device monitor
```
First boot: Node prints _Searching for motherships…_  
Mothership should show node under “Unpaired Nodes”.

---

## Current Status & Next Steps

**Current:**
- ✅ Robust node state model (Unpaired / Paired / Deployed)
- ✅ Pair / Deploy / Revert / Unpair flows end-to-end
- ✅ CSV logging to SD card
- ✅ Web UI for basic ops
- ✅ NVS persistence (mothership & nodes)

**Future:**
- DS3231 alarm-based deep sleep on nodes (wake, send, sleep)
- Real sensor integration (DS18B20, BME280, etc.)
- Per-node schedule overrides
- Additional Web UI charts / metrics

---
