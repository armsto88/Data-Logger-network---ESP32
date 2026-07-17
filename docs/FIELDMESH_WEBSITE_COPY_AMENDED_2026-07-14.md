# FieldMesh — Environmental Sensor Network

> Hardware / IoT

A battery-powered, low-cost environmental sensor network built on ESP32, designed for long-term ecological monitoring at remote sites without mains power or reliable internet.

| | |
|---|---|
| **Status** | Active Development |
| **Category** | Hardware / IoT |
| **Development focus** | Firmware, custom PCB, cloud backend, web dashboard |

**Project partners:** University of New England, REWI

---

## The problem

Organisms experience climate at centimeter-to-meter scales, not at the coarse scales represented by most standard weather stations. Conventional stations are intentionally standardized and well ventilated, which is essential for regional monitoring, but this setup often smooths out the local variation created by vegetation structure, soil moisture, topography, shading, and airflow. As a result, the conditions that actually drive biological responses can be under-sampled.

This scale mismatch is especially important in solar PV landscapes. Under-panel zones, inter-row corridors, and edge transitions create distinct microhabitats with different radiation, wind exposure, temperature, and moisture dynamics over very short distances.

Compounding the problem, most commercial environmental monitoring equipment is expensive, mains-powered, and assumes reliable internet — none of which hold at remote ecological field sites.

---

## What FieldMesh is

FieldMesh is a battery-powered, low-cost environmental sensor network built on ESP32 microcontrollers. It is designed for long-term ecological and environmental monitoring at remote sites without mains power or reliable internet.

There are two hardware tiers:

**Sensor nodes** — small, self-contained, battery-powered field devices. Each node sleeps almost all the time, wakes on a DS3231 real-time-clock alarm, samples its attached sensors, queues the reading locally, and — during a scheduled sync window — transmits a compact binary snapshot to the mothership over ESP-NOW (a low-power Wi-Fi protocol on channel 11). The node then powers off completely until the next alarm.

**Mothership (FieldHub)** — a central on-site hub. It wakes on its own RTC alarm, opens a coordinated sync window, collects snapshots from all deployed nodes, logs them to local flash storage (LittleFS) as a CSV file (`datalog.csv`), and periodically uploads accumulated data to the cloud over LTE (via a SIMCom A7670G-LABE Cat-1 cellular modem with SSL/TLS). It also hosts a captive Wi-Fi portal for field configuration — pairing nodes, setting recording intervals, and starting deployments.

The cloud backend is in migration from **Google Sheets via Google Apps Script** (the original backend) to **Supabase** (PostgreSQL + Auth + Row-Level Security + Edge Functions + Realtime). Both backends are currently supported behind a data-provider abstraction so the frontend can read from either, enabling a staged, rollback-safe cutover. A web dashboard (React + Vite, in a separate repository) visualises readings, system health, and node status.

---

## Data flow end-to-end

```
Sensor → Node ADC/I2C → V2 snapshot (local queue) → ESP-NOW → Mothership
    → LittleFS CSV (datalog.csv) → Upload queue (cursor-tracked)
    → JSON payload builder → HTTPS POST over LTE → Cloud backend
    → Web dashboard
```

Node local queues are stored in NVS-backed storage. A snapshot is only popped after a durable `SNAPSHOT_ACK` (persisted flag) from the mothership, so lost ACKs retain the record for the next sync window. The mothership logs every snapshot to flash CSV, then uploads via a cursor-tracked queue with retry and backoff. Local logging is always primary — upload failure never causes data loss.

---

## What it measures

Per the V2 snapshot protocol, each node can report the following channels. Passive sensors (soil, wind, AUX) are gated by an operator-configured sensor mask; self-identifying I2C sensors (SHT41, AS7341) are always auto-detected.

| Channel | Sensor | Unit |
|---|---|---|
| Air temperature | SHT41 | °C |
| Air relative humidity | SHT41 | % |
| Spectral light (8 channels) | AS7341 | raw counts at 415/445/480/515/555/590/630/680 nm |
| Wind speed | Ultrasonic anemometer or reed-cup (solder-jumper fallback) | m/s |
| Wind direction | Ultrasonic anemometer (planned; reed-cup provides speed only) | degrees |
| Soil volumetric water content (×2) | ADS1115 + VH-5 probes | m³/m³ |
| Soil temperature (×2) | ADS1115 + thermistor | °C |
| Battery voltage | Node ADC | V |
| Auxiliary I2C (×2) | Expansion ports | reserved |

> **Note on wind:** the ultrasonic wind backend is in active development; current firmware does not yet produce discriminative wind-speed readings. A reed-cup anemometer (selectable via solder jumper) is supported as a fallback for wind speed only.

---

## How the network operates

**Node states.** A node is always in one of three states: *UNPAIRED* (no mothership MAC known, radio listens for pairing), *PAIRED* (mothership MAC known but not yet deployed), or *DEPLOYED* (bound to a mothership, RTC synced, alarms armed). A fourth implicit mode — *standby* — is a DEPLOYED node with recording paused: it keeps its sync check-ins but takes no samples.

**Data wake (Alarm 1).** When the DS3231 Alarm 1 fires, the node reads all registered sensors into a V2 snapshot and enqueues it locally. The radio stays off during a data wake — sampling is silent and low-power. Alarms are re-armed before the hang-prone sensor work is done, so a stall can never leave the DS3231 without a future alarm. The node then releases `PWR_HOLD` and powers off completely.

**Sync wake (Alarm 2).** At the scheduled sync slot, the node brings up ESP-NOW and listens for a `SYNC_SESSION_OPEN` broadcast from the mothership. On hearing it, the node waits a deterministic per-node jitter delay (a hash of node ID + session ID) to spread HELLO responses, then sends a `NODE_HELLO`. The mothership responds with per-node config and a `DUMP_GRANT`, the node flushes its queue (each snapshot requiring a durable `SNAPSHOT_ACK` before being popped), and the mothership releases the node with `SYNC_RELEASE` carrying current Unix time and the active schedule. The node applies this, persists config, re-arms its alarms, and powers off.

**Coordinated sync window.** The mothership runs a structured, grant-based rendezvous — not a free-for-all. It broadcasts session-open frames, collects HELLOs, authorises only registered deployed nodes, and grants each node a time-limited transmit window with a record quota. Empty nodes are clock-synchronised and released immediately; nodes with backlog are served in round-robin until drained or the grant budget is exhausted. Every responder is released individually, even if backlog remains, so the next sync window picks up where this one left off.

**Pairing and deployment.** Operators discover, pair, and deploy nodes from the Field UI captive portal (served at `http://192.168.4.1/`). Deployment sets the node's RTC, marks it DEPLOYED, and arms both alarms. Config updates (recording interval, target state, sensor mask) are delivered declaratively and applied only if strictly newer, so re-broadcasting is idempotent. A rescue mode (three rapid boots within a 20-second window) wipes config and listens for re-pairing, recovering a node that has lost its mothership.

---

## Cloud and dashboard

The cloud tier has two layers: a backend that ingests, stores, and serves data, and a frontend dashboard that users interact with.

**Backend.** Two backends coexist behind a data-provider boundary — an abstraction layer that lets the frontend read from either without rewriting pages, enabling a staged, rollback-safe migration. The legacy backend is Google Apps Script / Google Sheets. The new backend is **Supabase** (PostgreSQL + Auth + Edge Functions + Realtime): a multi-tenant schema with a `projects → motherships → nodes → readings` hierarchy, row-level security on all browser-accessible tables, hashed device API keys (secrets never exposed to the browser), and Deno Edge Functions for authenticated, idempotent telemetry ingestion. Realtime delivery pushes new readings to connected dashboards without polling. The Supabase migration is underway; both backends are currently functional, and the cutover is staged so Google Sheets can remain a fallback.

**Frontend dashboard.** A single-page React app (hosted in a separate repository) built with Vite, Tailwind CSS, and TanStack Query. It uses React Router with authenticated, project-scoped routes and lazy-loaded pages (Charts, Nodes, Config, Motherships, Settings, About) with an ErrorBoundary so a failure in one page does not take down the whole app. Recharts powers time-series visualisation, React-Leaflet shows deployed nodes on a map, and Framer Motion drives transitions. QR code generation supports on-device provisioning flows.

The dashboard shows sensor readings (charts and map), node status, battery voltages, sync schedule, flash usage, upload queue depth, modem diagnostics, boot diagnostics, per-node status, and mothership device pages — with full handling of loading, empty, stale, error, offline, and permission states.

---

## Power and wake architecture

Both boards use the same fundamental pattern. A `PWR_HOLD` GPIO drives a FET gate to latch the switched rail on — asserted at the very first instruction of boot and released at the end of a wake cycle to power the board off completely. The DS3231 RTC is the sole wake source: Alarm 1 (recording, second-resolution) and Alarm 2 (sync, minute-resolution) are independently armed and verified. The RTC is backed by a coin cell so time survives main power loss.

The firmware explicitly does not use ESP32 deep sleep, because the wake path is the RTC-driven FET, not a GPIO interrupt. Power-off is the default — both boards spend almost all their time fully powered off, active only for the brief wake cycle (seconds for nodes, minutes for the mothership).

The fleet operates on a **phase-aligned interval schedule**: the mothership owns a sync anchor (an interval in minutes and a phase marking a slot boundary), and all sync wakes land on `phase + N × interval`. Schedule transitions are handled gracefully — when the operator changes the wake interval, the mothership keeps waking on the old schedule for up to three grace cycles while handing nodes the new schedule, so no node is stranded on an orphaned cadence.

---

## Design principles

- **Local-first** — all data is logged locally before any upload. Upload failure never causes data loss; the upload queue is cursor-tracked and retryable.
- **Power-off is the default** — both boards spend almost all their time fully powered off. The only active time is the brief wake cycle.
- **NVS is the source of truth** — all persistent state survives hard power cuts. RTC RAM is not trusted.
- **Durable ACKs** — a snapshot is only popped from the node's queue after the mothership confirms it was persisted to flash, not just received over the air.
- **Bounded recovery** — every critical operation (alarm arm, RTC init) has a bounded retry with a defined fallback, so a transient I2C failure never strands a device.
- **Coordinated, not contention-based** — the sync window is a structured grant-based protocol, not a free-for-all. The mothership schedules each node's transmit window to avoid collisions.
- **Backward compatibility** — V1 snapshots, legacy sync markers, and rolling-upgrade fallbacks are all handled, so a mixed-version fleet still operates.

---

## Current status

The project is in active development.

**Firmware:** Mothership V2 and Node V2 firmware flows are implemented and validated at the bring-up level on custom PCBs. RTC, ESP-NOW sync, sensor auto-detection, NVS queueing, LittleFS logging, and the modem (AT-level + Supabase dry-run POST) have been exercised. Full over-the-air HTTPS upload to a live backend and the ultrasonic wind measurement path remain in progress.

**Cloud backend:** Supabase migration is underway alongside the legacy Google Apps Script backend, behind a data-provider boundary for rollback-safe cutover. Both backends are currently functional.

**Dashboard:** React + Vite single-page app (in a separate repository) with charts, map, node management, and mothership device pages.

**Hardware:** Custom node PCB (NODE_v3) Gerbers exported and ready to order, with a hardware checklist (reverse-polarity protection, leakage verification, LED strategy) still being worked through. Node sensor housings are 3D-printed (STLs complete) and manufactured via FDM. The mothership uses an off-the-shelf industrial enclosure format.