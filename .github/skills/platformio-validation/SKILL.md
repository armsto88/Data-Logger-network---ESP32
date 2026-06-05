---
name: platformio-validation
description: 'Use for PlatformIO-focused firmware validation in this repo: selecting the right target, running the narrowest useful build, and checking changes against mothership, node, or bring-up environments.'
argument-hint: 'Describe the firmware area or target environment to validate'
---

# PlatformIO Validation

## When to Use
- Building or validating a firmware change
- Picking the narrowest sensible PlatformIO environment
- Checking whether a change belongs to mothership, node, or bring-up code
- Avoiding overly broad full-repo builds when only one target changed

## Repo Context
- `platformio.ini` at repo root contains shared or top-level environments.
- `node/firmware/platformio.ini` may contain node-specific target setup.
- `node/firmware/tests/` contains narrow bring-up firmware used for bench validation.

## Procedure
1. Identify the touched slice:
   - `mothership/firmware/src/` -> prefer a mothership-scoped build
   - `node/firmware/src/` -> prefer the sensor-node environment
   - `node/firmware/tests/` -> prefer the specific bring-up environment
2. Run the narrowest build that can falsify the change.
3. If the first narrow build passes but the change touches shared headers, consider one adjacent build only if justified.
4. Report the exact environment built and whether validation was build-only or also behavior-scoped.

## Output Shape
- likely target environment
- why that target is the right validation slice
- suggested narrow validation order
- risks if broader validation was not run
