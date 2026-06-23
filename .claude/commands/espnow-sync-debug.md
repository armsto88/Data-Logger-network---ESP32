---
description: "Debug ESP-NOW node-to-mothership coordination, RTC sync, wake/sync flow, config replay, stale-node recovery, and deployment state semantics."
argument-hint: "Describe the ESP-NOW, sync, or deployment workflow issue"
---

# ESP-NOW Sync Debug

You are helping debug or change ESP-NOW coordination, sync scheduling, or node deployment state in this repo.

## Key Docs
- `docs/FIRMWARE_AND_HARDWARE_NOTES.md`
- `docs/SONNET_HANDOFF_NODE_COMM_WORKFLOW.md`
- `docs/concept_overview.md`

## Core Distinction
Always keep these three concerns separate:
- **Node side**: local wake, persistence, and RTC behavior
- **Mothership side**: desired config, orchestration, and command state
- **Transport**: ESP-NOW timing windows and message delivery

## Procedure
1. Read the current confirmed workflow docs before changing code or notes.
2. Identify which side owns the behavior in question.
3. Keep these concepts explicit and never conflate them:
   - paired vs deployed node state
   - RTC synced vs stale node state
   - immediate data collection vs sync-window transfer
   - durable local storage vs command replay
4. When proposing fixes, separate into four concerns:
   - Transport reliability
   - Scheduling logic
   - Persisted state model
   - User-visible node state semantics

## Output Shape
- **Current behavior summary**: what the code/docs confirm today
- **Likely owning subsystem**: node / mothership / shared transport
- **Failure mode or ambiguity**: the specific gap
- **Smallest safe fix direction**: what to change and why it is bounded
- **Targeted validation suggestions**: how to confirm the fix without running the full system

$ARGUMENTS
