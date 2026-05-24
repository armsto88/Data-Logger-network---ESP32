---
name: esp32-bringup-debug
description: 'Use for firmware-adjacent bring-up and debug tasks involving ESP32 boot, UART flashing, power gating interactions, RTC alarm behavior, test-point expectations, and hardware/firmware boundary issues in node or mothership boards.'
argument-hint: 'Describe the bring-up symptom, board area, or boot/debug issue'
---

# ESP32 Bring-Up Debug

## When to Use
- Diagnosing flashing, boot, reset, or UART behavior
- Working through board bring-up where firmware and hardware interact
- Documenting expected rails, test points, or alarm-driven wake behavior
- Narrowing whether a failure is caused by firmware, pin assignment, or board-level electrical issues

## Repo Context
- Consolidated bring-up and validation history lives in `docs/FIRMWARE_AND_HARDWARE_NOTES.md`.
- Mothership power and RTC wake assumptions live in `docs/MOTHERSHIP_V2_POWER_AND_WAKE_DESIGN_NOTE.md`.
- Node bring-up targets live in `firmware/nodes/bringup/`.

## Procedure
1. Start from the concrete failing surface:
   - no flash
   - no boot
   - bad rail behavior
   - RTC wake failure
   - peripheral path not responding
2. Separate the problem into layers:
   - rail or power-gate issue
   - boot/reset/programming issue
   - pin mapping or peripheral issue
   - firmware state-machine issue
3. Prefer a dedicated bring-up target or narrow validation build over editing production flow first.
4. Keep expected measurements or test points explicit when documenting the issue.

## Output Shape
- likely failing layer
- most relevant files or docs
- one cheap discriminating check
- safest next validation step
