---
name: mothership-planning
description: 'Use for mothership V2 planning tasks: power and wake architecture, enclosure fit, LTE backhaul notes, PCB feature proposals, design-note updates, and separating confirmed decisions from open questions.'
argument-hint: 'Describe the mothership planning or documentation task'
---

# Mothership Planning

## When to Use
- Updating mothership V2 design notes
- Adding new proposed mothership subsystems such as LTE, power, wake, storage, or enclosure changes
- Turning rough hardware ideas into structured repo documentation
- Comparing confirmed current behavior against proposed future behavior

## Procedure
1. Identify the owning mothership design note in `docs/`.
2. Read only the sections directly related to the requested subsystem.
3. Preserve the repo's planning style:
   - current confirmed behavior
   - proposed feature or subsystem
   - open questions and risks
   - recommended next steps
4. Keep `ESP-NOW`, local storage, and field-power assumptions explicit when they are affected.
5. Cross-link the new note from related mothership docs when useful.

## Expected Output Shape
- feature summary
- hardware integration summary
- workflow impact
- open questions and risks
- next steps before PCB or firmware commitment
