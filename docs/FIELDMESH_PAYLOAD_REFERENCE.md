# FieldMesh Upload Payload — Complete Reference

**What this is:** every field the mothership firmware sends to the Supabase ingest
function (`ingest-fieldmesh`), with human-readable descriptions, units, and where
each value comes from. Kept alongside the firmware so it stays current.

Source of truth in firmware:
- Readings + wrapper: `mothership/firmware/v2/src/storage/json_payload.cpp`
- Status context: `StatusContext` in `json_payload.h`, populated in
  `performModemUpload` (`main.cpp`) and `handleManualUpload` (`config_server.cpp`)
- `nodes[]`: `buildNodesStatusJson` (`config/node_registry.cpp`)
- `transmission{}`: `buildTransmissionStatusJson` (`config/transmission_settings.cpp`)
- `modem{}`: `ModemDriver::getDiagnostics` / `modemDiagnosticsToJson` (`comms/modem_driver.cpp`)
- `diagnostics{}`: built inline in the two upload paths; `resetReason`/`bootCount`
  captured in `setup()`

---

## Transport

- **Endpoint:** `POST https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh`
- **Auth:** header-only `Authorization: Bearer <device API key>`. The device,
  project, and mothership identity are all derived server-side from the key — they
  are **not** in the payload.
- **Body:** one JSON object per POST. Readings are chunked to **≤100 per POST**
  (≤~1 MB). The `status` object rides only the **first** POST of a session (it
  doesn't change between chunks).
- **Dedup key:** `(nodeId, datetime, seqNum)` — retries are idempotent.
- **`?dry_run=true`** validates a payload (returns 200 + `appended`) without writing.

```json
{
  "meta":     { ... },      // every POST
  "readings": [ ... ],      // the sensor data (<= 100 per POST)
  "status":   { ... }       // mothership telemetry — FIRST POST of the session only
}
```

---

## 1. `meta` — envelope / who & when

Identifies the firmware and the upload event. Feeds the `sync_sessions` table.

| Field | Type | Meaning |
|---|---|---|
| `firmwareVersion` | string | Firmware release version (`FW_VERSION`). |
| `firmwareBuild` | string | Compile timestamp (`__DATE__ __TIME__`) — the exact binary running. *(first POST)* |
| `uploadTimeUnix` | unix sec | Mothership RTC time at the moment of upload. *(first POST)* |
| `uploadReason` | string | `"scheduled"` (automatic sync wake) or `"manual"` (operator pressed Upload). *(first POST)* |

---

## 2. `readings[]` — the actual field data

One object per logged sample, parsed from the mothership's CSV log. Each row is a
snapshot a **node** took and relayed at a sync. Missing sensors are `null`.
Field keys are identical to the CSV column headers.

| Field | Type / Unit | Describes |
|---|---|---|
| `datetime` | ISO-8601 UTC (`…Z`) | When the node took the sample (node RTC). Falls back to mothership RTC if the node time was unknown (never `null` — the backend rejects that). |
| `nodeId` | string | System node identifier, MAC-derived (e.g. `ENV_6C0A80`). |
| `seqNum` | int | Node's own monotonic sample counter — detects gaps/duplicates. |
| `sensorPresent` | bitmask | Which sensors were fitted/active on the node for this sample. |
| `qualityFlags` | bitmask | Per-sample data-quality / error flags (e.g. sensor read failure). |
| `configVersion` | int | Node config version in effect when the sample was taken. |
| `batVoltage` | V | **Node** battery voltage. |
| `airTemp` | °C | Air temperature. |
| `airHumidity` | % RH | Relative humidity. |
| `spectral_415` … `spectral_680` | counts | 8-channel spectral/light sensor, one column per wavelength (415, 445, 480, 515, 555, 590, 630, 680 nm) — visible-spectrum light, for canopy/plant-light analysis. |
| `windSpeed` | m/s | Wind speed. |
| `windDir` | degrees | Wind direction. |
| `soil1Vwc` | m³/m³ | Soil volumetric water content, probe 1. |
| `soil1Temp` | °C | Soil temperature, probe 1. |
| `soil2Vwc` | m³/m³ | Soil moisture, probe 2. |
| `soil2Temp` | °C | Soil temperature, probe 2. |
| `aux1`, `aux2` | numeric | Spare/auxiliary channels for future sensors. |

---

## 3. `status` — mothership telemetry (first POST only)

A point-in-time snapshot of the coordinator's own health and configuration.
Feeds `mothership_status` (flattened), plus `nodes` and `mothership_config`.

### 3a. Scalar fields

| Field | Type / Unit | Describes |
|---|---|---|
| `batVoltage` | V | **Mothership** battery, sampled **at rest before the modem powers on** (true state of charge). |
| `rtcUnix` | unix sec | Mothership RTC clock. |
| `deviceId` | string | Mothership WiFi MAC — its unique identity. |
| `wakeIntervalMinutes` | min | How often nodes wake to **record** a sample (RTC Alarm 1). |
| `syncIntervalMinutes` | min | How often the fleet **syncs**/uploads (RTC Alarm 2). |
| `syncMode` | string | `"interval"` (every N min) or `"daily"` (fixed time). |
| `syncDailyTime` | `HH:MM` | Daily sync time, when in daily mode (else `""`). |
| `nextSyncLocal` | ISO local | When the next sync window is scheduled. |
| `projectStarted` | unix sec | First-ever boot timestamp — deployment age / "running since". |
| `lastUploadResult` | string | `"success"` / `"failed"` / `"pending"` — outcome of the last upload attempt. |

### 3b. `nodes[]` — the fleet registry (one per known node)

Populates the backend `nodes` table.

| Field | Type / Unit | Describes |
|---|---|---|
| `nodeId` | string | System identifier (MAC-derived). |
| `userId` | string | Human-assigned number (e.g. `"001"`). |
| `name` | string | Human-assigned name (e.g. `"North Meadow"`). |
| `mac` | string | Node MAC address. |
| `state` | string | `DEPLOYED` / `PAIRED` / `UNPAIRED` — lifecycle stage. |
| `lastSeenUnix` | unix sec | When the mothership last heard from the node. |
| `wakeIntervalMin` | min | Node's configured recording interval. |
| `inferredWakeIntervalMin` | min | Interval the mothership *observes* from arrivals (sanity check vs configured). |
| `lastReportedBatV` | V | Node's most recent battery voltage (`null` if unknown). |
| `configVersion` | int | Config version currently applied on the node. |
| `notes` | string | Free-text operator notes. |
| `isActive` | bool | Node currently considered live. |
| `deployPending` | bool | A deploy command is queued for next contact. |
| `stateChangePending` | bool | A state transition is queued. |
| `pendingTargetState` | string | Queued target state (`NONE`/`UNPAIRED`/`PAIRED`/`DEPLOYED`). |
| `latitude`, `longitude` | ° | Node GPS position (`null` if not set). |
| `deployedSinceUnix` | unix sec | When the node became `DEPLOYED`. |
| `recordingPaused` | bool | Standby: node is deployed + syncing but recording is paused (remote seasonal pause). `state` stays `DEPLOYED`. |

### 3c. `fleet{}` — fleet health at a glance

| Field | Describes |
|---|---|
| `total` / `deployed` / `paired` / `unpaired` | Node counts by state. |
| `pending` | Nodes with a queued state/deploy change. |
| `paused` | Deployed nodes currently in standby (recording paused). |

### 3d. `upload{}` — upload queue + flash usage

| Field | Type / Unit | Describes |
|---|---|---|
| `flashUsagePct` | % | LittleFS storage used. |
| `flashTotalBytes` / `flashUsedBytes` | bytes | Flash capacity / used. |
| `pendingBytes` / `pendingRows` | bytes / rows | Data still queued to upload. |
| `rowsUploaded` | rows | Rows uploaded so far (cursor). |
| `retryCount` | int | Consecutive upload retries. |
| `lastUploadUnix` | unix sec | Last successful upload time. |
| `enabled` | bool | Whether cloud upload is switched on. |

### 3e. `dataLog{}` — the CSV log

| Field | Type / Unit | Describes |
|---|---|---|
| `records` | rows | Rows in the log. |
| `csvSizeBytes` | bytes | Log file size. |
| `lastConfirmedSync` | string | Last confirmed sync marker (currently blank — reserved). |

### 3f. `transmission{}` — upload configuration (no secrets)

| Field | Describes |
|---|---|
| `endpointUrl` | Upload endpoint. |
| `siteId` / `deploymentId` | Operator-set deployment tags. |
| `uploadIntervalMin` | Upload cadence override (0 = every sync). |
| `minBatteryMv` | Battery cutoff below which upload is skipped. |
| `maxBytesPerSession` | Byte budget per upload session. |
| `maxRetriesPerWindow` | Retry cap per window. |
| `allowManualUpload` / `useJsonUpload` | Feature toggles. |

> **Secrets are deliberately excluded:** `apiKey`, `authToken`, `mothershipId`,
> `projectId` are never in the payload — the backend derives identity from the
> Bearer API key.

### 3g. `modem{}` — LTE link quality & SIM/modem identity

Queried live while registered, each session. Signal fields are `null` when
unmeasured (so "not measured" is distinguishable from a real reading).

| Field | Type / Unit | Describes |
|---|---|---|
| `imei` | string | Modem hardware identity. |
| `iccid` | string | SIM card identity. |
| `rssiDbm` | dBm | Overall received signal strength (`AT+CSQ`). |
| `ber` | class | Bit-error-rate class (`0` = best). |
| `rsrpDbm` | dBm | LTE reference signal power — cell strength (`AT+CESQ`). |
| `rsrqDb` | dB | LTE reference signal quality — interference (`AT+CESQ`). |
| `operator` | string | Registered carrier (e.g. `Telekom.de`) (`AT+COPS?`). |
| `accessTech` | string | `LTE` / `GSM` / `UTRAN` … |
| `cpsi` | string | Raw serving-cell line: band, cell ID, TAC, frequency (`AT+CPSI?`) — richest single diagnostic. |
| `regTimeMs` | ms | How long network registration took this session (`null` on manual uploads). |

### 3h. `diagnostics{}` — mothership system health

| Field | Type / Unit | Describes |
|---|---|---|
| `resetReason` | string | Why it last rebooted (see health guide below). |
| `bootCount` | int | Monotonic power-on counter (spot reboot loops). |
| `freeHeap` / `minFreeHeap` | bytes | Current / lowest-ever free RAM — memory-pressure warning. |
| `snapQueueDropped` | int | ESP-NOW snapshot-queue overflows this cycle. |
| `batLoadedV` | V | Mothership battery **under modem TX load**. `batVoltage − batLoadedV` = rail sag = battery/regulator health. |
| `sessionMs` | ms | Total duration of the modem upload session. |

---

## 4. Table destinations

| Payload part | Backend table |
|---|---|
| `readings[]` | `readings` |
| `status.nodes[]` | `nodes` |
| `meta` + `status` (flattened) | `mothership_status` |
| `meta` (`uploadTimeUnix`, `uploadReason`) | `sync_sessions` |
| `status.transmission` | `mothership_config` |

**Hard-required fields:** `nodes`→`nodeId`; `readings`→`nodeId` + `datetime`;
`sync_sessions`→`uploadTimeUnix` + `uploadReason`. Everything else is optional
(`NULL`/defaults applied server-side).

---

## 5. How to read the health signals

A quick operator guide to what "good" vs "bad" looks like in the telemetry.

### Radio / LTE (`status.modem`)

| Signal | Good | Marginal | Bad |
|---|---|---|---|
| `rssiDbm` | ≥ −85 | −86 … −100 | ≤ −101 |
| `rsrpDbm` (LTE cell strength) | ≥ −90 | −91 … −105 | ≤ −106 |
| `rsrqDb` (LTE quality) | ≥ −10 | −11 … −15 | ≤ −16 |
| `ber` | 0 | 1–3 | ≥ 4 |

- A **failed or slow upload** with a weak `rsrpDbm`/`rssiDbm` = coverage problem
  (antenna, siting), not a firmware bug. Check `cpsi` for the band/cell.
- `accessTech` dropping from `LTE` to `GSM` = the modem fell back to 2G — much
  slower, higher failure rate. Worth relocating the antenna.
- A large `regTimeMs` (tens of seconds) trending upward = the modem is struggling
  to attach — early sign of a marginal site or SIM/carrier issue.

### Power / battery

- `status.batVoltage` is the **resting** voltage — use it for state of charge.
- **Sag** = `batVoltage − diagnostics.batLoadedV`. Under LTE TX the rail always
  sags a little; a **growing** sag over weeks means the battery or regulator can't
  hold up under load — replace before it starts browning out.
- `diagnostics.resetReason = BROWNOUT` (especially repeating) confirms the supply
  is collapsing under load. `minBatteryMv` in `transmission` is the guard that
  skips uploads below a safe threshold.

### System stability (`status.diagnostics`)

| `resetReason` | Meaning |
|---|---|
| `POWERON` | Clean cold start (power applied). |
| `DEEPSLEEP` | Normal scheduled wake — expected between syncs. |
| `BROWNOUT` | Supply voltage collapsed — power/battery/regulator problem. |
| `PANIC` / `INT_WDT` / `TASK_WDT` | Firmware crash or hang — investigate. |
| `SW` / `EXT` | Deliberate software / external reset. |

- `bootCount` jumping by many between two uploads = a reboot loop (cross-check the
  `resetReason`).
- `minFreeHeap` creeping toward zero over a long session = memory pressure; the
  JSON builder needs headroom, so watch for it before uploads start failing.
- `snapQueueDropped > 0` means ESP-NOW snapshots arrived faster than they were
  drained during the sync window. **Data is not lost** (un-ACKed snapshots are
  resent next cycle), but a persistently rising count suggests widening the queue.

### Data pipeline

- `upload.pendingRows` / `pendingBytes` should trend to ~0 after each successful
  sync. A steadily growing backlog = uploads aren't keeping up (coverage, battery
  cutoff, or `enabled=false`).
- `lastUploadResult` + `upload.retryCount`: repeated `failed` with a rising
  `retryCount` points at a persistent transport problem — correlate with the
  `modem` signals for the cause.
