---
description: "Plan mothership changes: power and wake architecture, enclosure fit, LTE backhaul, PCB feature proposals, and design-note updates. Keeps confirmed decisions separate from proposed work."
argument-hint: "Describe the mothership planning or documentation task"
---

# Mothership Planning

You are helping plan or document a mothership subsystem change for this ESP32-based system.

## Procedure
1. Identify the owning mothership design note in `mothership/docs/` or `docs/`.
2. Read only the sections directly related to the requested subsystem.
3. Preserve the repo's planning style — always separate these four layers:
   - **Confirmed current behavior**: what the hardware and firmware do today
   - **Proposed feature or subsystem**: what is being added or changed
   - **Open questions and risks**: unresolved choices before commitment
   - **Recommended next steps**: the smallest action that reduces uncertainty
4. Keep ESP-NOW, local storage, and field-power assumptions explicit when they are affected.
5. Cross-link the note from related mothership docs when useful.

## Key Docs to Check
- `mothership/docs/` — mothership-specific design notes
- `docs/FIRMWARE_AND_HARDWARE_NOTES.md` — consolidated bring-up and validation history
- `docs/concept_overview.md` — system-level context

## Output Shape
- **Feature summary**: one paragraph
- **Hardware integration summary**: what changes on the PCB or power tree
- **Workflow impact**: how ESP-NOW, SD logging, or LTE upload is affected
- **Open questions and risks**: unresolved before PCB or firmware commitment
- **Next steps**: ordered, smallest-first

$ARGUMENTS
