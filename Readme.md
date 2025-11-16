# ESP32 Sensor Network – Mothership + Nodes

A small **ESP-NOW**–based sensor network for environmental data logging, designed to survive field power cuts and resume logging automatically.

---

## System Overview

The system is built around:

- **Mothership (ESP32-S3):**
  - Runs a Wi-Fi access point (`Logger001`)
  - Exposes a web UI dashboard
  - Manages node discovery, pairing and deployment
  - Stores incoming sensor data to CSV on an SD card
  - Keeps time via a DS3231 RTC
  - Persists node state (paired / deployed, IDs, names, wake interval) in NVS

- **Sensor Nodes (ESP32-C3 Mini, one or more):**
  - Measure environmental variables (e.g., air temperature)
  - Communicate with the mothership using ESP-NOW
  - Use a DS3231 RTC + Alarm 1 to drive their wake/sampling interval
  - Persist their own state (mothership MAC, deployed flag, wake interval, RTC sync flag) in NVS
  - Are designed to resume operation automatically after a power cut

---

## Features

- **ESP-NOW mesh** style communication:
  - Data: node → mothership
  - Control: mothership → nodes
- **DS3231 RTC integration** (both mothership & nodes):
  - Accurate timestamps on measurements
  - Configurable wake/sampling intervals via DS3231 Alarm 1
- **CSV logging to SD card** (mothership):
  - Single `datalog.csv` (timestamp, node ID, name, MAC, sensor type, value)
  - Periodic `MOTHERSHIP,STATUS,ACTIVE` heartbeats
- **Web UI dashboard** (from mothership):
  - View RTC time
  - Node discovery, pairing, deployment & unpairing
  - Set global wake interval (1–60 mins)
  - Download CSV log
- **Persistent state in NVS** (both sides):
  - Mothership remembers paired/deployed nodes across reboots
  - Nodes remember:
    - Which mothership they belong to (MAC address)
    - Whether they are deployed
    - The current wake interval
    - Whether their RTC has been synced
- **Power-loss resilience**:
  - After a hard power cut, deployed nodes reload their state from NVS
  - If the RTC still has valid time (coin cell present), nodes resume sending data immediately without redeployment

---

## High-Level Architecture

```text
+---------------------+                     +--------------------------+
|   Sensor Node(s)    |  ESP-NOW (channel)  |        Mothership        |
|  (ESP32-C3 Mini)    | <-----------------> |      (ESP32-S3, AP)      |
+---------------------+                     +--------------------------+
  - NODE_ID, NODE_TYPE                         - Wi-Fi AP "Logger001"
  - DS3231 RTC + Alarm1                        - DS3231 RTC
  - RTC INT pin wake (future deep-sleep)       - SD card (datalog.csv)
  - ESP-NOW messages:                          - ESP-NOW manager
      DISCOVER_REQUEST                         - Web UI (HTTP server)
      PAIRING_REQUEST                          - Pair / Deploy / Unpair
      REQUEST_TIME
      SENSOR_DATA
      ...
```

---

## Node "Belonging" States

At a high level, nodes are in one of three effective states:

- **Unpaired:**  
  No mothership MAC known; node continuously broadcasts DISCOVER_REQUEST and polls PAIRING_REQUEST.

- **Paired / Bound:**  
  Mothership MAC known and stored in NVS; node is “owned” but not yet deployed. RTC may or may not be synced.

- **Deployed:**  
  Mothership MAC known, RTC has been synced from the mothership, and the node is actively sending sensor data on each wake.

In code, the effective node state is derived from:
```cpp
bool hasMothershipMAC();  // derived from stored MAC in NVS
bool rtcSynced;           // true once time is set via DEPLOY or TIME_SYNC
bool deployedFlag;        // persisted "this node is deployed" flag
```
and mapped to:
```cpp
enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + deployed flag set
};
```
The raw NodeState enum is also stored in NVS for debugging, but the real behaviour is driven by `hasMothershipMAC()`, `deployedFlag`, and `rtcSynced`.

On the mothership side, each node is tracked as:
```cpp
struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;
  String    nodeType;
  uint32_t  lastSeen;
  bool      isActive;
  NodeState state;    // UNPAIRED / PAIRED / DEPLOYED
  uint8_t   channel;
  String    humanName; // e.g. "Hello-v2"
};
```

---

### State Transitions

#### Discovery → Unpaired → Paired

- Node boots with no mothership MAC in NVS:  
  Effective state: `STATE_UNPAIRED`
- Node broadcasts DISCOVER_REQUEST.
- Mothership replies with DISCOVER_RESPONSE and shows node under “Unpaired Nodes”.
- User selects node(s) → Pair Selected Nodes.
- Mothership sends PAIR_NODE + PAIRING_RESPONSE.
- Node stores mothership MAC in NVS, sets `STATE_PAIRED`, and continues waiting for deployment.

---

#### Paired → Deployed

- User selects paired node(s) → Deploy Selected Nodes.
- Mothership sends:
  - SET_SCHEDULE (wake interval)
  - DEPLOY_NODE (including current RTC time)
- Node:
  - Sets its DS3231 time from the deployment command.
  - Stores:
    - mothershipMAC
    - deployedFlag = true
    - rtcSynced = true
    - g_intervalMin (wake interval)
  - Effective state: `STATE_DEPLOYED`
- On each interval tick, node sends `sensor_data_message_t` to the mothership.

---

#### Deployed → Paired (Revert)

- User clicks Revert to Paired in the web UI.
- Mothership updates `NodeInfo.state` to `STATE_PAIRED` and sends a suitable control message.
- Node clears `deployedFlag` but keeps `mothershipMAC`.
- Node stops sending data and waits for another deploy.

---

#### Unpair

- User selects nodes under Unpair Nodes and clicks Unpair.
- Mothership:
  - Sends UNPAIR_NODE.
  - Removes node from its registry / NVS.
- Node:
  - Clears mothershipMAC, deployedFlag, and rtcSynced.
  - Effective state becomes `STATE_UNPAIRED` and it resumes discovery broadcasts.

---

## Power-Loss Behaviour

### Normal Power Cut (RTC still has coin cell)
- Node and mothership lose 5 V, but:
  - Both have their state (paired / deployed / intervals) in NVS.
  - Node’s DS3231 keeps time from its coin cell.

On reboot:
- **Mothership:**  
  Loads node registry from NVS (e.g. TEMP_001 in STATE_DEPLOYED).
- **Node:**  
  Loads state=DEPLOYED, deployedFlag=1, rtcSynced=1, and mothershipMAC from NVS.  
  Detects that RTC has not lost power.  
  Immediately resumes sending sensor data.

No user action is required; deployed nodes come back on their own.

---

### RTC Lost Power (no coin cell)
If the DS3231 has lost power (no coin cell / fully discharged):

- `rtc.lostPower()` is true on boot.
- Node treats its RTC time as invalid and:
  - Sets rtcSynced = false (and optionally deployedFlag = false depending on configuration).
  - Either:
    - Drops to STATE_PAIRED and waits for a fresh DEPLOY, or
    - Stays deployed but repeatedly sends REQUEST_TIME until a TIME_SYNC response is received.

The exact policy can be configured in firmware; the important thing is that “RTC lost power” is detectable and handled explicitly.

---

## Node Firmware & Hardware

**Example Node Hardware:**
- MCU: ESP32-C3 Mini
- RTC: DS3231 (I²C: SDA/SCL from protocol.h, INT → GPIO with pull-up)
- Sensor: Dummy temperature (easily swapped for DS18B20, BME280, etc.)

**Boot lifecycle:**

1. Initialise NVS (`nvs_flash_init()` + Preferences).
2. Load persisted config from NVS:
    - state
    - rtcSynced
    - deployedFlag
    - g_intervalMin
    - mothershipMAC
3. Initialise I²C + DS3231:
    - If `rtc.lostPower() == true`, mark rtcSynced = false and decide on deployment policy.
4. Start ESP-NOW:
    - Add broadcast peer (FF:FF:FF:FF:FF:FF).
    - If `hasMothershipMAC()`, add mothership peer.
5. Log the resulting state and begin the main loop.

**Main loop:**
- **UNPAIRED:**  
  Periodically:
  - DISCOVER_REQUEST
  - PAIRING_REQUEST

- **PAIRED (not deployed):**  
  Log “waiting for DEPLOY”.  
  If RTC unsynced, optionally send REQUEST_TIME.

- **DEPLOYED:**  
  On each DS3231 alarm (or simple timer, in the C3 version):
    - Read sensor.
    - Send data to mothership.
    - Re-arm DS3231 Alarm 1 as needed.

---

## Mothership Firmware & Hardware

**Example Hardware:**
- MCU: ESP32-S3
- RTC: DS3231 (I²C)
- SD: datalog.csv (SPI)
- Wi-Fi: Soft AP (Logger001, password configurable)

**On Boot:**
1. Initialise NVS and load stored node registry.
2. Initialise RTC, SD card, Wi-Fi AP, HTTP server.
3. Start ESP-NOW, add broadcast peer and any known node peers.
4. Start periodic jobs:
    - Discovery scans
    - Status heartbeats
    - Node-liveness checks
5. Serve the web dashboard at http://192.168.4.1/.

**ESP-NOW packet handling:**
- DISCOVER_REQUEST → Register node, send DISCOVER_RESPONSE.
- PAIRING_REQUEST → Reply with PAIRING_RESPONSE reflecting node’s current state.
- SENSOR_DATA → Log to datalog.csv (if node in STATE_DEPLOYED).
- REQUEST_TIME → Reply with TIME_SYNC.
- Control messages (triggered via web UI):
    - PAIR_NODE
    - DEPLOY_NODE
    - UNPAIR_NODE
    - SET_SCHEDULE

---

## Web UI Dashboard

**Served at:** http://192.168.4.1/ (connect to Wi-Fi Logger001).

**Key sections:**

- **Wake Interval**
    - Dropdown (1–60 minutes).
    - Broadcasts SET_SCHEDULE to selected nodes.

- **Current RTC Time**
    - Shows mothership DS3231 time and keeps browser clock in sync.

- **Data Logging**
    - Logging status.
    - Download CSV button.

- **Node Discovery & Stats**
    - Mothership MAC.
    - Node counts.
    - Manual “Discover Nodes” button.

- **Unpaired Nodes**
    - Table of newly discovered nodes.
    - Actions: Pair.

- **Paired Nodes**
    - Nodes that belong to this mothership but are not deployed.
    - Action: Deploy.

- **Deployed Nodes**
    - Active field nodes sending data.
    - Action: Revert to Paired.

- **Unpair Nodes**
    - Nodes that can be fully unbound.
    - Action: Unpair.

---

## Build & Flash

Example for PlatformIO; adjust as needed.

### Mothership

- Board: ESP32-S3 dev module
- Project: mothership/

```
pio run -t upload
pio device monitor
```

Connect to Wi-Fi Logger001, open http://192.168.4.1/.

### Node(s)

- Board: ESP32-C3 Mini
- Project: e.g., nodes/air-temperature-node/

```
pio run -t upload
pio device monitor
```

On first boot, node will print “Searching for motherships…” and should appear in the mothership UI under Unpaired Nodes.

---

## Current Status & Next Steps

**Current:**
- ✅ Robust node state model (Unpaired / Paired / Deployed)
- ✅ End-to-end Pair / Deploy / Revert / Unpair flows
- ✅ CSV logging to SD card
- ✅ Web UI for discovery and control
- ✅ Confirmed NVS persistence on mothership and nodes
- ✅ Confirmed recovery from full power cuts (with RTC coin cell present)

**Future:**
- DS3231 alarm-based deep sleep on supported ESP32 variants
- Real sensor integration (DS18B20, BME280, soil moisture, PAR, etc.)
- Per-node wake intervals and richer node metadata
- OTA firmware updates (at least for mothership)
- Simple diagnostic charts in the web UI
- Data ingestion helpers for R / Python pipelines
