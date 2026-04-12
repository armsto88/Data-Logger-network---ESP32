# Firmware Build Log

Status: Active tracking document
Rule: Keep previous stable workflow under V1. Track all new architecture and experiments under V2 until changed.

## Versioning Policy

- V1: Original workflow and behavior used before queue-first and scheduled sync changes.
- V2: New workflow, in-progress architecture, and related nice-to-haves.
- This document is the running changelog for firmware behavior changes and design intent.

## V1 Workflow Baseline (Previous)

Date logged: 2026-04-12

Node behavior:
- Node measured sensors and sent data directly to mothership over ESP-NOW during alarm-driven wake cycles.
- Single schedule control (wake interval) effectively controlled both sampling and transmission cadence.
- WiFi/ESP-NOW stack was generally active in runtime (not strict duty-cycled off in deployed state).

Mothership behavior:
- Mothership ran WiFi AP + web UI continuously for local management.
- Discovery, pair, deploy, unpair, and schedule commands were managed via ESP-NOW + web routes.
- Data was logged to SD as rows when packets arrived.

Operational summary:
- Simpler live-send model.
- Limited outage tolerance because data path depended on immediate transmission opportunities.
- No dedicated local queue contract for delayed sync replay.

## V2 Build Log (Current Direction)

### 2026-04-12 - Queue-first node and scheduled sync foundation

Implemented:
- Added node local persistent queue module for internal data logging:
  - firmware/nodes/sensor-node/src/storage/local_queue.h
  - firmware/nodes/sensor-node/src/storage/local_queue.cpp
- Node alarm path now captures sensor samples to local queue first.
- Added sync schedule message to shared protocol (`SET_SYNC_SCHED`) for fleet timing alignment.
- Added mothership broadcast support for sync schedule and web control endpoint.
- Added periodic mothership re-broadcast of sync schedule to keep nodes aligned.
- Node deployed behavior now duty-cycles radio:
  - WiFi/ESP-NOW enabled for sync windows and required control interactions.
  - WiFi/ESP-NOW disabled outside sync windows in deployed mode.
- Added mock mothership bring-up target for scheduled sync testing:
  - firmware/nodes/bringup/bringup_mock_mothership_sync.cpp
  - platformio env: esp32s3-mock-mothership-sync

Build validation:
- Mothership env build: success (`esp32s3`).
- Node env build: success (`firmware/nodes/sensor-node`, env `esp32c3`).
- Mock mothership env build: success (`esp32s3-mock-mothership-sync`).

Notes:
- Current queue flush removes records after successful ESP-NOW send call.
- Full application-level ACK semantics are planned for stronger delivery guarantees.

## V2 Nice-to-Haves

### BLE Wake App for Mothership WiFi

Status: Nice-to-have
Priority: Medium

Concept:
- Keep mothership WiFi off by default to reduce power.
- Use BLE as low-power control channel.
- User app sends local wake command (no internet required).
- Mothership enables WiFi AP for a timed session, then auto-shuts down.

Feasibility:
- ESP32-S3 supports BLE, so hardware is suitable.
- A standalone offline-capable mobile app is practical.

MVP scope suggestion:
- App button: Wake Mothership WiFi.
- App status: sleeping, waking, WiFi ready, session time left.
- Optional action: extend session.

Security expectations:
- Authenticated wake command (not plain-text unauthenticated trigger).
- Rate limiting and wake event logging.

## V2 Open Items

- Add app-level ACK protocol for queue replay durability (`ack_upto_seq` style).
- Add per-node backlog telemetry and sync health visibility in mothership UI/logs.
- Move from sensor label payload identity to stable sensor_id mapping.
- Define final policy for mothership WiFi duty cycle when web UI access is needed.

## How to Update This Document

For each new firmware change:
- Add date.
- Add what changed.
- Add why it changed.
- Add validation result (build/test outcome).
- Keep V1 untouched as baseline reference.
