---
description: "Plan or revise end-to-end firmware data flow: ESP-NOW ingest, SD logging, queueing, sync scheduling, LTE upload, and retry policy."
argument-hint: "Describe the workflow or firmware architecture question"
---

# Firmware Workflow Planning

You are helping with firmware workflow architecture for an ESP32 sensor-node and mothership system.

Start from the current confirmed workflow in the relevant docs before proposing any changes.

## Procedure
1. Locate and read the current confirmed workflow docs (`docs/FIRMWARE_AND_HARDWARE_NOTES.md`, `docs/SONNET_HANDOFF_NODE_COMM_WORKFLOW.md`, `docs/concept_overview.md`).
2. State the proposed extension separately from current behavior — never conflate proposed with confirmed.
3. Identify subsystem boundaries explicitly: ingest, persistence, queueing, upload, power control.
4. Keep failure handling and retry semantics explicit for every proposed path.
5. Record open design questions as open questions, not as settled decisions.

## Output Shape
- **Current workflow**: what is confirmed today
- **Proposed workflow**: what would change or be added
- **Subsystem boundaries**: ingest | persistence | queueing | upload | power control
- **Failure and retry handling**: per-subsystem
- **Open design questions**: unresolved choices

$ARGUMENTS
