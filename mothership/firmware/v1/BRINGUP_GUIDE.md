# Mothership V1 PCB Bringup Guide

**Target hardware:** Mothership V1 PCB (ESP32-WROOM-based, power-gated hub)  
**Firmware path:** `mothership/firmware/v1/`  
**Date:** 2026-06-15

---

## 1. Overview

This guide walks through bringing up a fresh V1 mothership PCB one subsystem at a time. Each bringup test validates a specific hardware block before moving on. The tests are ordered so that earlier tests confirm the power and communication rails that later tests depend on.

### What you need

| Item | Purpose |
|---|---|
| Mothership V1 PCB | The board under test |
| ESP32-WROOM module (or dev board for initial bench testing) | Microcontroller |
| USB cable (micro-USB or USB-C per PCB design) | Power, serial, and flashing |
| Multimeter | Probe voltage rails and GPIO levels |
| MicroSD card (FAT32, 1–32 GB) | SD card bringup test |
| DS3231 module (if testing without PCB RTC) | RTC alarm bringup test |
| Second ESP32 running node firmware or a known-good ESP-NOW sender | ESP-NOW bringup test |
| PlatformIO + VS Code | Build and flash environment |

### Test order and dependencies

```
PWR_HOLD ──► Config Latch ──► RTC Alarm ──► SD Card ──► Battery ADC ──► ESP-NOW ──► Wake Reason ──► Modem Power
   │              │                │              │            │             │              │               │
   │              │                │              │            │             │              │               │
   └─ VSYS must hold  └─ GPIO works  └─ I2C works  └─ SPI works  └─ ADC works  └─ WiFi works  └─ Full boot   └─ 4V rail
```

> **Critical:** Every test sketch asserts `PWR_HOLD` (GPIO26) as its very first action. If PWR_HOLD doesn't work, the board will power off immediately. **Test PWR_HOLD first.**

---

## 2. Prerequisites

### PlatformIO setup

```bash
# Install PlatformIO (if not already installed)
pip install platformio

# Clone the repo and navigate to the V1 firmware
cd mothership/firmware/v1
```

### Board selection

All environments use `board = esp32dev` in `platformio.ini`. The ESP32-WROOM module on the PCB is compatible with this board definition.

### Serial monitor settings

| Setting | Value |
|---|---|
| Baud rate | 115200 |
| DTR | 0 (disabled) |
| RTS | 0 (disabled) |

These are configured in `platformio.ini` as `monitor_dtr = 0` and `monitor_rts = 0`.

### Build and flash pattern

All bringup tests follow the same pattern:

```bash
# From mothership/firmware/v1/
pio run -e mothership-v1-XXXXX -t upload -t monitor
```

Replace `XXXXX` with the test environment name (see each test below). The `-t upload -t monitor` flags build, flash, and open the serial monitor in one step.

To exit the serial monitor: `Ctrl+]`

---

## 3. Bringup Test Sequence

### 3.1 Test 1: PWR_HOLD (`mothership-v1-pwrhold`)

**What it validates:** GPIO26 can hold the VSYS rail on. This is the most fundamental test — if PWR_HOLD doesn't work, nothing else can run.

**Build and flash:**

```bash
pio run -e mothership-v1-pwrhold -t upload -t monitor
```

**What to observe:**

- Serial output shows `PWR_HOLD=ON` and `PWR_HOLD=OFF` alternating every 15 seconds.
- With a multimeter, probe the **VSYS** rail (should be ~3.7–4.2 V depending on battery, or ~5 V if USB-powered) during the ON phase.
- Probe the **3V3_SYS** rail (should be ~3.3 V) during the ON phase.
- During the OFF phase, the board should lose power entirely. If you can still read serial output during the OFF phase, the P-FET is not cutting power.

**Expected serial output:**

```
=== Mothership V1 PWR_HOLD Bring-up ===
PWR_HOLD pin = GPIO26 (active HIGH)
Pattern: ON 15000 ms, OFF 15000 ms
Probe VSYS and 3V3_SYS rails while states toggle.
Board should stay powered during ON phase and cut power during OFF phase.
t=0 ms | PWR_HOLD=ON  (board should stay powered)
...
t=15000 ms | PWR_HOLD=OFF (board will lose power in ~seconds)
>>> If you can read this, the OFF phase did not cut power. Check P-FET gate.
```

**Pass criteria:**

- VSYS rail reads expected voltage during ON phase.
- 3V3_SYS rail reads ~3.3 V during ON phase.
- Board powers off during OFF phase (serial stops, LED goes dark).
- Board powers back on when PWR_HOLD goes HIGH again.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| Board never powers on | PWR_HOLD GPIO not connected, P-FET gate stuck | Check GPIO26 solder joint, P-FET gate trace |
| Board stays on during OFF phase | P-FET gate not discharging, or USB/force-power holding VSYS | Remove USB, check P-FET gate pull-down resistor |
| VSYS present but 3V3_SYS missing | LDO not enabled or damaged | Check 3.3 V LDO enable pin and output |
| Board resets during ON phase | VSYS sagging under load | Check battery voltage, verify VSYS under load |

---

### 3.2 Test 2: Config Latch (`mothership-v1-config-latch`)

**What it validates:** The SN74LVC2G74DCUR config latch on GPIO32 (sense) and GPIO25 (clear). The latch holds a button press until firmware explicitly clears it.

**Build and flash:**

```bash
pio run -e mothership-v1-config-latch -t upload -t monitor
```

**What to observe:**

- On boot, the sketch reads the initial latch state. If the config button was pressed before power-on, `CONFIG_WAKE` will read LOW.
- Press the config button on the PCB. The serial output should show `CONFIG_WAKE=LOW — latch is SET`.
- The sketch automatically clears the latch and reports whether the clear succeeded.
- The LED on GPIO27 blinks continuously to show the board is alive.

**Expected serial output:**

```
=== Mothership V1 Config Latch Bring-up ===
CONFIG_WAKE  = GPIO32 (INPUT, active LOW)
CONFIG_CLEAR = GPIO25 (OUTPUT, pulse HIGH 20ms)
CFG_LED      = GPIO27
Initial CONFIG_WAKE = HIGH (no config wake)
>>> Press config button to set latch, then reset to test.
t=500 ms | CONFIG_WAKE=LOW — latch is SET
Clearing latch...
After clear: CONFIG_WAKE = HIGH (OK)
```

**Pass criteria:**

- `CONFIG_WAKE` (GPIO32) reads LOW when the config button is pressed.
- `CONFIG_CLEAR` (GPIO25) pulse successfully clears the latch (CONFIG_WAKE goes HIGH).
- LED on GPIO27 blinks.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| CONFIG_WAKE always reads HIGH | Latch IC not soldered, button not connected, or GPIO32 floating | Check SN74LVC2G74 solder, verify button continuity |
| CONFIG_WAKE always reads LOW | Latch stuck set, or GPIO32 shorted to ground | Check for solder bridges on GPIO32 |
| Clear pulse doesn't reset latch | GPIO25 not connected to latch CLR pin, or pulse too short | Verify GPIO25 to CLR trace, increase `CONFIG_CLEAR_PULSE_MS` |
| LED doesn't blink | GPIO27 not connected or LED polarity reversed | Check LED and GPIO27 solder |

---

### 3.3 Test 3: RTC Alarm (`mothership-v1-rtc-alarm`)

**What it validates:** DS3231 I2C communication on GPIO21 (SDA) / GPIO22 (SCL) and Alarm 1 programming. The test arms Alarm 1 for 10 seconds from now, then polls the A1F flag until it fires.

**Build and flash:**

```bash
pio run -e mothership-v1-rtc-alarm -t upload -t monitor
```

**What to observe:**

- The sketch sets the RTC to the build timestamp, then arms Alarm 1 for 10 seconds in the future.
- Every second, it prints the current RTC time.
- When the alarm fires, it prints `[A1] FIRED at <time>`, clears the flag, and re-arms for another 10 seconds.
- This repeats indefinitely.

**Expected serial output:**

```
=== Mothership V1 DS3231 Alarm Bring-up ===
[CFG] I2C SDA=21 SCL=22 RTC=0x68 INTERVAL=10s
[RTC] set to build time: 2026-06-15 10:30:00
[RTC] now: 2026-06-15 10:30:00
[A1] armed for 2026-06-15 10:30:10
[INFO] Polling DS3231 A1F flag. Alarm should fire every 10 s.
[RTC] 2026-06-15 10:30:05
[RTC] 2026-06-15 10:30:09
[A1] FIRED at 2026-06-15 10:30:10
[A1] flag cleared
[A1] armed for 2026-06-15 10:30:20
```

**Pass criteria:**

- DS3231 found at I2C address 0x68.
- Alarm fires every 10 seconds (±1 second tolerance).
- Alarm flag clears and re-arms successfully.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| `DS3231 not found at 0x68` | I2C wiring issue, DS3231 not powered, wrong I2C address | Check SDA/SCL solder, verify DS3231 VCC, probe I2C bus with scope |
| RTC time is wrong (e.g., 2000-01-01) | DS3231 lost power, coin cell missing or dead | Install/replace CR2032 coin cell, `FORCE_SET_RTC_ON_BOOT=1` will set time |
| Alarm never fires | Alarm registers not written correctly, INTCN/A1IE not set | Check I2C writes to registers 0x07–0x0A and 0x0E |
| Alarm fires but flag doesn't clear | Status register write failing | Verify I2C write to register 0x0F |

---

### 3.4 Test 4: SD Card (`mothership-v1-sd-card`)

**What it validates:** SPI SD card interface on GPIO13 (CS), GPIO18 (SCK), GPIO19 (MISO), GPIO23 (MOSI). The test writes a file, reads it back, and deletes it.

**Build and flash:**

```bash
pio run -e mothership-v1-sd-card -t upload -t monitor
```

**What to observe:**

- Insert a FAT32-formatted MicroSD card before powering on.
- The sketch initializes SPI, mounts the SD card, prints card type and size.
- It writes a test file `/bringup_test.txt`, reads it back, and deletes it.

**Expected serial output:**

```
=== Mothership V1 SD Card Bring-up ===
SD CS=13 SCK=18 MISO=19 MOSI=23
[SD] SD.begin() OK
[SD] Card type: SDHC
[SD] Total: 7632 MB
[SD] Writing test file: /bringup_test.txt
[SD] Write OK (15 ms)
[SD] Read-back:
  Mothership V1 SD bring-up test
  Written at millis=1234
  Card size: 8011120640 bytes
[SD] Test file removed
=== SD Card bring-up PASSED ===
```

**Pass criteria:**

- `SD.begin()` succeeds.
- Card type and size are printed correctly.
- Test file is written, read back with matching content, and deleted.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| `SD.begin() FAILED` | No card inserted, bad solder joint on SPI pins, wrong CS pin | Insert card, check GPIO13/18/19/23 solder, verify card format is FAT32 |
| Card type shows `UNKNOWN` | Card is not standard SD/SDHC | Try a different card (1–32 GB, FAT32) |
| Write succeeds but read-back is garbled | SPI speed too high, signal integrity | Reduce `SD_SPI_SPEED` in the test sketch (try 10 MHz) |
| Write fails | Card is write-protected or full | Check lock switch on SD adapter, try a fresh card |

---

### 3.5 Test 5: Battery ADC (`mothership-v1-battery-adc`)

**What it validates:** GPIO34 ADC with the 220 kΩ / 100 kΩ voltage divider, and GPIO35 for modem power-good status.

**Build and flash:**

```bash
pio run -e mothership-v1-battery-adc -t upload -t monitor
```

**What to observe:**

- The sketch reads the battery ADC every 2 seconds and prints the calculated voltage.
- It also reads GPIO35 (modem power-good) — this will read LOW when the modem rail is off.
- With a known battery voltage (measure with multimeter), verify the ADC reading is within ±0.1 V.

**Expected serial output:**

```
=== Mothership V1 Battery ADC Bring-up ===
BATTERY_ADC = GPIO34 (ADC1, 12-bit, 0–3.6 V)
MODEM_PG    = GPIO35 (input-only, external pull-up)
Divider: R1=220 kΩ, R2=100 kΩ, scale=0.00257 V/LSB
Samples per reading: 16
Reading battery voltage every 2 seconds...
t=2000 ms | V_bat=4.023 V | raw_avg=1567.2 | ESP_PG=LOW (power-bad or rail off)
t=4000 ms | V_bat=4.018 V | raw_avg=1565.1 | ESP_PG=LOW (power-bad or rail off)
```

**Pass criteria:**

- Battery voltage reads within ±0.1 V of multimeter measurement.
- Raw ADC value is reasonable (not stuck at 0 or 4095).
- `ESP_PG` reads LOW when modem rail is off (expected default state).

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| V_bat always reads 0 V | GPIO34 not connected, divider missing | Check voltage divider solder, verify GPIO34 connection |
| V_bat reads ~3.3 V regardless of battery | Divider R2 missing or open | Check R2 (100 kΩ) solder |
| V_bat is wildly inaccurate | ADC attenuation wrong, or divider values don't match firmware | Verify resistor values match `BAT_DIVIDER_R1`/`BAT_DIVIDER_R2` in `pins.h` |
| ESP_PG always HIGH | GPIO35 has external pull-up and modem rail is off — check if pull-up is too strong | Verify TPS63020 PG output drives GPIO35 correctly |

---

### 3.6 Test 6: ESP-NOW Basic (`mothership-v1-espnow-basic`)

**What it validates:** ESP-NOW receive on channel 11 without a WiFi AP. This requires a second ESP32 sending packets on the same channel.

**Build and flash:**

```bash
pio run -e mothership-v1-espnow-basic -t upload -t monitor
```

**What to observe:**

- The sketch initializes WiFi in STA mode on channel 11 and registers an ESP-NOW receive callback.
- It prints a heartbeat every second showing it's listening.
- When a packet arrives, it prints the sender MAC, length, and first 32 bytes as hex.

**Expected serial output:**

```
=== Mothership V1 ESP-NOW Basic Bring-up ===
Channel: 11
[WiFi] STA mode, channel 11
[ESP-NOW] init OK
[ESP-NOW] receive callback registered
[ESP-NOW] Listening for packets... Send from a node to test.
t=1000 ms | ESP-NOW listening on ch11
[ESP-NOW] RX from AA:BB:CC:DD:EE:FF | len=24 | data: 01 02 03 ...
t=2000 ms | ESP-NOW listening on ch11
```

**Pass criteria:**

- ESP-NOW initializes successfully.
- Packets from a node or second ESP32 are received and printed.
- No `esp_now_init()` or `esp_now_register_recv_cb()` errors.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| `ESP-NOW init FAILED` | WiFi not initialized, or ESP-NOW already initialized | Ensure `WiFi.mode(WIFI_STA)` is called before `esp_now_init()` |
| No packets received | Sender on wrong channel, MAC not matching, or sender not transmitting | Verify sender is on channel 11, check sender code |
| Packets received but garbled | Protocol mismatch or wrong struct size | Verify sender and receiver use the same `protocol.h` |
| `Add broadcast peer failed` | Peer already exists from previous run | Not a critical error — can be ignored |

---

### 3.7 Test 7: Wake Reason (`mothership-v1-wake-reason`)

**What it validates:** The full boot sequence: PWR_HOLD assertion → wake reason detection → branch → shutdown. Tests all three wake sources (RTC alarm, config button, USB service).

**Build and flash:**

```bash
pio run -e mothership-v1-wake-reason -t upload -t monitor
```

**What to observe:**

The sketch detects the wake reason and takes different actions:

| Wake reason | How to trigger | Expected behavior |
|---|---|---|
| `WAKE_CONFIG_BUTTON` | Press config button before/during boot | Fast LED blink (10 × 100 ms), latch cleared |
| `WAKE_RTC_ALARM` | Set DS3231 alarm before boot | Slow LED blink (5 × 500 ms), alarm flag cleared |
| `WAKE_USB_SERVICE` | Neither button nor alarm (just USB power) | Solid LED on |

After the LED pattern, the sketch counts down 10 seconds and releases PWR_HOLD.

**Expected serial output (config button wake):**

```
=== Mothership V1 Wake-Reason Detection Bring-up ===
[RTC] DS3231 found
Wake reason: CONFIG_BUTTON
>>> Config button wake — clearing latch
After clear: CONFIG_WAKE = HIGH (OK)
Shutting down in 10000 ms...
```

**Pass criteria:**

- Wake reason is correctly detected for each trigger method.
- Config latch is properly read and cleared.
- RTC alarm flag is properly read and cleared.
- LED pattern matches the detected wake reason.
- Board powers off after the countdown (PWR_HOLD released).

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| Always detects `WAKE_USB_SERVICE` | Config latch not set, RTC alarm not set | Press config button before reset, or set RTC alarm first |
| Config latch clear fails | GPIO25 not connected to latch CLR | Check GPIO25 solder and trace to SN74LVC2G74 |
| RTC alarm not detected | DS3231 not initialized, or alarm not armed | Run RTC alarm bringup test first to verify I2C |
| Board doesn't power off after countdown | PWR_HOLD not releasing, or USB holding VSYS | Disconnect USB, verify P-FET gate control |

---

### 3.8 Test 8: Modem Power (`mothership-v1-modem-power`)

**What it validates:** TPS63020 buck-boost regulator enable on GPIO33 and power-good feedback on GPIO35. **Does NOT boot the modem** — only tests the power rail.

**Build and flash:**

```bash
pio run -e mothership-v1-modem-power -t upload -t monitor
```

**What to observe:**

The test runs in four phases:

1. **Phase 1:** Check initial state (4V_EN=LOW, ESP_PG should be LOW)
2. **Phase 2:** Enable 4V_EN (GPIO33 HIGH), wait for ESP_PG to go HIGH (up to 5 seconds)
3. **Phase 3:** Hold rail on for 5 seconds — probe MODEM_VBAT_3V9 with multimeter (expect ~3.9 V)
4. **Phase 4:** Disable 4V_EN (GPIO33 LOW), verify ESP_PG goes LOW

**Expected serial output:**

```
=== Mothership V1 Modem Power Rail Bring-up ===
4V_EN   = GPIO33 (OUTPUT, active HIGH)
ESP_PG  = GPIO35 (INPUT, external pull-up)
PG timeout = 5000 ms
Rail-on time = 5000 ms

--- Phase 1: Initial state (4V_EN=LOW) ---
ESP_PG = LOW (expected LOW when rail off)

--- Phase 2: Enabling 4V_EN (rail ON) ---
4V_EN = HIGH at t=800 ms
ESP_PG = HIGH after 150 ms — power rail is GOOD

--- Phase 3: Holding rail ON for 5000 ms ---
Probe MODEM_VBAT_3V9 rail with multimeter. Expected ~3.9 V.

--- Phase 4: Disabling 4V_EN (rail OFF) ---
4V_EN = LOW at t=5950 ms
ESP_PG = LOW (expected LOW after rail off)
```

**Pass criteria:**

- ESP_PG reads LOW when 4V_EN is LOW (rail off).
- ESP_PG goes HIGH within 5 seconds of enabling 4V_EN.
- MODEM_VBAT_3V9 rail measures ~3.9 V (±0.2 V) when enabled.
- ESP_PG goes LOW after disabling 4V_EN.

**Common failure modes:**

| Symptom | Likely cause | Fix |
|---|---|---|
| ESP_PG never goes HIGH | TPS63020 not enabled, inductor missing, output shorted | Check GPIO33 solder, verify inductor and output capacitor |
| ESP_PG always HIGH (even in Phase 1) | GPIO35 external pull-up too strong, or TPS63020 PG output floating | Verify PG pin connection and pull-up/pull-down resistors |
| MODEM_VBAT_3V9 voltage wrong | TPS63020 output not regulating correctly | Check inductor value, input voltage, and output capacitor |
| Board resets when 4V_EN enabled | Inrush current causing VSYS sag | Add soft-start or verify VSYS supply can handle the load |

---

## 4. PCB Pin Reference

All GPIO assignments match the V1 schematic. Defined in `src/system/pins.h` with `#ifndef` guards for build-time overrides.

| GPIO | Net | Direction | Function |
|---:|---|---|---|
| 26 | `PWR_HOLD` | OUTPUT | Main power hold — assert HIGH immediately at boot |
| 32 | `CONFIG_WAKE` | INPUT | Config latch sense — LOW = config wake requested |
| 25 | `CONFIG_CLEAR` | OUTPUT | Config latch clear — pulse HIGH for 20 ms to clear |
| 27 | `CFG_LED` | OUTPUT | Config/status LED |
| 33 | `4V_EN` | OUTPUT | TPS63020 buck-boost enable (modem power rail) |
| 35 | `MODEM_PG` | INPUT | TPS63020 power-good (input-only, external pull-up) |
| 14 | `MODEM_PWRKEY` | OUTPUT | A7670G PWRKEY (NMOS gate) |
| 4 | `MODEM_STATUS` | INPUT | A7670G STATUS output |
| 34 | `BATTERY_ADC` | INPUT | Battery voltage via 220 kΩ / 100 kΩ divider (input-only) |
| 21 | `SDA` | I/O | I²C data (DS3231) |
| 22 | `SCL` | OUTPUT | I²C clock (DS3231) |
| 18 | `SD_SCK` | OUTPUT | SD SPI clock |
| 19 | `SD_MISO` | INPUT | SD SPI MISO |
| 23 | `SD_MOSI` | OUTPUT | SD SPI MOSI |
| 13 | `SD_CS` | OUTPUT | SD chip select |
| 17 | `MODEM_TX` | OUTPUT | UART2 TX → modem RXD |
| 16 | `MODEM_RX` | INPUT | UART2 RX ← modem TXD |

### Voltage divider constants

| Parameter | Value | Notes |
|---|---|---|
| `BAT_DIVIDER_R1` | 220 kΩ | Upper resistor |
| `BAT_DIVIDER_R2` | 100 kΩ | Lower resistor |
| `BAT_ADC_VREF` | 3.3 V | ADC reference voltage |
| `BAT_ADC_MAX` | 4095 | 12-bit ADC |
| `BAT_ADC_SAMPLES` | 16 | Oversampling count |

### Timing constants

| Parameter | Value | Notes |
|---|---|---|
| `CONFIG_CLEAR_PULSE_MS` | 20 ms | Latch clear pulse width |
| `MODEM_PG_TIMEOUT_MS` | 5000 ms | Power-good wait timeout |
| `MODEM_PWRKEY_ON_MS` | 1100 ms | A7670 PWRKEY pulse |
| `MODEM_BOOT_WAIT_MS` | 5000 ms | Modem boot wait |
| `SYNC_WINDOW_MS` | 60000 ms | ESP-NOW sync listen window |

---

## 5. Power Domain Reference

The V1 PCB has four power domains. Understanding these is essential for debugging.

```
                    ┌──────────────┐
   Battery ────────►│  KEEP_ALIVE  │  Always-on: DS3231, wake logic, pull-ups
   (3.7–4.2 V)      │   domain     │  Current: ~μA range
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
   PWR_HOLD ◄──────│    VSYS      │  Switched main rail: ESP32, SD, I²C
   (GPIO26)         │   domain     │  Enabled by PWR_HOLD or wake logic
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │  3V3_SYS     │  3.3 V LDO output: ESP32 VDD, SD, I²C
                    │   domain     │  Fed from VSYS via LDO
                    └──────────────┘

                    ┌──────────────┐
   VSYS ───────────►│ MODEM_VBAT   │  TPS63020 buck-boost output: ~3.9 V
   (via TPS63020)   │  _3V9 domain  │  Fed from VSYS, enabled by 4V_EN
                    └──────────────┘  Powers: A7670G modem
```

| Domain | Source | Voltage | Powers | Control |
|---|---|---|---|---|
| **KEEP_ALIVE** | Battery direct | 3.0–4.2 V | DS3231, wake logic OR gate, pull-ups | Always on |
| **VSYS** | Battery via P-FET | 3.0–4.2 V | ESP32-WROOM, SD card, I²C peripherals | PWR_HOLD (GPIO26) or wake logic |
| **3V3_SYS** | VSYS via LDO | 3.3 V | ESP32 VDD, SD card VDD, I²C pull-ups | Derived from VSYS |
| **MODEM_VBAT_3V9** | VSYS via TPS63020 | ~3.9 V | A7670G modem | 4V_EN (GPIO33) |

### Power sequencing

1. **Boot:** Wake logic (RTC alarm, config button, or USB) drives LOGIC high → P-FET enables VSYS → ESP32 boots
2. **Hold:** ESP32 asserts PWR_HOLD (GPIO26 HIGH) immediately → VSYS stays on
3. **Shutdown:** ESP32 releases PWR_HOLD (GPIO26 LOW) → P-FET disables VSYS → board powers off
4. **Modem (optional):** After VSYS is stable, enable 4V_EN (GPIO33 HIGH) → TPS63020 ramps up → ESP_PG (GPIO35) goes HIGH → modem can be booted

---

## 6. Troubleshooting

### Board won't stay on

1. **Check PWR_HOLD first.** GPIO26 must go HIGH within milliseconds of boot. Every test sketch does this as the first action.
2. **Check VSYS rail.** Probe the VSYS test point — should read battery voltage when PWR_HOLD is HIGH.
3. **Check 3V3_SYS.** Should read ~3.3 V when VSYS is present.
4. **Check P-FET.** The P-FET gate should be pulled LOW when PWR_HOLD is HIGH (inverted logic through the gate driver).
5. **USB power bypass.** If USB is connected, VBUS_USB may hold VSYS on regardless of PWR_HOLD. Disconnect USB to test power-gating.

### I2C not found (DS3231)

1. **Check wiring.** SDA=GPIO21, SCL=GPIO22. Verify continuity.
2. **Check pull-ups.** I²C needs 4.7 kΩ pull-ups on both SDA and SCL.
3. **Check DS3231 power.** The DS3231 is on the KEEP_ALIVE domain and should always have power.
4. **Scan the bus.** Use an I²C scanner sketch to enumerate all devices on the bus.

### SD card init fails

1. **Check card format.** Must be FAT32 (not exFAT or NTFS).
2. **Check card size.** Use 1–32 GB cards. Larger cards may not be SDHC-compatible.
3. **Check SPI pins.** CS=GPIO13, SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23.
4. **Check card insertion.** The card should click into the slot.
5. **Reduce SPI speed.** Try `SD_SPI_SPEED=10000000` (10 MHz) if 40 MHz fails.

### Battery ADC reads wrong

1. **Verify divider values.** R1=220 kΩ, R2=100 kΩ. Measure with multimeter.
2. **Check ADC attenuation.** Must be `ADC_11db` for 0–3.6 V range.
3. **Calibrate.** ESP32 ADC is non-linear. For production, consider `esp_adc_cal` or a lookup table.
4. **Check GPIO34.** It's input-only — no internal pull-up.

### ESP-NOW not receiving

1. **Check channel.** Both sender and receiver must be on channel 11.
2. **Check WiFi mode.** Must be `WIFI_STA` (no AP needed for receive).
3. **Check sender.** Verify the sender is actually transmitting on channel 11.
4. **Check protocol.** Both sides must use the same `protocol.h` struct definitions.

### Config latch not working

1. **Check SN74LVC2G74DCUR.** Verify the latch IC is soldered and powered.
2. **Check button.** Verify the config button connects D to VCC and CLK to the wake logic.
3. **Check GPIO32.** Must be configured as INPUT with no pull-up (external pull-up on PCB).
4. **Check GPIO25.** Must pulse HIGH for at least 20 ms to clear the latch.

### Board powers off unexpectedly

1. **PWR_HOLD released too early.** Ensure `assertPwrHold()` is called before any other peripheral init.
2. **Brownout.** If VSYS sags below the ESP32 brownout threshold (~2.7 V), the ESP32 resets. Check battery voltage under load.
3. **Watchdog.** If the ESP32 doesn't feed the watchdog, it resets. The bringup sketches don't use FreeRTOS tasks, so this is unlikely.
4. **Crash.** Check serial output for exception traces or stack dumps.