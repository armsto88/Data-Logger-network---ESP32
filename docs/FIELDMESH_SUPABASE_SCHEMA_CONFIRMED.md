# Supabase Ingest Schema — Confirmed

**Date:** 2026-06-30
**Status:** Confirmed by backend team

## Readings

**Batch:** YES — POST `{"readings": [...]}` with up to 100 readings per request.

**Field names (camelCase):**

| Field | Type | Required | Notes |
|-------|------|----------|-------|
| `nodeId` | string | Yes | Max 120 chars |
| `datetime` | string | Yes | ISO-8601 UTC: `2026-06-30T12:34:56Z` |
| `datetimeUnix` | number | Alt | Unix epoch seconds (alternative to `datetime`) |
| `seqNum` | integer | No | Defaults to 0 |
| `sensorPresent` | integer | No | Bitmask, defaults to 0 |
| `qualityFlags` | integer | No | Bitmask, defaults to 0 |
| `configVersion` | integer | No | |
| `batVoltage` | float | No | Volts |
| `airTemp` | float | No | null if absent |
| `airHumidity` | float | No | null if absent |
| `spectral_415`…`spectral_680` | float | No | Underscore format, null if absent |
| `windSpeed` | float | No | null if absent |
| `windDir` | float | No | null if absent |
| `soil1Vwc` | float | No | null if absent |
| `soil1Temp` | float | No | null if absent |
| `soil2Vwc` | float | No | null if absent |
| `soil2Temp` | float | No | null if absent |
| `aux1`, `aux2` | float | No | null if absent |

**`mothership_id` and `project_id`:** NOT sent by firmware — derived server-side from the device API key.

## Status Object

Include `status` in every POST. Server stores it to `mothership_status` automatically.

```json
"status": {
  "batVoltage": 3.80,
  "flashUsagePct": 5,
  "flashTotalBytes": 4194304,
  "flashUsedBytes": 209715,
  "uploadReason": "scheduled",
  "nextSyncLocal": "2026-06-30T01:30:00",
  "wakeIntervalMinutes": 5,
  "syncIntervalMinutes": 60,
  "syncMode": "interval",
  "fleetTotal": 3,
  "fleetDeployed": 3,
  "fleetPaired": 3,
  "fleetUnpaired": 0,
  "pendingRows": 0,
  "rowsUploaded": 3015,
  "retryCount": 0,
  "lastUploadUnix": 1751280000,
  "firmwareVersion": "1.2.3",
  "firmwareBuild": "2026-06-29T10:00:00Z",
  "rtcUnix": 1751280000,
  "deviceId": "48:9D:31:F8:16:A8"
}
```

Note: `fleetActive` → use `fleetDeployed`; `fleetConnected` → use `fleetPaired`.

## Meta Object

```json
"meta": {
  "deviceId": "48:9D:31:F8:16:A8",
  "firmwareVersion": "1.2.3",
  "firmwareBuild": "2026-06-29T10:00:00Z",
  "uploadReason": "scheduled"
}
```

## sync_sessions

Auto-created server-side — firmware does NOT post session metadata. One row per POST.

## Device Identity (Confirmed)

- `mothership_id`: `f68a7546-6727-42a0-948f-5eae5f521f66`
- `project_id`: `65a72fab-6014-4159-9198-575d695db66a`
- API key: (issued by backend team — enter via Settings page or QR string, do NOT commit to source)
- MAC: `48:9D:31:F8:16:A8`

## Verification

Use service role key to query readings (ask backend team for the key — do NOT commit it):
```
curl "https://unhzttnuayrgqrzeqetz.supabase.co/rest/v1/readings?mothership_id=eq.f68a7546-6727-42a0-948f-5eae5f521f66&select=node_id,recorded_at&order=recorded_at.desc&limit=5" \
  -H "apikey: <SERVICE_ROLE_KEY>" \
  -H "Authorization: Bearer <SERVICE_ROLE_KEY>"
```

## Complete POST Example

```json
{
  "readings": [
    {
      "nodeId": "ENV_6C0AA0",
      "datetime": "2026-06-30T12:34:56Z",
      "seqNum": 1,
      "sensorPresent": 311,
      "qualityFlags": 0,
      "configVersion": 1,
      "batVoltage": 3.79,
      "airTemp": 30.39,
      ...
    }
  ],
  "meta": {
    "deviceId": "48:9D:31:F8:16:A8",
    "firmwareVersion": "1.2.3",
    "firmwareBuild": "2026-06-29T10:00:00Z",
    "uploadReason": "scheduled"
  },
  "status": {
    "batVoltage": 3.80,
    "flashUsagePct": 5,
    "wakeIntervalMinutes": 5,
    "syncIntervalMinutes": 60,
    "syncMode": "interval",
    "fleetTotal": 3,
    "fleetDeployed": 3,
    "fleetPaired": 3,
    "pendingRows": 0,
    "rowsUploaded": 3015,
    "rtcUnix": 1751280000,
    "deviceId": "48:9D:31:F8:16:A8"
  }
}
```

Expected response: `200 {"success":true,"appended":1,"duplicate":0}`