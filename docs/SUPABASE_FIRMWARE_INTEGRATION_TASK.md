# Task: Adapt the mothership JSON upload payload to the Supabase backend format

## Context

This is an ESP32-based sensor network ("fieldMesh"). The mothership collects data from nodes via ESP-NOW, logs it to LittleFS as CSV, and uploads it via an A7670G LTE modem using HTTPS POST. The upload path already supports JSON (`useJsonUpload = true` by default), but the current JSON payload format (`{meta, status, readings: [...]}`) was designed for a Google Apps Script backend. The backend is migrating to Supabase, which expects a different JSON format.

## The Supabase backend spec

**Endpoint:** `https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh`

**Auth:** `Authorization: Bearer <device-api-key>` (already implemented — the firmware sends `txSettings.apiKey` as the auth header)

**Content-Type:** `application/json` (already implemented)

**Expected payload format — flat per-reading objects in a JSON array:**

```json
[
  {
    "mothership_id": "d7eaae7b-011e-4f57-a489-0fbaefb05ff0",
    "node_id": "ENV_6C0AA0",
    "seq_num": 1,
    "timestamp": "2026-06-29T12:34:56Z",
    "sensor_present": 311,
    "quality_flags": 0,
    "config_version": 1,
    "battery_voltage": 3.794,
    "air_temp": 30.394,
    "air_humidity": 42.322,
    "spectral_415": 0,
    "spectral_445": 1,
    "spectral_480": 1,
    "spectral_515": 2,
    "spectral_555": 2,
    "spectral_590": 2,
    "spectral_630": 2,
    "spectral_680": 3,
    "wind_speed": null,
    "wind_dir": null,
    "soil1_vwc": 3.96,
    "soil1_temp": 30.896,
    "soil2_vwc": 0.28,
    "soil2_temp": 30.368,
    "aux1": null,
    "aux2": null
  }
]
```

**Required fields:** `mothership_id`, `node_id`, `timestamp`, `battery_voltage`, `sensor_present`, `quality_flags`

**Optional fields:** all sensor readings can be `null`

**Timestamp format:** ISO 8601 UTC (`YYYY-MM-DDTHH:mm:ssZ`) — NOT Unix epoch

**Response codes:**

| Code | Meaning | Action |
|------|---------|--------|
| 200 | Success | Data stored |
| 401 | Unauthorized | Bad API key — log immediately, don't retry |
| 400 | Bad Request | Malformed payload — log, don't retry |
| 429 | Rate Limited | Retry with backoff |
| 5xx | Server Error | Retry with backoff |

**Retry strategy:** 2s → 3s → 4.5s → 6.75s → 10s (1.5x backoff, max 5 retries). Only retry on 429 and 5xx.

**Dry-run testing:** append `?dry_run=true` to the endpoint URL for validation without storing.

## Current firmware state

### What already works (don't change these)

1. **Modem HTTPS path** — `modem.httpsPost(url, body, contentType, authHeader)` handles SSL/TLS via A7670G CCH* API. Already sends `Authorization: Bearer <apiKey>`.
2. **Upload queue** — `UploadQueue` reads CSV from LittleFS, tracks cursor, chunks data, handles retries. The chunking and cursor-advance logic is correct and should be preserved.
3. **Settings storage** — `TransmissionSettings` struct with `endpointUrl`, `apiKey`, `useJsonUpload`, etc. stored in NVS namespace `"tx"`.
4. **Config UI** — Settings page already has fields for API key, endpoint URL, QR string. The QR string parser (`url|key`) already works.
   > **Superseded (historical note).** The `url|key` parser and the `url` field
   > have since been **removed**: both wrote the FieldMesh endpoint from request
   > data, bypassing the allow-list, so a direct POST could redirect a hub's
   > uploads and leak its device key to an unapproved host. Provisioning now goes
   > through a validated `FM1` payload at `/provision-apply`, which is the only
   > endpoint writer. Do not restore either field.
5. **Manual upload handler** — `handleManualUpload()` in `config_server.cpp` already does a blocking upload via the modem.

### What needs to change

#### 1. `json_payload.cpp` — rewrite the JSON builder

Current format (nested, for Google Apps Script):

```json
{
  "meta": { "deviceId": "001", "firmwareVersion": "v1.0.0", ... },
  "status": { "fleet": { ... }, "upload": { ... }, ... },
  "readings": [
    { "nodeId": "001", "nodeUnix": 1777028505, "batV": 3.85, ... }
  ]
}
```

Target format (flat array, for Supabase):

```json
[
  {
    "mothership_id": "d7eaae7b-011e-4f57-a489-0fbaefb05ff0",
    "node_id": "ENV_6C0AA0",
    "seq_num": 1,
    "timestamp": "2026-06-29T12:34:56Z",
    "sensor_present": 311,
    "quality_flags": 0,
    "config_version": 1,
    "battery_voltage": 3.794,
    "air_temp": 30.394,
    ...
  }
]
```

Key differences:

- **No `meta` or `status` wrapper** — just a flat JSON array of reading objects
- **`mothership_id`** — UUID string, needs to be added. Store it in `TransmissionSettings` (new field `mothershipId`) or hardcode for now. The backend issued: `d7eaae7b-011e-4f57-a489-0fbaefb05ff0`
- **`timestamp`** — ISO 8601 UTC string, not Unix epoch. Convert from the CSV's `node_unix` column.
- **Field names** — match the Supabase spec exactly (snake_case): `battery_voltage` not `batV`, `air_temp` not `airTemp`, etc.
- **`null` for missing sensors** — use JSON `null`, not string `"NaN"` or omitted fields

The CSV columns (from `upload_queue.h`):

```
datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,
batVoltage,airTemp,airHumidity,
spectral_415,spectral_445,spectral_480,spectral_515,
spectral_555,spectral_590,spectral_630,spectral_680,
windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2
```

The existing `appendReadingObject()` function in `json_payload.cpp` already parses these CSV columns. It needs to emit the Supabase field names and format instead.

#### 2. `transmission_settings.h` / `.cpp` — add `mothershipId` field

Add a `String mothershipId` field to `TransmissionSettings`. Default to `"d7eaae7b-011e-4f57-a489-0fbaefb05ff0"` for now. Store in NVS key `"mothership_id"`. This is the UUID the backend uses to route data to the correct project.

#### 3. `buildUploadUrl()` in `transmission_settings.cpp` — stop appending query params for Supabase

The current `buildUploadUrl()` appends `?token=xxx&siteId=yyy&deploymentId=zzz`. The Supabase endpoint doesn't use query params — auth is header-only. When the endpoint URL contains `supabase.co`, skip the query param appending. Or better: only append query params if `authToken` is non-empty AND `apiKey` is empty (the legacy Google Apps Script path). When `apiKey` is set (Supabase path), don't append query params.

#### 4. `transmission_settings.h` — update default endpoint URL

Change `DEFAULT_ENDPOINT_URL` from the Google Apps Script URL to:

```cpp
static constexpr const char* DEFAULT_ENDPOINT_URL =
    "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh";
```

#### 5. `main.cpp` `performModemUpload()` — update response handling

Currently checks `result.httpStatus == 200` for success. Update to:

- 200 = success (advance cursor, purge, reset retries)
- 401 = log "credential issue", don't retry, don't advance cursor
- 400 = log "bad payload", don't retry, don't advance cursor
- 429 = retry with backoff
- 5xx = retry with backoff

The existing retry logic (`incrementRetryCount`, `maxRetriesExceeded`) can be reused — just make sure 401/400 don't increment the retry counter (they're not transient).

#### 6. `config_server.cpp` `handleManualUpload()` — same response handling update

The manual upload handler also checks `result.httpStatus == 200`. Apply the same response code logic.

#### 7. Status-only uploads

The current code sends a `{meta, status, readings: []}` document when there's no new data (status-only push). For Supabase, an empty array `[]` is the correct payload when there are no readings. Consider whether status-only uploads are still needed — the Supabase backend may not have a use for them. If not, skip the status-only path when `useJsonUpload` is true and there are no pending rows.

## Key files

- `mothership/firmware/v2/src/storage/json_payload.cpp` — JSON builder (main change)
- `mothership/firmware/v2/src/storage/json_payload.h` — JsonPayload struct, JsonUploadContext
- `mothership/firmware/v2/src/config/transmission_settings.h` — TransmissionSettings struct
- `mothership/firmware/v2/src/config/transmission_settings.cpp` — `buildUploadUrl()`, defaults, NVS load/save
- `mothership/firmware/v2/src/main.cpp` — `performModemUpload()` (~line 197), response handling
- `mothership/firmware/v2/src/config/config_server.cpp` — `handleManualUpload()` (~line 2570), Settings page UI
- `mothership/firmware/v2/src/storage/upload_queue.h` — CSV header definition (field order reference)
- `docs/FIELDMESH_SUPABASE_FIRMWARE_INTEGRATION.md` — full backend spec (just created)

## Constraints

1. **Preserve the CSV parsing and chunking logic** in `json_payload.cpp` — only change the JSON output format, not how CSV rows are read and consumed.
2. **Preserve the upload queue cursor logic** — `csvBytesConsumed` must still be accurate so the cursor advances correctly.
3. **Keep the CSV fallback path** — if JSON build fails (heap/parse), fall back to CSV POST. But update the CSV fallback to also not append query params when using the Supabase endpoint.
4. **Don't break the config UI** — the Settings page should still work for entering the API key and endpoint. The QR string parser (`url|key`) should still work.
   > **Superseded (historical note).** This constraint no longer holds and must
   > not be honoured: the `url|key` parser and the endpoint field were removed
   > precisely because they bypassed the endpoint allow-list. Keeping them
   > working was the bug. See the note in section 4 above.
5. **Build target:** `pio run -e mothership-v1-main` (PlatformIO, ESP32 Arduino)
6. **The mothership MAC is `48:9D:31:F8:16:A8`** — the backend team needs this to issue a device API key. Include it in the response.
7. **Keep changes minimal** — don't refactor the upload queue or modem driver. Only change the JSON format, URL building, and response handling.

## Validation

1. `pio run -e mothership-v1-main` must pass
2. Serial log should show the JSON payload being built and POSTed
3. A dry-run test against `?dry_run=true` should return 200
4. A real upload should return 200 and the cursor should advance

## Open questions for the backend team

- **Device API key**: needs to be issued for this mothership (MAC `48:9D:31:F8:16:A8`). The firmware stores it in NVS via the Settings page or QR string.
- **Batch vs single**: the Supabase spec shows a single reading object. Can the endpoint accept a JSON array of multiple readings in one POST? The firmware chunks data (up to 96KB per session) and needs to send multiple readings per POST for efficiency. If the endpoint only accepts one reading per POST, the firmware needs to loop and POST each reading separately (much slower over LTE). **Assumption: the endpoint accepts a JSON array of readings.** Confirm with the backend team.
- **`mothership_id`**: is this a static UUID per device, or does it need to be configurable per deployment? For now, hardcode `d7eaae7b-011e-4f57-a489-0fbaefb05ff0` but make it a `TransmissionSettings` field so it can be changed via config UI later.