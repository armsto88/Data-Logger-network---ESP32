# Mothership PCB — Pre-Order Checks

Extracted from `MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md` section 15.
Work through each checkbox on the PCB layout and schematic before submitting the Gerbers.

---

## MOSFET and Diode Orientation

- [x] AO3407A source/drain orientation — input protection section
- [x] AO3407A source/drain orientation — main power switch (RAW_BAT → VSYS)
- [x] D1 orientation: anode to `LOGIC`, cathode/bar to `CONFIG_SET_N`
- [x] Q41 (2N7002): source → GND, drain → `CONFIG_CLEAR_N`, gate → `CONFIG_CLEAR_PIN`
- [x] Q42 (2N7002): source → GND, drain → `CONFIG_WAKE_PIN`, gate → U52 `Q`
- [x] Q14/Q15 (MMBT3904): collector/emitter orientation against ESP32 auto-program reference

## Rail Domain Separation

- [x] All downstream loads use `RAW_BAT`, not pre-fuse `BAT_BUS`
- [x] `KEEP_ALIVE` only powers RTC, latch, and wake pull-ups
- [x] No ESP32 GPIO pull-up is tied to `KEEP_ALIVE`
- [x] I²C pull-ups go to `3V3_SYS`
- [x] `CONFIG_WAKE_PIN` pull-up goes to `3V3_SYS`
- [x] SD card powered from `3V3_SYS`
- [x] CH340C powered from `3V3_SYS`
- [x] EN/BOOT pull-ups go to `3V3_SYS`

## USB and Programming

- [x] **Move USB ESD protection close to USB-C connector** (not near CH340C) — done
- [x] USB D+/D− 22 Ω resistors placed after ESD, toward CH340C
- [x] USB-C CC1/CC2 both have 5.1 kΩ pulldowns to GND
- [x] CH340C TXD → ESP32 RX0, RXD ← ESP32 TX0 crossover confirmed
- [x] CH340C V3 capacitor (100 nF) close to chip
- [x] Auto-program circuit verified against ESP32 reference design
- [x] EN and BOOT pull-ups: 10 kΩ to `3V3_SYS`

## ESP32 Core

- [x] GPIO14 confirmed as `M_PWRK` (not GPIO12)
- [x] GPIO34/35 used only as inputs (no internal pull-ups)
- [x] `ESP_PG` (GPIO35) has valid external pull-up if TPS63020 PG is open-drain
- [x] `4V_EN` (GPIO33) default state at boot does not unintentionally power the modem
- [x] No GPIO6–GPIO11 used (flash pins avoided)
- [x] `PWR_HOLD` asserted early in firmware boot
- [x] CFG_LED polarity confirmed in firmware

## Regulators and Decoupling

- [x] AP2112K-3.3 footprint pinout confirmed
- [x] TPL720F33 footprint pinout confirmed
- [x] TPS63020 footprint pinout confirmed
- [x] TPS63020 feedback divider gives intended ~3.9 V
- [x] PG and PS/SYNC are **not** tied together
- [x] ESP32 local decoupling: 47 µF + 10 µF + 100 nF near module
- [x] SD local decoupling: 100 nF + 10 µF near socket

## Solar Charging

- [x] CN3163 footprint pinout confirmed
- [x] `TEMP` tied to GND acceptable for deployment
- [x] `ISET` = 3.6 kΩ → ~330 mA suitable for panel and battery
- [x] CRG/DONE LED polarity confirmed
- [x] F5 polyfuse rating suitable for solar input current

## Modem and SIM

- [x] A7670G PWRKEY polarity and pulse timing confirmed
- [x] UART TX/RX crossover confirmed
- [x] Modem IO voltage and level-shifter direction confirmed
- [x] SIM series resistors moved close to modem SIM pins — done
- [x] SIM trace length target ≤ 45 mm if routing remains clean
- [x] SIM_VDD 100 nF decoupling near SIM socket
- [x] SIM ESD protection strategy confirmed
- [x] RF trace routing: away from regulator, inductor, SD, digital buses

## Config Latch

- [x] U52 `/Q` is unused or test-pad only
- [x] `CONFIG_CLEAR_N` pull-up goes to `KEEP_ALIVE`
- [x] R12 = 100 Ω confirmed intentional
- [x] Firmware asserts `PWR_HOLD` early after boot

## Miscellaneous

- [x] Battery connector polarity and silkscreen confirmed
- [x] Solar input connector polarity and silkscreen confirmed
- [x] SW9 does not leave main FET gate floating when open
- [x] `VBUS_USB` path does not charge or backfeed the battery
- [x] ~~`FORCE_POWER` does not connect to `CONFIG_SET_N`~~ — FORCE_POWER/SW11 removed from design
- [x] ADC calibration in firmware matches actual divider resistor values
- [x] ADC trace routed away from noisy switching nodes