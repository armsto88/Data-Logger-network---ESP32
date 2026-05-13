# Mothership V2 Power And Wake Design Note

This note defines the intended mothership V2 power and wake architecture for PCB design.

It is written for the next PCB pass, not as a claim that the breadboard or current firmware already implements all of this behavior.

## 1. Purpose

The mothership currently works as an always-on ESP32-S3 hub with:

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

## 2. Design Intent

The mothership V2 should behave as a power-gated field hub rather than an always-on Wi-Fi device.

Target behavior:

- The board remains fully off between scheduled sync windows.
- The DS3231 alarm can wake the mothership for scheduled sync events.
- A user can press one button to wake the mothership for local UI interaction.
- USB can optionally force the mothership on for servicing, flashing, and diagnostics.
- After boot, the ESP32-S3 asserts `PWR_HOLD` so the system stays alive long enough to complete its task.
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

The ESP32-S3, Wi-Fi, SD subsystem, and the rest of the higher-current mothership electronics should live on the switched main domain.

In other words:

- always-on domain: RTC + wake logic + any minimum required pull-ups
- switched domain: ESP32-S3 + radio + SD + UI-related peripherals + nonessential support rails

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

- ESP32-S3
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

This is simpler than trying to implement layered short/long/multi-press modes immediately.

### 7.3 USB service wake

Purpose:

- support flashing, bench testing, diagnostics, and recovery

Expected behavior:

- if USB service force is present, the board can stay on independent of RTC schedule
- useful during bring-up, firmware upload, SD diagnostics, and UI testing

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