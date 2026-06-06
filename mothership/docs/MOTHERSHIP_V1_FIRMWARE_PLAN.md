# Mothership V1 Firmware — Wake-Gated Architecture Plan

**Date:** 2026-06-05
**Status:** Planning — no code written yet
**Scope:** Transform the mothership from always-on to a power-gated, RTC-scheduled hub with config-button wake and optional LTE backhaul

---

## 1. Current State

The mothership firmware (`mothership/firmware/src/`) currently runs **24/7 with no sleep**:

- WiFi AP is always up
- ESP-NOW listener is always active
- Web UI is always served
- SD card is always mounted
- No `PWR_HOLD` or power-gating logic exists
- No DS3231 alarm programming exists
- No config-latch sensing exists
- No modem (A7670G) driver exists

The hardware **already supports** all the power-gating features (PCB is designed for it), but the firmware doesn't use them yet.

---

## 2. Target Behaviour

### 2.1 Three Wake Modes

| Wake Source | Trigger | Behaviour | AP? | ESP-NOW? | Modem? |
|---|---|---|---|---|---|
| **Scheduled sync** | DS3231 Alarm 1 | Wake → receive node data → log to SD → optional LTE upload → re-arm alarm → power down | No | Yes (brief) | Conditional |
| **Config / UI** | Config button (CONFIG_SET_N) | Wake → start AP → serve web UI → user interacts → timeout or manual shutdown → power down | Yes | Yes | No |
| **Service / USB** | SW10 + VBUS_USB | Wake → full runtime for flashing / diagnostics → stays on while USB connected | Optional | Optional | Optional |

### 2.2 Scheduled Sync Wake Cycle

```
RTC Alarm fires → LOGIC goes high → P-FET enables VSYS → ESP32 boots
  → assert PWR_HOLD immediately
  → read CONFIG_WAKE_PIN (LOW = config wake, HIGH = normal wake)
  → if normal wake:
      → init I2C, read RTC time
      → init ESP-NOW (no AP)
      → broadcast SYNC_WINDOW_OPEN
      → receive NODE_SNAPSHOT packets from waking nodes
      → drain snapshot queue to SD card
      → [optional] enable 4V rail, power on modem, upload queued data to webhook
      → [optional] shut down modem
      → re-arm DS3231 Alarm 1 for next sync window
      → release PWR_HOLD → board powers off
  → if config wake:
      → see 2.3 below
```

### 2.3 Config / UI Wake Cycle

```
Config button pressed → CONFIG_SET_N goes low → latch sets → LOGIC goes high → board powers on
  → assert PWR_HOLD immediately
  → read CONFIG_WAKE_PIN (LOW = config wake)
  → clear config latch (CONFIG_CLEAR_PIN HIGH briefly)
  → init I2C, RTC, SD card
  → start WiFi AP + web server + captive portal
  → init ESP-NOW (for node discovery/config while AP is up)
  → user interacts with web UI
  → on timeout (e.g. 10 min idle) or user "Shut Down" button:
      → re-arm DS3231 Alarm 1 for next scheduled sync
      → release PWR_HOLD → board powers off
```

### 2.4 Service / USB Wake

```
USB plugged in → VBUS_USB forces LOGIC high → board powers on
  → assert PWR_HOLD
  → full runtime: serial console, flashing, diagnostics
  → board stays on while USB present
  → when USB removed: normal shutdown sequence
```

---

## 3. Required Firmware Changes

### 3.1 New: Power-Gating Module (`system/power.h`)

| Function | Purpose |
|---|---|
| `assertPwrHold()` | Drive `PWR_HOLD` (GPIO26) HIGH immediately at boot |
| `releasePwrHold()` | Drive `PWR_HOLD` LOW, board powers off |
| `readConfigWake()` | Read `CONFIG_WAKE_PIN` (GPIO32) — LOW = config wake requested |
| `clearConfigLatch()` | Pulse `CONFIG_CLEAR_PIN` (GPIO25) HIGH for 20ms to clear latch |
| `schedulePowerDown(int delayMs)` | Set a timed power-down after completing work |
| `isUsbPowered()` | Check if `VBUS_USB` is present (service mode) |

### 3.2 New: RTC Alarm Module (`time/rtc_alarm.h`)

| Function | Purpose |
|---|---|
| `armNextSyncAlarm(int intervalMin)` | Program DS3231 Alarm 1 for next sync window |
| `armDailyAlarm(int hour, int minute)` | Program DS3231 Alarm 1 for daily sync time |
| `clearAlarmFlag()` | Clear DS3231 Alarm 1 flag after wake |
| `readAlarmTime()` | Read current alarm setting for verification |

**Key difference from node:** The mothership uses **Alarm 1 only** (no Alarm 2). The node uses Alarm 1 for sampling and Alarm 2 for sync. The mothership only wakes for sync events.

### 3.3 New: Wake Reason Detection (`system/wake_reason.h`)

| Enum | Meaning |
|---|---|
| `WAKE_RTC_ALARM` | DS3231 Alarm 1 fired — scheduled sync |
| `WAKE_CONFIG_BUTTON` | Config latch detected — user wants AP/UI |
| `WAKE_USB_SERVICE` | SW10 + USB — service/debug mode (FORCE_POWER removed) |
| `WAKE_UNKNOWN` | Can't determine — default to sync mode |

Detection logic:
1. Read `CONFIG_WAKE_PIN` — if LOW, config wake
2. Check DS3231 alarm flag — if set, RTC wake
3. Check `VBUS_USB` — if present, service/debug mode (SW10 must also be in RUN position)
4. Otherwise, unknown → default to sync mode

> **Note:** FORCE_POWER (SW11) has been removed from the design. The service/debug use case is covered by SW10 + USB, which holds LOGIC high via VBUS_USB. The config button provides the momentary wake trigger for user interaction.

### 3.4 Modified: Boot Sequence (`main.cpp` setup)

Current boot order:
```
Serial → RTC → SD → WiFi AP → ESP-NOW → BLE → Web server
```

New boot order (wake-gated):
```
Serial → assertPwrHold() → detectWakeReason() → branch:
  if WAKE_RTC_ALARM:
    → RTC init → SD init → ESP-NOW init (no AP) → sync cycle → power down
  if WAKE_CONFIG_BUTTON:
    → RTC init → SD init → WiFi AP → ESP-NOW → Web server → UI loop → power down on timeout
  if WAKE_USB_SERVICE:
    → full init (same as current) → stay on
```

### 3.5 Modified: Main Loop (`main.cpp` loop)

Current: runs forever, always-on.

New: depends on wake reason:
- **Sync wake:** one-shot cycle, then power down
- **Config wake:** interactive loop with idle timeout
- **Service wake:** same as current (runs until USB removed)

### 3.6 Modified: ESP-NOW Init

Current: always initialised with AP on channel 11.

New:
- **Sync wake:** init ESP-NOW without AP (STA mode only, or brief AP for node discovery)
- **Config wake:** init ESP-NOW with AP (current behaviour)
- **Service wake:** init ESP-NOW with AP (current behaviour)

**Open question:** Can ESP-NOW receive without an active AP? ESP-NOW on ESP32 requires WiFi to be initialised, but the AP doesn't need to be started. Need to verify that `WiFi.mode(WIFI_STA)` with ESP-NOW works for receiving broadcasts from nodes that are already on channel 11.

### 3.7 New: Modem Driver (`comms/modem.h`)

| Function | Purpose |
|---|---|
| `modemPowerOn()` | Enable 4V rail (GPIO33 `4V_EN`), wait for `ESP_PG` (GPIO35) |
| `modemPowerOff()` | Disable 4V rail |
| `modemSendAT(cmd, timeout)` | Send AT command, wait for response |
| `modemBoot()` | Pulse `M_PWRK` (GPIO14) for boot sequence |
| `modemRegister()` | Wait for network registration |
| `modemHttpPost(url, payload)` | POST data to webhook |
| `modemShutdown()` | Clean modem power-down sequence |

**Power sequence (from LTE backhaul concept doc):**
1. Enable 4V rail → wait for `ESP_PG` (power-good)
2. Pulse `M_PWRK` LOW for ≥500ms → wait for modem boot (~15s)
3. AT command init → check SIM → register network
4. HTTP POST queued data
5. Clean shutdown → disable 4V rail

### 3.8 New: Upload Queue (`storage/upload_queue.h`)

The modem should not upload live ESP-NOW packets. Instead:

1. Node snapshots are written to SD card (existing flow)
2. After SD drain, a separate upload queue marks which CSV rows need uploading
3. On sync wake with modem enabled: read queued rows → POST to webhook → mark as uploaded
4. This keeps LTE complexity downstream of local storage

---

## 4. Pin Mapping (from PCB Schematic Review)

| GPIO | Net | Direction | Purpose |
|---:|---|---|---|
| 26 | `PWR_HOLD` | OUTPUT | Main power hold — assert HIGH immediately at boot |
| 32 | `CONFIG_WAKE_PIN` | INPUT | Config latch sense — LOW = config wake requested |
| 25 | `CONFIG_CLEAR_PIN` | OUTPUT | Config latch clear — pulse HIGH to clear latch |
| 27 | `CFG_LED` | OUTPUT | Config/status LED |
| 33 | `4V_EN` | OUTPUT | Modem regulator enable |
| 35 | `ESP_PG` | INPUT | Modem power-good (input-only, needs external pull-up) |
| 14 | `M_PWRK` | OUTPUT | Modem PWRKEY |
| 4 | `M_STS` | INPUT | Modem status |
| 34 | `VOLT_ESP` | INPUT | Battery ADC (input-only) |
| 17 | `TX2` | OUTPUT | Modem UART TX |
| 16 | `RX2` | INPUT | Modem UART RX |
| 21 | `SDA` | I/O | I²C data (RTC + sensors) |
| 22 | `SCL` | OUTPUT | I²C clock |
| 18 | `SD_SCK` | OUTPUT | SD SPI clock |
| 19 | `SD_MISO` | INPUT | SD SPI MISO |
| 23 | `SD_MOSI` | OUTPUT | SD SPI MOSI |
| 13 | `SD_CS` | OUTPUT | SD chip select |

---

## 5. Sync Scheduling — Mothership vs Node

### 5.1 Current Node Behaviour (reference)

The node uses **two DS3231 alarms:**
- **Alarm 1:** sampling interval (e.g., every 5 min) — wake, sample sensors, go back to sleep
- **Alarm 2:** sync interval (e.g., every 90 min) — wake, turn on radio, exchange data with mothership

### 5.2 Proposed Mothership Behaviour

The mothership uses **one DS3231 alarm:**
- **Alarm 1:** sync interval — wake, receive node data, log to SD, optionally upload via modem, go back to sleep

The mothership does **not** sample independently. It only wakes when nodes are expected to send data.

### 5.3 Alignment Problem — Timing

**This is the critical design question.** For the mothership and nodes to exchange data, they must be awake at the same time.

**Option A: Mothership wakes first, stays awake for a window**
1. Mothership RTC alarm fires at T=0
2. Mothership boots, asserts PWR_HOLD, starts ESP-NOW listener
3. Mothership broadcasts SYNC_WINDOW_OPEN
4. Nodes that wake within the sync window hear the broadcast and send data
5. After a timeout (e.g., 60s), mothership powers down

**Problem:** If the mothership wakes first and broadcasts, nodes that wake later in the window may miss the broadcast. The current firmware already handles this with the `SYNC_WINDOW_OPEN` message, but the timing window needs to be generous enough.

**Option B: Nodes wake on their own schedule, mothership listens for a window**
1. Mothership RTC alarm fires at T=0
2. Mothership boots, starts ESP-NOW listener (no broadcast)
3. Nodes wake on their own Alarm 2 schedule and send NODE_HELLO
4. Mothership receives NODE_HELLO, responds with CONFIG_SNAPSHOT if needed
5. Nodes send NODE_SNAPSHOT data
6. After a timeout with no new data, mothership powers down

**Problem:** This requires nodes to already know the mothership's MAC and channel. Currently they do (stored in NVS after pairing). But if the mothership's MAC changes (new board), all nodes need re-pairing.

**Option C: Hybrid — mothership broadcasts, then listens**
1. Mothership RTC alarm fires
2. Mothership boots, starts ESP-NOW
3. Mothership broadcasts SYNC_WINDOW_OPEN (like current firmware)
4. Then listens for NODE_HELLO and NODE_SNAPSHOT
5. After timeout, powers down

This is closest to the current firmware's sync model and is the recommended approach.

### 5.4 Recommended Sync Window Timing

```
T+0s:    RTC alarm fires, mothership boots
T+2s:    PWR_HOLD asserted, ESP32 ready
T+3s:    ESP-NOW initialised, SD card mounted
T+5s:    Broadcast SYNC_WINDOW_OPEN
T+5-60s: Receive node data, log to SD
T+60s:   [Optional] Start modem upload
T+60-120s: [Optional] Upload queued data
T+120s:  Re-arm RTC alarm, release PWR_HOLD, power off
```

Total sync wake: ~2 minutes without modem, ~4 minutes with modem upload.

---

## 6. Confusing Points and Potential Issues

### 6.1 ⚠️ ESP-NOW Without AP — Needs Verification

The current mothership starts a WiFi AP on channel 11 and uses that same channel for ESP-NOW. If we want sync-wake mode to run **without** the AP (to save power and time), we need to verify that:

- ESP-NOW can receive on a fixed channel without an active AP
- Nodes can still find and communicate with the mothership on that channel
- The `WiFi.mode(WIFI_STA)` + ESP-NOW init path works for receiving

**If this doesn't work**, we may need to briefly start the AP during sync wakes, which adds ~3-5 seconds and more power draw.

### 6.2 ⚠️ Config Wake vs Sync Wake — Latch Clearing Timing

The config latch (`SN74LVC2G74DCUR`) is set by the button press and must be cleared by firmware. But:

- If the latch is cleared **before** reading it, we lose the wake reason
- If the latch is cleared **too late**, the latch output keeps the FET on unnecessarily

**Recommended:** Read `CONFIG_WAKE_PIN` first, then clear the latch immediately after. The latch only needs to be held long enough for firmware to read it.

### 6.3 ⚠️ PWR_HOLD Must Be Asserted Very Early

The PCB design note states: "PWR_HOLD must be asserted very early in firmware boot." The ESP32 boot time is ~300ms before `setup()` runs. During this time, the board is held on only by the wake-source logic (RTC alarm or config latch).

**Risk:** If the wake-source pulse is too short (e.g., RTC alarm flag cleared before PWR_HOLD is asserted), the board could power off during boot.

**Mitigation:** The DS3231 alarm output stays asserted until the flag is cleared by I2C. The config latch stays set until cleared by firmware. Both should hold long enough for boot. But this needs verification on real hardware.

### 6.4 ⚠️ SD Card Mount Time

SD card initialisation can take 200-500ms. In sync-wake mode, this adds to the total wake time. Not a problem functionally, but worth noting for power budget calculations.

### 6.5 ⚠️ Modem Power-On Sequence Complexity

The A7670G modem has a complex boot sequence:
- 4V rail enable → wait for power-good
- PWRKEY pulse → wait 10-15 seconds for modem boot
- AT command init → SIM check → network registration (30-90 seconds)
- HTTP POST → response

Total modem session: **45-120 seconds** added to each sync wake if uploading.

**Recommendation:** Don't enable the modem on every sync wake. Only upload when:
- A configurable number of sync cycles have accumulated data
- The SD card has more than N unsent records
- A manual "upload now" command from the web UI

### 6.6 ⚠️ WiFi AP Start Time

Starting a WiFi AP takes ~2-3 seconds. In config-wake mode, this is acceptable (user pressed the button and is waiting). In sync-wake mode, this is wasted time if we don't need the AP.

### 6.7 ⚠️ RTC Alarm Re-Arming After Sync

After a sync wake, the mothership must re-arm the DS3231 alarm for the next sync window **before** releasing PWR_HOLD. If the alarm re-arm fails, the board will power off and never wake again.

**Mitigation:** Verify alarm was set correctly by reading back the alarm registers before powering down. If verification fails, keep the board on and retry.

### 6.8 ⚠️ Cold Start vs Warm Wake

On first power-up (or after RTC battery loss), the DS3231 has no alarm set. The mothership would never wake up.

**Mitigation:** On cold start (RTC power-lost flag), default to config-wake mode (start AP, let user set time and schedule). Or add a fallback: if no alarm is set, the config button is the only way to wake.

### 6.9 ⚠️ Node Discovery During Sync Wake

If a new unpaired node broadcasts DISCOVER_REQUEST during a sync wake, the mothership needs to respond. But in sync-wake mode (no AP), the pairing flow may be incomplete.

**Recommendation:** During sync wake, respond to DISCOVER_REQUEST with a minimal pairing response, but don't attempt full deployment. Full pairing and deployment should happen during config-wake mode when the AP is available.

### 6.10 ⚠️ Web UI "Shut Down" Button

When the user is done with config mode, they need a way to tell the mothership to power off. Options:
- A "Shut Down" button on the web UI that releases PWR_HOLD
- An idle timeout (e.g., 10 minutes of no web requests)
- Both (timeout is a safety net)

---

## 7. Implementation Order

Suggested phased approach:

### Phase 1: Power Gating (essential, no modem)
1. Add `PWR_HOLD`, `CONFIG_WAKE_PIN`, `CONFIG_CLEAR_PIN` GPIO defines
2. Add `assertPwrHold()` and `releasePwrHold()` functions
3. Add wake reason detection (RTC alarm vs config button vs USB)
4. Modify `setup()` to assert PWR_HOLD first, then branch on wake reason
5. Add DS3231 alarm programming (Alarm 1 only)
6. Add config-wake mode: start AP + web UI, idle timeout, then power down
7. Add sync-wake mode: ESP-NOW receive only, SD logging, then power down
8. Add "Shut Down" button to web UI

### Phase 2: Sync Scheduling
1. Port the existing sync scheduling logic (interval/daily modes) to work with alarm-based wakes
2. Implement SYNC_WINDOW_OPEN broadcast on sync wake
3. Implement sync timeout and power-down after data exchange
4. Verify node-mothership timing alignment

### Phase 3: Modem Upload (optional, later)
1. Add A7670G modem driver (AT commands over UART2)
2. Add 4V rail enable/disable with power-good check
3. Add modem boot, registration, and HTTP POST
4. Add upload queue (mark SD rows as "pending upload")
5. Add configurable upload schedule (every N syncs, or when queue exceeds threshold)

### Phase 4: Robustness
1. Add alarm verification before power-down
2. Add cold-start detection and fallback
3. Add watchdog for stuck states
4. Add brownout detection during sync wake
5. Add battery voltage logging on each wake

---

## 8. Files to Create or Modify

| File | Action | Purpose |
|---|---|---|
| `system/power.h` | **Create** | PWR_HOLD, config latch, wake reason detection |
| `system/power.cpp` | **Create** | Implementation of power gating functions |
| `time/rtc_alarm.h` | **Create** | DS3231 alarm programming for mothership |
| `time/rtc_alarm.cpp` | **Create** | Alarm set/clear/read implementation |
| `comms/modem.h` | **Create** (Phase 3) | A7670G modem driver interface |
| `comms/modem.cpp` | **Create** (Phase 3) | AT command and power sequence implementation |
| `storage/upload_queue.h` | **Create** (Phase 3) | Upload queue management |
| `storage/upload_queue.cpp` | **Create** (Phase 3) | SD-based upload queue |
| `main.cpp` | **Modify** | Add PWR_HOLD assertion, wake reason branching, power-down logic |
| `comms/esp_now.h` | **Modify** | Add ESP-NOW init without AP mode |
| `comms/esp_now.cpp` | **Modify** | Support sync-wake receive-only mode |
| `time/rtc.h` | **Modify** | Add alarm programming functions |
| `time/rtc.cpp` | **Modify** | Implement alarm set/clear |

---

## 9. Open Questions

1. **ESP-NOW without AP:** Does `WiFi.mode(WIFI_STA)` + ESP-NOW work for receiving on a fixed channel? If not, we need a brief AP burst during sync wake.
2. **Modem upload frequency:** How often should the modem wake and upload? Every sync? Every N syncs? Only when the SD queue exceeds a threshold?
3. **Webhook format:** What JSON schema should the modem POST? This depends on the receiving service (not yet configured).
4. **Config timeout:** How long should the AP stay up in config mode before auto-shutdown? 10 minutes? 5?
5. **Cold start fallback:** If the RTC has no alarm set and no time, should the board default to config-wake mode?
6. **Node pairing during sync wake:** Should the mothership respond to DISCOVER_REQUEST during sync wakes, or only during config wakes?
7. **Battery monitoring:** Should the mothership log battery voltage on every sync wake? How often to check?