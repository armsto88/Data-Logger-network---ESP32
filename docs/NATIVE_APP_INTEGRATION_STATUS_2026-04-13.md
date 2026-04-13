# Firmware Response to Native App PM

Date: 2026-04-13  
From: Firmware Team (VSCode)  
Status: Phase C implemented; ready for full app workflow integration testing

## Executive Status

Thank you for the integration handoff. We audited the current firmware in this repository against the required BLE app contract.

Current state:
- Mothership firmware currently exposes Web UI + ESP-NOW control/data paths.
- BLE GATT transport is now implemented in mothership firmware (service + 3 characteristics).
- Sensor identity fields required by the app are implemented in node-to-mothership telemetry.
- Fleet sync scheduling with phaseUnix is implemented in ESP-NOW command path.

## 1) Open Questions (I-003) Answered

### BLE GATT Implementation Status
- [x] Is the BLE service (6f880001-...) implemented?
  - Yes.
- [x] Are all three characteristics exposed?
  - Yes.
- [ ] Is chunked message framing implemented?
  - Yes. 8-byte header framing and reassembly are implemented.
- [x] Does firmware echo correlationId in responses?
  - Yes for implemented Phase B command paths (success and error responses).

### Protocol Alignment
- [x] Does node_config_apply_request include phaseUnix field?
  - Yes. BLE `node_config_apply` handler supports `phaseUnix` and sync scheduling alignment.
- [x] Are sensor identity fields preserved in telemetry events?
  - Yes, telemetry includes sensorId, sensorType, sensorLabel.
- [x] Is timestamp validation implemented (>5 min stale rejection)?
  - Yes. Commands outside +/-5 minutes are rejected.

### Platform Support
- [ ] Does firmware advertise stable identifier for iOS?
  - Partial: device name advertises as `Microclimate-<deviceId>` and service UUID is advertised.
- [ ] Is bonded pairing supported for iOS reconnection?
  - Not yet.
- [ ] What MTU size is negotiated?
  - Not explicitly configured yet (default stack behavior).

## 2) What Is Already Implemented (Useful for Fast BLE Parity)

The firmware command behaviors already exist in web/ESP-NOW flows and can be reused behind BLE handlers:
- set_time (web route exists)
- discover_nodes (web route + ESP-NOW broadcast)
- set_wake_interval (web route + ESP-NOW broadcast)
- set_sync_interval with phaseUnix (web route + ESP-NOW broadcast)
- node deploy/start, stop/revert, and unpair flows (web route + ESP-NOW)
- CSV download/export exists via web path

Telemetry schema already supports app-required sensor identity fields:
- sensorId: uint16
- sensorType: char[16]
- sensorLabel: char[24]

## 3) Integration Gaps vs App Contract

Required but missing in firmware:
- iOS-safe stable identifier strategy in advertisements.
- BLE pairing/bonding strategy and reconnect hardening for iOS/Android edge cases.

## 4) Proposed Firmware Delivery Plan

### Phase A (1-2 days): BLE transport bring-up
- Implement GATT service + 3 characteristics with exact UUIDs. (Done)
- Advertise service UUID and device name containing Microclimate. (Done)
- Add basic get_status command over BLE returning mothership_status_result. (Done)

### Phase B (2-3 days): command envelope + routing
- Add canonical request envelope parser:
  - command
  - correlationId
  - timestampUnix
  - payload
- Echo correlationId in all ack/result/error responses. (Done for implemented commands)
- Implement stale timestamp rejection (>5 min). (Done)
- Add command router mapping to existing internal handlers. (Done for: `set_time`, `discover_nodes`, `set_wake_interval`, `set_sync_interval`, `list_nodes`)

### Phase C (2-4 days): full command parity + export
- Complete all 10 app commands. (Done)
- Add chunked framing for request/response payloads. (Done)
- Add export_csv_request -> export_csv_result with chunked transfer. (Done)
- Add status notify stream for node telemetry events. (Done)

### Phase D (1-2 days): platform hardening
- Validate iOS reconnection behavior; add stable device identifier strategy.
- Validate Android reconnect and MTU negotiation behavior.
- Run integration matrix tests with app team.

## 5) Command Mapping (Target)

Planned BLE command -> existing firmware behavior:
- get_status -> mothership status snapshot from current dashboard internals
- set_time -> existing RTC setter
- discover_nodes -> existing discovery broadcast
- set_wake_interval -> existing wake schedule broadcast
- set_sync_interval -> existing sync schedule broadcast (phaseUnix)
- list_nodes -> existing node registry view
- node_config_apply -> existing pair/deploy/start path
- node_revert -> existing revert to paired path
- node_unpair -> existing unpair path
- export_csv_request -> existing SD CSV data source

## 6) App Team Clarifications (Received)

Confirmed by app team on 2026-04-13:
1. timestampUnix unit
- Use seconds since Unix epoch (integer seconds).

2. Chunking header size
- Use 8-byte header:
  - 4 bytes messageId
  - 1 byte chunkIndex
  - 1 byte chunkCount
  - 2 bytes payloadLength

## 7) Immediate Next Action

Firmware team will start Phase D hardening: platform-specific BLE reliability testing (iOS/Android), bonded reconnect strategy, and MTU/throughput validation in integration runs with the app team.

## 8) Test Execution Reference

For app-team execution steps and pass criteria, use:
- `docs/PHASE_C_APP_INTEGRATION_CHECKLIST.md`
