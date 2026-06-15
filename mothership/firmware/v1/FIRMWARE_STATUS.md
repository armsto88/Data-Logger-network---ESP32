# Mothership V1 Firmware Status

**Last updated:** 2026-06-15  
**Firmware path:** `mothership/firmware/v1/`  
**Plan reference:** `mothership/docs/MOTHERSHIP_V1_FIRMWARE_PLAN.md`

---

## 1. Architecture Overview

The V1 firmware implements a **wake-gated architecture** where the mothership is fully powered off between activity periods. The DS3231 RTC, config button latch, or USB connection triggers a power-on event, and the ESP32 must assert `PWR_HOLD` (GPIO26) immediately to stay alive.

### Three wake modes

| Wake source | Trigger | Behavior | WiFi AP? | ESP-NOW? | Modem? |
|---|---|---|---|---|---|
| **RTC alarm** | DS3231 Alarm 1 | Wake → receive node data → log to SD → re-arm alarm → power off | No | Yes (brief) | No |
| **Config button** | SN74LVC2G74 latch | Wake → start AP + web UI → user config → power off | Yes | Yes | No |
| **USB service** | VBUS_USB / SW10 | Wake → full runtime for flashing/diagnostics → stays on | Optional | Optional | Optional |

### Boot sequence

```
Power on → assert PWR_HOLD (GPIO26) → detect wake reason → branch:
  WAKE_RTC_ALARM:     init RTC → init SD → init ESP-NOW → sync window → re-arm alarm → release PWR_HOLD
  WAKE_CONFIG_BUTTON: clear latch → init RTC → init SD → [TODO: WiFi AP + web server] → re-arm alarm → release PWR_HOLD
  WAKE_USB_SERVICE:   init RTC → init SD → init ESP-NOW → [TODO: full runtime] → stay on
```

---

## 2. Module Status

| Module | File | Status | Notes |
|---|---|---|---|
| Pin definitions | `system/pins.h` | ✅ Complete | All GPIO with `#ifndef` guards |
| Power control | `system/power.h/cpp` | ✅ Mostly done | Missing `schedulePowerDown()` and `isUsbPowered()` |
| Wake reason | `system/wake_reason.h/cpp` | ✅ Complete | Detects RTC/config/USB wake |
| RTC alarm | `time/rtc_alarm.h/cpp` | ✅ Complete | Alarm 1 programming, verification |
| ESP-NOW sync | `comms/espnow_sync.h/cpp` | ✅ Mostly done | Receive-only, no `node_snapshot_t` handling |
| SD logger | `storage/sd_logger.h/cpp` | ✅ Mostly done | Uses simplified `NodeSnapshot` struct |
| Main firmware | `main.cpp` | ✅ Boot sequence done | Config/service handlers are stubs |
| Config wake handler | `main.cpp` | ⚠️ Stub | Blinks LED, no WiFi AP or web server |
| Service wake handler | `main.cpp` | ⚠️ Stub | Logs battery voltage, no full runtime |
| Modem driver | `comms/modem.h/cpp` | ❌ Not started | Phase 3 |
| Upload queue | `storage/upload_queue.h/cpp` | ❌ Not started | Phase 3 |
| WiFi AP + web server | — | ❌ Not ported | Exists in `mothership/firmware/src/` |
| BLE | — | ❌ Not started | Not in V1 scope |
| Node registry | — | ❌ Not started | No `NodeInfo` vector or NVS persistence |
| Sync scheduling | — | ❌ Not ported | Interval/daily mode logic from current firmware |

---

## 3. What's Implemented

### Power gating (`system/power.h/cpp`)
- `powerInit()` — initializes all GPIO (PWR_HOLD, config latch, LED, battery ADC, modem pins)
- `assertPwrHold()` / `releasePwrHold()` — control VSYS rail via GPIO26
- `readConfigWake()` — reads config latch state (active LOW)
- `clearConfigLatch()` — pulses GPIO25 HIGH for 20 ms to clear the latch
- `readBatteryVoltage()` — reads GPIO34 ADC with 16-sample averaging and voltage divider math
- `setLed()` / `toggleLed()` — controls the config/status LED on GPIO27
- `isPwrHoldAsserted()` — returns current PWR_HOLD state

### Wake reason detection (`system/wake_reason.h/cpp`)
- `detectWakeReason()` — checks config latch → RTC alarm flag → defaults to USB service
- `wakeReasonStr()` / `printWakeReason()` — human-readable wake reason output
- Returns `WAKE_RTC_ALARM`, `WAKE_CONFIG_BUTTON`, `WAKE_USB_SERVICE`, or `WAKE_UNKNOWN`

### RTC alarm (`time/rtc_alarm.h/cpp`)
- `initRTC()` — initializes I2C and DS3231, reports if RTC lost power
- `getRTCTime()` / `setRTCTime()` — read/write RTC time
- `armNextSyncAlarm(int intervalMin)` — programs Alarm 1 for N minutes from now
- `armDailyAlarm(int hour, int minute)` — programs Alarm 1 for a specific time of day
- `clearAlarmFlag()` / `readAlarmFlag()` — manage DS3231 status register
- `verifyAlarmSet()` — reads back alarm registers and confirms they match what was written

### ESP-NOW sync (`comms/espnow_sync.h/cpp`)
- `initEspNowSyncOnly(int channel)` — WiFi STA mode, ESP-NOW init, broadcast peer added
- `broadcastSyncWindowOpen()` — sends `SYNC_WINDOW_OPEN` broadcast to nodes
- `registerReceiveCallback()` — registers user callback for received data
- `espnowSyncLoop()` — placeholder for future queue-drain logic

### SD logger (`storage/sd_logger.h/cpp`)
- `initSD()` — initializes SPI and mounts SD card, creates CSV log file
- `logSnapshot()` — logs a `NodeSnapshot` struct as a CSV row
- `logCSVRow()` — appends an arbitrary string as a CSV row
- `getCSVStats()` — returns line count and storage stats
- `sdIsReady()` — returns SD card ready state

### Main firmware (`main.cpp`)
- Boot sequence: `powerInit()` → `assertPwrHold()` → `detectWakeReason()` → branch
- **Sync wake handler:** Full implementation — init RTC, SD, ESP-NOW; broadcast sync window; receive data for 60 s; re-arm alarm; power down
- **Config wake handler:** Stub — clears latch, inits RTC/SD, blinks LED 30 times, re-arms alarm, powers down
- **Service wake handler:** Stub — inits all subsystems, logs battery voltage every 30 s, runs indefinitely
- ESP-NOW receive callback: parses `sensor_data_message_t` and logs to SD

### Bringup tests (`tests/`)
Eight self-contained test sketches, one per subsystem:
1. `bringup_pwr_hold.cpp` — PWR_HOLD toggle (ON 15 s / OFF 15 s)
2. `bringup_config_latch.cpp` — Config latch read/clear with LED feedback
3. `bringup_rtc_alarm.cpp` — DS3231 I2C and Alarm 1 (10 s interval)
4. `bringup_sd_card.cpp` — SD card init, write, read-back, delete
5. `bringup_battery_adc.cpp` — Battery ADC and modem power-good read
6. `bringup_espnow_basic.cpp` — ESP-NOW receive on channel 11
7. `bringup_wake_reason.cpp` — Full wake-reason detection and LED patterns
8. `bringup_modem_power.cpp` — TPS63020 enable/power-good validation

---

## 4. What's TODO

### Config wake handler (currently a stub)
- **WiFi AP + web server:** Port from `mothership/firmware/src/`. The current firmware has a full captive-portal web UI for configuration. This needs to be adapted for the V1 wake-gated architecture.
- **Config timeout / shutdown button:** The config handler currently blinks the LED and powers down. It needs a proper idle timeout (e.g., 10 minutes) and a web UI "Shut Down" button that calls `releasePwrHold()`.
- **Reference:** `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §2.3

### Service wake handler (currently a stub)
- **Full runtime mode:** The service handler currently just logs battery voltage. It needs the full WiFi AP, web server, and ESP-NOW receive loop from the current firmware.
- **USB disconnect detection:** No `isUsbPowered()` implementation exists. The V1 firmware plan calls for detecting USB VBUS to know when to stay on vs. power down.
- **Reference:** `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §2.4

### ESP-NOW receive (incomplete protocol handling)
- **Only handles `sensor_data_message_t`:** The `onEspNowData()` callback in `main.cpp` only parses the legacy `sensor_data_message_t` struct. It does not handle `node_snapshot_t` (the current protocol) or `node_hello_message_t`.
- **Node registry:** No `NodeInfo` vector or NVS persistence. The current firmware tracks known nodes; V1 needs this for sync window management.
- **Reference:** `node/firmware/shared/protocol.h`

### Sync scheduling
- **Interval/daily mode:** The current firmware supports both interval-based (every N minutes) and daily (at a specific time) sync scheduling. V1 only has `armNextSyncAlarm(int intervalMin)`.
- **NVS persistence:** Sync interval, wake interval, and node registry are not persisted to NVS. If the board loses power, these settings are lost.
- **Reference:** `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §5

### SD logger improvements
- **Filenames use `millis()`:** Log files are named `/log_<seconds_since_boot>.csv` instead of using RTC timestamps. This means files from different boots can collide or have meaningless names.
- **Simplified `NodeSnapshot` struct:** The SD logger uses its own `NodeSnapshot` struct instead of the full `node_snapshot_t` from `protocol.h`. This means it can't log all snapshot fields (spectral data, wind, soil moisture, etc.).
- **Reference:** `storage/sd_logger.h/cpp`

### Cold start fallback
- **No alarm = no wake:** If the DS3231 loses power (coin cell dies) and no alarm is set, the board will never wake from RTC. The firmware needs a fallback: either set a default alarm on first boot, or detect `lostPower()` and set a conservative alarm.
- **Reference:** `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §6

### `isUsbPowered()`
- **No GPIO for VBUS detection:** The firmware plan calls for `isUsbPowered()` in `system/power.h`, but there is no GPIO defined for VBUS detection on the V1 PCB. The current `detectWakeReason()` falls through to `WAKE_USB_SERVICE` when neither config nor RTC alarm is detected, which is a heuristic rather than a direct measurement.
- **Reference:** `system/power.h`, `system/wake_reason.cpp`

---

## 5. What's Not Started

### Phase 3: Modem (A7670G LTE)

| Item | Description | Reference |
|---|---|---|
| Modem driver | `comms/modem.h/cpp` — power on, AT commands, boot, register, HTTP POST | `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §3.7 |
| Upload queue | `storage/upload_queue.h/cpp` — mark CSV rows for upload, track upload status | `MOTHERSHIP_V1_FIRMWARE_PLAN.md` §3.8 |
| LTE session management | Power on modem → boot → register → upload → shutdown sequence | `MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md` |
| Modem UART | Serial2 on GPIO17 (TX) / GPIO16 (RX) for AT command communication | `system/pins.h` |

### Phase 4: Robustness

| Item | Description | Reference |
|---|---|---|
| Watchdog | Hardware or software watchdog for stuck states (infinite loops in handlers) | — |
| Brownout detection | Detect and log VSYS brownout events | `MT3608_BROWNOUT_DESIGN_NOTE.md` |
| Alarm verification retry | If `verifyAlarmSet()` fails, retry with backoff before fatal hang | `time/rtc_alarm.cpp` |
| Boot count / reset reason | NVS-stored boot count and ESP32 reset reason for diagnostics | — |
| Config timeout | Idle timeout for config mode (e.g., 10 min no activity → power down) | — |
| Shutdown button | Web UI button to trigger `releasePwrHold()` from config/service mode | — |

---

## 6. Known Issues

### Protocol mismatch
- `onEspNowData()` in `main.cpp` only handles `sensor_data_message_t` (legacy protocol, 68 bytes). The current node firmware sends `node_snapshot_t` (124 bytes). The V1 firmware will silently drop snapshot packets because `len >= sizeof(sensor_data_message_t)` passes but the data is parsed as the wrong struct.
- **Impact:** Node data from current firmware nodes will not be logged correctly.
- **Fix:** Add `node_snapshot_t` handling to the ESP-NOW callback, or detect message type by the `command` field prefix.

### Redundant I2C initialization
- Both `wake_reason.cpp` and `rtc_alarm.cpp` call `Wire.begin(PIN_SDA, PIN_SCL)` independently. This is harmless (Arduino `Wire` handles re-initialization) but wasteful.
- **Impact:** None in practice, but could cause confusion if I2C settings are changed in one place.
- **Fix:** Move I2C init to a single `busInit()` function called once in `setup()`.

### Simplified NodeSnapshot struct
- `sd_logger.h` defines its own `NodeSnapshot` struct with only the fields from `sensor_data_message_t`. The full `node_snapshot_t` from `protocol.h` has additional fields (spectral data, wind, soil moisture, battery voltage, etc.) that are not captured.
- **Impact:** SD logs are missing data that nodes are sending.
- **Fix:** Replace `NodeSnapshot` with `node_snapshot_t` and update the CSV header accordingly.

### No WiFi AP or web server
- Config and service wake handlers are stubs. The config handler blinks the LED and powers down. The service handler logs battery voltage and runs indefinitely.
- **Impact:** No way to configure the mothership in the field without a serial connection.
- **Fix:** Port WiFi AP + web server from `mothership/firmware/src/`.

### No NVS persistence
- Sync interval, wake interval, node registry, and configuration are not stored in NVS. Every boot starts with defaults.
- **Impact:** Any configuration changes are lost on power-down.
- **Fix:** Add `Preferences`-based NVS storage for critical settings.

### SD log filenames
- Log files are named `/log_<seconds_since_boot>.csv` using `millis()`. This means:
  - Files from different boots can have the same name if the boot time is similar.
  - Filenames don't reflect actual wall-clock time.
- **Fix:** Use RTC timestamps for filenames once `initRTC()` has been called.

---

## 7. Integration with Current Firmware

The V1 firmware (`mothership/firmware/v1/`) is a **parallel implementation** of the mothership firmware, not a modification of the existing codebase (`mothership/firmware/src/`).

### What needs to be ported from `mothership/firmware/src/`

| Feature | Current location | V1 status |
|---|---|---|
| WiFi AP + captive portal | `src/wifi_ap.h/cpp` | ❌ Not ported |
| Web server + UI | `src/web_server.h/cpp` | ❌ Not ported |
| ESP-NOW with AP | `src/espnow_manager.h/cpp` | ⚠️ Partial — receive-only without AP |
| Node registry | `src/node_manager.h/cpp` | ❌ Not ported |
| Sync scheduling | `src/sync_scheduler.h/cpp` | ❌ Not ported |
| SD logging | `src/sd_logger.h/cpp` | ✅ Rewritten for V1 |
| Configuration | `src/config.h/cpp` | ❌ Not ported |
| BLE | `src/ble_manager.h/cpp` | ❌ Not in V1 scope |

### Key architectural differences

| Aspect | Current firmware (`src/`) | V1 firmware (`v1/`) |
|---|---|---|
| Power model | Always-on | Wake-gated (power off between syncs) |
| Boot sequence | Init everything, run forever | Assert PWR_HOLD → detect wake → branch → shutdown |
| WiFi | Always-on AP | Only during config/service wake |
| ESP-NOW | Always listening | Only during sync window |
| SD logging | Always mounted | Mounted per-wake, closed before power-down |
| RTC | Used for timestamps only | Used for alarm-driven wake scheduling |
| Modem | Not implemented | Planned (Phase 3) |

### Migration path

1. **Phase 1 (current):** V1 bringup tests and core wake-gated boot sequence.
2. **Phase 2 (next):** Port WiFi AP + web server for config/service modes. Add `node_snapshot_t` handling.
3. **Phase 3:** Add modem driver and upload queue for LTE backhaul.
4. **Phase 4:** Robustness (watchdog, brownout, NVS persistence, alarm retry).

---

## 8. Build Instructions

### Prerequisites

- PlatformIO Core (`pip install platformio`)
- ESP32 platform installed (`pio platform install espressif32`)

### Build environments

All environments are defined in `mothership/firmware/v1/platformio.ini`.

#### Bringup tests

```bash
# From mothership/firmware/v1/

# Test 1: PWR_HOLD
pio run -e mothership-v1-pwrhold -t upload -t monitor

# Test 2: Config latch
pio run -e mothership-v1-config-latch -t upload -t monitor

# Test 3: RTC alarm
pio run -e mothership-v1-rtc-alarm -t upload -t monitor

# Test 4: SD card
pio run -e mothership-v1-sd-card -t upload -t monitor

# Test 5: Battery ADC
pio run -e mothership-v1-battery-adc -t upload -t monitor

# Test 6: ESP-NOW basic
pio run -e mothership-v1-espnow-basic -t upload -t monitor

# Test 7: Wake reason
pio run -e mothership-v1-wake-reason -t upload -t monitor

# Test 8: Modem power
pio run -e mothership-v1-modem-power -t upload -t monitor
```

#### Main firmware

```bash
# Build and flash the full V1 firmware
pio run -e mothership-v1-main -t upload -t monitor
```

#### Build without flashing

```bash
# Just compile (check for errors)
pio run -e mothership-v1-main

# Compile a specific test
pio run -e mothership-v1-pwrhold
```

### Serial monitor

```bash
# Open serial monitor separately
pio device monitor -b 115200 --dtr 0 --rts 0
```

> **Note:** DTR and RTS are disabled (`monitor_dtr = 0`, `monitor_rts = 0`) to prevent the ESP32 from entering bootloader mode when the serial monitor connects. This is important for the PWR_HOLD test — if DTR/RTS cause a reset, the board may lose power before the test starts.

### Dependencies

| Library | Version | Used by |
|---|---|---|
| `adafruit/RTClib` | ^1.14.1 | RTC alarm test, wake reason test, main firmware |

The bringup tests for PWR_HOLD, config latch, SD card, battery ADC, ESP-NOW, and modem power do not require external libraries — they use the ESP32 Arduino core and standard libraries only.