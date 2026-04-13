# Phase C App Integration Checklist

Date: 2026-04-13
Audience: Native app team + firmware team
Scope: Validate Phase C BLE integration end-to-end

## Preconditions

- Mothership firmware flashed with current Phase C build.
- At least one node is powered and visible to mothership.
- Mothership has existing `/datalog.csv` data for export tests.
- App uses:
  - `timestampUnix` in seconds.
  - 8-byte chunk header for chunked BLE payloads.

## BLE Discovery and Connection

1. Scan for BLE devices and find `Microclimate-<deviceId>`.
2. Confirm service UUID is discoverable:
   - `6f880001-2d0f-4aa0-8f9e-8f8b7e5a0001`
3. Confirm all 3 characteristics are present:
   - request write: `6f880002-2d0f-4aa0-8f9e-8f8b7e5a0001`
   - response notify: `6f880003-2d0f-4aa0-8f9e-8f8b7e5a0001`
   - status notify: `6f880004-2d0f-4aa0-8f9e-8f8b7e5a0001`

Expected result:
- Connect succeeds and notifications can be enabled on response and status characteristics.

## Envelope Validation

Send malformed requests and confirm error responses include `correlationId` echo:

1. Missing `command`.
2. Missing `timestampUnix`.
3. Stale `timestampUnix` older than 5 minutes.

Expected result:
- Error response is returned via response notify.
- `ok=false` and `error.code` is populated.
- Original `correlationId` is echoed if provided.

## Command Success Path

For each command below, send valid envelope and verify `ok=true`:

1. `get_status`
2. `set_time`
3. `discover_nodes`
4. `set_wake_interval`
5. `set_sync_interval`
6. `list_nodes`
7. `node_config_apply`
8. `node_revert`
9. `node_unpair`
10. `export_csv_request`

Expected result:
- Response notify returns matching `correlationId`.
- Response `type` matches command intent.
- Command result fields contain expected values.

## Chunking and Reassembly

1. Send request payload > 20 bytes using chunk framing.
2. Verify mothership reassembles and processes request.
3. Trigger large response (`export_csv_request`) and verify app reassembles chunked response.

Chunk header format (8 bytes):
- 4 bytes `messageId`
- 1 byte `chunkIndex`
- 1 byte `chunkCount`
- 2 bytes `payloadLength`

Expected result:
- No JSON parse failures after reassembly.
- Chunk order handling is correct.
- Full response body reconstructed in app.

## Telemetry Status Stream

1. Keep status notifications enabled.
2. Wait for node telemetry to arrive at mothership.
3. Verify `node_telemetry_event` packets appear on status notify.
4. Confirm fields present:
   - `nodeId`, `mac`, `sensorId`, `sensorType`, `sensorLabel`, `value`, `nodeTimestamp`, `qualityFlags`

Expected result:
- Events stream in near real-time while nodes transmit.

## CSV Export Behavior

1. Run `export_csv_request`.
2. Validate response type is `export_csv_result`.
3. Validate metadata fields:
   - `fileName`, `totalBytes`, `returnedBytes`, `truncated`, `csvData`
4. If `truncated=true`, verify app displays partial-export warning.

Expected result:
- Export works and app handles partial payload safely.

## Platform Smoke Tests

iOS:
- Reconnect after app background/foreground cycle.
- Reconnect after BLE toggle on phone.

Android:
- Reconnect after disconnect.
- Retry behavior after transient connection failure.

Expected result:
- No stuck state requiring firmware reboot.

## Pass Criteria

Phase C can be marked integration-ready when all are true:

- All 10 commands return valid responses with correlation tracking.
- Chunked request and response paths pass without data corruption.
- Telemetry status events stream correctly.
- CSV export result is parseable and usable in app.
- No critical reconnect failures in basic iOS and Android smoke tests.
