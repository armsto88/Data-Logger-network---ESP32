# Firmware Functionality Overview — End-to-End Workflow

**Date:** 24 April 2026  
**Audience:** Ecologists, field practitioners, and environmental monitoring teams  
**Purpose:** Complete reference for how the system works from power-on through to CSV data download, including the web UI, node lifecycle, data flow, schedule control, and field edge cases.

---

## Contents

1. [System Overview](#1-system-overview)
2. [First Access — Connecting to the Mothership](#2-first-access--connecting-to-the-mothership)
3. [The Web UI — Page by Page](#3-the-web-ui--page-by-page)
4. [Node Lifecycle — From Discovery to Deployment](#4-node-lifecycle--from-discovery-to-deployment)
5. [Node Operating Behaviour When Deployed](#5-node-operating-behaviour-when-deployed)
6. [Sync Windows — How Data Moves](#6-sync-windows--how-data-moves)
7. [Data Logging — CSV Format and Storage](#7-data-logging--csv-format-and-storage)
8. [Schedule Control](#8-schedule-control)
9. [Edge Cases and Recovery](#9-edge-cases-and-recovery)
10. [Removing a Node from Service](#10-removing-a-node-from-service)
11. [Power and Battery Strategy](#11-power-and-battery-strategy)
12. [Current Limits and Planned Work](#12-current-limits-and-planned-work)
13. [Suggested Manuscript Framing](#13-suggested-manuscript-framing)

---

## 1. System Overview

The platform has two device roles:

**Node** — A field sensor unit. It wakes on a timer, reads sensors, stores readings in local memory, and goes back to sleep. It does not depend on the mothership being present at the time of measurement. Up to 24 samples can be queued before the oldest begin to be overwritten.

**Mothership** — A collection and coordination unit. It runs a WiFi access point and a web server, receives uploaded node data, writes records to SD card CSV, and manages node schedules and configuration. Currently it runs continuously. A low-power scheduled-wake architecture is planned for field deployment.

**Core design principle:** nodes protect data first. Data is captured locally on the node and transferred to the mothership in bulk during scheduled sync windows. If the mothership is unavailable at sync time, the queue retains its data for the next window.

---

## 2. First Access — Connecting to the Mothership

1. Power on the mothership.
2. On your phone, tablet, or laptop, connect to the WiFi network named **`Logger001`** (or `Logger` + the configured device ID).
3. No password is required.
4. On most phones a captive portal page will open automatically. On a laptop, open a browser and navigate to **`http://192.168.4.1/`**.

The mothership serves its web interface on WiFi channel 11. Nodes communicate on the same channel. No internet connection is needed.

---

## 3. The Web UI — Page by Page

### 3.1 Home Page (`/`)

The home page is the control centre. It shows:

**Top bar — Mothership time**
The current RTC time read from the mothership's DS3231 clock. This is the time reference for all scheduling and logging. Can be corrected using the SET TIME panel.

**Fleet summary chips**
Three counts at a glance:
- **Deployed** — nodes actively sampling and syncing.
- **Paired** — nodes that have been paired but not yet deployed.
- **Unpaired** — nodes seen but not yet paired.

**Action buttons**
- **Discover Nodes** — sends a broadcast requesting all nearby powered nodes to announce themselves. New nodes appear in the fleet within seconds.
- **Node Manager** — opens the full node list and configuration page.
- **CSV** — downloads the current `datalog.csv` file directly to your device.
- **INFO** — shows mothership device ID, firmware version, SSID, IP, and MAC address.

**Global Schedules section**

Controls the timing shared across the whole fleet:

| Control | What it does |
|---|---|
| SET TIME | Manually set or correct the mothership DS3231 clock. Use the browser time button for convenience. |
| Node interval | Sampling interval shared across the whole fleet (1, 5, 10, 20, 30, or 60 min). This is the **only scheduling knob**. |
| Auto-sync | Derived automatically from the wake interval: `syncMin = wakeMin × 18`. Shown as a read-only info panel. No manual setting — changing the wake interval updates sync automatically. |

All schedule settings are persisted to NVS and survive a mothership reboot.

**Data status section**
Shows SD card summary — record count and last logged timestamp.

---

### 3.2 Node Manager (`/nodes`)

Lists every known node with one row per node. Refreshes automatically every 15 seconds.

| Column | Meaning |
|---|---|
| ID / Name | User-assigned ID (e.g. `001`) and name (e.g. `River bank North`). Falls back to hardware-derived node ID if not yet assigned. |
| Interval | The configured target wake interval for this node. Reflects the global wake interval immediately when changed — no redeploy required. |
| Next wake | Estimated next wake time, computed from last contact time and configured interval. |
| Deploy chip | `Deployed`, `Paired`, or `Unpaired` — the node's lifecycle state. |
| Battery chip | Last measured battery voltage. Green ≥ 3.9 V, orange 3.5–3.9 V, red < 3.5 V, grey `n/a` before first data received. Persists across mothership reboots. |

Clicking a node row opens its individual configuration page.

---

### 3.3 Node Configuration Page (`/node-config?node_id=...`)

**Information fields**
- Firmware ID, hardware MAC address, last seen time, inferred wake cadence, queue depth.

**Configurable fields**
- User-assigned ID and name (used in the CSV and UI).
- Per-node wake interval (overrides global default when set).
- Notes field.

**Actions**
- **Deploy** — triggers the full deploy sequence with current settings.
- **Unpair** — removes the node from the fleet. The UNPAIR command is sent at the next sync window.
- **Pair only** — pair the node without deploying (useful for staging before committing schedules).

---

## 4. Node Lifecycle — From Discovery to Deployment

### 4.1 States

| State | Meaning |
|---|---|
| **Unpaired** | Node is powered and broadcasting. Mothership knows it exists but has no association. |
| **Paired** | Mothership has registered the node. The node knows the mothership's MAC. |
| **Deployed** | Node is actively sampling, queuing, and syncing. RTC alarms are armed. |

### 4.2 Step-by-Step Commissioning

**Step 1 — Discover**
Press **Discover Nodes** on the home page. The mothership broadcasts a discovery request. All powered unpaired nodes respond with their hardware ID, MAC address, and firmware type. New nodes appear in the Node Manager.

**Step 2 — Configure**
Click the node in the Node Manager. Assign a user ID and friendly name. Set the sampling interval if it differs from the global default.

**Step 3 — Deploy**
Click **Deploy** in the node config page. The mothership:
1. Sends a PAIR command if not already paired.
2. Sends a DEPLOY command with: current RTC time, wake interval, sync interval (auto-derived as `wakeMin × 18`), and the next fleet sync slot's Unix timestamp (the phase anchor — the actual next scheduled fleet sync time, not deploy time).
3. Marks the node DEPLOYED locally and saves to NVS.
4. Resets the sync slot guard (`gLastSyncIntervalSlot = -1`) so the next fleet sync fires cleanly from the new anchor.

The node:
1. Sets its internal RTC from the deploy payload.
2. Arms Alarm1 (A1) to `now + intervalMin` for the first data wake.
3. Arms Alarm2 (A2) directly to the phase anchor (the next fleet sync slot).
4. Sends DEPLOY_ACK back to the mothership.
5. Runs an immediate first sample cycle, queues the data, then cuts power.

**Step 4 — Verify**
In the Node Manager: Deploy chip shows **Deployed** and Config chip shows **Config updated** after the node's next contact. Queue counter rises with each data wake.

### 4.3 Configuration Handshake

Every sync wake, the node sends NODE_HELLO with its current config version and queue depth. If the mothership has a newer desired config, it pushes a CONFIG_SNAPSHOT. The node applies it and sends CONFIG_ACK, which flips the chip from **Config pending** to **Config updated**. If an ACK is dropped, the mothership infers convergence when the node's reported wake interval matches the desired setting.

---

## 5. Node Operating Behaviour When Deployed

### 5.1 Data Wake (Alarm 1 — A1)

Fires every N minutes. No radio activity.

1. Power-on (A1 alarm fires, gate powers the board).
2. Load config from NVS.
3. Read all sensors.
4. Append readings to local queue.
5. Re-arm A1 to `now + intervalMin`.
6. Cut power.

Boot to power-off: approximately 2–3 seconds. Radio stays entirely off.

### 5.2 Sync Wake (Alarm 2 — A2)

Fires at the next scheduled fleet sync slot. This is when data is transmitted.

1. Power-on (A2 alarm fires).
2. Load config from NVS.
3. Bring up WiFi + ESP-NOW radio.
4. Send NODE_HELLO to mothership (unicast + broadcast for reliability).
5. Wait up to 60 seconds for SYNC_WINDOW_OPEN marker from mothership.
6. If marker received:
   - Flush all queued records to the mothership.
   - Each record is delivery-confirmed via ESP-NOW callback before being removed from the queue.
   - Flush stops 2 seconds before the window deadline — any remaining records carry forward.
7. If no marker in 60 seconds: queue is preserved, alarms re-armed, power cut.
8. Re-arm A2 to next sync slot.
9. Shut down radio and cut power.

### 5.3 Combined Wake (A1 + A2 align)

If both alarms fire at the same time, one combined cycle runs: sample → sync flush → re-arm both alarms → power down.

### 5.4 Queue Capacity and Overflow

The queue holds 24 records. When full, the oldest record is dropped to make room. Dropped records are flagged with quality flag `QF = 0x0001` so the gap is visible in the CSV.

---

## 6. Sync Windows — How Data Moves

### 6.1 The Sync Anchor

When a node is deployed, the mothership calculates the **next fleet sync slot** as the deploy anchor — the Unix timestamp of the next scheduled sync boundary. All deployed nodes share the same anchor and interval, so they all wake at the same moment fleet-wide.

### 6.2 Mothership Sync Broadcast

At the sync slot boundary + 5 seconds (5 s offset to allow node boot and radio-up time), the mothership sends:

1. **SET_SYNC_SCHED** — 3 broadcasts, 200 ms apart. Carries the auto-derived sync interval and phase anchor.
2. **SYNC_WINDOW_OPEN** — 3 broadcasts, 200 ms apart. This is the marker that gates node queue flushing.

All 6 transmissions use true ESP-NOW broadcast (`FF:FF:FF:FF:FF:FF`). Total radio time is fixed regardless of fleet size — a node count of 1 or 20 generates the same 6 packets.

### 6.3 What the Node Does on Receiving the Marker

The node receives the SYNC_WINDOW_OPEN marker and immediately begins transmitting queued records to the mothership. Each record is delivery-confirmed before removal from queue. Remaining records stay in queue for the next window.

### 6.4 What the Mothership Does on Receiving Records

Records arrive via ESP-NOW callback and are placed into a small in-RAM buffer, then drained to the SD card in the main loop. This decoupling means further incoming packets are not blocked during SD writes. Each record is appended to `/datalog.csv`.

---

## 7. Data Logging — CSV Format and Storage

### 7.1 File

Data is written to `/datalog.csv` on the mothership's SD card.

On first boot (or after a firmware update that changes the header format), the file is created with the correct header. If an existing file has a mismatched header, it is automatically renamed to `/datalog_legacy.csv` and a new file is started — previous data is preserved.

### 7.2 CSV Format

The CSV uses a **wide format** — one row per node per wake cycle, all sensor channels in a single row. Sensors not fitted on a given node appear as `NaN` in the relevant columns.

**Columns (30 total):**

| Column | Example | Meaning |
|---|---|---|
| `ms_datetime` | `11:02:05 24-04-2026` | Mothership RTC time at moment of logging |
| `ms_sync_unix` | `1777028400` | Mothership Unix timestamp of the sync window |
| `node_id` | `001` | User-assigned node ID |
| `node_name` | `NODE 1` | User-assigned node name |
| `node_mac` | `d4:e9:f4:94:5d:c4` | Node hardware MAC address |
| `fw_id` | `ENV_945DC4` | Firmware-derived node identifier |
| `node_datetime` | `11:01:45 24-04-2026` | Node RTC time at moment of measurement |
| `node_unix` | `1777028505` | Node Unix timestamp of measurement |
| `bat_v` | `3.85` | Battery voltage (V), `NaN` if not fitted |
| `air_temp_c` | `18.44` | Air temperature (°C), `NaN` if not fitted |
| `air_hum_pct` | `44.8` | Relative humidity (%), `NaN` if not fitted |
| `spectral_415nm`–`spectral_680nm` | `0.032` | Spectral irradiance channels ×8 (415, 445, 480, 515, 555, 590, 630, 680 nm), `NaN` if not fitted |
| `wind_speed_ms` | `2.1` | Wind speed (m/s), `NaN` if not fitted |
| `wind_dir_deg` | `247.0` | Wind direction (°), `NaN` if not fitted |
| `soil1_vwc` | `0.440` | Soil 1 volumetric water content, `NaN` if not fitted |
| `soil1_temp_c` | `14.2` | Soil 1 temperature (°C), `NaN` if not fitted |
| `soil2_vwc` | `NaN` | Soil 2 VWC, `NaN` if not fitted |
| `soil2_temp_c` | `NaN` | Soil 2 temperature, `NaN` if not fitted |
| `aux1` | `NaN` | Auxiliary channel 1, `NaN` if not used |
| `aux2` | `NaN` | Auxiliary channel 2, `NaN` if not used |
| `sensor_present` | `0x0023` | Bitmask of sensors fitted on this node |
| `quality_flags` | `0x0000` | Data quality flags (see below) |
| `seq_num` | `42` | Queue sequence number since last deploy |

**Key timing note:** `ms_datetime` is when the mothership received and logged the record. `node_unix` is when the node actually measured the sample. The difference is the buffering delay — expected and correct. Use `node_unix` for ecological analysis of measurement timing.

**Quality flags (`quality_flags`):**
- `0x0000` = clean measurement.
- `0x0001` = this record was preceded by a queue overflow (a gap may exist before it in the data stream).

### 7.3 Downloading Data

From the home page, click the **CSV** button. The browser downloads `datalog.csv` directly from the mothership over the WiFi connection. Open in any spreadsheet or analysis tool.

---

## 8. Schedule Control

### 8.1 The Scheduling Model

There is a **single scheduling knob**: the global wake interval (1, 5, 10, 20, 30, or 60 minutes). The sync interval is derived automatically:

$$\text{syncMin} = \text{wakeMin} \times 18$$

This keeps the queue at approximately 75% fill between syncs, leaving a 4-wake soft headroom buffer before any overflow risk. The derived sync interval is shown as a read-only info panel on the home page.

**Example configurations:**

| Wake interval | Auto sync interval | Queue depth at sync | Headroom |
|---|---|---|---|
| 1 min | 18 min | 18/24 slots (75%) | 6 wakes |
| 5 min | 90 min | 18/24 slots (75%) | 6 wakes |
| 10 min | 3 h | 18/24 slots (75%) | 6 wakes |
| 30 min | 9 h | 18/24 slots (75%) | 6 wakes |
| 60 min | 18 h | 18/24 slots (75%) | 6 wakes |

### 8.2 Changing the Schedule

Change the global wake interval on the home page. The new sync interval computes immediately. At the next sync window, all deployed nodes receive `SET_SYNC_SCHED` with the new derived `syncMin` and a fresh phase anchor, and re-arm their A2 alarms accordingly. No per-node action is required.

### 8.3 Setting the Time

The mothership DS3231 is the time source for the whole fleet. Correct it via the SET TIME panel on the home page. Use **Use browser time** to pull the current time from the connected device automatically.

After a correction, the mothership will include a fresh timestamp in the next sync broadcast payload and all nodes will re-sync their clocks.

---

## 9. Edge Cases and Recovery

### 9.1 Node Misses a Sync Window

The node waited 60 seconds, received no marker, re-armed its alarms, and cut power. All queued data is intact — it will retry at the next sync slot. No action required.

### 9.2 Mothership Reboots Mid-Cycle

The sync slot guard is persisted to NVS. After reboot, the mothership reloads it and will not re-fire a sync burst that already happened in the current slot. Nodes that were mid-flush when the mothership rebooted will complete their listen window, note the missing marker, and retry at the next slot.

### 9.3 Node Clock Drift or Stale Sync

If a node's last time-sync is more than 24 hours old, stale recovery activates during the next data wake:
1. Node brings up radio briefly and sends NODE_HELLO + REQUEST_TIME.
2. If mothership replies with TIME_SYNC, node updates its clock and re-arms alarms.
3. If a SYNC_WINDOW_OPEN marker is also received, node flushes its queue.

The mothership independently infers stale nodes from contact age and sends a unicast SET_SYNC_SCHED + TIME_SYNC as a proactive assist.

### 9.4 3-Boot Rescue Mode

If a node is stuck and unreachable through normal means:

1. Power-cycle the node **three times** rapidly (within 20 seconds). For example, remove and reinsert the battery three times.
2. On the third boot the node detects the rapid-reboot pattern and enters rescue mode.
3. Node config is wiped to UNPAIRED, the radio stays on, and the node continuously broadcasts discovery beacons every 5 seconds.
4. The mothership sees the node as UNPAIRED in the Node Manager — re-pair and re-deploy normally.

No serial connection or programming hardware is required. The streak counter resets itself after a successful normal boot, so two rapid reboots during normal field work (e.g. reseating a battery connector) will not accidentally trigger rescue.

### 9.5 Duplicate Deploy

If a node already deployed receives a second deploy command (e.g. operator re-deploys to update config), the extra immediate sample cycle is suppressed. Config values are updated and alarms are re-armed from the new phase anchor if it is fresher than the stored one.

---

## 10. Removing a Node from Service

When an operator clicks **Unpair** in the node config page:

1. The mothership queues an UNPAIR command for that node.
2. At the next sync window, the UNPAIR command is broadcast alongside the sync burst (3 transmissions).
3. The node receives the UNPAIR during its sync wake, after completing any queue flush.
4. The node clears all deployment state, wipes its queue, disables its RTC alarms, and powers off cleanly — it does not re-arm any alarms.
5. The node will not wake again autonomously.

The node returns to a factory-fresh state, ready to be rediscovered and re-deployed.

**Idle timeout:** If a node sits in UNPAIRED or PAIRED state for 15 minutes with no deploy, it automatically cuts power. This prevents battery drain on un-commissioned or recently unpaired nodes left powered on. Cycling power restarts the discovery beacon so the node can be found again.

---

## 11. Power and Battery Strategy

### 11.1 Node Power Model

The node uses a hardware power gate (PWR_HOLD pin) for a complete power cut between wake cycles — not a microcontroller deep-sleep. The processor, RAM, and radio are fully off. On the next RTC alarm, the board powers up from cold.

This means:
- No RAM state survives between wakes. All config is reloaded from NVS on every boot.
- The DS3231 RTC continues running on its own coin-cell battery.
- Boot time (NVS load, sensor init, I2C scan) is approximately 2–2.5 seconds.

### 11.2 Battery Voltage Monitoring

Every data wake, the node reads battery voltage via an ADC channel (GPIO35) connected through a resistor divider. The reading is included in every snapshot packet under the `bat_v` CSV column. A calibration scale factor (`BAT_DIVIDER_SCALE`, default 3.58) converts the ADC reading to volts; this was determined by DMM measurement against the actual battery voltage during commissioning.

The Node Manager displays the last received battery voltage for each node as a colour-coded chip:
- **Green** — ≥ 3.9 V (healthy)
- **Orange** — 3.5–3.9 V (moderate, plan recharge)
- **Red** — < 3.5 V (low — recharge or replace before next field period)
- **Grey n/a** — no snapshot received yet since last mothership reboot

Battery readings persist across mothership reboots. The last value is stored in NVS alongside node pairing state.

**Note on accuracy:** The ESP32 ADC has ±5–10% inherent nonlinearity. Battery readings are accurate to approximately ±0.2 V relative to a DMM reference. This is sufficient for state-of-health monitoring and low-battery alerting but not for precise charge estimation.

### 11.2 Battery Voltage Monitoring

Every data wake, the node reads battery voltage via an ADC channel (GPIO35) connected through a resistor voltage divider on the PCB. The reading is included in every snapshot as `bat_v` in the CSV.

The Node Manager displays the last received value as a colour-coded chip:
- **Green** — ≥ 3.9 V (healthy)
- **Orange** — 3.5–3.9 V (moderate, plan recharge)
- **Red** — < 3.5 V (low — recharge or replace before next field period)
- **Grey `n/a`** — no snapshot received yet since last mothership reboot

This value persists across mothership reboots — you do not need to wait for the next sync window to see the last known battery state after cycling power on the mothership.

**Note on accuracy:** The reading is calibrated against a DMM reference at commissioning (`BAT_DIVIDER_SCALE = 3.58`, 24 April 2026) but the ESP32 ADC has ±5–10% inherent nonlinearity. Readings are accurate to approximately ±0.2 V — sufficient for state-of-health monitoring and low-battery alerts, not for precise charge estimation.

### 11.3 Radio Duty Cycle

A deployed node keeps radio fully off during data wakes. Radio is on only during:
- Sync wakes — up to 60 seconds.
- NODE_HELLO at sync wake start.
- Stale-sync recovery windows (brief, on data wakes only when clock age exceeds 24 hours).

For a 1-minute wake interval, sync fires every 18 minutes — radio is active at most 60 seconds per 18-minute period, approximately 6% radio duty cycle. For a 10-minute wake interval, sync fires every 3 hours — radio duty cycle drops to under 1%.

### 11.3 Mothership Power

The mothership currently runs continuously (WiFi AP + web server + ESP-NOW always on). A scheduled field mode where it powers on only for sync windows is planned but not yet implemented.

---

## 12. Current Limits and Planned Work

| Area | Status |
|---|---|
| Mothership always-on power model | Planned: field mode will use RTC to power mothership on for sync windows only. |
| Fleet sync contention (>8 simultaneous nodes) | No per-node stagger offset yet. Profiling needed at bench scale before large deployment. |
| Long-term multi-season durability | Not yet validated. Testing is currently bench and short-term field. |
| Sensor calibration | Bring-up establishes functional readiness only. Formal metrological calibration is a separate activity. |
| Mothership SD write (decoupled from receive) | Already implemented: SD writes are queued in RAM and drained in main loop, preventing receive-path blocking. |
| Battery voltage accuracy | ±0.2 V vs DMM reference (ESP32 ADC nonlinearity). Suitable for health monitoring, not precision charge estimation. Per-board calibration constants would improve accuracy. |

---

## 13. Suggested Manuscript Framing

> The firmware architecture was designed for ecological field reliability under intermittent communications and constrained power budgets. Sensor nodes employ a queue-first data capture strategy, accumulating measurements in local persistent memory and transmitting in batch to a central mothership during scheduled synchronisation windows. Nodes achieve low radio duty cycle through RTC-gated wake/sleep cycles governed by a hardware power gate rather than microcontroller deep-sleep, ensuring complete power removal between measurement events. Recovery mechanisms include automated stale-synchronisation correction initiated by both node and mothership, and an operator-triggered rescue mode permitting field recovery without specialised hardware interfaces. Removal of a node from the active fleet is handled gracefully at the next synchronisation window, after which the node powers down and does not consume further battery capacity.

### Recommended follow-on sections for the manuscript

1. Validation summary table — single-node and multi-node test pass/fail outcomes.
2. Battery budget estimates — awake time, sleep current, and estimated runtime for representative sampling configurations.
3. Data completeness metrics — records received vs expected across induced communication outage scenarios.
4. Field protocol appendix — deployment procedure, time-setting, routine health checks, recovery steps, and CSV download workflow.
