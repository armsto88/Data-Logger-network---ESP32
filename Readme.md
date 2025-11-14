
# ESP32 Sensor Network – Mothership + Nodes

A small ESP-NOW–based sensor network for environmental data logging.

The system is built around:

- A **mothership** (ESP32-S3) that:
  - runs a Wi-Fi access point (`Logger001`)
  - exposes a web UI dashboard
  - manages node discovery, pairing and deployment
  - stores incoming sensor data to CSV on an SD card
  - keeps time via a DS3231 RTC

- One or more **sensor nodes** (ESP32-C3 Mini) that:
  - measure environmental variables (e.g. air temperature)
  - talk to the mothership using ESP-NOW
  - wake on an RTC alarm interval
  - send sensor readings when deployed



---

## Features

- **ESP-NOW mesh** style communication (unidirectional node → mothership data, with control messages mothership → nodes)
- **DS3231 RTC integration** on mothership and nodes for:
  - accurate timestamps on measurements
  - configurable wake/sampling intervals via the DS3231 Alarm 1
- **CSV logging to SD card** on the mothership:
  - single `datalog.csv` with timestamp, node ID, MAC, sensor type, and value
  - periodic “MOTHERSHIP STATUS” heartbeats for basic health checks
- **Web UI dashboard** (served from the mothership):
  - view current RTC time
  - manage node discovery, pairing, deployment and unpairing
  - set global wake interval (1, 5, 10, 20, 30, 60 minutes)
  - download the full CSV log
- **Persistent state in NVS** (Preferences) on both sides:
  - mothership remembers paired / deployed nodes across reboots
  - nodes remember which mothership they belong to, wake interval and RTC sync status

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

Each node only “fully belongs” to a mothership once deployed:

    Unpaired – no mothership MAC known

    Paired / Bound – mothership MAC known, but RTC not yet synced

    Deployed – mothership MAC known and RTC synced ⇒ node sends data on each wake

State Model

On the node, the effective state comes from:

    mothershipMAC – all zeros = not bound

    rtcSynced – true once the node has received a deployment time

The derived state is:

enum NodeState {
  STATE_UNPAIRED = 0,   // no mothership MAC known
  STATE_PAIRED   = 1,   // has mothership MAC, but RTC not synced
  STATE_DEPLOYED = 2    // has mothership MAC + RTC synced
};

On the mothership, each node is tracked as:

struct NodeInfo {
  uint8_t   mac[6];
  String    nodeId;
  String    nodeType;
  uint32_t  lastSeen;
  bool      isActive;
  NodeState state;    // UNPAIRED / PAIRED / DEPLOYED
  uint8_t   channel;
};

State transitions:

    Discovery → Unpaired → Paired

        node broadcasts DISCOVER_REQUEST

        mothership replies with DISCOVER_RESPONSE

        node shows up in “Unpaired Nodes” list

        user selects node(s) → Pair Selected Nodes

        mothership sends PAIR_NODE + PAIRING_RESPONSE

        node stores mothershipMAC, sets STATE_PAIRED, rtcSynced=false

    Paired → Deployed

        user selects paired node(s) → Deploy Selected Nodes

        mothership sends DEPLOY_NODE with current RTC time

        node sets DS3231 time, rtcSynced=true, derived state = STATE_DEPLOYED

        node starts sending sensor_data_message_t periodically

    Deployed → Paired (Revert)

        user clicks Revert to Paired in the Deployed section

        mothership:

            flips its internal state for the node to PAIRED

            sends PAIR_NODE again

        node:

            keeps mothershipMAC

            sets rtcSynced=false, STATE_PAIRED

            stops sending sensor data and logs “Bound, waiting for DEPLOY command…”

    Unpair

        user selects node(s) in “Unpair Nodes”

        mothership sends UNPAIR_NODE and locally demotes/removes them

        node clears mothershipMAC, sets rtcSynced=false, STATE_UNPAIRED

        node immediately starts broadcasting discovery again

Node Firmware
Hardware

Example node hardware:

    MCU: ESP32-C3 Mini

    RTC: DS3231

        I²C pins:

            RTC_SDA_PIN = 8

            RTC_SCL_PIN = 9

            RTC_INT_PIN = 3 (INT/SQW)

    Sensor: currently a dummy temperature reading, easily swapped for a real sensor.

Behaviour

On boot:

    Load persisted node config from NVS:

        mothershipMAC

        rtcSynced

        g_intervalMin (wake interval)

    Initialise I²C + DS3231.

        If rtc.lostPower() → treat as unsynced (rtcSynced=false).

    Start ESP-NOW in station mode and add:

        broadcast peer (FF:FF:FF:FF:FF:FF)

        pre-configured mothership peer (if mothershipMAC is non-zero)

In loop():

    If UNPAIRED:

        periodically send:

            DISCOVER_REQUEST

            PAIRING_REQUEST

        log 🔍 Searching for motherships…

    If PAIRED (bound, not deployed):

        log 🟡 Bound, waiting for DEPLOY command…

        if RTC unsynced, occasionally send REQUEST_TIME (optional helper)

    If DEPLOYED:

        on each interval tick (currently a simple millis-based demo; DS3231 alarm available):

            read sensor

            send sensor_data_message_t to mothershipMAC

            log 🟢 Deployed — sending sensor data…

Node Message Types

    To mothership:

        DISCOVER_REQUEST (discovery_message_t)

        PAIRING_REQUEST (pairing_request_t)

        REQUEST_TIME (time_sync_request_t)

        sensor_data_message_t (actual measurements)

    From mothership:

        DISCOVER_RESPONSE / DISCOVERY_SCAN

        PAIR_NODE (pairing_command_t)

        PAIRING_RESPONSE (pairing_response_t)

        DEPLOY_NODE (deployment_command_t)

        UNPAIR_NODE (unpair_command_t)

        SET_SCHEDULE (schedule_command_message_t)

        TIME_SYNC (time_sync_response_t)

Mothership Firmware
Hardware

Example mothership hardware:

    MCU: ESP32-S3

    RTC: DS3231 (I²C)

    Storage: SD card (SPI), datalog.csv

    Wi-Fi: soft AP mode, SSID = Logger001, password = logger123

Behaviour

On boot:

    Initialise RTC and SD card.

    Start Wi-Fi AP Logger001 (channel 1) and web server on port 80.

    Initialise ESP-NOW, add:

        broadcast peer

        pre-configured sensor peers (KNOWN_SENSOR_NODES), plus any restored from NVS.

    Load previously paired/deployed nodes from NVS.

    Print current RTC time, wake interval, and start auto-discovery loop.

Periodic tasks in loop():

    server.handleClient() – handle web UI HTTP requests.

    espnow_loop() – auto discovery and node liveness:

        send DISCOVERY_SCAN every 30s

        mark nodes inactive if they haven’t been seen for a while

    Every 60s:

        log “MOTHERSHIP,STATUS,ACTIVE” into datalog.csv.

On incoming ESP-NOW packets:

    sensor_data_message_t:

        ensure node is registered and marked DEPLOYED

        log timestamp,nodeId,mac,sensorType,value to CSV

    DISCOVER_REQUEST:

        register/refresh node (without downgrading state)

        send DISCOVER_RESPONSE (broadcast)

    PAIRING_REQUEST:

        send PAIRING_RESPONSE reflecting current state (paired/deployed vs unpaired)

    REQUEST_TIME:

        send TIME_SYNC with the current DS3231 time

    Other control messages are sent proactively from web UI actions.

Web UI (Dashboard)

The web UI is served by the mothership at:

    URL: http://192.168.4.1/ (when connected to Logger001)

Layout

    (Insert screenshots here)
    e.g.
    ![Dashboard overview](docs/img/dashboard-overview.png)
    ![Node lists](docs/img/dashboard-nodes.png)
    ![RTC settings](docs/img/dashboard-rtc-settings.png)

Sections

    Wake Interval

        Dropdown to choose: 1, 5, 10, 20, 30, 60 minutes.

        “Broadcast” button sends SET_SCHEDULE to all paired/deployed nodes.

        Current value is stored in NVS and restored on boot.

    Screenshot placeholder:
    ![Wake interval section](docs/img/ui-wake-interval.png)

    Current RTC Time

        Shows DS3231 time from the mothership.

        Live-ticks in the browser (JavaScript clock aligned to RTC).

        Useful sanity check that deployments use the correct time.

    Screenshot placeholder:
    ![RTC time section](docs/img/ui-rtc-time.png)

    Data Logging

        Shows basic CSV logging status.

        “Download CSV Data” button streams datalog.csv from the SD card.

        Includes mothership status rows and node measurement rows.

    Screenshot placeholder:
    ![Data logging section](docs/img/ui-data-logging.png)

    Node Discovery & Stats

        Displays:

            Mothership MAC address

            Counts of:

                Deployed nodes

                Paired nodes

                Unpaired nodes

        “Discover New Nodes” button sends a DISCOVERY_SCAN packet.

    Screenshot placeholder:
    ![Discovery & stats](docs/img/ui-discovery-stats.png)

    Unpaired Nodes

        List of nodes seen via DISCOVER_REQUEST but not yet paired.

        Each item shows:

            Node ID

            Node type

            MAC address

            “Seen X seconds ago”

        Select node(s) and click Pair Selected Nodes to:

            set mothership state to PAIRED

            send PAIR_NODE + PAIRING_RESPONSE

    Screenshot placeholder:
    ![Unpaired nodes](docs/img/ui-unpaired.png)

    Paired Nodes (Ready to Deploy)

        Nodes with state == PAIRED (known & bound, RTC not synced).

        Checkboxes (pre-checked) to select nodes for deployment.

        Deploy Selected Nodes:

            sends DEPLOY_NODE with RTC time

            updates state to DEPLOYED

    Screenshot placeholder:
    ![Paired nodes](docs/img/ui-paired.png)

    Unpair Nodes

        List of paired nodes with checkboxes.

        Unpair Selected:

            sends UNPAIR_NODE to each node

            demotes/removes them from mothership’s NVS registry

        Nodes go back to discovery broadcasts and appear under “Unpaired”.

    Screenshot placeholder:
    ![Unpair nodes](docs/img/ui-unpair.png)

    Active Deployed Nodes

        Shows nodes currently deployed and actively sending data.

        Each item shows:

            Node ID, type

            MAC address

            “Revert to Paired” button:

                flips state back to PAIRED

                sends PAIR_NODE to the node

                node stops sending data and waits for a new deployment

    Screenshot placeholder:
    ![Deployed nodes](docs/img/ui-deployed.png)

    RTC Settings Panel

        Hidden by default; toggled by “Show RTC Settings”.

        Allows manual setting of DS3231 time:

            input: YYYY-MM-DD HH:MM:SS

            “Auto-Detect Current Time” button pre-fills from the browser’s local clock.

        Only needed for initial setup or time corrections.

    Screenshot placeholder:
    ![RTC settings panel](docs/img/ui-rtc-settings-panel.png)

    Footer

        Sticky footer bar with:

            Refresh button

            CSV Download button

        Dashboard auto-refreshes every ~10 seconds.

Build & Flash

    This section assumes PlatformIO, but you can adapt for Arduino IDE.

Mothership

    Board: ESP32-S3 dev module

    Project: e.g. mothership/

    Key files:

        src/main.cpp (web UI + ESP-NOW + RTC + SD)

        src/espnow_manager.cpp

        src/rtc_manager.cpp

        src/sd_manager.cpp

        include/protocol.h (message definitions, pins, channel)

Flash:

pio run -t upload
pio device monitor

Connect WLAN to Logger001, open http://192.168.4.1/ in a browser.
Node(s)

    Board: ESP32-C3 Mini

    Project: e.g. nodes/air-temperature-node/

    Key files:

        src/main.cpp (node logic)

        include/protocol.h (shared with mothership)

        rtc_manager.h (if factored out)

Flash:

pio run -t upload
pio device monitor

On first boot, expect:

    Node prints 🔍 Searching for motherships…

    Mothership sees discovery and shows node under “Unpaired Nodes”.

Current Status & Next Steps

Current system:

    ✅ Robust node state model (Unpaired / Paired / Deployed)

    ✅ Pair / Deploy / Revert / Unpair flows working end-to-end

    ✅ CSV logging to SD card

    ✅ Web UI for basic operations

    ✅ NVS persistence across reboots for both mothership and nodes

Nice future additions:

    DS3231 alarm-based deep sleep on nodes (wake, send, sleep again)

    Real sensor integration (DS18B20, BME280, soil moisture, PAR, etc.)

    Per-node schedule overrides (e.g. fast sampling on some nodes, slow on others)

    Additional metrics / charts in the web UI (basic plots of recent values)
