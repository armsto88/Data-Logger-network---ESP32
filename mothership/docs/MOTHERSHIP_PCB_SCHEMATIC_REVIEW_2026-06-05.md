# Mothership PCB Schematic Review — 2026-06-05

**Date:** 2026-06-05  
**Status:** Pre-order review — action items need confirmation before PCB fab  
**Scope:** Full mothership schematic review covering input protection, power switching, config latch, regulators, RTC, SD card, solar charging, modem power, SIM routing, USB programming, and ESP32 MCU core

---

## Signal Name Mapping (Schematic ↔ Design Notes)

| Schematic Name | Design Note Name | Purpose |
|---|---|---|
| `LOGIC` | `WAKE_START` | Shared wake/hold logic node |
| `INT_RTC` | `RTC_INT_N` | DS3231 alarm wake output |
| `VSYS` | `VSYS_SW` | Switched system rail |
| `3V3_SYS` | `MAIN_3V3` | Switched 3.3 V rail |
| `VOLT` | `BATTERY_VSENSE` | Battery voltage divider midpoint |
| `BAT_BUS` | — | Pre-fuse battery/solar bus |
| `RAW_BAT` | — | Post-fuse protected battery rail |
| `FORCE_POWER` | — | Manual service/debug override |
| `CONFIG_SET_N` | — | Config latch preset (active low) |
| `CONFIG_CLEAR_N` | — | Config latch clear (active low) |
| `VBUS_USB` | `USB_FORCE_ON` | USB service wake/power rail |
| `4V_EN` | — | TPS63020 enable (GPIO33) |
| `ESP_PG` | — | TPS63020 power-good (GPIO35) |
| `M_PWRK` | — | Modem PWRKEY (GPIO14) |
| `M_STS` | — | Modem status (GPIO4) |
| `CFG_LED` | — | Config LED (GPIO27) |
| `PROG_EN` | — | ESP32 EN/CHIP_PU |
| `PROG_BOOT` | — | ESP32 IO0/BOOT |

---

## Power Domain Summary

| Rail | Source | Purpose |
|---|---|---|
| `BAT_BUS` | Solar charger BAT output | Pre-fuse battery/solar bus |
| `RAW_BAT` | BAT_BUS → polyfuse → RAW_BAT | Protected battery rail |
| `KEEP_ALIVE` | RAW_BAT → TPL720F33 | Always-on 3.3 V for RTC/latch |
| `VSYS` | RAW_BAT → AO3407A P-FET → VSYS | Switched system rail |
| `3V3_SYS` | VSYS → AP2112K-3.3 | Switched 3.3 V for ESP32/SD/active logic |
| `4V` | VSYS → TPS63020 → ~3.9 V | Modem supply rail |
| `VBUS_USB` | USB-C VBUS → fuse | USB service wake only (not charging) |

---

# 1. Input Protection

## Power path

```text
Solar charger BAT output → BAT_BUS → 1812L110PR polyfuse → RAW_BAT → downstream
```

- `BAT_BUS` is the pre-fuse battery/solar bus.
- `RAW_BAT` is the post-fuse protected rail used by all downstream loads.
- An AO3407A P-FET is in the input/protection section. Source faces the battery side.

## Battery voltage sensing

```text
RAW_BAT → R157 220k → VOLT → R158 100k → GND
```

At 4.2 V: VOLT ≈ 1.31 V, divider current ≈ 13 µA.

Local ADC filter:

```text
VOLT → 2.2 kΩ → VOLT_ESP → 100 nF → GND
```

GPIO34 (`VOLT_ESP`) is ADC1-capable and input-only — suitable for battery sensing.

---

# 2. Power Switching Logic

## Main power switch

```text
RAW_BAT → AO3407A P-FET → VSYS
```

- Gate pull-up to `RAW_BAT` → default OFF.
- `SW9` is a hard enable/cut/storage switch in the gate path. Open = board cannot wake.

## Shared wake/hold logic

The shared control node is `LOGIC`. Wake sources:

| Signal | Role |
|---|---|
| `INT_RTC` | RTC scheduled wake |
| `PWR_HOLD` | ESP32 firmware hold (GPIO26) |
| `VBUS_USB` | USB/service wake |
| `FORCE_POWER` | Manual service override |
| `CONFIG button via D1` | Config-mode wake |

`PWR_HOLD` must be asserted very early in firmware boot.

`FORCE_POWER` pulls `LOGIC` active but does **not** set the config latch — correct for service/debug without triggering config mode.

---

# 3. Config Wake Latch

## Component

TI `SN74LVC2G74DCUR` flip-flop, powered from `KEEP_ALIVE`.

| U52 pin | Connection |
|---|---|
| `VCC` | `KEEP_ALIVE` |
| `GND` | `GND` |
| `D` | `GND` |
| `CLK` | `GND` |
| `/PRE` | `CONFIG_SET_N` |
| `/CLR` | `CONFIG_CLEAR_N` |
| `Q` | Q42 gate |
| `/Q` | NC or test pad |

Only asynchronous preset/clear behaviour is used.

## Config button path

```text
KEEP_ALIVE → 100k → CONFIG_SET_N → U52 /PRE + config button → GND
```

Button press → `CONFIG_SET_N` low → latch sets → `Q` high → Q42 on → `CONFIG_WAKE_PIN` low.

## D1 isolation diode

```text
LOGIC ----|<|---- CONFIG_SET_N
anode             cathode/bar
```

D1 allows the config button to wake the board but blocks other wake sources from falsely setting the latch.

## Clear path: Q41 (2N7002)

```text
CONFIG_CLEAR_PIN (GPIO25) → Q41 gate
Q41 drain → CONFIG_CLEAR_N / U52 /CLR
Q41 source → GND
```

`CONFIG_CLEAR_N` has 100k pull-up to `KEEP_ALIVE`. Q41 gate has 100k pulldown to GND.

## Sense path: Q42 (2N7002)

```text
U52 Q → Q42 gate
CONFIG_WAKE_PIN (GPIO32) → Q42 drain
Q42 source → GND
```

`CONFIG_WAKE_PIN` has 10k pull-up to `3V3_SYS` (not `KEEP_ALIVE` — avoids back-powering ESP32).

Firmware reads: `digitalRead(CONFIG_WAKE_PIN) == LOW` → config wake requested.

---

# 4. Regulators

## AP2112K-3.3 (VSYS → 3V3_SYS)

- Input caps: 10 µF + 100 nF
- Output caps: 100 nF + 10 µF + 100 µF/10 V
- 100 µF bulk cap supports ESP32 Wi-Fi/AP and SD card current bursts.

## TPL720F33 (RAW_BAT → KEEP_ALIVE)

- Input caps: 10 µF + 100 nF
- Output caps: 10 µF + 100 nF
- Always-on, low-current only. Must not power ESP32, SD, CH340, or LEDs.

---

# 5. DS3231 RTC

- Powered from `KEEP_ALIVE` (not `3V3_SYS`).
- I²C: SDA → GPIO21, SCL → GPIO22.
- **I²C pull-ups to `3V3_SYS`** (not `KEEP_ALIVE`) — avoids back-powering ESP32.
- `Int/SQW` → `INT_RTC` → shared power-switching logic.
- Firmware must clear the RTC alarm flag after wake.
- Backup cell: confirm polarity and holder footprint.

---

# 6. microSD Card

- Powered from `3V3_SYS`.
- SPI mapping:

| SD function | Net | ESP32 GPIO |
|---|---|---:|
| CLK | `SD_SCK` | GPIO18 |
| DAT0/DO | `SD_MISO` | GPIO19 |
| CMD/DI | `SD_MOSI` | GPIO23 |
| DAT3/CS | `SD_CS` | GPIO13 |

- `SD_CS` has 10k pull-up to `3V3_SYS`.
- 22 Ω series resistors on SCK, MOSI, CS. MISO is direct.
- Local decoupling: 100 nF + 10 µF near socket.
- No GPIO6–GPIO11 used (flash pins avoided).

---

# 7. Solar Charging (CN3163)

```text
Solar connector (U47) → F5 1812L050PR → CN3163 VIN → CN3163 BAT → BAT_BUS
```

- `TEMP` tied to GND (no NTC — confirm acceptable for deployment).
- `ISET` = 3.6 kΩ → ~330 mA charge current.
- `CRG` and `DONE` LEDs with 2.2 kΩ resistors.
- Input/output caps: 10 µF + 100 nF each side.
- `BAT_BUS` feeds downstream input protection → `RAW_BAT`.

---

# 8. Modem Power (TPS63020 + A7670G)

## 4 V regulator

```text
VSYS → TPS63020 → ~3.9 V modem rail
```

- Feedback divider: 680 kΩ / 100 kΩ → ~3.9 V.
- Enable: GPIO33 → `4V_EN`.
- Power-good: `ESP_PG` → GPIO35 (input-only, needs external pull-up if TPS63020 PG is open-drain).
- **PG and PS/SYNC are NOT tied together** (confirmed — was a visual misread).

## Layout priorities

- Wide copper for 4 V rail and ground return.
- Local bulk caps close to modem VBAT/GND pins.
- Keep RF trace away from regulator, inductor, SD, and digital buses.

---

# 9. SIM Card Interface

- Longest SIM trace ≈ 53 mm; target ≤ 45 mm if routing remains clean.
- **Move 22 Ω series resistors close to modem SIM pins** (not near SIM socket).
- SIM_VDD: 100 nF decoupling near SIM socket.
- SIM ESD protection recommended if SIM is user-accessible.
- Keep SIM traces grouped with solid ground return, away from switching/RF/high-current paths.

---

# 10. USB-C Programming Interface

## USB-C configuration

- CC1/CC2 each have 5.1 kΩ pulldowns to GND (correct for device/sink).
- VBUS → resettable fuse → `VBUS_USB` (service wake only, not charging).

## USB ESD — layout change required

**Move USB ESD protection close to the USB-C connector**, not near the CH340C.

Correct physical order:

```text
USB-C connector → ESD diode array → 22 Ω D+/D− resistors → CH340C
```

## CH340C

- Powered from `3V3_SYS`.
- V3 capacitor: 100 nF close to chip.
- TX/RX crossover: CH340C TXD → ESP32 RX0, CH340C RXD ← ESP32 TX0.

## Auto-program circuit

- Q14/Q15 MMBT3904 NPN transistors for RTS/DTR auto-programming.
- `PROG_EN` → ESP32 EN/CHIP_PU (10k pull-up to `3V3_SYS`).
- `PROG_BOOT` → ESP32 IO0/BOOT (10k pull-up to `3V3_SYS`).
- Manual EN and BOOT buttons pull to GND.

---

# 11. ESP32 MCU Core

## Pin mapping (confirmed)

| GPIO | Net | Function |
|---:|---|---|
| 34 | `VOLT_ESP` | Battery ADC input (input-only, ADC1) |
| 35 | `ESP_PG` | Modem power-good input (input-only) |
| 32 | `CONFIG_WAKE_PIN` | Config latch sense |
| 33 | `4V_EN` | Modem regulator enable |
| 25 | `CONFIG_CLEAR_PIN` | Config latch clear |
| 26 | `PWR_HOLD` | Main power hold |
| 27 | `CFG_LED` | Config/status LED |
| 14 | `M_PWRK` | Modem PWRKEY (**NOT GPIO12**) |
| 13 | `SD_CS` | SD chip select |
| 23 | `SD_MOSI` | SD MOSI |
| 22 | `SCL` | I²C clock |
| 21 | `SDA` | I²C data |
| 19 | `SD_MISO` | SD MISO |
| 18 | `SD_SCK` | SD clock |
| 17 | `TX2` | Modem UART TX |
| 16 | `RX2` | Modem UART RX |
| 4 | `M_STS` | Modem status |

## Key design notes

- GPIO14 is `M_PWRK` — confirmed, not GPIO12 (screenshot was a visual misread).
- GPIO34/35 are input-only with no internal pull-ups. `ESP_PG` needs external pull-up.
- I²C pull-ups (4.7 kΩ) go to `3V3_SYS`, not `KEEP_ALIVE`.
- ESP32 local decoupling: 47 µF + 10 µF + 100 nF near module.
- EN and BOOT each have 10 kΩ pull-ups to `3V3_SYS`.
- `PWR_HOLD` must be asserted very early in firmware boot.

---

# 12. Power-Domain Rules

| Net / function | Correct rail |
|---|---|
| DS3231 VCC | `KEEP_ALIVE` |
| SN74LVC2G74 VCC | `KEEP_ALIVE` |
| `CONFIG_SET_N` pull-up | `KEEP_ALIVE` |
| `CONFIG_CLEAR_N` pull-up | `KEEP_ALIVE` |
| ESP32-WROOM | `3V3_SYS` |
| SD card VDD | `3V3_SYS` |
| SD CS pull-up | `3V3_SYS` |
| I²C SDA/SCL pull-ups | `3V3_SYS` |
| `CONFIG_WAKE_PIN` pull-up | `3V3_SYS` |
| CH340C VCC | `3V3_SYS` |
| EN/BOOT pull-ups | `3V3_SYS` |

**Rule:** Signals connected to ESP32 GPIOs must not be pulled high by `KEEP_ALIVE` while the ESP32 is unpowered, to avoid back-powering through input protection diodes.

---

# 13. Firmware Expectations

```cpp
// Early in boot — assert power hold immediately
pinMode(PWR_HOLD_PIN, OUTPUT);
digitalWrite(PWR_HOLD_PIN, HIGH);

pinMode(CONFIG_WAKE_PIN, INPUT);
pinMode(CONFIG_CLEAR_PIN, OUTPUT);
digitalWrite(CONFIG_CLEAR_PIN, LOW);  // keep clear transistor off

bool configWake = digitalRead(CONFIG_WAKE_PIN) == LOW;

if (configWake) {
    // start AP/config mode
    digitalWrite(CONFIG_CLEAR_PIN, HIGH);
    delay(20);
    digitalWrite(CONFIG_CLEAR_PIN, LOW);
}
```

`PWR_HOLD` must be asserted before slow operations (SD init, Wi-Fi AP, RTC comms).

---

# 14. Components Not Previously in Design Notes

| Ref | Part | Purpose |
|---|---|---|
| AO3407A | P-FET | Main power switch (RAW_BAT → VSYS) |
| SN74LVC2G74DCUR | D flip-flop | Config wake latch |
| 2N7002 (×2) | N-FET | Q41 clear path, Q42 sense path |
| D1 | Schottky | Config wake isolation diode |
| R12 | 100 Ω | Gate resistor (inherited from node design) |
| AP2112K-3.3 | LDO | VSYS → 3V3_SYS |
| TPL720F33 | LDO | RAW_BAT → KEEP_ALIVE |
| CN3163 | Charger | Solar Li-ion charger (~330 mA) |
| TPS63020 | Buck-boost | VSYS → ~3.9 V modem rail |
| A7670G | Modem | LTE Cat-1 cellular |
| CH340C | USB-UART | Programming and serial debug |

---

# 15. Critical Checks Before PCB Order

## MOSFET and diode orientation

- [ ] AO3407A source/drain orientation in input protection section
- [ ] AO3407A source/drain orientation in main power switch (RAW_BAT → VSYS)
- [ ] D1 orientation: anode to `LOGIC`, cathode/bar to `CONFIG_SET_N`
- [ ] Q41 (2N7002): source → GND, drain → `CONFIG_CLEAR_N`, gate → `CONFIG_CLEAR_PIN`
- [ ] Q42 (2N7002): source → GND, drain → `CONFIG_WAKE_PIN`, gate → U52 `Q`
- [ ] Q14/Q15 (MMBT3904): verify collector/emitter orientation against ESP32 auto-program reference

## Rail domain separation

- [ ] All downstream loads use `RAW_BAT`, not pre-fuse `BAT_BUS`
- [ ] `KEEP_ALIVE` only powers RTC, latch, and wake pull-ups
- [ ] No ESP32 GPIO pull-up is tied to `KEEP_ALIVE`
- [ ] I²C pull-ups go to `3V3_SYS`
- [ ] `CONFIG_WAKE_PIN` pull-up goes to `3V3_SYS`
- [ ] SD card powered from `3V3_SYS`
- [ ] CH340C powered from `3V3_SYS`
- [ ] EN/BOOT pull-ups go to `3V3_SYS`

## USB and programming

- [ ] **Move USB ESD protection close to USB-C connector** (not near CH340C)
- [ ] USB D+/D− 22 Ω resistors placed after ESD, toward CH340C
- [ ] USB-C CC1/CC2 both have 5.1 kΩ pulldowns to GND
- [ ] CH340C TXD → ESP32 RX0, RXD ← ESP32 TX0 crossover confirmed
- [ ] CH340C V3 capacitor (100 nF) close to chip
- [ ] Auto-program circuit verified against ESP32 reference design
- [ ] EN and BOOT pull-ups: 10 kΩ to `3V3_SYS`

## ESP32 core

- [ ] GPIO14 confirmed as `M_PWRK` (not GPIO12)
- [ ] GPIO34/35 used only as inputs (no internal pull-ups)
- [ ] `ESP_PG` (GPIO35) has valid external pull-up if TPS63020 PG is open-drain
- [ ] `4V_EN` (GPIO33) default state at boot does not unintentionally power the modem
- [ ] No GPIO6–GPIO11 used (flash pins avoided)
- [ ] `PWR_HOLD` asserted early in firmware boot
- [ ] CFG_LED polarity confirmed in firmware

## Regulators and decoupling

- [ ] AP2112K-3.3 footprint pinout confirmed
- [ ] TPL720F33 footprint pinout confirmed
- [ ] TPS63020 footprint pinout confirmed
- [ ] TPS63020 feedback divider gives intended ~3.9 V
- [ ] PG and PS/SYNC are **not** tied together
- [ ] ESP32 local decoupling: 47 µF + 10 µF + 100 nF near module
- [ ] SD local decoupling: 100 nF + 10 µF near socket

## Solar charging

- [ ] CN3163 footprint pinout confirmed
- [ ] `TEMP` tied to GND acceptable for deployment
- [ ] `ISET` = 3.6 kΩ → ~330 mA suitable for panel and battery
- [ ] CRG/DONE LED polarity confirmed
- [ ] F5 polyfuse rating suitable for solar input current

## Modem and SIM

- [ ] A7670G PWRKEY polarity and pulse timing confirmed
- [ ] UART TX/RX crossover confirmed
- [ ] Modem IO voltage and level-shifter direction confirmed
- [ ] SIM series resistors moved close to modem SIM pins
- [ ] SIM trace length target ≤ 45 mm if routing remains clean
- [ ] SIM_VDD 100 nF decoupling near SIM socket
- [ ] SIM ESD protection strategy confirmed
- [ ] RF trace routing: away from regulator, inductor, SD, digital buses

## Config latch

- [ ] U52 `/Q` is unused or test-pad only
- [ ] `CONFIG_CLEAR_N` pull-up goes to `KEEP_ALIVE`
- [ ] R12 = 100 Ω confirmed intentional
- [ ] Firmware asserts `PWR_HOLD` early after boot

## Miscellaneous

- [ ] Battery connector polarity and silkscreen confirmed
- [ ] Solar input connector polarity and silkscreen confirmed
- [ ] SW9 does not leave main FET gate floating when open
- [ ] `VBUS_USB` path does not charge or backfeed the battery
- [ ] `FORCE_POWER` does not connect to `CONFIG_SET_N`
- [ ] ADC calibration in firmware matches actual divider resistor values
- [ ] ADC trace routed away from noisy switching nodes