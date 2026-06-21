# Mothership V1 Modem Upload Plan

**Date:** 2026-06-21
**Status:** Planning — awaiting implementation
**Scope:** LTE modem upload subsystem for Mothership V1 firmware: production modem driver, upload cursor/queue, Google Drive webhook HTTPS POST transport, flash storage purge logic, transmission settings, sync-wake integration, and web UI additions.

---

## 1. Context and Constraints

### 1.1 Hardware status

- **Modem hardware fully validated** (Tests 9–13, 2026-06-21): TPS63020 rail, PWRKEY, STATUS, UART2 AT path, SIM detection, network status commands, power cycling — all PASS.
- **Antenna not yet connected** (~2 days away). Network registration and actual HTTPS POST cannot be tested until then.
- **SD card broken on V1 PCB** (MOSI on wrong pin — GPIO23 routed to SD socket pin 8 instead of pin 3). LittleFS `/datalog.csv` is the authoritative local store for V1.
- **LittleFS partition**: 768 KB.

### 1.2 Current sync-wake flow (`main.cpp` → `handleSyncWake()`)

1. Assert `PWR_HOLD`
2. Load sync interval from NVS
3. Init RTC, SD (falls back to flash), ESP-NOW
4. Broadcast `SYNC_WINDOW_OPEN` every 5 s
5. Receive `NODE_SNAPSHOT` packets → `logSnapshotRow(snap)` to flash
6. Track deployed nodes for early shutdown
7. After sync window closes: re-arm RTC alarm, release `PWR_HOLD` (power off)

**Upload step does not exist yet.** It must be inserted between step 6 (sync window closed) and step 7 (re-arm alarm).

### 1.3 Existing reusable code

| Source | What it provides |
|--------|-----------------|
| `tests/modem_at_helper.h` | `modemRailOn()`, `modemRailOff()`, `modemPulsePwrkey()`, `modemReadStatus()`, `modemWaitBoot()`, `modemSendAT()`, `modemSendATRaw()`, `modemGracefulOff()`, `modemInitUart()` — inline header-only, to be promoted to a proper class |
| `src/storage/flash_logger.{h,cpp}` | `initFlash()`, `logSnapshotRow()`, `flashLogCSVRow()`, `flashGetCSVStats()`, `readCSVFile()`, `getCSVFileSize()`, `getCSVRecordCount()`, `flashIsReady()` |
| `src/system/power.{h,cpp}` | `readBatteryVoltage()`, `assertPwrHold()`, `releasePwrHold()`, `setLed()` |
| `src/system/pins.h` | All modem pins + timing constants |
| `src/config/config_server.cpp` | Web server routes, NVS patterns, dashboard HTML, `buildStatusJson()` |

### 1.4 Key design constraints

1. **No antenna yet** — code must compile and be testable now; network registration and HTTPS POST are deferred.
2. **Flash is primary store** — 768 KB LittleFS, `/datalog.csv` is authoritative.
3. **Power budget** — modem draws up to 2.8 A peak. Minimize modem-on time: power on → upload → power off within a single sync wake.
4. **No duplicates** — upload cursor tracks last successfully uploaded byte offset; only data after the cursor is sent.
5. **Flash purge** — after successful upload, purge uploaded data. If flash usage > 80%, emergency-purge oldest rows regardless of upload status.
6. **Mirror to SD later** — storage interface should abstract write target for future SD+flash mirroring.
7. **Google Drive webhook** — endpoint is a Google Apps Script web app URL accepting POST. Auth is a simple token in URL or header. Mothership just POSTs CSV data.
8. **A7670G HTTPS** — AT commands: `AT+HTTPINIT`, `AT+HTTPPARA`, `AT+HTTPACTION=1` (POST), `AT+HTTPREAD`. Exact command set for firmware `A110B06A7670M7` needs verification with antenna.

---

## 2. Architecture Overview

```
Sync Wake
  │
  ├── ESP-NOW sync window (existing)
  │     └── logSnapshotRow() → /datalog.csv (LittleFS)
  │
  ├── [NEW] Upload decision gate
  │     ├── Is upload enabled? (NVS tx.enabled)
  │     ├── Does upload policy say "upload this wake"? (counter / interval)
  │     └── Is battery >= minBatteryMv?
  │
  ├── [NEW] Modem upload sequence
  │     ├── modemDriver.powerOn() → rail → PG → PWRKEY → boot → UART ready
  │     ├── modemDriver.registerNetwork(timeout) → may fail (no antenna) → skip
  │     ├── uploadQueue.getNewData() → read /datalog.csv from cursor to EOF
  │     ├── modemDriver.httpsPost(url, payload, headers) → POST to webhook
  │     ├── On 200 OK: uploadQueue.advanceCursor() → persist new offset in NVS
  │     ├── uploadQueue.purgeUploaded() → rewrite /datalog.csv keeping only unuploaded rows
  │     ├── uploadQueue.emergencyPurgeIfFull() → if flash > 80%, drop oldest rows
  │     └── modemDriver.gracefulShutdown() → AT+CPOF → PWRKEY → rail off
  │
  └── Re-arm RTC alarm → release PWR_HOLD (existing)
```

---

## 3. Staged Implementation Plan

### Stage 1: Production Modem Driver (`src/comms/modem_driver.{h,cpp}`)

Promote `tests/modem_at_helper.h` inline functions into a proper `ModemDriver` class with state machine, timeouts, and recovery.

**Key methods:**
- `init()` — configure pins, UART not started
- `powerOn()` — rail → PG → PWRKEY → boot → UART ready
- `waitForNetwork(timeoutMs)` — AT+CREG / AT+CEREG, returns false on timeout (must NOT block forever)
- `httpsPost(url, payload, contentType, authToken)` — HTTPINIT → HTTPPARA → HTTPACTION → HTTPREAD
- `gracefulShutdown()` — AT+CPOF → PWRKEY fallback → rail off
- `forcePowerCycle()` — recovery: rail off → wait → rail on
- `getImei()`, `getSignalQuality()` — diagnostic queries

**State machine:**
```
OFF → RAIL_ENABLED → RAIL_STABLE → BOOT_REQUESTED → STATUS_HIGH →
UART_READY → REGISTERED → TRANSPORT_OPEN → UPLOAD_ACTIVE →
SHUTTING_DOWN → RAIL_DISABLED
```
Plus RECOVERY branch for modem hangs.

**HTTPS POST AT command sequence** (needs antenna to verify exact syntax for A110B06A7670M7):
```
AT+HTTPINIT                          → OK
AT+HTTPPARA="URL","<url>"             → OK
AT+HTTPPARA="CONTENT","text/csv"      → OK
AT+HTTPDATA=<len>,<timeout>           → wait for "DOWNLOAD" prompt → send payload
AT+HTTPACTION=1                       → +HTTPACTION: 1,<status>,<response_len>
AT+HTTPREAD                           → +HTTPREAD: <len> / <body>
AT+HTTPTERM                           → OK
```

**What can be tested now (no antenna):**
- `powerOn()` / `gracefulShutdown()` — already proven in Tests 9–13
- `sendAT()` — AT handshake works
- `getImei()`, `getSignalQuality()` — respond (CSQ=99 expected without antenna)
- `waitForNetwork()` — returns `false` after timeout (expected, CREG=0,2)

**What requires antenna:**
- `waitForNetwork()` returning `true`
- Full `httpsPost()` end-to-end
- `AT+HTTPDATA` / `AT+HTTPACTION` exact syntax verification

---

### Stage 2: Upload Cursor / Queue System (`src/storage/upload_queue.{h,cpp}`)

Track byte offset of last successfully uploaded data in `/datalog.csv`. Persist in NVS namespace `"tx"`.

**Key methods:**
- `init()` — load cursor from NVS, validate against file
- `getNewData(maxBytes)` — read from cursor to EOF (capped), prepend CSV header, return payload
- `advanceCursor(newOffset)` — persist new cursor after successful upload
- `purgeUploaded()` — rewrite `/datalog.csv` keeping only rows after cursor (streaming rewrite)
- `emergencyPurgeIfFull(thresholdPct)` — purge oldest rows if flash > threshold
- `shouldUploadThisWake(policyIntervalWakes)` — true every N wakes
- `getPendingBytes()`, `getPendingRows()` — status queries

**Cursor persistence** (NVS namespace `"tx"`):
| Key | Type | Default |
|-----|------|---------|
| `cursor_offset` | uint32 | 0 |
| `rows_uploaded` | uint32 | 0 |
| `last_upload_unix` | uint32 | 0 |
| `retry_count` | uint8 | 0 |
| `wake_counter` | uint32 | 0 |

**`getNewData(maxBytes)`:**
1. Open `/datalog.csv` for read
2. Seek to `m_cursor.byteOffset`
3. Read up to `maxBytes` bytes (or to EOF)
4. Ensure we don't split a row — read to the next `\n` boundary
5. Prepend the CSV header line so the webhook receives a complete CSV fragment
6. Return `UploadPayload` with data, start offset, byte length, row estimate

**`purgeUploaded()` — streaming rewrite:**
1. Open old file for read, open temp file for write
2. Seek old file to cursor, copy line by line to temp
3. Close both, remove old, rename temp
4. Reset cursor to end of header line
5. Save cursor to NVS
6. Memory-safe: no need to load whole file into RAM

**`emergencyPurgeIfFull(thresholdPct)`:**
1. Check `LittleFS.usedBytes() * 100 / LittleFS.totalBytes()`
2. If > threshold: purge oldest 50% of data rows (keep header + newest half)
3. Adjust cursor: if purged data was before cursor, cursor moves to new file start; if after cursor (unuploaded), those rows are lost — log warning

**`validateCursor()`:** on `init()`, if file size < cursor offset (file was purged or replaced), reset cursor to end of header line.

---

### Stage 3: Transmission Settings (`src/config/transmission_settings.{h,cpp}`)

NVS namespace `"tx"` (separate from `"ui"`). Loaded at boot, configurable via web UI.

**Settings struct:**
```cpp
struct TransmissionSettings {
  bool     enabled;
  String   endpointUrl;
  String   authToken;
  String   siteId;
  String   deploymentId;
  uint16_t uploadIntervalMin;     // 0 = every sync wake
  uint32_t uploadPhaseUnix;
  uint16_t minBatteryMv;           // default 3700
  uint32_t maxBytesPerSession;     // default 262144 (256 KB)
  uint8_t  maxRetriesPerWindow;    // default 3
  bool     allowManualUpload;      // default true
};
```

**NVS keys** (namespace `"tx"`):
| Key | Type | Default |
|-----|------|---------|
| `enabled` | bool | false |
| `url` | string | "" |
| `token` | string | "" |
| `site_id` | string | "" |
| `deploy_id` | string | "" |
| `upload_min` | uint16 | 0 |
| `phase_unix` | uint32 | 0 |
| `min_bat_mv` | uint16 | 3700 |
| `max_bytes` | uint32 | 262144 |
| `max_retries` | uint8 | 3 |
| `allow_manual` | bool | true |

**Note:** Use fixed-buffer NVS reads for strings (avoid `Preferences::getString()` dynamic allocation crash — see bring-up notes).

---

### Stage 4: Flash Purge Logic (within `upload_queue.cpp`)

**Purge strategy: Single-file streaming rewrite (recommended for V1)**

- `/datalog.csv` is the only file
- Purge = stream rows from cursor to EOF → write to temp file → swap
- Memory-safe: line-by-line streaming, no need to load whole file into RAM
- Flash wear acceptable: purges happen at most once per sync wake (typically every 18+ min)

**Emergency purge:**
- Trigger: flash usage > 80%
- Action: purge oldest 50% of data rows (keep header + newest half)
- If cursor is in purged range: reset cursor (those rows are lost — log warning)

---

### Stage 5: Integration into `handleSyncWake()` (`main.cpp`)

Insert upload block after sync window closes, before alarm re-arming:

```cpp
  // --- [NEW] LTE upload phase ---
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);

  if (txSettings.enabled) {
    uploadQueue.incrementWakeCounter();
    if (uploadQueue.shouldUploadThisWake(...)) {
      float batV = readBatteryVoltage();
      if ((uint16_t)(batV * 1000) >= txSettings.minBatteryMv) {
        if (!uploadQueue.maxRetriesExceeded(txSettings.maxRetriesPerWindow)) {
          if (uploadQueue.getPendingBytes() > 0) {
            performModemUpload(txSettings);
          }
        }
      }
    }
  }
```

**`performModemUpload()` flow:**
1. `modem.powerOn()` — if fails, increment retry, return
2. `modem.waitForNetwork(60000)` — if fails (no antenna), shutdown, increment retry, return
3. `uploadQueue.getNewData(maxBytes)` — if empty, shutdown, return
4. `modem.httpsPost(url, payload, "text/csv", token)` — POST to webhook
5. On 200 OK: `uploadQueue.advanceCursor()` + `uploadQueue.purgeUploaded()`
6. On failure: `uploadQueue.incrementRetryCount()`
7. `uploadQueue.emergencyPurgeIfFull(80)` — regardless of upload success
8. `modem.gracefulShutdown()`

**Critical safety rules:**
- Upload failure must NEVER block the sync wake from completing
- Local logging is always primary
- `emergencyPurgeIfFull(80)` should also run at START of sync wake (before ESP-NOW window) to ensure space for new data

---

### Stage 6: Web UI Additions (`config_server.cpp`)

**New routes:**

| Method | Route | Function |
|--------|-------|----------|
| GET | `/upload` | Transmission settings page (form + status) |
| POST | `/set-transmission` | Save transmission settings |
| POST | `/manual-upload` | Trigger manual upload (if enabled) |
| GET | `/upload-status` | JSON upload status |

**Dashboard additions:**
- "LTE Upload" button/section on main dashboard
- Upload status in `buildStatusJson()`:
  ```json
  "upload": {
    "enabled": true,
    "cursorOffset": 1024,
    "pendingBytes": 5120,
    "pendingRows": 17,
    "rowsUploaded": 340,
    "lastUploadUnix": 1777028400,
    "retryCount": 0,
    "flashUsagePct": 42
  }
  ```
- Upload status in data status section (cursor position, pending rows, last upload time)

**Transmission settings page (`/upload`):**
- Form: enabled, endpoint URL, auth token, site ID, deployment ID, upload interval, min battery, max bytes, max retries, allow manual
- Status display: cursor offset, pending bytes/rows, rows uploaded, last upload time, retry count, flash usage
- Manual upload button (if allowed)

---

### Stage 7: Testing Strategy

**New test environments:**

| Test | Env | Antenna? | What it validates |
|------|-----|----------|-------------------|
| Upload cursor | `mothership-v1-upload-cursor` | No | Cursor init/load/save, getNewData, advanceCursor, validateCursor, NVS persistence |
| Flash purge | `mothership-v1-flash-purge` | No | Fill flash >80%, emergency purge triggers, file rewritten, cursor adjusted, header preserved |
| Modem HTTPS | `mothership-v1-modem-https` | **Yes** | Full HTTPS POST to Google webhook, response parsing, end-to-end |
| Full firmware compile | `mothership-v1-main` | No | All new modules compile, handleSyncWake includes upload block |

**Validation checklist:**
1. `pio run -e mothership-v1-main` — full firmware compiles
2. `pio run -e mothership-v1-upload-cursor` — cursor test compiles
3. `pio run -e mothership-v1-flash-purge` — purge test compiles
4. Upload main firmware, enter config mode, verify web UI shows upload settings
5. Run cursor test on board — verify cursor logic
6. Run purge test on board — verify purge logic
7. (When antenna arrives) Run HTTPS test — verify end-to-end POST

---

## 4. File Summary

| File | Action | Stage |
|------|--------|-------|
| `src/comms/modem_driver.h` | New | 1 |
| `src/comms/modem_driver.cpp` | New | 1 |
| `src/storage/upload_queue.h` | New | 2 |
| `src/storage/upload_queue.cpp` | New | 2, 4 |
| `src/config/transmission_settings.h` | New | 3 |
| `src/config/transmission_settings.cpp` | New | 3 |
| `src/main.cpp` | Modify `handleSyncWake()` | 5 |
| `src/config/config_server.cpp` | Modify — routes, dashboard, status JSON | 6 |
| `tests/bringup_upload_cursor.cpp` | New | 7 |
| `tests/bringup_flash_purge.cpp` | New | 7 |
| `tests/bringup_modem_https.cpp` | New | 7 |
| `platformio.ini` | Modify — add test envs | 7 |

---

## 5. Ordered Implementation Stages

| Order | Stage | Designer? | Depends on |
|-------|-------|-----------|------------|
| 1 | Transmission settings | No | None |
| 2 | Upload cursor / queue | No | `flash_logger.h` |
| 3 | Production modem driver | No | `modem_at_helper.h` patterns |
| 4 | Flash purge logic | Confirm strategy A | Stage 2 |
| 5 | Sync-wake integration | No | Stages 1, 2, 3 |
| 6 | Web UI additions | No | Stages 2, 3 |
| 7 | Testing | No | Stages 1–6 |

---

## 6. Risks and Open Questions

### Design risks

1. **A7670G HTTPS AT command syntax** — exact `AT+HTTPDATA` / `AT+HTTPACTION` syntax for firmware `A110B06A7670M7` not yet verified. Mitigation: write to documented AT command set, verify with `bringup_modem_https` test when antenna arrives.

2. **Google Drive webhook format** — user must provide: webhook URL, auth method (URL param vs header), POST body format (raw CSV vs multipart vs JSON), expected success response.

3. **Flash rewrite memory** — use streaming approach (line-by-line) to avoid loading whole file into RAM.

4. **Modem-on time vs power budget** — modem draws up to 2.8 A. 60 s registration + 30 s upload = 90 s per wake. Mitigation: configurable timeout, upload every Nth wake.

5. **NVS string storage crash** — use fixed-buffer reads for all string settings.

6. **LittleFS wear** — frequent file rewrites cause flash wear. Mitigation: only purge when file is non-trivially large (> 10 KB uploaded).

### Open questions for user decision

1. **Purge strategy**: Confirm single-file streaming rewrite (Strategy A) vs rotating files (Strategy B). Recommendation: A for V1.

2. **Webhook POST body format**: Raw CSV (`Content-Type: text/csv`), multipart form, or JSON-wrapped CSV?

3. **Auth method**: Token in URL query param (`?token=xxx`) or `Authorization` header?

4. **Upload schedule**: Every sync wake, or less frequently? Default `uploadIntervalMin` = 360 (6 hours) = every ~20th sync wake at 18 min intervals.

5. **Emergency purge threshold**: 80% flash usage confirmed? Purge amount: 50% of data rows?

6. **Manual upload in config mode**: Allow powering on modem while WiFi AP is running? Both draw significant power.

---

## 7. Scope Coverage

| # | Scope item | Covered in |
|---|-----------|------------|
| 1 | Production modem driver | Stage 1 |
| 2 | Upload cursor/queue (no duplicates) | Stage 2 |
| 3 | Flash purge logic (post-upload + emergency) | Stage 4 |
| 4 | Transmission settings (NVS, web UI) | Stage 3 + 6 |
| 5 | Integration into handleSyncWake() | Stage 5 |
| 6 | Web UI additions | Stage 6 |
| 7 | Testing strategy (now vs antenna) | Stage 7 |

---