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
  - Measure environmental variables such as:
    - Air temperature (DS18B20 backend)
    - Soil volumetric water content + soil temperature (ADS1115 + thermistors backend)
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

- **Sensor backend abstraction**
  - Nodes use a small registry of logical “sensor slots”:
    ```c++
    struct SensorSlot {
      const char* label;      // e.g. "DS18B20_TEMP_1", "SOIL1_VWC"
      const char* sensorType; // e.g. "DS18B20", "SOIL_VWC", "SOIL_TEMP"
    };

    extern SensorSlot g_sensors[];
    extern size_t     g_numSensors;
    ```
  - Each backend populates slots and implements `read(index, float&)`:
    - **DS18B20 backend** (`sensors_ds18b20.*`)
      - Scans a OneWire bus and registers one slot per DS18B20:
        - Labels like `DS18B20_TEMP_1`, `DS18B20_TEMP_2`, …
        - Type string typically `DS18B20`
    - **Soil moisture + temperature backend** (`soil_moist_temp.*`)
      - Uses one ADS1115 on the root I²C bus (no mux) to provide:
        - `SOIL1_VWC` (ADS ch0) – θv from mV via polynomial calibration
        - `SOIL2_VWC` (ADS ch1)
        - `SOIL1_TEMP` (ADS ch2) – thermistor with Steinhart–Hart fit
        - `SOIL2_TEMP` (ADS ch3)
  - `sendSensorData()` walks `g_sensors[0..g_numSensors-1]` and sends one
    `SENSOR_DATA` packet per slot; the CSV uses the `label`/`sensorType`
    string as the `sensor_type` column.

- **CSV logging to SD card (mothership)**
  - Single `datalog.csv` with rows:
    ```text
    timestamp,node_id,node_name,mac,sensor_type,value
    ```
  - `sensor_type` is a free string such as:
    - `DS18B20_TEMP_1`, `DS18B20_TEMP_2`
    - `SOIL1_VWC`, `SOIL2_VWC`
    - `SOIL1_TEMP`, `SOIL2_TEMP`
  - Periodic mothership heartbeats:
    ```text
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

```text
+---------------------------+               +------------------------------+
|       Sensor Node(s)      |  ESP-NOW      |          Mothership          |
|       (ESP32-C3 Mini)     | <-----------> |       (ESP32-S3, AP)         |
+---------------------------+               +------------------------------+
  - Firmware ID (e.g. TEMP_001)              - Wi-Fi AP "Logger001"
  - DS3231 RTC + Alarm 1                      - DS3231 RTC
  - (Future) RTC INT → FET/wake               - SD card (datalog.csv)
  - NVS   (MAC, deployedFlag, etc.)          - ESP-NOW manager
  - Sensor backends:                          - Web UI (HTTP server)
      DS18B20                                 - Node Manager + TIME_SYNC
      soil_moist_temp (ADS1115)              
  - Packets:                                 Control packets:
      DISCOVER_REQUEST                        DISCOVER_RESPONSE / SCAN
      PAIRING_REQUEST                         PAIR_NODE / PAIRING_RESPONSE
      REQUEST_TIME                            DEPLOY_NODE
      SENSOR_DATA                             SET_SCHEDULE
                                              UNPAIR_NODE
                                              TIME_SYNC (+ fleet broadcast)
```
*(Current implementation polls the DS3231 Alarm 1 flag in firmware; no GPIO wiring to INT is required yet, but the design is ready for INT→FET / wake pin.)*

### Node “Belonging” States

At a high level, nodes are in one of three effective states:

- **Unpaired**

  No mothership MAC known.

  Node periodically sends DISCOVER_REQUEST and PAIRING_REQUEST broadcasts.

- **Paired / Bound**

  Mothership MAC known and stored in NVS.

  Node is “owned” but not yet deployed.

  RTC may or may not be synced (rtcSynced flag).

- **Deployed**

  Mothership MAC known.

  deployedFlag == true.

  RTC has been synced from a DEPLOY_NODE or TIME_SYNC message.

  Node arms the DS3231 Alarm 1 based on g_intervalMin and sends data on each alarm.

Internally:
```c++
bool hasMothershipMAC();  // derived from stored MAC in NVS
bool rtcSynced;           // true once time is set via DEPLOY or TIME_SYNC
bool deployedFlag;        // persisted "this node is deployed" flag

enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but not deployed
  STATE_DEPLOYED = 2    // has mothership MAC + deployed flag set
};
struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;         // firmware ID (e.g. "TEMP_001")
  String    nodeType;       // e.g. "AIR_SOIL"
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
The Node Manager page uses lastTimeSyncMs to show a small “time health” pill per node:

- **Fresh:** < 6 h since last TIME_SYNC
- **OK:** 6–24 h
- **Stale:** > 24 h
- **Unknown:** no TIME_SYNC yet

---

## Current TEMP_001 Sensor Profile

The nodes/air-temperature-node firmware currently exposes:

- **DS18B20 backend (sensors_ds18b20.\*)**

  - One OneWire bus on DS18B20_PIN
  - All DS18B20s on the bus are registered:
    - Slots like DS18B20_TEMP_1, DS18B20_TEMP_2, …
    - Each slot is read via DallasTemperature and sent as its own SENSOR_DATA packet

- **Soil moisture + temp backend (soil_moist_temp.\*)**

  - One ADS1115 on the root I²C bus (same as RTC)
  - Channels are used as:
    - ch0 → SOIL1_VWC (Probe 1 moisture, calibrated to θv)
    - ch1 → SOIL2_VWC (Probe 2 moisture)
    - ch2 → SOIL1_TEMP (Probe 1 thermistor → °C)
    - ch3 → SOIL2_TEMP (Probe 2 thermistor → °C)

  - Moisture uses polynomial coefficients ported from the earlier MicroPython logger

  - Thermistors use Steinhart–Hart fits based on your anchor measurements (cold/room/warm)

On each DS3231 alarm:

- Node checks it is STATE_DEPLOYED, rtcSynced == true, and has a mothership MAC.
- It iterates g_sensors and sends one SENSOR_DATA packet per slot.
- Mothership logs one CSV row per packet.

---

## Time Sync Behaviour

**Node-initiated TIME_SYNC**

- If `hasMothershipMAC()` && `!rtcSynced`:
  - Send REQUEST_TIME every ~30 s until TIME_SYNC arrives.
- If `rtcSynced == true`:
  - If >24 h since lastTimeSyncUnix, send another REQUEST_TIME
    (rate-limited to max once per 30 s).
- When TIME_SYNC is received:
  - Node sets DS3231, rtcSynced = true, lastTimeSyncUnix = dt.unixtime().
  - Persists everything to NVS.
  - If STATE_DEPLOYED and interval is set, it re-arms the DS3231 alarm.

**Fleet-wide TIME_SYNC**

- On mothership:
  - `espnow_loop()` calls `broadcastTimeSyncIfDue(false)`:
    - If >24 h since last fleet sync, broadcast TIME_SYNC to all PAIRED/DEPLOYED.
    - Log the event and a CSV row.
  - `broadcastTimeSyncIfDue(true)` forces an immediate fleet sync (e.g. from UI).

---

## Power-Loss Behaviour

**Normal Power Cut (RTC coin cell OK)**

- NVS state retained, node DS3231 keeps time.
- On reboot:
  - Mothership reloads nodes from NVS.
  - Node reloads its state: often STATE_DEPLOYED with a valid RTC.
  - Node re-arms Alarm 1 based on stored g_intervalMin.
  - Alarm-driven sends resume without manual intervention.

**RTC Lost Power (no coin cell / dead cell)**

- If DS3231 lost backup:
  - `rtc.lostPower() == true` at boot.
  - Node clears rtcSynced, deployedFlag, lastTimeSyncUnix.
  - Keeps mothershipMAC.
  - Effective state becomes STATE_PAIRED.
  - Node begins REQUEST_TIME messages until re-synced and re-deployed.

---

## Build & Flash

### Mothership

```sh
# in mothership/ project dir
pio run -t upload
pio device monitor
```
Then:

- Connect to Logger001 (default password logger123)
- Open http://192.168.4.1/ in a browser

### Node(s)

```sh
# in nodes/air-temperature-node/ project dir
pio run -t upload
pio device monitor
```
On first boot you should see logs like:
```text
STATE_UNPAIRED ...
📡 Discovery request sent
⏰ Bound but RTC unsynced → requesting initial TIME_SYNC
⏰ Time sync request sent
```
Once the mothership is running and you click “Discover Nodes” in the UI, the node will appear in /nodes.

---

## Current Status & Next Steps

### Current

- ✅ Robust node state model (Unpaired / Paired / Deployed)
- ✅ End-to-end Pair / Deploy / Stop / Unpair flows via web UI
- ✅ CSV logging to SD card with node ID + friendly name
- ✅ Web UI for discovery, control, and RTC management
- ✅ NVS persistence on both mothership and nodes
- ✅ Recovery from full power cuts (with RTC coin cell present)
- ✅ Explicit REQUEST_TIME / TIME_SYNC handshake per node
- ✅ Fleet-wide periodic TIME_SYNC
- ✅ Per-node time-health indicators in the Node Manager
- ✅ Modular sensor backend system:
  - DS18B20 OneWire air temperature
  - ADS1115-based soil moisture + soil temperature (2 probes)

### Future

- Wire DS3231 INT → FET / wake pin; enable true alarm-driven deep sleep
- Per-node wake intervals instead of a global broadcast
- Additional sensor backends (BME280, PAR, ultrasonic wind, etc.)
- OTA firmware updates (at least for the mothership)
- Diagnostic charts in web UI (per-node sparklines)
- Data ingestion helpers for R/Python pipelines
