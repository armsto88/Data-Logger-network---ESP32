# Mothership V2 Power And Wake Design Note

This note defines the intended mothership V2 power and wake architecture for PCB design.

It is written for the next PCB pass, not as a claim that the breadboard or current firmware already implements all of this behavior.

## 1. Purpose

The mothership currently works as an always-on hub with:

- DS3231 wall-clock time
- AP web UI
- ESP-NOW runtime
- SD logging
- configurable node wake and sync scheduling

What it does not yet have as a complete hardware system is:

- RTC-controlled hard power-down between sync windows
- RTC alarm-driven wake for scheduled sync activity
- a manual user wake button for local UI sessions
- a clean separation between sync wakes, manual UI wakes, and service/debug wakes

The purpose of this note is to define that missing hardware architecture before PCB layout is finalized.

Related V2 feature-planning note:

- For the proposed LTE backhaul subsystem using `SIMCom A7670G`, see `MOTHERSHIP_V2_LTE_BACKHAUL_CONCEPT.md`.

## 2. Design Intent

The mothership V2 should behave as a power-gated field hub rather than an always-on Wi-Fi device.

Target behavior:

- The board remains fully off between scheduled sync windows.
- The DS3231 alarm can wake the mothership for scheduled sync events.
- A user can press one button to wake the mothership for local UI interaction.
- USB can optionally force the mothership on for servicing, flashing, and diagnostics.
- After boot, the ESP32-WROOM asserts `PWR_HOLD` so the system stays alive long enough to complete its task.
- When the task is complete, firmware releases `PWR_HOLD` and the system powers fully down.

This matches the existing project direction that field mode should move away from an always-on mothership power model.

## 3. Recommended High-Level Architecture

Use a hard power-latch architecture with three wake-entry sources.

### 3.1 Wake-entry sources

- `RTC_WAKE`: DS3231 `INT/SQW` alarm output
- `USER_WAKE`: manual momentary button for UI access
- `USB_FORCE`: optional service/debug force-on path when USB is present

These three sources should be logically ORed into the latch-start path.

### 3.2 Hold path

- `PWR_HOLD`: ESP32-controlled hold signal that keeps the main switched rail alive after boot

### 3.3 Main principle

The RTC and wake-start logic must live on an always-available domain.

The ESP32-WROOM, Wi-Fi, SD subsystem, and the rest of the higher-current mothership electronics should live on the switched main domain.

In other words:

- always-on domain: RTC + wake logic + any minimum required pull-ups
- switched domain: ESP32-WROOM + radio + SD + UI-related peripherals + nonessential support rails

### 3.4 Hard-Kill Switch Semantics

`SW9` is a hard-kill slide switch with clear labeling.

It is not part of normal operating logic and it is not a user interaction control.

Its role is:

- storage isolation
- servicing isolation
- true hard power cut

It should be interpreted as a master hardware enable.

Expected meaning:

- `SW9 = RUN`: the always-on domain is allowed to exist, so RTC wake, config wake, service-force wake, and `PWR_HOLD` behavior can operate normally
- `SW9 = KILL`: the board is intentionally dead; RTC wake, config wake, and normal soft wake behavior are not expected to operate

### 3.5 Wake Behavior Table

Use the following table as the intended behavior reference for schematic capture and bring-up.

| Condition | SW9 | RTC alarm | Config button | USB / service force | PWR_HOLD | Expected result |
|---|---|---|---|---|---|---|
| Storage / hard-off state | KILL | ignored | ignored | optional, only if intentionally routed ahead of SW9 | low | board remains off |
| Scheduled sync wake | RUN | asserted | not pressed | inactive | low at start, then high after boot | board powers on, firmware identifies RTC wake, performs sync work, re-arms RTC, powers down |
| Manual UI wake | RUN | inactive | pressed momentarily | inactive | low at start, then high after boot | board powers on, firmware detects config wake, enters AP / UI mode, later powers down on timeout |
| Service / debug force-on | RUN | inactive or ignored | not pressed | asserted | optional high after boot | board powers on for service, flashing, or diagnostics |
| Normal active runtime | RUN | inactive | not pressed | inactive | high | board stays alive under firmware control |
| Clean shutdown | RUN | inactive after re-arm | not pressed | inactive | low | main switched rail powers off |

Design interpretation:

- `SW9` is outside the normal wake-reason logic.
- RTC alarm, config wake, and service-force are the soft wake-entry paths.
- `PWR_HOLD` is the firmware hold path after boot.
- The config latch is only for remembering wake reason, not for replacing the main power latch.

## 4. Required Hardware Blocks

### 4.1 Always-on domain

Must remain powered even while the mothership is otherwise off.

Recommended contents:

- DS3231 RTC
- RTC backup cell circuit
- alarm pull-up and flag-clear-capable interface
- wake OR / latch-start logic
- any passive bias network required for stable OFF behavior

### 4.2 Switched main domain

Powered only when wake has occurred and the latch is being held.

Recommended contents:

- ESP32-WROOM
- SD logging subsystem
- Wi-Fi / AP runtime support
- local UI peripherals
- nonessential regulators and interface rails

### 4.3 Power-latch / load-switch block

The PCB needs a real power gating stage, not just MCU reset logic.

The latch block must support:

- OFF by default
- wake on RTC alarm
- wake on button press
- optional wake on USB service presence/path
- hold-on after ESP32 boot via `PWR_HOLD`
- full power removal when `PWR_HOLD` is released

### 4.4 Manual wake button

The user button should enter the same wake-start path as the RTC alarm.

Important design rule:

- Do not use a button wired only to `EN` as the sole manual wake method.

Why:

- `EN` only resets an already-powered ESP32.
- It does not cold-start a fully power-gated board.

The button must therefore participate in the latch-start path, not just MCU reset.

Important behavioral note:

- The manual wake button is the normal user-accessible wake input for AP / config mode.
- `SW9` is not the normal user wake control.

## 5. Recommended Signal List

The following named signals are recommended for the mothership V2 schematic.

- `RAW_BAT` or equivalent unswitched input supply
- `VSYS_SW` or equivalent switched main system rail
- `RTC_INT_N` for the DS3231 alarm output
- `USER_WAKE_N` or `USER_WAKE` for the manual wake button path
- `USB_FORCE_ON` for optional service-force wake input
- `PWR_HOLD` for MCU hold control
- `WAKE_START` as the combined pre-latch wake request node
- `MS_EN` or similar if a dedicated regulator enable chain is used downstream

If the design uses active-low naming, keep it consistent. The most important thing is to make OFF-default and wake-source polarity obvious on the schematic.

## 5.1 Current Working Pin Allocation (Schematic Target)

The current mothership V2 PCB direction uses the following WROOM-native pin allocation as the working schematic target.

This table is a build-reference allocation, not a claim that all behaviors are bench-validated yet.

| Signal | GPIO | Direction | Notes |
|---|---:|---|---|
| BATTERY_VSENSE | GPIO34 | Input (ADC) | Battery voltage divider sense; firmware ADC scaling required |
| LTE_REG_PG | GPIO35 | Input | LTE regulator power-good; input-only pin |
| CONFIG_WAKE_PIN | GPIO32 | Input | Config/manual wake-reason sense path |
| LTE_REG_EN | GPIO33 | Output | Enable for LTE regulator; default intended OFF |
| CONFIG_CLEAR_PIN | GPIO25 | Output | Clears config wake latch |
| PWR_HOLD | GPIO26 | Output | Main firmware hold output for switched rail |
| CONFIG_LED | GPIO27 | Output | Config/status LED drive |
| LTE_PWRKEY | GPIO14 | Output | Modem PWRKEY control via NMOS stage |
| SD CS | GPIO13 | Output | SPI SD chip select; keep pulled high when idle |
| SD MOSI | GPIO23 | Output | SPI SD data out |
| I2C SCL | GPIO22 | Bidirectional | DS3231 and related control bus clock |
| I2C SDA | GPIO21 | Bidirectional | DS3231 and related control bus data |
| SD MISO | GPIO19 | Input | SPI SD data in |
| SD SCK | GPIO18 | Output | SPI SD clock |
| LTE TX2 | GPIO17 | Output | UART2 TX from ESP32 to modem RXD via level shift |
| LTE RX2 | GPIO16 | Input | UART2 RX to ESP32 from modem TXD via level shift |
| LTE_STATUS | GPIO4 | Input | Modem status input via level shifter |

## 5.2 Reserved/Avoided ESP32 Pins

Keep the following pins reserved or avoided in mothership V2 unless a deliberate, reviewed exception is documented.

| GPIO | Status | Reason |
|---:|---|---|
| GPIO0 | Avoid for general I/O | Boot strap sensitivity (download/boot mode interactions) |
| GPIO1 | Reserved | Primary UART0 TX for boot logs/programming |
| GPIO2 | Avoid for general I/O | Boot strap sensitivity |
| GPIO3 | Reserved | Primary UART0 RX for programming/service |
| GPIO5 | Avoid for general I/O | Strap/boot interaction risk in some startup states |
| GPIO12 | Avoid | MTDI strap; wrong level can affect flash voltage behavior |
| GPIO15 | Avoid | Strap behavior risk during reset/boot |
| GPIO6-GPIO11 | Do not use | Connected to internal SPI flash on ESP32-WROOM |

Design caution:

- `GPIO4` is currently used for modem `STATUS` because the modem rail is intentionally default-OFF at boot, reducing strap-conflict risk during power-up.

## 5.3 I2C Pull-Up Rule

The DS3231 remains powered from `KEEP_ALIVE` so it can maintain time while the main rail is off.

However, the I2C pull-ups should be tied to `MAIN_3V3`, not `KEEP_ALIVE`.

Reason:

- the ESP32-WROOM lives on the switched main 3.3 V rail
- pulling SDA/SCL up to `KEEP_ALIVE` while the ESP32 is unpowered could back-power the MCU through its GPIO protection structures
- pulling SDA/SCL up to `MAIN_3V3` keeps the bus inactive while the mothership is off, but allows normal I2C operation when the board is awake

Current planned pull-ups:

- SDA: `4.7k` to `MAIN_3V3`
- SCL: `4.7k` to `MAIN_3V3`

This means the DS3231 can stay alive on `KEEP_ALIVE` without forcing the I2C bus high into an unpowered ESP32 domain.

## 5.4 SD Rail Rule

The SD card should be powered from `MAIN_3V3`, not `KEEP_ALIVE`.

Reason:

- the SD subsystem belongs to the switched main domain
- it should not remain powered while the board is hard-off between wake windows
- this avoids unnecessary standby current and simplifies full-off behavior

## 5.5 LTE Modem Subsystem Summary

Current subsystem direction for mothership V2:

- Selected modem: `SIMCom A7670G`
- System workflow extension: existing ESP-NOW ingest to SD logging path is extended with scheduled, power-gated LTE upload windows
- Control model: modem controlled over UART2 AT commands, with planned support for queued CSV transfer, MQTT/HTTPS transport options, and basic modem status/health reporting

This section documents the current integration intent and working schematic allocation.

## 5.6 LTE Digital Interface Mapping (Working)

| Function | ESP32 pin | External endpoint | Interface detail |
|---|---|---|---|
| Modem rail enable | `GPIO33` | TPS63020 `EN` | `EN` has pulldown for default-OFF behavior |
| Modem rail power-good | `GPIO35` | TPS63020 `PG` | External pull-up required; ESP32 pin is input-only |
| Modem PWRKEY control | `GPIO14` | A7670G `PWRKEY` (via 2N7002) | NMOS pulldown stage for controlled key pulse |
| Modem STATUS sense | `GPIO4` | A7670G `STATUS` (via level shifter) | 1.8 V domain translated to 3.3 V logic |
| UART2 TX | `GPIO17` | A7670G `RXD` (via level shifter) | ESP32 command path to modem |
| UART2 RX | `GPIO16` | A7670G `TXD` (via level shifter) | Modem response path to ESP32 |

## 5.7 Modem Rail And VBAT Distribution (Working)

Power chain intent:

- `VSYS` -> TPS63020 -> `MODEM_VBAT_3V9` -> A7670G `VBAT`
- Target modem rail: `3.9 V`
- Planned sense divider: `680k / 100k` for modem/battery telemetry scaling in firmware
- Control and readiness mapping: `GPIO33` (`EN`) and `GPIO35` (`PG`)

Local protection and decoupling near modem VBAT entry:

- `470 uF`
- `10 uF`
- `1 uF`
- `100 nF`
- `5 V` unidirectional TVS

Routing rule:

- route `MODEM_VBAT_3V9` to A7670G `VBAT` pins `55/56/57` using short, wide copper with local bulk close to the module pins.

## 5.8 Level Shifting Strategy (Working)

Use `SN74LVC1T45` for modem digital level translation where required.

- `VCCA = M_1V8`
- `VCCB = 3V3_SYS`
- `M_1V8` source: A7670G `VDD_EXT` / `VDD_1V8` with local `100 nF` decoupling
- treat `M_1V8` as a logic-reference rail only, not a general external load rail

## 5.9 Level-Shifted Signal Directions And DIR Settings (Working)

| Path | Intended data direction | DIR intent |
|---|---|---|
| `GPIO17 (TX2)` -> modem `RXD` | 3.3 V side to 1.8 V side | Set for B-to-A if B=3.3 V, A=1.8 V |
| modem `TXD` -> `GPIO16 (RX2)` | 1.8 V side to 3.3 V side | Set for A-to-B if B=3.3 V, A=1.8 V |
| modem `STATUS` -> `GPIO4` | 1.8 V side to 3.3 V side | Set for A-to-B if B=3.3 V, A=1.8 V |

## 5.10 PWRKEY, RESET, And Recovery Access Notes

PWRKEY control (working circuit intent):

- `GPIO14` drives a `2N7002` gate through approximately `4.7k`
- gate pulldown approximately `100k` keeps NMOS off during reset/boot
- NMOS drain pulls modem `PWRKEY` low when activated
- logic interpretation: transistor on = `PWRKEY` low (assert), transistor off = `PWRKEY` released high

RESET note:

- modem `RESET` remains unassigned in current working allocation
- provide at least a test pad and optional DNI population path for future firmware/hardware recovery control.

## 5.11 SIM Interface And ESD Notes

- SIM holder signals should follow A7670G USIM reference guidance and keep trace lengths short/matched where practical
- use ESD protection on SIM lines (target: `ESDA6V1W5` / `C48677`)
- power SIM `VCC` only from modem `USIM1_VDD`.

## 5.12 NETLIGHT Drive Note

If exposing modem `NETLIGHT`, use a transistor driver stage:

- `MMBT3904`
- resistor set: `4.7k`, `47k`, `2.2k`

This keeps modem-side signaling isolated from direct higher-current LED drive assumptions.

## 5.13 LTE Antenna Path Note

- keep modem RF net explicitly named (for example `LTE_RF`) and avoid ambiguous net aliases during layout/export
- add RF ESD near antenna launch (target: `ESD9L5.0ST5G` / `C82326`)
- route away from switching-noise nets and fast digital edges
- internal FPC antenna placement should be verified against enclosure wall/lid geometry and ground-clearance requirements
- preserve an optional external path (`u.FL` to SMA) for enclosure-dependent field deployments.

## 6. RTC Alarm Behavior Requirements

The DS3231 alarm output is suitable for this role, but one behavioral detail matters:

- the alarm pin remains asserted until the alarm flag is cleared over I2C

This has direct design implications.

Required sequence:

1. RTC alarm pulls `RTC_INT_N` active
2. latch-start path turns main power on
3. ESP32 boots
4. firmware asserts `PWR_HOLD`
5. firmware reads RTC and identifies wake reason
6. firmware clears the alarm flag
7. firmware performs sync task
8. firmware re-arms next alarm
9. firmware releases `PWR_HOLD` to shut down

Important rule:

- Do not clear the RTC alarm before the MCU has successfully taken ownership of the hold path.

Otherwise the board may collapse before firmware is fully up.

## 7. Recommended Wake Modes

### 7.1 RTC sync wake

Purpose:

- wake only for scheduled sync window operation

Expected behavior:

- boot from RTC alarm
- assert `PWR_HOLD`
- determine wake reason from RTC alarm status
- bring up only the subsystems needed for sync and logging
- run sync window
- re-arm next RTC alarm
- shut down cleanly

### 7.2 Manual UI wake

Purpose:

- allow a user to wake the mothership locally for AP/UI interaction

Expected behavior:

- boot from button press
- assert `PWR_HOLD`
- bring up AP/UI mode
- keep system awake while UI is active
- reset inactivity timeout on meaningful user activity
- power down after timeout if no service condition is present

Recommended prototype behavior:

- one press from OFF wakes directly into full AP/UI mode

This assumes `SW9` is already in `RUN`.

This is simpler than trying to implement layered short/long/multi-press modes immediately.

### 7.3 USB service wake

Purpose:

- support flashing, bench testing, diagnostics, and recovery

Expected behavior:

- if USB service force is present, the board can stay on independent of RTC schedule
- useful during bring-up, firmware upload, SD diagnostics, and UI testing

This path should not be treated as a config wake unless the logic explicitly chooses to do so.

## 8. Recommended User-Interaction Model

For V2, keep the user interaction model simple.

Recommended behavior:

- Button from OFF: wake into UI mode
- Optional firmware timeout: auto power-down after inactivity
- Optional button while ON: request shutdown, but this is lower priority than just getting wake working

Avoid overloading the first PCB revision with complicated button semantics unless there is a clear use case already proven on the bench.

## 9. Firmware Responsibilities

The current mothership firmware already contains schedule concepts and RTC-backed time handling, but it does not yet expose a full alarm-driven power-cycle implementation.

The firmware work implied by this PCB design is:

### 9.1 RTC layer additions

- add DS3231 alarm configuration helpers
- add alarm clear helpers
- add next-sync alarm scheduling helpers
- add wake-reason inspection helpers

### 9.2 Boot flow additions

- identify whether boot came from RTC alarm, manual wake, USB service, or generic reset
- assert `PWR_HOLD` early in boot
- clear RTC alarm only after hold is established
- if using a config-wake latch, read and clear that latch without confusing it with service-force wake

### 9.3 Runtime state additions

- sync wake state
- manual UI wake state
- service/debug wake state
- inactivity timeout handling for UI wake mode

### 9.4 Shutdown flow additions

- flush pending logging where needed
- persist any required wake/sync bookkeeping
- re-arm next RTC alarm
- release `PWR_HOLD`

### 9.5 Proposed LTE Upload Cycle (Unvalidated)

This is the current proposed sequence for scheduled, power-gated LTE upload windows.

1. Complete ESP-NOW ingest and local SD write path first.
2. Assert `GPIO33` to enable modem regulator (`TPS63020 EN`).
3. Wait for `GPIO35` power-good indication.
4. Pulse modem `PWRKEY` through `GPIO14` (NMOS pulldown stage).
5. Wait for modem `STATUS` on `GPIO4` to indicate ready state.
6. Bring up UART2 on `GPIO16/GPIO17` and start AT session.
7. Run SIM/reg/network checks, then perform queued upload (MQTT or HTTPS) from SD-backed queue.
8. Log success/failure and queue state locally for retry accounting.
9. Issue graceful modem shutdown commands and wait for status drop.
10. Deassert `GPIO33` to hard-disable modem rail.

This flow should be treated as design intent until validated during bring-up.

## 10. PCB Design Requirements

The mothership V2 PCB should explicitly support the following.

### 10.1 Power-gate requirements

- OFF-default main rail
- wake-start path from RTC alarm
- wake-start path from user button
- optional wake-start path from USB service
- MCU `PWR_HOLD` takeover path
- clean full-off state with low leakage

### 10.2 RTC requirements

- DS3231 alarm output routed to wake logic
- DS3231 alarm output also readable by MCU if useful for diagnostics
- RTC remains powered when main system is off
- access to backup cell and replacement path

### 10.3 User button requirements

- button reaches wake-start path, not just reset
- debounce strategy appropriate to latch input
- ESD protection if externally accessible
- enclosure placement suitable for field UI access

### 10.4 Service/debug requirements

- reliable programming/debug path even when normal field power logic is in place
- USB or equivalent service-force path clearly separated from normal RTC wake behavior
- test points on key wake-control nets

### 10.5 Pre-Fabrication Checks (LTE + Power/Wake Integration)

Before releasing PCB fabrication outputs, confirm:

- `GPIO4` boot-safety assumption remains valid because modem rail is default-OFF at boot
- `GPIO35` has a valid external pull-up in the correct translated domain (ESP32 pin is input-only)
- `GPIO34` ADC divider definition is explicitly documented and firmware scaling constants are aligned
- `GPIO14` NMOS gate has a pulldown so modem `PWRKEY` is not spuriously asserted at reset
- all modem ground pins are tied to solid local ground with low-impedance return
- modem `VBAT` pins `55/56/57` have short/wide copper and local bulk decoupling
- `M_1V8` is used only as logic reference for translators, not as a general supply rail
- SIM `VCC` is sourced only from modem `USIM1_VDD`
- RF routing is clean, short, and separated from switching/noisy nets
- recovery/test pads exist for modem `RESET`, `USB_BOOT`, `USB_DP`, `USB_DM`, and `USB_VBUS`

### 10.6 Candidate enclosure note

Potential mothership housing candidate identified on 2026-05-17:

- QWORK industrial enclosure
- IP65 ABS plastic housing with transparent hinged PC lid
- listed size: `150 x 100 x 70 mm`
- measured internal usable footprint: `132 x 80 mm`
- measured internal usable height: `53 mm`
- two internal nutserts on the enclosure centerline with `100 mm` center-to-center spacing
- Amazon.de link: `https://www.amazon.de/-/en/QWORK-Industrial-Waterproof-Transparent-Distribution/dp/B08XNMZF2V`

Why it may fit this design direction:

- transparent hinged lid could work well for field inspection without opening the full enclosure
- the measured `132 x 80 x 53 mm` internal envelope looks plausible for a compact WROOM mothership PCB, battery/power wiring, and cable gland entry if the molded bosses do not consume too much area
- the two centerline nutserts may provide a simple way to mount a carrier plate or directly pick up a long-axis PCB/bracket if the board outline is designed around the `100 mm` spacing
- off-the-shelf industrial box format is aligned with the project's serviceable outdoor hub direction

Before locking the PCB or panel layout to this housing, verify:

- the `132 x 80 x 53 mm` internal envelope against the actual PCB outline, connector overhang, and wiring bend radius
- whether the `100 mm` centerline nutsert spacing is better used for a dedicated mounting plate, an internal bracket, or direct PCB support
- cable gland placement space for power, sensor, and any external antenna routing
- whether the transparent lid and enclosure wall thickness interfere with Wi-Fi performance or suggest an external antenna option
- whether the user button, service access, status visibility, and hard-kill switch placement are still practical in this form factor

Suggested test points:

- `RTC_INT_N`
- `USER_WAKE`
- `USB_FORCE_ON`
- `WAKE_START`
- `PWR_HOLD`
- `VSYS_SW`
- RTC supply rail

## 11. Risks To Avoid

The following are the main design traps.

### 11.1 Treating reset as wake

A reset button is not a substitute for a wake button on a fully power-gated board.

### 11.2 Clearing RTC alarm too early

If the RTC alarm is cleared before `PWR_HOLD` takes over, the board may drop power mid-boot.

### 11.3 Powering too much from the always-on domain

If the always-on domain includes unnecessary loads, field idle current will be much worse than intended.

### 11.4 Forgetting service-force behavior

Without a predictable service-force path, bench bring-up and firmware recovery become more awkward.

### 11.5 Designing the button only for firmware semantics

The first job of the button is to wake the board from OFF. Rich UI behavior comes after that.

## 12. Recommended First Prototype Behavior

For the first mothership PCB implementing this concept, the recommended minimum behavior is:

- RTC alarm wakes the mothership for sync windows
- one button wakes the mothership for UI mode
- USB can force the mothership on for service
- ESP32 asserts `PWR_HOLD` after boot
- firmware auto-shuts down after sync completion or UI inactivity timeout

This is enough to validate the core field-power concept without overcomplicating the first revision.

## 13. Out Of Scope For This Note

This note does not define:

- final PMOS/load-switch part numbers
- exact resistor values for the latch network
- final enclosure mechanics for the button
- final UI inactivity timeout values
- final software state-machine implementation details

Those should be chosen during schematic capture, PCB review, and later bench validation.

## 14. Bottom Line

The mothership V2 should not be designed as an always-on Wi-Fi device with an RTC attached. It should be designed as a power-gated hub with three explicit wake sources:

- RTC alarm for scheduled sync operation
- user button for manual UI access
- USB/service force for bench and recovery use

The PCB must therefore provide a real latch/hold power architecture. Firmware can then prototype the wake behavior on top of that hardware, rather than trying to fake field power control in software alone.