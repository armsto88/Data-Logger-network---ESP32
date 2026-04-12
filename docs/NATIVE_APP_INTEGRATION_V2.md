# Native App Integration Plan (V2)

Status: Draft for implementation planning
Scope: Replace web-first operations with native app operations while keeping web UI available for prototyping and fallback.

## 1. Objectives

- Provide full feature parity with the current mothership web workflow in a native iOS/Android app.
- Keep the existing web UI operational during prototyping and migration.
- Move toward low-power operations where WiFi AP is optional and user-triggered.
- Reuse mothership business logic so web and app remain consistent.

## 2. Current Workflow Baseline (Web)

Current runtime and routes are implemented in:
- firmware/mothership/src/main.cpp
- firmware/mothership/src/comms/espnow_manager.cpp

Current web capabilities include:
- Dashboard and device status
- RTC set/time display
- CSV download
- Node discovery
- Wake interval broadcast
- Sync interval broadcast
- Node manager list and node configuration flow
- Pair/deploy/revert/unpair node actions

Core route surface today:
- GET /
- POST /set-time
- GET /download-csv
- POST /discover-nodes
- POST /set-wake-interval
- POST /set-sync-interval
- GET /nodes
- GET /node-config
- POST /node-config
- POST /revert-node

## 3. Target Architecture (App-First)

### 3.1 Transport split

- BLE: control and management channel (app <-> mothership)
- ESP-NOW: sensor transport channel (node <-> mothership)
- WiFi AP: fallback/debug channel and optional high-bandwidth mode

### 3.2 Firmware layering target

Introduce a shared command/service layer inside mothership firmware:
- Domain layer: command handlers and state transitions
- Transport adapters:
  - Web adapter (existing routes)
  - BLE adapter (new)

This allows both web and app to call the same internal operations.

## 4. Web-to-App Capability Parity Map

### 4.1 Dashboard

Web source:
- handleRoot in firmware/mothership/src/main.cpp

Native app equivalents:
- Mothership summary card
- Fleet counts (total, unpaired, paired, deployed)
- RTC time and health
- Current wake/sync interval

### 4.2 Time management

Web source:
- /set-time -> handleSetTime

Native app equivalents:
- Set RTC to device time
- Manual RTC set form
- Last sync timestamp view

### 4.3 Discovery

Web source:
- /discover-nodes -> handleDiscoverNodes -> sendDiscoveryBroadcast

Native app equivalents:
- Discover nodes button
- Discovery progress state
- New node list with live status updates

### 4.4 Scheduling

Web source:
- /set-wake-interval -> broadcastWakeInterval
- /set-sync-interval -> broadcastSyncSchedule

Native app equivalents:
- Fleet wake interval control
- Fleet sync interval control
- Future per-node schedule profiles

### 4.5 Node manager

Web source:
- /nodes -> handleNodesPage
- /node-config (GET/POST) -> handleNodeConfigForm / handleNodeConfigSave
- /revert-node -> handleRevertNode

Native app equivalents:
- Node list with state chips and time health
- Node detail view with:
  - numeric ID
  - friendly name
  - interval
  - action (start/deploy, stop/revert, unpair)

### 4.6 CSV export

Web source:
- /download-csv -> handleDownloadCSV

Native app equivalents:
- Export CSV command
- Save/share dialog
- Optional in-app preview of latest rows

## 5. BLE API Surface Proposal (MVP)

Design as a single custom service with request/response characteristics.

### 5.1 Command set

- get_status
- set_time
- discover_nodes
- set_wake_interval
- set_sync_interval
- list_nodes
- node_config_apply
- node_revert
- node_unpair
- export_csv_request

### 5.2 Response patterns

- Immediate ack with command_id + accepted/rejected
- Async result updates via notify characteristic
- Standard result fields:
  - ok
  - message
  - data payload
  - timestamp

### 5.3 Payload format

Use compact JSON for MVP to simplify debugging and iteration.
Binary encoding can be introduced later if needed.

## 6. Migration Plan (Phased)

### Phase 0 - Keep web active

- No user-facing regression in existing web UI.
- Keep current routes as source of truth for behavior.

### Phase 1 - Internal command abstraction

- Refactor route handlers so each action calls a shared service function.
- Avoid duplicate logic between route handler and future BLE handler.

### Phase 2 - BLE transport implementation

- Add BLE service and command dispatcher.
- Map each app command to the same shared service functions.

### Phase 3 - Native app MVP

- Recommended stack: Flutter for one codebase.
- Build parity screens in this order:
  1. connect/wake/status
  2. dashboard
  3. discover + node list
  4. node config actions
  5. schedules
  6. RTC set
  7. CSV export

### Phase 4 - Field hardening

- Add retries/timeouts for BLE command pipeline.
- Add user feedback for command success/failure.
- Add command/event logs in app and firmware.

### Phase 5 - App-first rollout

- Continue web as fallback initially.
- Optionally gate web to maintenance mode later.

## 7. Security and Safety Requirements

- BLE control channel must require authenticated commands.
- Rate-limit sensitive actions (wake, unpair, deploy).
- Log all state-changing commands with timestamp and origin.
- Keep safe defaults if malformed/unknown command is received.

## 8. Prototyping Guidance (Now)

For current prototyping, keep using the web app while native integration is built.

Recommended working mode:
- Web app remains primary operational UI during firmware refactors.
- Each native feature is validated against existing web behavior for parity.
- Do not remove web endpoints until native app passes parity checklist.

## 9. Parity Acceptance Checklist

App can be considered parity-ready when it can:
- Show mothership summary and fleet state
- Trigger discovery and show discovered nodes
- Pair, deploy, revert, and unpair nodes
- Set wake and sync schedules
- Set RTC
- Export CSV
- Surface clear errors and command results

## 10. Implementation References

Current web and command flow references:
- firmware/mothership/src/main.cpp
- firmware/mothership/src/comms/espnow_manager.cpp
- firmware/nodes/shared/protocol.h

This document is a V2 planning artifact and should be updated as command schemas and app screens are finalized.
