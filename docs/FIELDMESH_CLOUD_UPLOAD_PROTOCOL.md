# fieldMesh — Cloud Upload Protocol Spec

**Date:** 2026-06-26
**Status:** Confirmed spec (from frontend team)
**Purpose:** Specification for the mothership's cloud data upload, to be implemented by the firmware developer
**Audience:** Firmware developer
**Companion docs:** `docs/FIELDMESH_DASHBOARD_DESIGN.md`, `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md`, `mothership/firmware/v2/src/config/config_server.cpp`

---

## 1. Current State & Problem

### What happens now
The mothership POSTs **raw CSV text** to the Google Apps Script URL. Each row is a sensor snapshot:
```
2026-06-26T10:59:46,ENV_6C0AA0,1,0x0137,0x0000,1,4.12,28.05,48.88,1.0,3.0,...
```

The Apps Script splits by comma and appends rows to a Google Sheet.

### Problems
1. **CSV is heavy** — datetime strings, hex flags, repeated nodeId, no compression
2. **No system status** — fleet counts, upload queue, flash usage, battery, sync schedule, firmware version etc. are never sent to the cloud. They only exist in the mothership's memory and are visible via the Field UI at `192.168.4.1`
3. **No node metadata** — friendly names, MAC addresses, states (deployed/paired/unpaired), notes, wake intervals are never sent
4. **No config data** — transmission settings (endpoint URL, site ID, upload interval, min battery) are never sent
5. **Dashboard is blind** — the Cloud Dashboard can only see sensor readings, not system health

### Goal
Replace the CSV upload with a **structured JSON upload** that carries both sensor data AND system status in a single efficient payload. The dashboard gets full visibility into the field system.

---

## 2. Proposed Upload Format

### Endpoint
```
POST https://script.google.com/macros/s/AKfyc.../exec?action=uploadSync
Content-Type: application/json
```

### Payload structure

A single JSON object per sync cycle containing three sections:

```json
{
  "meta": { ... },       // Mothership identity + this upload's metadata
  "status": { ... },     // System status (fleet, upload queue, flash, config)
  "readings": [ ... ]    // Sensor data (batch of snapshots since last upload)
}
```

---

## 3. Detailed Schema

### 3.1 `meta` — upload metadata

```json
{
  "meta": {
    "deviceId": "001",
    "firmwareVersion": "v1.0.0",
    "firmwareBuild": "Jun 26 2026 10:30:00",
    "uploadTimeUnix": 1779872386,
    "uploadReason": "scheduled",
    "csvRowsIncluded": 42
  }
}
```

| Field | Type | Description |
|---|---|---|
| deviceId | string | Mothership device ID (e.g. "001") |
| firmwareVersion | string | Firmware version string (e.g. "v1.0.0") |
| firmwareBuild | string | Compile timestamp (`__DATE__ " " __TIME__`) |
| uploadTimeUnix | uint32 | Unix timestamp of this upload (from mothership RTC) |
| uploadReason | string | "scheduled" \| "manual" \| "config_change" |
| csvRowsIncluded | uint16 | Number of readings in the `readings` array |

### 3.2 `status` — system status (replaces separate status upload)

This is the data currently only available via `buildStatusJson()` in the Field UI. Send it with every upload so the dashboard always has current system state.

```json
{
  "status": {
    "rtcUnix": 1779872386,
    "wakeIntervalMinutes": 10,
    "syncIntervalMinutes": 180,
    "syncMode": "interval",
    "syncDailyTime": "06:00",
    "nextSyncLocal": "2026-06-26T11:15:00",

    "fleet": {
      "total": 3,
      "deployed": 2,
      "paired": 1,
      "unpaired": 0,
      "pending": 0,
      "pendingToPaired": 0,
      "pendingToUnpaired": 0
    },

    "upload": {
      "enabled": true,
      "pendingBytes": 89000,
      "pendingRows": 1240,
      "rowsUploaded": 5000,
      "lastUploadUnix": 1779870000,
      "retryCount": 0,
      "flashUsagePct": 28,
      "flashTotalBytes": 2097152,
      "flashUsedBytes": 587202
    },

    "transmission": {
      "endpointUrl": "https://script.google.com/macros/s/AKfyc.../exec",
      "siteId": "site-001",
      "deploymentId": "deploy-001",
      "uploadIntervalMin": 0,
      "minBatteryMv": 3500,
      "maxBytesPerSession": 50000,
      "maxRetriesPerWindow": 3,
      "allowManualUpload": true
    },

    "dataLog": {
      "records": 1240,
      "csvSizeBytes": 89000,
      "lastConfirmedSync": "2026-06-26T10:59:46"
    }
  }
}
```

### 3.3 `nodes` — per-node metadata (inside status)

Array of node records from the mothership's node registry. This is the data currently shown on the Field UI's Node Manager page.

```json
{
  "status": {
    ...
    "nodes": [
      {
        "nodeId": "ENV_6C0AA0",
        "userId": "001",
        "name": "Ridge Plot A",
        "mac": "24:6F:28:6C:0A:A0",
        "state": "DEPLOYED",
        "lastSeenUnix": 1779872386,
        "wakeIntervalMin": 10,
        "inferredWakeIntervalMin": 0,
        "lastReportedBatV": 4.05,
        "configVersion": 1,
        "notes": "North-facing slope",
        "isActive": true,
        "deployPending": false,
        "stateChangePending": false,
        "pendingTargetState": "NONE"
      }
    ]
  }
}
```

### 3.4 `readings` — sensor data (replaces CSV)

Array of snapshot objects. Each object is one sensor reading cycle from one node.

```json
{
  "readings": [
    {
      "datetime": "2026-06-26T10:59:46",
      "nodeId": "ENV_6C0AA0",
      "seqNum": 1,
      "sensorPresent": "0x0137",
      "qualityFlags": "0x0000",
      "configVersion": 1,
      "batVoltage": 4.12,
      "airTemp": 28.05,
      "airHumidity": 48.88,
      "spectral_415": 1.0,
      "spectral_445": 3.0,
      "spectral_480": 2.0,
      "spectral_515": 4.0,
      "spectral_555": 9.0,
      "spectral_590": 9.0,
      "spectral_630": 6.0,
      "spectral_680": 10.0,
      "windSpeed": 3.5,
      "windDir": 180.0,
      "soil1Vwc": 0.12,
      "soil1Temp": 27.78,
      "soil2Vwc": 0.08,
      "soil2Temp": 27.92,
      "aux1": null,
      "aux2": null
    }
  ]
}
```

**Notes on readings format:**
- `datetime` can be either ISO 8601 string OR Unix timestamp (uint32). ISO string is preferred for readability. If you send Unix timestamp, use field `datetimeUnix` instead and the backend will convert it.
- `null` values for sensors that aren't present (instead of `"nan"` or empty strings)
- Only include fields that the node actually measured — missing fields are treated as "sensor not present"
- Hex fields (`sensorPresent`, `qualityFlags`) can be strings like `"0x0137"` or integers like `311`

---

## 4. Full Example Payload

```json
{
  "meta": {
    "deviceId": "001",
    "firmwareVersion": "v1.0.0",
    "firmwareBuild": "Jun 26 2026 10:30:00",
    "uploadTimeUnix": 1779872386,
    "uploadReason": "scheduled",
    "csvRowsIncluded": 2
  },
  "status": {
    "rtcUnix": 1779872386,
    "wakeIntervalMinutes": 10,
    "syncIntervalMinutes": 180,
    "syncMode": "interval",
    "syncDailyTime": "06:00",
    "nextSyncLocal": "2026-06-26T11:15:00",
    "fleet": {
      "total": 2,
      "deployed": 2,
      "paired": 0,
      "unpaired": 0,
      "pending": 0,
      "pendingToPaired": 0,
      "pendingToUnpaired": 0
    },
    "upload": {
      "enabled": true,
      "pendingBytes": 0,
      "pendingRows": 0,
      "rowsUploaded": 5002,
      "lastUploadUnix": 1779872386,
      "retryCount": 0,
      "flashUsagePct": 28,
      "flashTotalBytes": 2097152,
      "flashUsedBytes": 587202
    },
    "transmission": {
      "endpointUrl": "https://script.google.com/macros/s/AKfyc.../exec",
      "siteId": "site-001",
      "deploymentId": "deploy-001",
      "uploadIntervalMin": 0,
      "minBatteryMv": 3500,
      "maxBytesPerSession": 50000,
      "maxRetriesPerWindow": 3,
      "allowManualUpload": true
    },
    "dataLog": {
      "records": 5002,
      "csvSizeBytes": 358000,
      "lastConfirmedSync": "2026-06-26T10:59:46"
    },
    "nodes": [
      {
        "nodeId": "ENV_6C0AA0",
        "userId": "001",
        "name": "Ridge Plot A",
        "mac": "24:6F:28:6C:0A:A0",
        "state": "DEPLOYED",
        "lastSeenUnix": 1779872386,
        "wakeIntervalMin": 10,
        "inferredWakeIntervalMin": 0,
        "lastReportedBatV": 4.05,
        "configVersion": 1,
        "notes": "North-facing slope",
        "isActive": true,
        "deployPending": false,
        "stateChangePending": false,
        "pendingTargetState": "NONE"
      }
    ]
  },
  "readings": [
    {
      "datetime": "2026-06-26T10:59:46",
      "nodeId": "ENV_6C0AA0",
      "seqNum": 501,
      "sensorPresent": "0x0137",
      "qualityFlags": "0x0000",
      "configVersion": 1,
      "batVoltage": 4.05,
      "airTemp": 22.15,
      "airHumidity": 55.30,
      "spectral_415": 1.0,
      "spectral_445": 3.0,
      "spectral_480": 2.0,
      "spectral_515": 4.0,
      "spectral_555": 9.0,
      "spectral_590": 9.0,
      "spectral_630": 6.0,
      "spectral_680": 10.0,
      "windSpeed": 3.2,
      "windDir": 185,
      "soil1Vwc": 0.28,
      "soil1Temp": 20.10,
      "soil2Vwc": 0.31,
      "soil2Temp": 19.85,
      "aux1": null,
      "aux2": null
    }
  ]
}
```

---

## 5. Backend Handling

The backend (Google Apps Script or Supabase Edge Function) will handle this payload:

```
POST /exec?action=uploadSync
```

The backend will:
1. Parse the JSON
2. Write `status.fleet`, `status.upload`, `status.transmission`, `status.dataLog` to the **Status** sheet/table (key/value, overwriting previous)
3. Upsert each entry in `status.nodes` into the **Nodes** sheet/table (update if nodeId exists, append if new)
4. Write `status` config values to the **Config** sheet/table
5. Append each reading in `readings` to the **Data** sheet/table
6. Return `{ "success": true, "appended": N, "totalRows": M }`

---

## 6. Backward Compatibility

The backend will continue to accept the old raw CSV POST (no `action` param) so the mothership doesn't break if the firmware update is rolled out gradually. The detection logic:

- `action=uploadSync` → new JSON format (status + readings)
- `action=uploadData` → JSON readings only (no status)
- No action + CSV content → legacy CSV append (current behaviour)

---

## 7. Size Considerations

| Payload type | Typical size | Notes |
|---|---|---|
| Status only (no readings) | ~1.5 KB | Send on config changes or manual sync |
| 10 readings | ~3 KB | Typical sync cycle (10 min wake, 18× sync = 18 readings) |
| 50 readings | ~12 KB | Large batch after connectivity gap |
| 100 readings | ~24 KB | Well within LTE Cat-1 and Apps Script limits |

JSON is slightly larger than CSV per reading due to key names, but:
- The `status` section is sent once per upload (not per row)
- The status data eliminates the need for a separate status endpoint

**Optimization option:** If size is a concern, the readings array can use a compact format:
```json
{
  "readingsCompact": {
    "columns": ["datetime","nodeId","seqNum","batVoltage","airTemp","airHumidity"],
    "rows": [
      ["2026-06-26T10:59:46","ENV_6C0AA0",501,4.05,22.15,55.30],
      ["2026-06-26T10:59:40","ENV_6C0BB1",501,3.92,19.20,68.50]
    ]
  }
}
```
This reduces size by ~40% by not repeating key names. The backend handles both formats.

---

## 8. Firmware Data Sources

All data is already available in the firmware:

| JSON section | Firmware source |
|---|---|
| `meta` | `DEVICE_ID`, `FW_VERSION`, `FW_BUILD`, `getRTCTimeUnix()` |
| `status.rtcUnix` | `getRTCTimeUnix()` |
| `status.wakeIntervalMinutes` | `gWakeIntervalMin` |
| `status.syncIntervalMinutes` | `gSyncIntervalMin` |
| `status.syncMode` | `gSyncMode` (SYNC_MODE_INTERVAL / SYNC_MODE_DAILY) |
| `status.syncDailyTime` | `formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute)` |
| `status.nextSyncLocal` | `computeNextSyncIsoLocal()` |
| `status.fleet.*` | `getRegisteredNodes()`, `getPairedNodes()`, `getUnpairedNodes()` counts |
| `status.upload.*` | `loadTransmissionSettings(tx)`, `gUploadQueue.getCursor()`, `LittleFS.totalBytes()/usedBytes()` |
| `status.transmission.*` | `loadTransmissionSettings(tx)` |
| `status.dataLog.*` | Flash logger file size + record count |
| `status.nodes[]` | `getRegisteredNodes()` → each node's fields |
| `readings[]` | Parse the queued CSV rows from flash into JSON objects |

---

## 9. Open Questions

1. **JSON library** — does the mothership firmware already have a JSON serializer (ArduinoJson etc.), or should the payload be built with string concatenation? ArduinoJson is recommended if not already included.

2. **Upload trigger** — should the status section be sent on every sync upload, or only when something has changed? Recommendation: send on every upload (it's small, ~1.5 KB, and ensures the dashboard is always current).

3. **Compact readings format** — is the compact array format (Section 7) worth implementing, or is the full JSON object format fine? Recommendation: start with full JSON, optimize later if needed.

4. **Partial uploads** — if the upload queue is large (e.g. 500 rows after a long connectivity gap), should we chunk the upload into multiple POSTs? Recommendation: yes, cap at 100 readings per POST to stay within Apps Script's 50 MB request limit and avoid timeouts.

5. **Error response** — the backend returns `{ "success": true, "appended": N }`. Should the firmware check this response before clearing the queue? (It already does this for CSV uploads.)