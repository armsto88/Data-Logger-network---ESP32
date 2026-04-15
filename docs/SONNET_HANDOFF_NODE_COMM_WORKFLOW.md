# Sonnet Handoff: Node Communication + Workflow Simplification

Date: 2026-04-15
Project: Data-Logger-network---ESP32

## 1) Problem Summary

Observed behavior:
- Mothership serial sometimes reports "no eligible PAIRED/DEPLOYED nodes".
- UI can still show one node as deployed.
- Node now power-cycles quickly (RTC gate + PWR_HOLD), so node serial is often unavailable after wake cycle.

This makes field diagnosis hard and creates confusion between:
- configured state (what the system intends), and
- reachability now (whether node is currently awake/listening).

## 2) Current Architecture (as-is)

Mothership:
- Tracks nodes in memory + NVS (paired/deployed list).
- Pushes commands (SET_SCHEDULE, SET_SYNC_SCHED, etc.) via ESP-NOW.
- Uses activity timeout to mark nodes inactive.

Node:
- Wakes from DS3231 alarm path.
- Asserts PWR_HOLD (IO23 active-high).
- Captures sensor data, may sync if slot is due.
- Rearms next alarm.
- Releases PWR_HOLD and powers down quickly.

## 3) Root-Cause Findings

### A) Reachability and state are conflated in some flows
- Some mothership paths treat only active nodes as eligible.
- UI state chip is primarily based on state enum (UNPAIRED/PAIRED/DEPLOYED), which can remain DEPLOYED while node is asleep.

Result: user sees DEPLOYED in UI, but serial says no eligible in paths that require active/reachable nodes.

### B) Push-only command timing is brittle for sleeping nodes
- If command is pushed while node is off, it is missed.
- With short hold window, command race conditions are common.

### C) Messaging is ambiguous
- "No eligible..." can mean:
  - no paired/deployed nodes,
  - no active nodes,
  - send failed,
  - command not acknowledged.

## 4) Confirmed Bench Behavior

Validated in serial:
- Alarm event runs.
- Next alarm is armed.
- A1F cleared.
- PWR_HOLD released immediately after cycle completion.

This sequence is working as intended.

## 5) Recommended Simpler Workflow

Adopt "desired state + node pull" as primary model.

### Core model
1. Mothership stores desired config per node (persistent):
   - desired_mode (ACTIVE / PAUSED)
   - wake_interval_min
   - sync_interval_min (or daily sync policy)
   - config_version
2. Node wakes and sends HELLO/STATE with current config_version.
3. Mothership responds with config only if version differs.
4. Node applies config, ACKs version, performs cycle, sleeps.

Benefits:
- No dependence on precise push timing.
- UI can clearly show desired vs reachable.
- Deterministic convergence after each wake.

## 6) Minimal-Risk Patch Set (incremental)

1. Separate deployment from liveness in UI and logs.
- Keep state enum as configured lifecycle.
- Add explicit link health fields:
  - reachable_now
  - last_seen_age

2. Make inactivity threshold sleep-aware.
- Replace fixed timeout with adaptive threshold:
  - max(5 min, 3 * wake_interval + guard)

3. Improve result taxonomy (messages + logs).
- Replace generic "no eligible" with exact outcomes:
  - NO_PAIRED_OR_DEPLOYED
  - NO_ACTIVE_TARGETS
  - SEND_FAILED
  - SEND_QUEUED_NO_ACK (if applicable)

4. Add optional short receive window after wake (configurable).
- Keep hold asserted for 1-3 s after HELLO or after sample cycle for command pickup.
- Feature-flagged for testing.

## 7) Ideal Patch Set (clean architecture)

1. Introduce protocol messages:
- NODE_HELLO { nodeId, state, currentConfigVersion, rtcUnix, queueDepth }
- CONFIG_SNAPSHOT { configVersion, desiredMode, wakeIntervalMin, syncPolicy, ... }
- CONFIG_APPLY_ACK { appliedVersion, ok, errorCode }

2. Mothership stores desired config in NVS per node and serves snapshots on HELLO.

3. Node applies idempotently:
- If received version <= current, ignore.
- If newer, apply and persist.

4. UI actions mutate desired config and bump version; no immediate success claim unless node ACKs.

## 8) Suggested Acceptance Criteria

1. UI/serial consistency:
- If node is DEPLOYED but asleep, UI shows DEPLOYED + REACHABILITY=ASLEEP/STALE.
- No misleading "no eligible" while a deployed node exists.

2. Config convergence:
- Change wake interval in UI.
- Node applies within next wake cycle and ACKs version.

3. Power-cycle resilience:
- Node can be off for long periods and still converge config on next wake.

4. Observability:
- Every command path logs exact reason for failure/outcome category.

## 9) Concrete Code Areas To Inspect

Mothership:
- firmware/mothership/src/comms/espnow_manager.cpp
- firmware/mothership/src/comms/espnow_manager.h
- firmware/mothership/src/main.cpp

Node:
- firmware/nodes/sensor-node/src/main.cpp
- firmware/nodes/shared/protocol.h

## 10) Sonnet Task Prompt (copy/paste)

You are implementing a robustness refactor for an ESP32 mothership-node ESP-NOW system where nodes are power-cycled and often asleep.

Goals:
1) Remove ambiguity between configured state and current reachability.
2) Make node config updates reliable even when nodes sleep most of the time.
3) Keep low-power behavior and existing deployment model compatible.

Please do the following:
1. Analyze and patch mothership eligibility logic so messaging distinguishes:
   - no paired/deployed nodes,
   - no currently reachable nodes,
   - send failure.
2. Add explicit reachability representation in UI and API payloads (without changing existing state enum semantics).
3. Implement a minimal pull-based handshake:
   - node sends HELLO with config version on wake,
   - mothership responds with config if newer,
   - node ACKs apply.
4. Keep current push paths for backward compatibility, but de-emphasize them in UI success wording.
5. Add feature flags for any timing-sensitive behavior (post-wake command window).
6. Provide migration-safe changes and a short validation checklist.

Constraints:
- Preserve low-power power-hold flow.
- Avoid breaking existing packet handling.
- Keep changes incremental and testable in bench conditions with intermittent node serial.

Deliverables:
- Code changes in mothership and node firmware.
- Updated protocol structs/messages.
- Updated UI/status wording.
- Notes on how to test with sleeping nodes.

## 11) Optional Extra: Queue-full handling

Current node queue can fill and reject new samples. Consider adding a policy switch:
- DROP_NEW (current),
- DROP_OLDEST (ring overwrite),
- FORCE_FLUSH_ON_FULL (if radio window permits).

For bench reliability, DROP_OLDEST is often preferred.
