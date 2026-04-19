# Firmware Sync Workflow and Testing Strategy

Date: 2026-04-16
Scope: Mothership + sensor-node power, wake, sample, and sync behavior

## 1) Intended Operating Model (formalized)

### 1.1 Mothership (target field behavior)

- Mothership should be mostly off in the field.
- It should power on for two reasons:
  - Scheduled fleet sync windows.
  - User interaction requests (for example via reed or magnetic wake trigger).
- During sync windows, mothership should have WiFi/ESP-NOW available to receive queued uploads from all nodes.
- Mothership writes received records to SD card CSV.

### 1.2 Node (target field behavior)

- Node wake interval is configured from the mothership UI.
- At each node interval wake:
  - Wake from RTC alarm.
  - Take sensor readings with WiFi normally off.
  - Store readings in local flash queue.
  - Re-arm next RTC alarm.
  - Power down again.
- At scheduled sync times:
  - Wake as normal on RTC alarm.
  - Bring up radio.
  - Flush queued records to mothership.
  - Return to low-power state.

### 1.3 Conflict rule: sample interval vs sync schedule

- If a sync time and a sample wake align, the node should do one combined wake cycle:
  - sample first,
  - then sync flush,
  - then sleep.
- If they do not align exactly, sync should occur at the first wake that falls into the sync slot logic.

## 2) Current Codebase State vs Target Model

This section is based on current code in:
- firmware/mothership/src/main.cpp
- firmware/mothership/src/comms/espnow_manager.cpp
- firmware/nodes/sensor-node/src/main.cpp
- firmware/nodes/sensor-node/src/storage/local_queue.cpp

### 2.1 Implemented now (good alignment)

1. Node queue-first design is implemented.
- Node samples to local queue every alarm cycle.
- Node flushes queue only when sync slot is due.
- Evidence: shouldSyncAt(...) + captureSensorsToQueue(...) + flushQueuedToMothership(...).

2. Node radio duty-cycling is implemented.
- ESP-NOW/WiFi is brought up when needed and shut down after flush.
- A post-wake command window exists for config pull testing.

3. Pull-based config convergence is implemented.
- Node sends NODE_HELLO on wake.
- Mothership can reply with CONFIG_SNAPSHOT when desired config version is newer.
- Node replies with CONFIG_ACK after apply.

4. Queue overflow policy is now DROP_OLDEST.
- Full queue no longer rejects new data; oldest record is dropped first.

5. Mothership node-liveness messaging has improved.
- Distinguishes sleeping deployed nodes from absent/unpaired fleet cases.

### 2.2 Partially implemented / gaps

1. Mothership low-power operation is NOT implemented.
- Mothership currently runs continuously.
- In loop(), web server, ESP-NOW, BLE, time/status logs run all the time.
- Daily sync trigger broadcasts schedule, but does not power-cycle mothership hardware.

2. UI semantics still look always-on in some places.
- Current UX supports always-available AP/web interactions.
- No explicit "wake mothership" mode state machine exists yet.

3. Scale behavior for many nodes is not yet flow-controlled.
- Current flush behavior is simple send loop per node.
- No contention management (slot offsets, jitter windows, node backoff, airtime fairness) is defined.

4. Testing ergonomics remain difficult for long sync periods.
- One-day sync cadence is valid for field, but too slow for development feedback.

## 3) Notes on Many-Node Sync Effectiveness

With many nodes, simultaneous radio activity can cause:
- increased collisions,
- retries/dropouts,
- uneven upload completion times.

Recommended production pattern:

1. Keep one fleet sync window anchor (phaseUnix), but spread node uploads using deterministic per-node offset.
- Example: offsetSec = hash(nodeId) mod windowSeconds.

2. Keep upload window long enough for worst-case fleet size.
- Example: N nodes x average packet count x airtime + retry margin.

3. Add per-node bounded retry and resume.
- If window closes, continue next sync slot from remaining queue.

4. Add observability counters.
- queue depth before/after sync,
- sent count,
- failed count,
- retries,
- time spent in sync window.

## 4) Practical Testing Strategy (methodical)

Use two runtime profiles: bench and field.

### 4.1 Bench profile (fast feedback)

Purpose: validate full workflow quickly.

Recommended bench settings:
- node interval: 1 min
- sync interval: 5 min
- post-wake window: 1500 to 3000 ms
- mothership always-on AP/web enabled

Bench acceptance checks:
1. Node wakes every minute and queues data.
2. Queue depth increases between sync slots.
3. At sync slot, queue drains to mothership.
4. Mothership logs contiguous CSV rows with expected node timestamps.
5. Node powers down after cycle (verify gate drain voltage drop).

### 4.2 Field profile (deployment realism)

Purpose: validate long duty cycle and power budget.

Recommended field settings:
- node interval: required agronomy cadence (for example 5 or 10 min)
- sync: daily or few-times-per-day per battery budget
- post-wake window: minimum stable value
- mothership wake mechanism: scheduled + user wake trigger

Field acceptance checks:
1. No data loss over long idle periods.
2. Queue growth and flush behavior match expected sync cadence.
3. End-to-end SD logs remain consistent across power cycles.

### 4.3 Testability improvements to add

1. Add a dedicated TEST MODE flag in node firmware.
- Keep power logic intact but print a compact cycle summary each wake.
- Optional: keep serial alive for a short grace window before cut.

2. Add mothership "force sync now" action (already partly present via schedule broadcasts).
- Make it explicit in UI as a test button.

3. Add per-cycle telemetry row type.
- NODE_CYCLE_START, NODE_CYCLE_END, QUEUE_FLUSH_RESULT.

4. Add compile-time profiles in platformio.
- env:esp32wroom-bench
- env:esp32wroom-field
with different POST_WAKE_WINDOW_MS and debug verbosity.

## 5) Immediate Next Steps (recommended)

1. Keep developing against bench profile until stable.
2. Add explicit mothership power-state design doc (always-on vs scheduled-on architecture).
3. Implement many-node sync staggering before large fleet tests.
4. Add a one-page test checklist and run log template for repeatable validation.

---

## 6) Current Reality Summary

- Node queue-then-sync architecture: implemented.
- Pull config update handshake: implemented.
- Drop-oldest queue overflow policy: implemented.
- Mothership low-power scheduled wake architecture: not implemented yet.
- Many-node upload collision management: not implemented yet.

This means the firmware core is close to your intended node behavior, but mothership power-state architecture and fleet-scale sync coordination are the two major remaining design items.

## 7) Known True State (Validated 2026-04-16 Evening)

This section captures what is confirmed by recent bench logs and code behavior after the latest mothership updates.

### 7.1 Confirmed working behaviors

1. Interval-mode sync scheduling is working as configured.
- Mothership emits one sync trigger per interval slot.
- SYNC_AUDIT countdown is consistent with configured interval (for example 5 min).

2. Node queue-first behavior is working.
- Node records are generated at node cadence and uploaded in batches during sync windows.
- Evidence pattern: multiple SENSOR packets arrive close together on mothership time with different NODE_TS values.

3. Config apply loop now converges even when HELLO is missed.
- New fallback behavior: on sensor contact, mothership can push CONFIG_SNAPSHOT if desired config is still pending.
- Evidence in logs: "CONFIG_SNAPSHOT vX push ... on sensor contact: OK" followed by "CONFIG_ACK ... ok=1".

4. UI config truth now has hard protocol confirmation.
- CONFIG_ACK updates node applied version on mothership.
- When applied version reaches desired version, Node Manager can correctly show Config updated.

5. Awake/asleep chip is now a short-lived contact indicator.
- AWAKE means recent packet contact.
- ASLEEP indicates no recent packets, not a deployment failure.

### 7.2 Confirmed caveats (still true)

1. NODE_HELLO is not guaranteed every cycle at mothership.
- HELLO can be missed due to radio timing/window overlap.
- System should not rely on HELLO alone for config convergence.

2. Direct unicast send callback failures can coexist with successful broadcast-driven workflow.
- "send_cb FAIL" to node MAC may appear while broadcast command path still succeeds.

3. Transport sync interval and node sample interval are separate.
- Example observed: sync interval 5 min while inferred node sample cadence ~2 min.
- This is valid and expected under queue-first design.

### 7.3 Operator quick checks (current reliable method)

Use these three checks during bench tests:

1. Sampling check:
- Verify NODE_TS advances by expected sample cadence (for example 120 s means ~2 min).

2. Sync check:
- Verify batch arrival near sync boundary and SYNC_AUDIT count increment.

3. Config check:
- Look for CONFIG_SNAPSHOT push and CONFIG_ACK.
- Node Manager should move from Config pending to Config updated after ACK.

### 7.4 Practical interpretation rule

If data batches arrive and CONFIG_ACK confirms applied version, the system is behaving correctly even if HELLO logs are sparse.

## 8) Tomorrow Priority: UI Confidence Pass (Short-Horizon)

Goal: make the UI useful, simple, and trustworthy at a glance for real operators under time pressure.

### 8.1 Node Card “Truth Line”

- Keep one plain-English status sentence per node.
- Show: observed sample cadence, last contact age, config state, awake/asleep state.
- If pending, include reason: desired vs observed interval.

Done when:
- A user can explain node state from one card without opening serial logs.

### 8.2 Status Chips: deterministic behavior

- Keep only essential chips in Node Manager:
  - Config pending / Config updated
  - Awake / Asleep
- Remove any chip that does not map to a clear protocol or timing condition.

Done when:
- Chips never contradict serial evidence over a full sync cycle.

### 8.3 Fast visual rhythm

- Keep Node Manager auto-refresh at a stable interval (currently 15s).
- Ensure refresh does not jump-scroll or cause click interruptions.
- Keep row layout compact for many nodes.

Done when:
- Page remains usable with 20+ node rows and frequent updates.

### 8.4 “At a glance” header strip

- Add a compact summary row above node list:
  - Deployed count
  - Pending config count
  - Nodes seen in last minute
  - Last sync trigger age

Done when:
- Operator can tell fleet health in under 5 seconds.

### 8.5 Explain failures, not just states

- For config pending, show one short reason label:
  - Waiting ACK
  - Desired 1 min / observed 2 min
  - No recent contact
- For asleep, avoid warning styling unless it is actually abnormal.

Done when:
- Users stop needing serial logs for routine interpretation.

### 8.6 Confidence checks before closing the day

- Run one scripted bench check:
  1. Change interval in UI.
  2. Wait one sync window.
  3. Confirm CONFIG_ACK seen.
  4. Confirm UI flips to Config updated.
- Capture screenshot set: before change, during pending, after updated.

Done when:
- A new team member can reproduce the same interpretation with only the UI.

## 9) Informal Parking Lot (Next Discussions)

Keeping this informal for now so nothing gets lost.

### 9.1 Rolling firmware to multiple devices

- Start with a tiny canary batch first (1 mothership + 1 node), then widen.
- Keep a simple release note each flash day: version, what changed, known risks.
- Keep last known-good binaries handy so rollback is quick if anything gets weird.
- Capture "first good cycle" evidence per device (paired, deployed, data seen, config ack seen).

### 9.2 Proper radios-off test

- Need a clean proof run where node is mostly radio-off between wake cycles.
- Confirm radio only comes up during sync slots / command window.
- Log current draw phases (sleep, wake, sync) so we have real numbers, not guesses.
- Define a pass/fail line before test day to avoid "it feels fine" decisions.

### 9.3 Mothership power-up on sync time

- Wire mothership power latch so RTC/schedule can wake it for sync windows.
- On boot: bring radio up fast, collect uploads, then power down cleanly.
- Keep manual wake path for bench/debug and emergency access.
- Add watchdog/timeout so it cannot stay on forever if sync stalls.

### 9.4 Why this matters

- This is the bridge from bench-success to field-reliability.
- If we do these three well, deployment confidence goes up a lot.

### 9.5 Revisit sensor data packet schema

- Need a quick schema review so payloads stay future-proof as we scale nodes/sensors.
- Current packet works, but we should decide what is mandatory vs optional before wider rollout.
- Topics to revisit:
  - Explicit schema version field for forward compatibility.
  - Clear timestamp semantics (sample time vs upload time).
  - Stable sensor identity model (sensorId + type + label rules).
  - Units and scaling conventions per sensor type.
  - Quality flags definition table (what each bit means).
  - Sequence / batch hints for better replay and gap detection.
  - Max packet size guardrails and fragmentation strategy (if ever needed).

- Keep goal practical: improve reliability and interpretation without overcomplicating the wire format.

## 10) Future Sync-Alarms Architecture (Planned)

Date added: 2026-04-19
Status: Planned design direction for next sync reliability iteration.

### 10.1 Why this change is needed

- Current sync behavior is slot-based and robust, but node wake timing can drift relative to a single fleet sync broadcast window.
- For larger fleets, we want all deployed nodes awake and listening inside the same global sync window.
- Minute-level resolution is acceptable as long as windows include safety buffers.

### 10.2 Target behavior (global sync truth)

1. One global sync phase and interval applies to all deployed nodes.
2. Node sampling interval remains independent from sync interval.
3. Each node wakes into a sync window before the mothership sync broadcast.
4. Mothership emits sync broadcast bursts during the shared window.
5. Nodes flush queued records during that window, then return to low power.

### 10.3 DS3231 dual-alarm strategy

- Alarm1 (A1): data-cycle wake path.
  - Trigger next sample wake from end of data cycle using node data interval.
- Alarm2 (A2): global sync-window wake path.
  - Trigger at next global sync minute boundary (or buffered pre-sync minute logic).
- Both A1F and A2F must be checked and cleared each wake.
- If both alarms are due, do one combined wake cycle:
  - sample,
  - then sync flush,
  - then re-arm both alarms,
  - then power down.

### 10.4 Sync-window buffer policy (minute-resolution)

Initial defaults for bench validation:

- Pre-sync wake buffer: 15 to 30 seconds.
- Post-sync listen buffer: 30 to 60 seconds.
- Mothership sync burst count: 2 to 3 broadcasts spaced about 200 ms.

Acceptance intent:

- All deployed nodes should have radio up before first burst.
- At least one burst is received by every listening node in normal RF conditions.

### 10.5 Command/config contract expectations

- On deploy, mothership provides:
  - node data interval,
  - global sync interval,
  - global sync phase anchor.
- Node stores all three and computes:
  - next data alarm (A1),
  - next sync alarm/window (A2).
- Global sync interval changes must propagate to all desired node configs so late deployments still converge to the same sync phase.

### 10.6 Planned implementation steps

1. Add A2 read/write helpers and A2F handling in node firmware.
2. Introduce explicit wake-reason evaluation (A1, A2, both).
3. Add shared re-arm routine that always reprograms both alarms before power cut.
4. Add configurable sync window constants (pre/post buffer) behind compile-time flags.
5. Add mothership sync broadcast burst scheduling tied to global phase boundary.
6. Add debug counters:
  - sync window entered,
  - bursts sent,
  - bursts received,
  - queue before/after flush,
  - missed window count.

### 10.7 Validation checklist for this architecture

Bench checklist (single node first):

1. Deploy node with data interval and sync interval set.
2. Confirm immediate data cycle on deploy.
3. Confirm A1 and A2 are both armed before power-down.
4. Confirm wake reason logs correctly identify A1/A2/both.
5. Confirm node is awake during pre-sync buffer and receives mothership burst.
6. Confirm queue drains during sync window.

Fleet checklist (4+ nodes):

1. All nodes report same sync phase and interval.
2. All nodes enter sync window for the same global slot.
3. Mothership receives uploads from all nodes within configured window budget.
4. No persistent starvation of any node across 10+ sync slots.

### 10.8 Risks and mitigations

- Risk: Minute-only A2 precision can cause edge misses.
  - Mitigation: keep pre/post buffers and burst broadcasts.
- Risk: Combined A1+A2 cycles increase awake time.
  - Mitigation: bounded windows and deterministic shutdown.
- Risk: Fleet airtime collisions in bigger deployments.
  - Mitigation: add deterministic per-node upload offset within sync window.

## 11) Detailed Deploy Workflow (Current Code, 2026-04-19)

This section captures the deploy path exactly as currently implemented in firmware.

### 11.1 Mothership-side workflow

1. Operator opens node config and submits action `start` with interval.
2. Mothership clamps interval to allowed set and resolves the node record.
3. Mothership writes desired config snapshot:
  - `wakeIntervalMin` from UI,
  - `syncIntervalMin` from current global sync interval,
  - `syncPhaseUnix` from current global phase (`gLastSyncBroadcastUnix`, fallback to RTC now),
  - increments `configVersion` when changed.
4. If node is `UNPAIRED`, mothership sends pair commands (`PAIR_NODE` + `PAIRING_RESPONSE`) and flips local state to `PAIRED`.
5. Mothership sends deploy request:
  - pushes pre-deploy `CONFIG_SNAPSHOT` if available,
  - sends `DEPLOY_NODE` with RTC timestamp payload,
  - marks local node state `DEPLOYED` immediately,
  - queues pending target `PENDING_TO_DEPLOYED`.
6. Mothership persists paired/deployed state and waits for runtime evidence (`DEPLOY_ACK`, HELLO, sensor contact, etc.) to clear pending.

### 11.2 Node-side deploy workflow

1. Node receives `DEPLOY_NODE` and validates node id.
2. Node sets RTC from deploy payload and marks runtime state:
  - `rtcSynced=true`,
  - `deployedFlag=true`,
  - `lastTimeSyncUnix=rtc.now()`,
  - initializes sync slot tracking.
3. Node sends `DEPLOY_ACK` to mothership.
4. Node executes immediate first deployed cycle:
  - samples sensors,
  - appends records to local queue,
  - does not require radio flush in this immediate step.
5. Node re-arms wake alarms before sleep:
  - Alarm1 (A1): next data wake from `now + data interval`,
  - Alarm2 (A2): next sync wake from global phase/interval with pre-wake buffer.
6. Node enables both alarm interrupts, clears alarm flags, powers radio off, then schedules power cut.

### 11.3 Node wake behavior while deployed

Wake source resolution is flag-based:

1. Data wake (`A1F=1`):
  - clear A1 flag,
  - sample to queue,
  - keep radio off.
2. Sync wake (`A2F=1`):
  - clear A2 flag,
  - bring radio up,
  - send `NODE_HELLO`,
  - hold listen window,
  - flush queued data.
3. Combined wake (`A1F=1` and `A2F=1`):
  - execute both steps in one cycle,
  - re-arm both alarms,
  - power down.

### 11.4 Global sync behavior (current)

1. Global sync interval changes update all desired node configs to same interval/phase.
2. `SET_SYNC_SCHED` broadcast now uses burst send (3 packets, 200 ms spacing).
3. Node stores `syncIntervalMin` and `syncPhaseUnix` and uses these for sync-wake scheduling.

### 11.5 Current fail points / considerations

1. `SET_SYNC_SCHED` apply path does not currently force immediate re-arm of Alarm2.
  - Effect: node may continue with previously armed A2 until next cycle re-arm.
  - Mitigation: on receiving `SET_SYNC_SCHED`, immediately call shared alarm re-arm helper when deployed.

2. Sync wake flush is time-window driven, not explicit "broadcast received" gated.
  - Effect: node may flush after listen window even if no sync burst was observed.
  - Mitigation: add explicit sync-burst marker command/flag and gate flush on marker or timeout policy.

3. Radio-on window cost can dominate battery if oversized.
  - Effect: strong battery penalty with long `SYNC_LISTEN_WINDOW_MS`.
  - Mitigation: tune pre/post buffers from measured success rate and keep hard max window.

4. Combined A1+A2 wake can increase cycle duration under heavy backlog.
  - Effect: longer awake time and potential missed next intervals in extreme queue conditions.
  - Mitigation: add per-cycle flush budget and resume remainder next sync window.

5. Mothership burst broadcasts improve reception but do not guarantee all-node capture in congested RF.
  - Effect: occasional missed sync windows for some nodes.
  - Mitigation: maintain deterministic retries and add per-node success/miss counters for adaptive tuning.

### 11.5.1 Fixed in current code (2026-04-19)

1. `SET_SYNC_SCHED` now immediately re-arms deployed node alarms.
- Previous risk: node could keep stale A2 timing until next cycle.
- Current behavior: on receiving `SET_SYNC_SCHED`, deployed+synced node re-arms A1/A2 immediately.

2. Sync wake flush is now gated by explicit sync-window marker.
- Previous risk: node could flush after a listen delay even if no sync call was observed.
- Current behavior: mothership broadcasts `SYNC_WINDOW_OPEN` marker bursts; sync wake listens for marker and only flushes when marker is seen.

3. Mothership now emits separate sync-window marker bursts during sync trigger.
- Current behavior: interval/daily sync trigger sends both schedule burst and marker burst.

### 11.6 Validation checklist for this implemented flow

1. Deploy one node and confirm immediate sample is queued before first sleep.
2. Confirm both A1 and A2 are armed after deploy.
3. Confirm data wake samples with radio off.
4. Confirm sync wake brings radio up, listens, then flushes queue.
5. Change global sync interval and verify all desired configs update to same phase.
6. Verify pending deploy clears on runtime confirmation evidence.

