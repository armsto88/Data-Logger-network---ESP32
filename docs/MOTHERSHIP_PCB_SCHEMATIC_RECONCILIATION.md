# Mothership PCB Schematic vs Design Note — Reconciliation

**Date:** 2026-06-05
**Status:** Open — requires schematic confirmation before PCB order

This document cross-references the PCB schematic review
(`MOTHERSHIP_PCB_SCHEMATIC_REVIEW_2026-06-05.md`) against the existing
design notes (`MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md` and
`MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md`) to flag discrepancies, confirm
alignment, and identify new information that the design notes don't yet cover.

---

## 1. Signal Naming Discrepancies

The schematic uses different signal names from the design note in
several places. These must be reconciled to avoid confusion during bring-up
and firmware development.

| Schematic Name | Design Note Name | Notes |
|---|---|---|
| `INT_RTC` | `RTC_INT_N` | The design note uses active-low `_N` suffix. The schematic uses `INT_RTC` without polarity suffix. The DS3231 SQW/INT is active-low, so `RTC_INT_N` is more explicit. **Recommendation: adopt `RTC_INT_N` consistently.** |
| `LOGIC` | `WAKE_START` | §5 defines `WAKE_START` as the combined pre-latch wake request node. The schematic uses `LOGIC`. **Recommendation: adopt `WAKE_START` to match the design note, or document the alias clearly on the schematic.** |
| `CONFIG_SET_N` | (not in design note) | New signal — the design note doesn't name the config latch preset line. The schematic introduces `CONFIG_SET_N`. This is fine; add to the design note pin table. |
| `CONFIG_CLEAR_N` | (not in design note) | The design note names the ESP32 output `CONFIG_CLEAR_PIN` (GPIO25) but doesn't name the active-low latch clear line. The schematic introduces `CONFIG_CLEAR_N` for the `/CLR` net. **Recommendation: add `CONFIG_CLEAR_N` to the design note signal list as the net name on the latch side.** |
| `CONFIG_WAKE_PIN` | `CONFIG_WAKE_PIN` | ✅ Matches (GPIO32). |
| `CONFIG_CLEAR_PIN` | `CONFIG_CLEAR_PIN` | ✅ Matches (GPIO25). |
| `PWR_HOLD` | `PWR_HOLD` | ✅ Matches (GPIO26). |
| `VBUS_USB` | `USB_FORCE_ON` | The design note §3.1 uses `USB_FORCE_ON`. The schematic uses `VBUS_USB`. These may be different signals — see §2 below. |
| `FORCE_POWER` | (not in design note) | New signal — the design note mentions "force power" conceptually but doesn't name a discrete signal. The schematic introduces `FORCE_POWER` as a separate service/debug override. |

---

## 2. USB Wake Path — Clarification Needed

The design note defines `USB_FORCE_ON` as an "optional service-force wake
input when USB is present." The schematic lists `VBUS_USB` as one of the
sources that can pull `LOGIC` active.

**Question for schematic confirmation:**

- Is `VBUS_USB` literally the USB VBUS rail (5 V from USB connector)?
- Or is it a derived signal (e.g., VBUS through a voltage divider or comparator)?
- Does `VBUS_USB` serve **both** wake and power purposes, or is there a separate
  `FORCE_POWER` signal for the service/debug override?

The design note separates these into two concepts:
1. `USB_FORCE_ON` — automatic wake when USB is plugged in
2. A separate force-on path for service/debug

The schematic lists both `VBUS_USB` and `FORCE_POWER` as separate sources
on the `LOGIC` node, which aligns with the design intent. But the naming
discrepancy should be resolved.

**Recommendation:** Use `VBUS_USB` for the raw USB presence detect and
`FORCE_POWER` for the manual service override. Update the design note to include
both signal names.

---

## 3. New Hardware Not in Design Note

The schematic review introduces several hardware elements that the
design note doesn't yet document:

### 3.1 SN74LVC2G74DCUR Config Latch (U52)

- **Part:** TI SN74LVC2G74DCUR — single D-type flip-flop with preset and clear
- **Purpose:** Stores config-button wake reason across power-gate transitions
- **Power:** `KEEP_ALIVE` (always-on domain)
- **Usage:** Only `/PRE` and `/CLR` are used; `D` and `CLK` tied to GND
- **Design note gap:** The design note mentions a "config wake latch" concept (§4.4) but
  doesn't specify the part number or pin-level wiring. This should be added.

### 3.2 AO3407A P-Channel MOSFETs

- **Part:** AO3407A — P-channel MOSFET (SOT-23)
- **Usage:** Two instances:
  1. Input protection (source toward battery input)
  2. Main power switch (`RAW_BAT` → `VSYS`)
- **Design note gap:** The design note doesn't specify the MOSFET part number for the
  main power switch. Add AO3407A to the BOM and design note.

### 3.3 2N7002 N-Channel MOSFETs (Q41, Q42)

- **Part:** 2N7002 — N-channel MOSFET (SOT-23)
- **Usage:**
  - Q41: Low-side pull-down for config latch `/CLR`
  - Q42: Level translator for `CONFIG_WAKE_PIN` sense
- **Design note gap:** Not documented. Add pin-level wiring.

### 3.4 D1 — Schottky Diode

- **Purpose:** Isolates `CONFIG_SET_N` from `LOGIC` wake node
- **Orientation:** Anode to `LOGIC`, cathode to `CONFIG_SET_N`
- **Design note gap:** Not documented. Add to design note.

### 3.5 R12 = 100 Ω

- **Purpose:** Mentioned in the schematic review as "inherited from the tested node
  power-switching design"
- **Location:** In the gate control path of the main AO3407A
- **Design note gap:** Not documented. Confirm value and add to design note.

---

## 4. Rail Naming Alignment

| Schematic | Design Note | Aligned? |
|---|---|---|
| `BAT_BUS` | Not named | The design note doesn't name the pre-fuse bus. Add to design note. |
| `RAW_BAT` | Not explicitly named | The design note uses `VSYS_SW` for the switched rail but doesn't name the pre-switch battery rail. Add `RAW_BAT` to design note. |
| `VSYS` | `VSYS_SW` | ⚠️ **Name mismatch.** The schematic uses `VSYS`, the design note uses `VSYS_SW`. These refer to the same switched system rail. **Recommendation: pick one name and use it consistently.** `VSYS` is shorter and matches ESP32 convention; `VSYS_SW` is more explicit about it being switched. |
| `KEEP_ALIVE` | `KEEP_ALIVE` | ✅ Matches. |
| `3V3_SYS` | `MAIN_3V3` | ⚠️ **Name mismatch.** The schematic uses `3V3_SYS`, the design note uses `MAIN_3V3`. Same rail. **Recommendation: pick one.** `3V3_SYS` is more descriptive of the switched nature; `MAIN_3V3` is more explicit about voltage. |
| `VOLT` | Not named | The design note mentions `BATTERY_VSENSE` (GPIO34). The schematic uses `VOLT` as the divider midpoint net. These are the same signal. **Recommendation: use `VOLT` as the net name and `BATTERY_VSENSE` as the ESP32 pin label.** |

---

## 5. Pin Allocation Cross-Check

The design note §5.1 defines the working pin allocation. Cross-checking against the schematic review:

| Signal | Design Note GPIO | Schematic Review | Status |
|---|---|---|---|
| `BATTERY_VSENSE` / `VOLT` | GPIO34 | Not specified, but review says "ADC1-capable" | ✅ GPIO34 is ADC1. Aligned. |
| `PWR_HOLD` | GPIO26 | `PWR_HOLD` | ✅ Aligned. |
| `CONFIG_WAKE_PIN` | GPIO32 | `CONFIG_WAKE_PIN` | ✅ Aligned. |
| `CONFIG_CLEAR_PIN` | GPIO25 | `CONFIG_CLEAR_PIN` | ✅ Aligned. |
| `LTE_REG_EN` | GPIO33 | Not in schematic review (different section) | N/A |
| `LTE_REG_PG` | GPIO35 | Not in schematic review (different section) | N/A |

**No conflicts found.** The schematic review is consistent with the design note pin table for
the signals it covers.

---

## 6. Design Rule Confirmations That the Schematic Should Verify

The design note establishes several rules that should be checked against
the schematic:

1. **I2C pull-ups to `MAIN_3V3` / `3V3_SYS`, not `KEEP_ALIVE`** (design note §5.3)
   - Schematic review confirms `CONFIG_WAKE_PIN` pull-up goes to `3V3_SYS` ✅
   - Check that DS3231 I2C pull-ups also go to `3V3_SYS`, not `KEEP_ALIVE`

2. **SD card powered from `MAIN_3V3` / `3V3_SYS`, not `KEEP_ALIVE`** (design note §5.4)
   - Confirmed in schematic review ✅

3. **DS3231 powered from `KEEP_ALIVE`** (design note §5.3)
   - Confirmed in schematic review ✅

4. **PWR_HOLD asserted before slow operations** (design note §6)
   - Schematic review confirms this firmware requirement ✅

5. **SW9 as hard-kill, not user wake** (design note §3.4)
   - Schematic review confirms SW9 is a hard enable/cut switch ✅

---

## 7. Open Questions for Schematic Confirmation

These items from the schematic review need direct verification against the schematic
before the PCB order:

1. **AO3407A orientation** — both input protection and main power switch.
   Source must face the higher-voltage side (battery input for input protection,
   `RAW_BAT` for main switch).

2. **D1 orientation** — anode to `LOGIC`, cathode to `CONFIG_SET_N`. If
   reversed, RTC/PWR_HOLD/FORCE_POWER wake events would falsely set the
   config latch.

3. **Q41/Q42 2N7002 pin mapping** — SOT-23 pinout varies between manufacturers.
   Confirm symbol pinout matches the actual part (gate=1, drain=3, source=2
   for standard SOT-23 NMOS).

4. **`CONFIG_WAKE_PIN` pull-up rail** — must be `3V3_SYS`, not `KEEP_ALIVE`.
   Back-powering the ESP32 through GPIO protection diodes would be a
   reliability risk.

5. **`CONFIG_CLEAR_N` pull-up rail** — must be `KEEP_ALIVE` (not `3V3_SYS`)
   because the latch clear must work before `3V3_SYS` is stable.

6. **R12 = 100 Ω** — confirm this is intentional (current-limiting in the
   gate control path) and not a placeholder.

7. **`VBUS_USB` vs `FORCE_POWER`** — confirm these are separate signals
   on the `LOGIC` node and that `VBUS_USB` does not backfeed the battery.

8. **`/Q` output of U52** — confirm it's left unconnected or on a test pad
   only, not driving any other circuit.

---

## 8. Action Items Summary

### Must confirm before PCB order

- [ ] AO3407A orientation (input protection + main switch)
- [ ] D1 orientation (anode=LOGIC, cathode=CONFIG_SET_N)
- [ ] Q41/Q42 2N7002 pin mapping matches actual part
- [ ] `CONFIG_WAKE_PIN` pull-up to `3V3_SYS` (not `KEEP_ALIVE`)
- [ ] `CONFIG_CLEAR_N` pull-up to `KEEP_ALIVE` (not `3V3_SYS`)
- [ ] `VBUS_USB` does not backfeed battery
- [ ] All downstream loads on `RAW_BAT`, not `BAT_BUS`

### Should add to design note

- [ ] SN74LVC2G74DCUR part number and wiring for config latch
- [ ] AO3407A part number for main power switch
- [ ] 2N7002 part numbers for Q41 and Q42
- [ ] D1 Schottky diode isolation between LOGIC and CONFIG_SET_N
- [ ] R12 = 100 Ω in gate control path
- [ ] `BAT_BUS` and `RAW_BAT` rail definitions
- [ ] Reconcile `VSYS` vs `VSYS_SW` naming
- [ ] Reconcile `3V3_SYS` vs `MAIN_3V3` naming
- [ ] Add `FORCE_POWER` and `VBUS_USB` signal definitions
- [ ] Add `CONFIG_SET_N` and `CONFIG_CLEAR_N` net names

### New from 3V3/KEEP_ALIVE/RTC/SD review

- [ ] AP2112K-3.3 part number for main 3.3 V regulator
- [ ] TPL720F33 part number for keep-alive 3.3 V regulator
- [ ] AP2112K output cap values: 100 nF, 10 µF, 100 µF/10 V
- [ ] TPL720F33 output cap values: 10 µF, 100 nF
- [ ] SD card SPI pin mapping (GPIO18/19/23/13) — matches design note §5.1
- [ ] SD CS 10 kΩ pull-up to `3V3_SYS`
- [ ] SD 22 Ω series resistors on SCK, MOSI, CS
- [ ] SD MISO direct (no series resistor)
- [ ] DS3231 powered from `KEEP_ALIVE`
- [ ] DS3231 I²C pull-ups to `3V3_SYS` (not `KEEP_ALIVE`)
- [ ] DS3231 backup cell holder footprint and polarity
- [ ] `INT_RTC` transistor stage for wake logic

### New from solar charging review

- [ ] CN3163 solar charger part number and pin wiring
- [ ] CN3163 `TEMP` tied to GND (no NTC) — confirm acceptable for deployment
- [ ] CN3163 `ISET` = 3.6 kΩ → ~330 mA charge current — confirm suitable for panel and battery
- [ ] CN3163 `FB` feedback network — verify against datasheet
- [ ] F5 = 1812L050PR polyfuse on solar input — confirm rating
- [ ] U47 solar input connector — confirm part and polarity silkscreen
- [ ] CRG/DONE LED polarity and 2.2 kΩ resistor values
- [ ] `BAT_BUS` pre-fuse relationship to `RAW_BAT` confirmed
- [ ] Solar charger input/output capacitor placement near CN3163

### New from modem power / SIM / routing review

- [ ] TPS63020 footprint and pinout
- [ ] TPS63020 feedback divider: 680 kΩ / 100 kΩ → ~3.9 V output
- [ ] `4V_EN` (GPIO33) enable polarity and startup behaviour
- [ ] `ESP_PG` (GPIO35) power-good pull-up/reference
- [ ] `PG` and `PS/SYNC` confirmed NOT tied together
- [ ] `PS/SYNC` configuration matches intended operating mode
- [ ] TPS63020 inductor value and saturation current suitable for cellular bursts
- [ ] TPS63020 output caps: low-ESR, close to regulator and modem
- [ ] A7670G PWRKEY circuit polarity and pulse timing
- [ ] UART TX/RX cross-over: ESP32 GPIO17→modem RX, modem TX→GPIO16
- [ ] Level shifter direction for 1.8 V modem IO
- [ ] SIM holder footprint pinout
- [ ] SIM_VDD voltage level matches modem/SIM requirement
- [ ] SIM ESD protection strategy (ESDA6V1W5 recommended in LTE concept)
- [ ] SIM 22 Ω series resistors moved close to modem SIM pins
- [ ] SIM trace length: target ≤45 mm, currently ~53 mm
- [ ] SIM_VDD 100 nF decoupling near SIM socket
- [ ] Modem 4 V rail: wide copper, bulk caps near VBAT pins
- [ ] RF trace routing: away from regulator, inductor, SD, digital buses

### New from USB programming / MCU core review

- [ ] USB ESD protection moved close to USB-C connector (not near CH340C)
- [ ] USB D+/D− 22 Ω series resistors placed after ESD, toward CH340C
- [ ] USB-C CC1/CC2 both have 5.1 kΩ pulldowns to GND
- [ ] CH340C powered from `3V3_SYS`
- [ ] CH340C TX/RX crossover confirmed (TXD→ESP RX0, RXD←ESP TX0)
- [ ] CH340C V3 capacitor (100 nF) placed close to chip
- [ ] RTS/DTR auto-program circuit verified against ESP32 reference design
- [ ] Q14/Q15 MMBT3904 footprint pinout and orientation confirmed
- [ ] EN and BOOT pull-ups: 10 kΩ to `3V3_SYS`
- [ ] `PROG_EN` connects to ESP32 EN/CHIP_PU
- [ ] `PROG_BOOT` connects to ESP32 IO0/BOOT
- [ ] ESP32 local decoupling: 47 µF + 10 µF + 100 nF near module
- [ ] GPIO14 confirmed as `M_PWRK` (not GPIO12)
- [ ] GPIO34 (VOLT_ESP) and GPIO35 (ESP_PG) used only as inputs
- [ ] `ESP_PG` has valid external pull-up (GPIO34/35 have no internal pull-ups)
- [ ] I²C pull-ups to `3V3_SYS` (not `KEEP_ALIVE`) — confirmed again
- [ ] Battery ADC filter: VOLT → 2.2 kΩ → VOLT_ESP → 100 nF → GND
- [ ] `4V_EN` (GPIO33) default state at boot does not unintentionally power modem
- [ ] `CFG_LED` (GPIO27) polarity confirmed in firmware
- [ ] `PWR_HOLD` (GPIO26) asserted early in firmware boot

### Firmware alignment

- [ ] Confirm `PWR_HOLD` (GPIO26) is asserted as first action in `setup()`
- [ ] Confirm `CONFIG_WAKE_PIN` (GPIO32) read logic: LOW = config wake requested
- [ ] Confirm `CONFIG_CLEAR_PIN` (GPIO25) pulse sequence: HIGH 20ms, then LOW
- [ ] Add `FORCE_POWER` and `VBUS_USB` wake-reason detection to firmware if
      those signals are routed to ESP32 inputs (currently not in pin table)