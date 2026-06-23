---
description: "Select and run the narrowest PlatformIO validation build for a firmware change: mothership, node, or bring-up environment."
argument-hint: "Describe the firmware area or target environment to validate"
---

# PlatformIO Validation

You are helping select and run the correct PlatformIO validation build for this ESP32 repo.

## Key Files
- `platformio.ini` — shared or top-level environments
- `node/firmware/platformio.ini` — node-specific environments
- `node/firmware/tests/` — narrow bring-up firmware for bench validation

## Procedure
1. Identify the touched slice from the task or diff:
   - `mothership/firmware/src/` → prefer a mothership-scoped build
   - `node/firmware/src/` → prefer the sensor-node environment
   - `node/firmware/tests/` → prefer the specific bring-up environment
2. Read `platformio.ini` to confirm the exact environment name before running.
3. Run the narrowest build that can falsify the change:
   ```
   pio run -e <environment>
   ```
4. If the first narrow build passes and the change touches shared headers, consider one adjacent build only if justified — state the justification explicitly.
5. Report the exact environment built and whether validation was build-only or also behavior-scoped.

## Output Shape
- **Likely target environment**: name and why
- **Why that target is the right validation slice**: one sentence
- **Suggested narrow validation order**: environment names in order
- **Risks if broader validation was not run**: what could still be broken

$ARGUMENTS
