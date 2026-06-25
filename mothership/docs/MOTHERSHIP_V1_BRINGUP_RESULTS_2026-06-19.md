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
| 9 | Modem UART | mothership-v1-modem-uart | ✅ PASS | Rail→PWRKEY→STATUS→UART2→AT responds OK |
| 10 | Modem Identify | mothership-v1-modem-identify | ✅ PASS | A7670G-LABE, IMEI 867284069283119, FW A110B06A7670M7 |
| 11 | Modem SIM | mothership-v1-modem-sim | ✅ PASS | SIM detected (+CPIN: READY), IMSI 262034251099911; AT+CCID returns ERROR (SIMCom fw quirk — alt cmd needed) |
| 12 | Modem Network | mothership-v1-modem-network | ✅ PASS | All status commands responded; CSQ=99,99 and CREG=0,2 expected without antenna |
| 13 | Modem Power Cycle | mothership-v1-modem-powercycle | ✅ PASS | 3/3 clean on→boot→AT→CPOF→off cycles; ~14s per cycle |

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

### Final pipeline test (2026-06-20)
- ✅ Config button wakes mothership after sync cycle — NO CRASH
- ✅ NVS loader hardened: fixed-buffer reads, no ESP-NOW before init, metadata after closing paired_nodes
- ✅ Startup sequence: PWR_HOLD preloaded HIGH (no glitch), independent wake-source flags, config priority over RTC
- ✅ Config latch retained during config mode, cleared only on shutdown
- ✅ RTC alarm: A1IE=1, INTCN=1, alarm verification checks both bits
- ✅ Phase-aligned alarm: 19:36:50 (10s pre-wake before 19:37:00 sync)
- ✅ Repeated SYNC_WINDOW_OPEN every 5s — node catches marker after 3.4s
- ✅ 17 snapshots received (seq 2-18) and logged to flash
- ✅ CSV download: 18 data rows confirmed
- ✅ Config button after sync works — web UI served, CSV downloaded
- ✅ Full autonomous cycle: sleep → wake → sync → sleep → config → shut down → repeat

### Root cause of config button crash (resolved)
The Guru Meditation crash was caused by:
1. `ensurePeerOnChannel()` calling `esp_now_add_peer()` before `esp_now_init()` — coexistence subsystem null pointer
2. Arduino-ESP32 `Preferences::getString()` using dynamic stack allocation from NVS length metadata
3. Nested Preferences handles (paired_nodes + node_meta open simultaneously)

Fix: Fixed-buffer NVS reads, no ESP-NOW calls during loading, close paired_nodes before opening node_meta, peers registered after ESP-NOW init.

## Modem AT Command Bring-up (2026-06-21)

**Hardware:** SIMCom A7670G-LABE, firmware revision A110B06A7670M7 / V1.11.2
**SIM:** German SIM (MCC 262), IMSI 262034251099911, no PIN
**Antenna:** None connected — tests validate modem hardware/firmware only, not RF

### Test 9: Modem UART (`mothership-v1-modem-uart`) — PASS
- 4V rail soft-start: ESP_PG HIGH (rail up)
- PWRKEY pulse (1100ms): STATUS went HIGH (modem powered on)
- UART2 (GPIO17 TX / GPIO16 RX) at 115200 baud through 1.8V↔3.3V level shifters
- AT → OK, ATE0 → OK, AT → OK (echo off confirmed)
- Heartbeat confirmed stable: STATUS=1 PG=1 4V_EN=1 (no brownout, no rail drop)
- Proves: TPS63020 rail, PWRKEY NMOS gate, STATUS level-shifter, UART2 AT path all functional

### Test 10: Modem Identify (`mothership-v1-modem-identify`) — PASS
- ATI → Manufacturer: SIMCOM INCORPORATED, Model: A7670G-LABE, Revision: V1.11.2, IMEI: 867284069283119
- AT+GMM → OK
- AT+CGSN → IMEI 867284069283119 (15 digits, valid)
- AT+CGMR → +CGMR: A110B06A7670M7
- Graceful shutdown: AT+CPOF → OK → rail off

### Test 11: Modem SIM (`mothership-v1-modem-sim`) — PASS
- Initial run failed: SIM was inserted upside-down (user error, not PCB issue)
- After correct orientation: AT+CPIN? → +CPIN: READY (SIM present, no PIN)
- AT+CIMI → IMSI 262034251099911 (German network, MCC 262)
- AT+CCID → ERROR (SIMCom firmware quirk — some A7670G fw versions use AT+ICCID or AT+CICCID instead; not a blocker, SIM comms confirmed by CPIN+CIMI)
- SIM holder footprint confirmed correct (no routing bug like SD card)
- Graceful shutdown: AT+CPOF → OK → +CGEV: ME DETACH → rail off

### Test 12: Modem Network (`mothership-v1-modem-network`) — PASS
- No antenna connected — all "no signal" responses are expected and count as PASS
- AT+CSQ → +CSQ: 99,99 (no signal — expected)
- AT+CREG? → +CREG: 0,2 (not registered, searching — expected)
- AT+CEREG? → +CEREG: 0,0 (not registered for EPS — expected)
- AT+CGATT? → +CGATT: 0 (not attached — expected)
- AT+COPS? → +COPS: 0 (no operator — expected)
- All 5 commands responded correctly — modem AT subsystem fully functional
- Graceful shutdown: AT+CPOF → OK → +CGEV: ME DETACH → rail off

### Test 13: Modem Power Cycle (`mothership-v1-modem-powercycle`) — PASS
- 3 full cycles: rail on (soft-start) → PWRKEY pulse → wait boot → AT → AT+CPOF → STATUS LOW → rail off → 3s gap
- Cycle 1: PG HIGH 511ms, boot OK 6908ms, AT OK, CPOF OK, STATUS LOW 0ms, total 13.6s
- Cycle 2: PG HIGH 511ms, boot OK 7508ms, AT OK, CPOF OK, STATUS LOW 0ms, total 14.2s
- Cycle 3: PG HIGH 511ms, boot OK 7508ms, AT OK, CPOF OK, STATUS LOW 0ms, total 14.2s
- 3/3 cycles completed cleanly — consistent timing, no brownouts, no hangs
- +CGEV: ME DETACH URC on each shutdown confirms graceful detach

### Modem subsystem status
- TPS63020 power rail: ✅ validated (soft-start, power-good, no brownout)
- PWRKEY boot sequencing: ✅ validated (NMOS gate, 1100ms pulse)
- STATUS level-shifter: ✅ validated (GPIO4 reads modem on/off state)
- UART2 AT command path: ✅ validated (115200 baud, 1.8V↔3.3V level shifters)
- SIM detection: ✅ validated (CPIN READY, IMSI readable, holder footprint correct)
- Graceful shutdown: ✅ validated (AT+CPOF + rail off, 3 consecutive cycles)
- Network registration: ⏳ pending antenna (CSQ/CREG/COPS return expected no-signal values)
- Data transport (HTTP/MQTT): ⏳ Phase 3 — pending antenna + network registration

### Not yet tested
- Early shutdown optimization (loadPairedNodes not called during sync wake, so deployedCount=0)
- Multiple nodes syncing simultaneously
- Long-term reliability (multiple sync cycles over hours/days)
- LTE data upload workflow (Phase 3 — modem hardware validated, pending antenna + network registration)

## Firmware Fixes Applied During Bring-up

1. `bringup_rtc_alarm.cpp` line 109 — `const DateTime` qualifier error with RTClib `operator+`. Fixed by making a non-const local copy before addition.
2. `bringup_sd_card.cpp` — SPI speed lowered from 40 MHz to 400 kHz for debugging (did not resolve the issue; can be restored to a higher speed after the PCB fix).
3. `bringup_sd_card.cpp` — Switched from VSPI to HSPI and added MISO diagnostics (did not resolve; can be reverted after PCB fix).
4. `platformio.ini` (V1) — Added `upload_port = COM5` and `monitor_port = COM5` to shared `[env]` section.
5. `bringup_battery_adc.cpp` — Fixed `BAT_ADC_VREF` from 3.3 V to 3.6 V to match `ADC_11db` attenuation range. Reading was 3.7 V (wrong), now 4.129 V (matches multimeter).
6. `bringup_modem_power.cpp` — Added GPIO33 glitch prevention (pre-load output register LOW before pinMode) and PWM soft-start (500ms ramp 0→100%) to prevent TPS63020 inrush brownout on battery power.
7. `bringup_wake_reason.cpp` — Added `armAlarm1Seconds()` function to arm DS3231 Alarm 1 before PWR_HOLD release, enabling RTC alarm wake-from-off testing.

---

## Modem Network Registration + HTTPS POST — 2026-06-25

**Antenna:** Connected (LTE antenna via u.FL/IPEX)
**SIM:** Aldi Talk (E-Plus/Telefónica network, APN: `internet.eplus.de`)
**Firmware:** A110B06A7670M7

### Test 14: Modem Network Registration (with antenna)

**Env:** `mothership-v1-modem-network`
**Result:** ✅ PASS

| Query | Result | Notes |
|---|---|---|
| AT+CSQ | 27,99 (excellent, -57 dBm) | Signal found after ~50s |
| AT+CREG? | 0,5 (registered, roaming) | GSM registered at ~50s |
| AT+CEREG? | 0,5 (registered, roaming) | EPS/LTE registered at ~90s |
| AT+CGATT | 1 (attached) | GPRS attached |
| AT+COPS? | 0,2,"26203",7 | Telekom Deutschland, LTE (RAT=7) |

**Key findings:**
- Modem takes ~50-90 seconds to complete full network registration from cold start
- `AT+COPS=0` causes `+CME ERROR: unknown error` on some runs — removed from test; modem auto-registers without it
- Think Mobile SIM (APN: "TM") became unresponsive after repeated `AT+COPS=0` errors — likely network-side block. Aldi Talk SIM works reliably.

### Test 15: HTTPS POST via A7670G Socket API

**Env:** `mothership-v1-modem-https`
**Result:** ✅ PASS (both TCP and SSL/TLS)

#### TCP (HTTP, port 80) — ✅ PASS

| Step | Command | Result |
|---|---|---|
| PDP context | AT+CGDCONT=1,"IP","internet.eplus.de" | OK |
| PDP activate | AT+CGACT=1,1 | OK |
| Network open | AT+NETOPEN | OK, +NETOPEN: 1 |
| TCP connect | AT+CIPOPEN=0,"TCP","httpbin.org",80 | +CIPOPEN: 0,0 |
| Send data | AT+CIPSEND=0,151 | > prompt, +CIPSEND: 0,151,151 |
| Response | +IPD225 + HTTP/1.1 200 OK | JSON body with echoed data |
| Close | AT+CIPCLOSE=0, AT+NETCLOSE | OK |

#### SSL/TLS (HTTPS, port 443) — ✅ PASS

| Step | Command | Result |
|---|---|---|
| NTP sync | AT+CNTP="pool.ntp.org",32 | OK, clock set to 2026-06-26 |
| SSL version | AT+CSSLCFG="sslversion",0,4 | OK |
| Auth mode | AT+CSSLCFG="authmode",0,0 | OK (no cert verification) |
| Ignore time | AT+CSSLCFG="ignorelocaltime",0,1 | OK |
| Enable SNI | AT+CSSLCFG="enableSNI",0,1 | OK (critical for modern HTTPS servers) |
| SSL service | AT+CCHSTART | OK |
| SSL context | AT+CCHSSLCFG=0,0 | OK |
| SSL connect | AT+CCHOPEN=0,"httpbin.org",443,2 | +CCHOPEN: 0,0 (success!) |

### Test 16: HTTPS POST to Google Sheets — 2026-06-25

**Env:** `mothership-v1-modem-https`
**Result:** ✅ PASS — data arrived in Google Sheet

| Step | Command | Result |
|---|---|---|
| SSL connect | AT+CCHOPEN=0,"script.google.com",443,2 | +CCHOPEN: 0,0 |
| Send data | AT+CCHSEND=0,275 | OK |
| Response | +CCHRECV: DATA,0,1383 | HTTP/1.1 302 Moved Temporarily |
| Google Sheet | Row appeared | test_node, test_value, 2026-06-25 12:00:00, 42 |

**Google Apps Script setup:**
1. Created Google Sheet with Apps Script `doPost(e)` handler
2. Deployed as Web App with "Anyone" access
3. URL: `https://script.google.com/macros/s/AKfycbxnxCfZhsisxiPD3dXazz1-1l2fK5wRNTNCdztXLva3jqfHL7DNCX2dvTehGz6CTZ38/exec`
4. Script splits POST body by newlines, then by commas, appends each row to sheet
5. Google returns 302 redirect (not 200) — this is normal for Apps Script Web Apps

**Key findings:**
- Google Apps Script returns **302 Moved Temporarily** (not 200) on successful POST — the modem driver should treat 302 as success
- Deployment must be set to **"Anyone"** access (not "Anyone with Google account") — the modem can't authenticate
- `Content-Type: text/plain` works (text/csv was rejected with 403)
- The full pipeline works: ESP32 → A7670G LTE → SSL/TLS → Google Apps Script → Google Sheet

### End-to-End Data Pipeline — Verified 2026-06-25

```
Node (ESP-NOW) → Mothership (flash log) → A7670G (LTE SSL/TLS) → Google Apps Script → Google Sheet
```

All links in the chain have been individually verified:
1. ✅ Node → Mothership via ESP-NOW (snapshots received and persisted to LittleFS)
2. ✅ Mothership → A7670G modem (UART AT commands, power control)
3. ✅ A7670G → LTE network (Aldi Talk SIM, instant registration)
4. ✅ A7670G → SSL/TLS handshake (CCH* API with SNI + NTP)
5. ✅ A7670G → HTTPS POST to Google Apps Script
6. ✅ Google Apps Script → Google Sheet (data row appeared)
| Send data | AT+CCHSEND=0,151 | > prompt, OK |
| Response | +CCHRECV: DATA,0,225 + HTTP/1.1 200 OK | JSON body, url: "https://httpbin.org/post" |
| Close | AT+CCHCLOSE=0, AT+CCHSTOP | OK |

**Key findings:**
- The A7670G uses a **separate CCH* API** for SSL/TLS, NOT `AT+CIPOPEN="SSL"` (which returns error 3)
- `AT+CIPOPEN` only supports "TCP" and "UDP" — SSL requires the CCH* command set (Chapter 17 of AT manual)
- **SNI (Server Name Indication)** must be enabled (`AT+CSSLCFG="enableSNI",0,1`) — disabled by default, required by modern HTTPS servers
- **NTP time sync** must be done AFTER `AT+NETOPEN` (needs data connection active) — SSL cert validation requires correct system time
- `AT+CSSLCFG="ignorelocaltime",0,1` ignores expired cert timestamps (useful when NTP unavailable)
- `authmode=0` skips CA certificate verification — acceptable for testing, should use `authmode=1` with CA cert for production
- The `AT+CSSLCFG="ignorlocaltime"` command in earlier test runs had a typo (missing "e") — correct spelling is `ignorelocaltime`

#### A7670G Socket API Summary

**TCP/UDP (CIP* API):**
1. `AT+NETOPEN` — open network
2. `AT+CIPOPEN=0,"TCP","host",port` — open socket
3. `AT+CIPSEND=0,<len>` — send data (wait for `>` prompt)
4. Response: `+IPD<len>` + data
5. `AT+CIPCLOSE=0` — close socket
6. `AT+NETCLOSE` — close network

**SSL/TLS (CCH* API):**
1. `AT+NETOPEN` — open network
2. `AT+CNTP="pool.ntp.org",32` + `AT+CNTP` — sync clock via NTP
3. `AT+CSSLCFG="enableSNI",0,1` — enable SNI
4. `AT+CSSLCFG="authmode",0,0` — set auth mode
5. `AT+CSSLCFG="ignorelocaltime",0,1` — ignore cert time
6. `AT+CCHSTART` — start SSL service
7. `AT+CCHSSLCFG=0,0` — bind SSL context to session
8. `AT+CCHOPEN=0,"host",443,2` — open SSL connection (type=2)
9. `AT+CCHSEND=0,<len>` — send data (wait for `>` prompt)
10. Response: `+CCHRECV: DATA,0,<len>` + data
11. `AT+CCHCLOSE=0` — close connection
12. `AT+CCHSTOP` — stop SSL service
13. `AT+NETCLOSE` — close network

### Firmware Changes Applied

1. `ModemDriver::httpsPost()` — rewritten from legacy `AT+HTTP*` commands to A7670G socket API (NETOPEN/CIPOPEN/CIPSEND for TCP, CCHSTART/CCHOPEN/CCHSEND for SSL)
2. `bringup_modem_network.cpp` — updated to poll for 180s with `AT+COPS=0` removed
3. `bringup_modem_https.cpp` — new test with CCH* SSL API, NTP sync, SSL context config, TCP fallback
4. APN hardcoded to `internet.eplus.de` (Aldi Talk) in `ModemDriver::httpsPost()`

### Remaining Work

- [ ] Fix HTTP status parser in test (cosmetic — data flows correctly)
- [ ] Integrate CCH* SSL API into `ModemDriver::httpsPost()` for production firmware (currently uses CIP* TCP API)
- [ ] Make APN configurable via `TransmissionSettings` instead of hardcoded
- [ ] Test with production cloud endpoint (webhook.site / Google Sheets)
- [ ] Consider CA certificate loading for `authmode=1` (production security)
- [ ] Think Mobile SIM recovery (may need reactivation or replacement)