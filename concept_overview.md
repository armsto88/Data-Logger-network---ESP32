# ESP32 Sensor Network – Node Lifecycle & Web Dashboard

This document describes how the **sensor nodes** and **mothership** interact from the user’s point of view, with a focus on:

- Node lifecycle (Unpaired → Paired → Deployed → Stopped / Unpaired)
- Web dashboard behaviour
- Data format and what’s currently working
- Current sensor stack (air + soil moisture + soil temperature)

---

## System Overview

The system is a small, self-contained **wireless sensor network** for environmental monitoring.

- A central **“mothership”** unit sits on site.
  - It creates its own Wi-Fi network so you can connect with a phone or laptop.
  - It keeps accurate time via a real-time clock (DS3231).
  - It stores all incoming data onto an SD card as a CSV file.
  - It remembers which nodes are **paired** and **deployed**, plus their IDs, names, and wake interval, in non-volatile storage (NVS).
  - It answers **time sync requests** from nodes and can push periodic fleet-wide time sync.
  - It exposes a simple web dashboard to manage the fleet.

- Multiple **sensor nodes** are deployed around the site.
  - They measure environmental variables such as:
    - Air temperature (DS18B20)
    - Soil volumetric water content (θv) via custom soil probes (ADS1115 channels A0/A1)
    - Soil temperature via thermistors mounted on those same probes (ADS1115 channels A2/A3)
  - Additional sensor backends can be added later (e.g. PAR, humidity, wind).
  - They use a low-power wireless protocol (**ESP-NOW**) to send data back to the mothership.
  - They wake on a configurable interval driven by the **DS3231 alarm** and send one packet per configured sensor slot each time (so a single node can produce multiple CSV rows per wake).
  - They store their own state in NVS, including:
    - Which mothership they belong to (MAC address)
    - Whether they are deployed
    - Their wake interval
    - Whether their RTC is synced, plus the last time-sync timestamp
  - On boot, they reload this state and continue from where they left off.

Everything is designed to be:

- **Low-cost**  
- **Field-friendly** (minimal physical UI; a clean web dashboard)  
- **Robust to power loss**  
  - The mothership and nodes keep their pairing / deployment state in NVS.  
  - Each node’s DS3231 is backed by a coin cell so time survives main power loss.  
  - If the RTC is still valid, a deployed node will restart and simply keep sending data.

---

## Node Lifecycle: from “seen” to “deployed”

### 1. Discovery

When a sensor node is powered up for the first time, it doesn’t know which mothership it belongs to.  

It simply announces something like:

> “I’m here, my ID is `TEMP_001`, I’m an air + soil node.”

The mothership hears these announcements and registers the node.  
The node then appears in the **Node Manager** list in the web dashboard with state **Unpaired**.

On subsequent boots:

- If the node has already been paired, it loads the stored mothership MAC from NVS.
- It skips the initial “who’s out there?” discovery and reconnects automatically as **Paired** or **Deployed** depending on its saved state.

### 2. Pairing

From the **Node Manager**, you tap a node to open its **Configure & Start** page.

There you can:

- Set a **numeric Node ID** (e.g. `001`) – used in CSV logs.
- Set a **friendly name** (e.g. `North Hedge 01`).

Choosing **Action: Start / deploy** on an **unpaired** node causes the mothership to:

1. Send a `PAIR_NODE` command to the node.  
2. Send a `PAIRING_RESPONSE` confirming the association.  
3. Store the pairing in its own NVS.  
4. The node stores the mothership MAC in its NVS.

At this point the node is **bound/paired**, but not yet actively sending measurements until it’s deployed.

### 3. Deployment

When you’re ready to start logging:

1. Open the node’s **Configure & Start** page.
2. Choose a wake interval (e.g. 5 minutes).
3. Select **Action: Start / deploy** and submit.

The mothership will:

- Broadcast the chosen wake interval to paired/deployed nodes (`SET_SCHEDULE`).
- Send a `DEPLOY_NODE` command which includes the current time from the mothership’s DS3231.

The node will:

- Set its DS3231 to the time in the `DEPLOY_NODE` message.
- Store in NVS:
  - Mothership MAC  
  - Deployed flag  
  - Wake interval  
  - “RTC is synced” flag + last time-sync timestamp
- Begin using the DS3231 alarm to wake and, on each interval, read:
  - All DS18B20 air temperature sensors on its OneWire bus.
  - ADS1115 channels A0–A3:
    - A0/A1 → soil moisture (θv) for two probes.
    - A2/A3 → soil temperatures for those two probes via thermistors.

On each alarm, the node sends one `SENSOR_DATA` packet per logical sensor slot, for example:

- `DS18B20_TEMP_1`
- `DS18B20_TEMP_2`
- `SOIL1_VWC`
- `SOIL2_VWC`
- `SOIL1_TEMP`
- `SOIL2_TEMP`

In the dashboard, the node now shows as **Deployed**, with a “Fresh / OK / Stale / Unknown” time-health pill based on when it last received a `TIME_SYNC`.

> After a power cut, a deployed node reloads this state from NVS.  
> If its RTC still has valid time, it sees it is deployed + synced and simply **resumes sending data** without needing to be redeployed.

### 4. Reverting / Unpairing

#### Revert to Paired (Stop)

If you want a node to stop sending data but keep its association with the mothership:

- Open **Configure & Start**.
- Choose **Action: Stop / keep paired**.

The node will stop transmitting regular measurements but still:

- Remembers which mothership it belongs to.
- Can be redeployed later without going through discovery again.

#### Unpair

If you want a node to completely forget the mothership and behave as “new” again:

- Open **Configure & Start**.
- Choose **Action: Unpair / forget**.

The mothership:

- Removes the node from its paired list.
- Clears its ID/name metadata.

The node:

- Clears its stored mothership MAC.
- Clears deployed/synced flags and related state.

On next boot, it will appear again as **Unpaired**, advertising itself to any listening mothership.

---

## Web Dashboard

The mothership exposes a simple web interface.

1. Connect to the Wi-Fi network **`Logger001`** (default password can be configured in firmware).
2. Open a browser and go to **`http://192.168.4.1/`**.

From there you can:

- See the **current time** on the mothership (live-updating from the DS3231).
- Set the **global sampling / wake interval** for nodes  
  (e.g. every 1, 5, 10, 20, 30, or 60 minutes).
- Start a **Discovery scan** so new nodes can announce themselves.
- Use the **Node Manager** to:
  - View all nodes and their states (Unpaired / Paired / Deployed).
  - See per-node time-health indicators (Fresh / OK / Stale / Unknown).
  - Open each node’s **Configure & Start** page to:
    - Set numeric Node ID and friendly name.
    - Deploy / stop / unpair that node.
- See which nodes are currently deployed and actively sending data (air + soil).
- Download the full **CSV log** of all data recorded to date.

### Screens / Views

- **Main dashboard view**
  - Current RTC time.
  - Global wake interval controls.
  - System status and links.

- **Node Manager**
  - List of nodes with:
    - Firmware ID (e.g. `TEMP_001`).
    - Friendly name / numeric ID (if configured).
    - State chip: Unpaired / Paired / Deployed.
    - Time-health pill: Fresh / OK / Stale / Unknown.
  - Tap-through to per-node configuration.

- **Configure & Start (per node)**
  - Shows:
    - Firmware ID.
    - Current state.
    - Stored numeric Node ID.
    - Friendly name.
    - Current wake interval.
  - Actions:
    - **Start / deploy** (set interval + deploy).
    - **Stop / keep paired**.
    - **Unpair / forget**.

- **CSV / Logs**
  - Button to download the main CSV file from SD.
  - Optional system status messages (e.g. TIME_SYNC fleet events).

(*Screenshots are referenced as `UI_Images/1.png` … `UI_Images/5.png` in the repo.*)

---

## Data Format

All data is stored in a single CSV file on the SD card.

Each **sensor data row** includes:

- **Timestamp**  
  From the mothership RTC (DS3231), set either manually or via NTP / browser controls.

- **Node ID**  
  Numeric ID (e.g. `001`) if configured in the UI; otherwise the firmware ID is used.

- **Node name**  
  Human-readable name (e.g. `North Hedge 01`, if configured).

- **Node MAC address**  
  The 6-byte ESP32 MAC for traceability.

- **Sensor type / label**, for example:
  - `DS18B20_TEMP_1`
  - `DS18B20_TEMP_2`
  - `SOIL1_VWC`
  - `SOIL2_VWC`
  - `SOIL1_TEMP`
  - `SOIL2_TEMP`

- **Value**  
  The numeric reading, e.g.:
  - Air temperature in °C.
  - Soil volumetric water content (m³/m³), already clamped to a plausible range (0–0.6).
  - Soil temperature in °C derived via thermistor calibration (Steinhart–Hart).

Because each node can expose multiple logical sensors, a **single wake event** (one DS3231 alarm) typically produces **multiple CSV rows** – one per sensor slot.

### Status Rows

The mothership also logs simple status rows, such as:

- Periodic `MOTHERSHIP,STATUS,ACTIVE` heartbeats (to show the system is alive).
- Optional `MOTHERSHIP,TIME_SYNC_FLEET,OK` rows when a fleet-wide time sync is performed.

This format keeps the file easy to open in **Excel**, **R**, **Python**, etc., while still containing enough metadata (ID + name + MAC + sensor label) to trace which physical node and which sensor produced each measurement.

---

## Sensor Backends (Current)

At the node level, sensors are managed via small modular backends:

- **DS18B20 backend**
  - Handles OneWire bus scan and temperature reads.
  - Exposes one sensor slot per detected DS18B20, named:
    - `DS18B20_TEMP_1`, `DS18B20_TEMP_2`, …

- **Soil moisture + temperature backend (`soil_moist_temp`)**
  - Uses ADS1115 on the root I²C bus (same bus as the RTC/mux).
  - Channels:
    - A0 → SOIL1 moisture raw mV → `SOIL1_VWC`.
    - A1 → SOIL2 moisture raw mV → `SOIL2_VWC`.
    - A2 → thermistor on probe 1 → `SOIL1_TEMP`.
    - A3 → thermistor on probe 2 → `SOIL2_TEMP`.
  - Moisture conversion:
    - Uses calibration curves derived from lab logs (quadratic fits).
    - Output is volumetric water content (θv, m³/m³) clamped to 0–0.6.
  - Temperature conversion:
    - Uses Steinhart–Hart coefficients fit from calibration points (cold / room / warm).
    - Outputs °C, with optional trim gain/offset if future tweaks are needed.
  - All four channels are read together once per wake; values are cached per cycle so other code can query them without hammering the ADS.

These backends populate a global sensor registry (`g_sensors[]`), which `sendSensorData()` iterates over to send the correct `(sensorType, value)` pairs each time the node wakes.

---

## What’s Working Now

- Nodes can be:
  - **Discovered**
  - **Paired**
  - **Deployed**
  - **Stopped (but kept paired)**
  - **Unpaired / forgotten**  
  – all from the web UI.

- The system behaves correctly across reboots and power cycles:
  - The mothership remembers which nodes are paired or deployed via NVS.
  - Nodes remember:
    - Which mothership they belong to.
    - Their wake interval.
    - Whether they are deployed.
    - Whether their RTC is synced and when it was last synced.
  - If the node’s RTC still has valid time (coin cell present), deployed nodes resume sending data automatically after power is restored.

- Nodes and mothership perform explicit **time sync** using:
  - `REQUEST_TIME` (node → mothership).
  - `TIME_SYNC` (mothership → node).
  - The UI surfaces a simple time-health indicator per node.

- **Sensor integrations (current)**
  - DS18B20 air temperature.
  - ADS1115-based soil moisture + soil temperature (two integrated probes).
  - All four soil channels + DS18B20 readings are visible in the CSV and in the serial log on each alarm.

- Data is reliably logged to the SD card in a simple, analysis-friendly CSV format that’s ready for downstream processing in R/Python.

---

*This document tracks the current behaviour of the node + UI layer as of the latest firmware changes (DS18B20 + soil moisture + soil thermistors). Future sensor types (PAR, humidity, wind, etc.) will follow the same “backend → registry → CSV” pattern.*
