# Mothership V1 Bring-up Results — 2026-06-19

**Board:** Mothership V1 PCB (ESP32-WROOM, power-gated hub)
**Port:** COM5 (CH340C)
**Firmware:** `mothership/firmware/v1/`

---

## Test Results Summary

| # | Test | Env | Result | Notes |
|---|---|---|---|---|
| 1 | PWR_HOLD | mothership-v1-pwrhold | ✅ PASS | GPIO26 holds VSYS; board powers off when PWR_HOLD released |
| 2 | Config Latch | mothership-v1-config-latch | ✅ PASS | Latch read/clear works; wake-from-off via config button confirmed |
| 3 | RTC Alarm | mothership-v1-rtc-alarm | ✅ PASS | DS3231 I2C on GPIO21/22; alarm fires every 10s; flag clears and re-arms |
| 4 | SD Card | mothership-v1-sd-card | ❌ FAIL | PCB routing bug — see below |
| 5 | Battery ADC | mothership-v1-battery-adc | ✅ PASS | V_bat=4.129 V matches multimeter 4.12 V; ESP_PG=LOW (correct, modem rail off) |
| 6 | ESP-NOW Basic | mothership-v1-espnow-basic | ⏳ Pending | |
| 7 | Wake Reason | mothership-v1-wake-reason | ✅ PASS | All three wake sources confirmed: config button wake-from-off, RTC alarm wake-from-off (15s alarm armed before shutdown), USB service on first boot; PWR_HOLD shutdown works |
| 8 | Modem Power | mothership-v1-modem-power | ✅ PASS | TPS63020 rail works with PWM soft-start; ESP_PG HIGH on enable, LOW on disable; brownout fixed |

---

## Test 4: SD Card — PCB Routing Bug (confirmed)

**Root cause:** GPIO23 (MOSI) is routed to SD socket pin 8 (DAT1) instead of pin 3 (CMD).

In SPI mode, the SD card receives commands on pin 3 (CMD). With MOSI on pin 8 (DAT1, unused in SPI mode), the card never receives any commands and never responds on MISO. This produces `sdSelectCard(): Select Failed` regardless of SPI speed, card brand, or SPI peripheral.

**Verified:**
- Card power (VCC) present ✅
- CS (GPIO13) → pin 2 (CD/DAT3) ✅
- SCK (GPIO18) → pin 5 (CLK) ✅
- MISO (GPIO19) → pin 7 (DAT0) ✅
- MOSI (GPIO23) → pin 8 (DAT1) ❌ should be pin 3 (CMD)
- Two different SD cards tested, both fail identically
- SPI speed tested from 400 kHz to 40 MHz, no difference
- VSPI and HSPI both fail

**Fix for next PCB revision:**
- Route GPIO23 (MOSI) to SD socket pin 3 (CMD)
- Leave pin 8 (DAT1) unconnected (unused in SPI mode)

**Fix for current prototype:**
- Bodge wire from GPIO23 (or 22Ω series resistor output) to pin 3 (CMD) of SD socket
- Not applied during this bring-up session — deferred to a later rework pass

---

## Config Mode Milestone (2026-06-19)

The V1 main firmware with ported config mode has been flashed and tested on hardware:

- ✅ Board off → config button press → wakes into config mode
- ✅ WiFi AP "Logger001" served on channel 11
- ✅ Web UI accessible at http://192.168.4.1/ (full dashboard, node manager, settings)
- ✅ RTC time set via web UI (set to 2026-06-19 12:15:19)
- ✅ Config button press again → exits config mode → arms RTC alarm → powers off
- ✅ Flash logger (LittleFS) active as storage fallback (SD card PCB bug workaround)
- ✅ ESP-NOW sync window broadcast confirmed (sync mode)

### Node pipeline tested (2026-06-19)
- ✅ Node discovery via web UI — mothership broadcasts DISCOVERY_SCAN, node responds with DISCOVER_REQUEST, appears in node manager
- ✅ Node pairing/deployment — deploy command sent via web UI, node receives DEPLOY_NODE with RTC time + wake interval + sync schedule
- ✅ Node → mothership data pipeline — node sends node_snapshot_t (124 bytes), mothership receives and logs to flash (LittleFS)
- ✅ CSV download with actual node data — web UI CSV button serves flash-stored datalog.csv with node sensor readings
- ✅ ESP-NOW command dispatch fix — changed from length-based to command-based dispatch to resolve struct size collisions (discovery_message_t and node_hello_message_t both 56 bytes)

### Full autonomous sync pipeline PROVEN (2026-06-19 20:55)
- Mothership woke at 20:54:50 (10s pre-wake, phase-aligned alarm with A1M4=1 fix)
- Node woke at 20:55:00 (A2 sync alarm)
- Mothership broadcast SYNC_WINDOW_OPEN repeatedly every 5s
- Node received marker after 3.4s, flushed 17 queued snapshots (seq 2-18)
- Mothership received all 17 NODE_SNAPSHOT packets (124 bytes each)
- All snapshots logged to flash (LittleFS)
- Mothership re-armed for next sync at 21:12:50 (phase-aligned, 18 min interval)
- Both boards powered off autonomously
- Cycle repeats indefinitely without user intervention

### Firmware fixes applied for sync pipeline
8. `rtc_alarm.cpp` — Fixed A1M4=0 to A1M4=1 (alarm now fires on any day, not just matching day-of-month)
9. `rtc_alarm.cpp` — Added `armNextSyncAlarmPhase()` with phase anchor alignment + 10s pre-wake offset
10. `main.cpp` — `handleSyncWake()` loads sync interval from NVS (was defaulting to 60 min instead of 18 min)
11. `config_server.cpp` — Set `gLastSyncBroadcastUnix` phase anchor on node deploy
12. `main.cpp` — Repeated SYNC_WINDOW_OPEN broadcast every 5s during sync window (was single broadcast)
13. `pins.h` — Increased SYNC_WINDOW_MS from 60s to 120s for multi-node support
14. `main.cpp` — Added intelligent early shutdown when all deployed nodes have synced
15. `main.cpp` — Disabled config button exit (latch bounce unreliable), added web UI Shut Down button
16. `config_server.cpp` — Added /shutdown route + Shut Down button in web UI dashboard
17. `node_registry.cpp` — Crash-safe loadPairedNodes() with NVS validation
18. `espnow_config.cpp` — Command-based ESP-NOW dispatch (fixes struct size collisions)
19. `main.cpp` — Service mode just powers off (no alarm arming after flash)
20. `main.cpp` — `onEspNowData()` updates node registry for early shutdown tracking

### Not yet tested
- Early shutdown optimization (mothership listened full 120s, didn't trigger early exit)
- Multiple nodes syncing simultaneously
- Long-term reliability (multiple sync cycles over hours/days)

## Firmware Fixes Applied During Bring-up

1. `bringup_rtc_alarm.cpp` line 109 — `const DateTime` qualifier error with RTClib `operator+`. Fixed by making a non-const local copy before addition.
2. `bringup_sd_card.cpp` — SPI speed lowered from 40 MHz to 400 kHz for debugging (did not resolve the issue; can be restored to a higher speed after the PCB fix).
3. `bringup_sd_card.cpp` — Switched from VSPI to HSPI and added MISO diagnostics (did not resolve; can be reverted after PCB fix).
4. `platformio.ini` (V1) — Added `upload_port = COM5` and `monitor_port = COM5` to shared `[env]` section.
5. `bringup_battery_adc.cpp` — Fixed `BAT_ADC_VREF` from 3.3 V to 3.6 V to match `ADC_11db` attenuation range. Reading was 3.7 V (wrong), now 4.129 V (matches multimeter).
6. `bringup_modem_power.cpp` — Added GPIO33 glitch prevention (pre-load output register LOW before pinMode) and PWM soft-start (500ms ramp 0→100%) to prevent TPS63020 inrush brownout on battery power.
7. `bringup_wake_reason.cpp` — Added `armAlarm1Seconds()` function to arm DS3231 Alarm 1 before PWR_HOLD release, enabling RTC alarm wake-from-off testing.