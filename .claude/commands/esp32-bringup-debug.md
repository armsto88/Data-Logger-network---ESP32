---
description: "Diagnose ESP32 bring-up and boot issues: UART flashing, power gating, RTC alarm wake, test-point expectations, and hardware/firmware boundary failures on node or mothership boards."
argument-hint: "Describe the bring-up symptom, board area, or boot/debug issue"
---

# ESP32 Bring-Up Debug

You are helping diagnose a hardware/firmware bring-up issue on an ESP32 sensor-node or mothership board.

## Key Docs
- Consolidated bring-up history: `docs/FIRMWARE_AND_HARDWARE_NOTES.md`
- Mothership power and RTC wake: `mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md`
- Node bring-up targets: `node/firmware/tests/`

## Procedure
1. Start from the concrete failing surface:
   - no flash / UART not detected
   - no boot or reset loop
   - bad rail behavior or power sequencing
   - RTC wake failure
   - peripheral path not responding
2. Separate the problem into layers and assign a likely layer:
   - Rail or power-gate issue
   - Boot/reset/programming issue
   - Pin mapping or peripheral issue
   - Firmware state-machine issue
3. Read the relevant bring-up docs and design notes before proposing anything.
4. Prefer a dedicated bring-up target or narrow validation build over editing production flow first.
5. Keep expected measurements or test points explicit when documenting the issue.

## Output Shape
- **Likely failing layer**: which of the four layers above
- **Most relevant files or docs**: with paths
- **One cheap discriminating check**: the single fastest way to confirm or rule out the hypothesis
- **Safest next validation step**: least-invasive action to take

$ARGUMENTS
