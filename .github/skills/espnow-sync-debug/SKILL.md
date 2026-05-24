---
name: espnow-sync-debug
description: 'Use for firmware coding and debugging tasks involving ESP-NOW, node deployment state, RTC sync, scheduled wake/sync flow, config replay, stale-node recovery, and mothership-node coordination.'
argument-hint: 'Describe the ESP-NOW, sync, or deployment workflow issue'
---

# ESP-NOW Sync Debug

## When to Use
- Debugging node-to-mothership coordination
- Changing deployment, wake, sync, or config-replay behavior
- Working on stale-node recovery or node liveness semantics
- Updating docs or code around `SET_SCHEDULE`, `SET_SYNC_SCHED`, `TIME_SYNC`, or sync-window behavior

## Repo Context
- The current workflow is documented in `docs/FIRMWARE_AND_HARDWARE_NOTES.md`, `docs/SONNET_HANDOFF_NODE_COMM_WORKFLOW.md`, and `docs/concept_overview.md`.
- The core distinction in this repo is between:
  - local node persistence and wake behavior
  - mothership-side desired config and orchestration
  - ESP-NOW transport and timing windows

## Procedure
1. Start from the current confirmed workflow in the docs before changing code or notes.
2. Identify which side owns the behavior:
   - node wake and persistence
   - mothership orchestration and config state
   - shared protocol or timing boundary
3. Keep these concepts explicit:
   - paired vs deployed state
   - RTC synced vs stale state
   - immediate data collection vs sync-window transfer
   - durable local storage vs command replay
4. When proposing fixes, separate:
   - transport reliability
   - scheduling logic
   - persisted state model
   - user-visible node state semantics

## Output Shape
- current behavior summary
- likely owning subsystem
- failure mode or ambiguity
- smallest safe fix direction
- targeted validation suggestions
