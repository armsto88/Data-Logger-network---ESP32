# Firmware Functionality Overview for Ecologists

Date: 2026-04-21
Audience: Ecologists, field practitioners, and environmental monitoring teams
Purpose: Explain what the firmware does in practical terms, how data moves through the system, and how field edge cases are handled.

## 1. Why this firmware exists

This firmware is designed for ecological monitoring where sensors may be deployed in remote locations, run on limited power, and need to keep collecting data even when communications are intermittent.

The system is built around two priorities:

1. Protect field data first.
2. Use radio and power only when needed.

## 2. System at a glance

The platform has two device roles:

1. Node
- A field sensor unit that wakes, measures, stores readings, and returns to low power.
- It does not depend on constant network connectivity.

2. Mothership
- A collection and coordination unit.
- It receives uploaded node data, writes records to storage, and manages schedules/configuration.

In practical terms, nodes do the measuring and buffering, while the mothership does the coordination and aggregation.

## 3. Core behavior in normal operation

### 3.1 Node behavior

At each scheduled wake:

1. Wake from RTC alarm.
2. Capture sensor readings.
3. Store readings in local queue memory.
4. Return to low power.

At sync windows:

1. Wake and enable radio.
2. Listen for mothership sync marker.
3. If marker is seen, upload queued records.
4. Re-arm alarms and sleep.

This queue-first design means the node can continue sampling even if the mothership is unavailable.

### 3.2 Mothership behavior

The mothership:

1. Broadcasts sync schedule and sync markers.
2. Receives node uploads.
3. Writes records to CSV storage.
4. Tracks node status for the web UI.
5. Pushes configuration updates when nodes check in.

## 4. Data integrity approach

The firmware prioritizes no-loss behavior under normal fault conditions.

1. Queue-first sampling
- Data is collected locally first and uploaded later.

2. Marker-gated uploads
- Nodes upload only when sync marker conditions are met, reducing accidental out-of-window traffic.

3. Overflow policy
- Queue uses controlled overwrite strategy (drop oldest) when full.
- This protects latest observations in prolonged outage conditions.

4. Configuration handshakes
- Config snapshots and acknowledgements are used to converge node settings safely.

## 5. Power and battery strategy

The node is optimized for low duty cycle operation:

1. Radio mostly off between required communication windows.
2. Sensor and communication tasks are brief and bounded.
3. RTC alarms drive deterministic wake/sleep cycles.

The mothership can support always-on bench operation now, with planned field mode where it powers on only for sync windows and user interaction.

## 6. Typical field workflow

A practical deployment cycle looks like this:

1. Discover and pair node.
2. Deploy with sampling and sync settings.
3. Node collects data autonomously.
4. Node uploads buffered data during sync windows.
5. Mothership stores data and reports health in UI.

When working correctly, operators can verify health through:

1. Recent node contact.
2. Queue depth trends.
3. Config status.
4. Presence of fresh CSV records.

## 7. Edge cases and how firmware handles them

### 7.1 Missed sync windows

If a node misses one or more sync opportunities, data remains in local queue and is retried in later windows.

### 7.2 Time drift or stale sync

Both sides support stale-sync recovery logic:

1. Node-side stale recovery
- Node can request time and seek re-lock during bounded recovery windows.

2. Mothership-side stale assist
- Mothership infers stale behavior and can push schedule and time corrections.

### 7.3 Duplicate deploy commands

Duplicate deploy handling is hardened to prevent unintended extra immediate sampling cycles.

### 7.4 Boot alarm ambiguity

Boot-time alarm flag handling preserves wake reason information so wake classification is not masked.

### 7.5 Recovery from difficult field state

A 3-boot rescue mode is implemented for field recovery:

1. Trigger: three quick power cycles.
2. Action: node resets to unpaired-safe state.
3. Behavior: node stays awake, listens, and announces status.
4. UI: mothership receives explicit node status updates and reflects unpaired state.

This gives teams a physical recovery path without needing extra wiring or deep technical intervention.

## 8. Operator-facing status meaning

To reduce confusion, the UI is designed to represent:

1. Intended interval (configured target) rather than only inferred runtime interval.
2. Queue depth as pending samples reported by the node.
3. Config pending versus config updated state.
4. Node contact freshness (awake/asleep interpretation).

These indicators help users interpret whether a node is healthy, delayed, unsynced, or awaiting configuration actions.

## 9. Current strengths and practical limits

### Strengths

1. Robust queue-first data capture.
2. Strong low-power behavior for field nodes.
3. Improved recovery from sync and deployment edge cases.
4. Field-friendly rescue mode for hard recovery.

### Practical limits (current stage)

1. Fleet-scale contention at larger node counts still needs tuning and profiling.
2. Mothership full scheduled-power architecture is still evolving.
3. Long-term validation across many-node deployments is ongoing.

## 10. Suggested wording for manuscript framing

Suggested high-level framing:

The firmware architecture was designed for ecological field reliability under intermittent communications and constrained power budgets. Nodes prioritize local persistence and deferred transfer, while the mothership coordinates synchronization and collection windows. Recovery paths include automated stale-sync correction and an operator-triggered rescue mode to restore field recoverability without specialized hardware interfaces.

## 11. Next manuscript additions

Recommended follow-on sections for the manuscript:

1. Validation summary table (single-node and multi-node test outcomes).
2. Battery budget estimates for representative sampling intervals.
3. Data completeness metrics across induced communication outages.
4. Field protocol appendix for deployment, recovery, and troubleshooting.
