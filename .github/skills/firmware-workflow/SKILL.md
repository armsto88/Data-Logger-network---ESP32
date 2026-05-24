---
name: firmware-workflow
description: 'Use for workflow and architecture notes involving ESP-NOW ingest, SD logging, queueing, sync scheduling, LTE upload flow, retry policy, and separation between local persistence and remote transfer.'
argument-hint: 'Describe the workflow or firmware architecture question'
---

# Firmware Workflow Planning

## When to Use
- Describing or revising end-to-end data flow
- Adding backhaul, queueing, retry, or scheduling concepts
- Converting rough workflow ideas into implementation-ready notes
- Comparing current workflow against proposed workflow extensions

## Procedure
1. Start from the current confirmed workflow in the relevant docs.
2. State the proposed extension separately from current behavior.
3. Identify subsystem boundaries such as ingest, persistence, queueing, upload, and power control.
4. Keep failure handling and retry semantics explicit.
5. Record open design questions rather than presenting them as settled decisions.

## Expected Output Shape
- current workflow
- proposed workflow
- subsystem boundaries
- failure and retry handling
- open design questions
