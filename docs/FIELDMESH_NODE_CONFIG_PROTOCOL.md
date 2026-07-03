# FieldMesh `NODE_CONFIG` — unified declarative mothership→node control

**Status:** implementing (2026-07-03). Replaces the fire-and-forget commands
(`SET_SCHEDULE`, `SET_SYNC_SCHED`, `UNPAIR_NODE`) for **deployed** nodes with a
single declarative desired-state message reconciled during the sync window.

## Why

Deployed nodes deep-sleep and are only reachable during their brief sync window.
The old commands were sent **imperatively from config mode**, while deployed
nodes are asleep — so they were silently lost (schedule changes didn't apply,
unpair didn't stop the node, data kept flowing from "removed" nodes). See the
investigation in the git history / memory `unpair-stop-delivery-gap`.

## Principle

- **In-hand / provisioning** (node awake, you're next to it): `PAIR`, initial
  `DEPLOY` stay imperative — unchanged.
- **Deployed / field** (node asleep, reachable only at sync): everything that
  changes an already-deployed node is **declarative** — the mothership holds a
  per-node *desired config* and re-broadcasts it every sync window until the node
  converges. Idempotent, self-healing, never orphans.

## Wire message (shared `protocol.h`)

```c
typedef struct node_config_message {
    char     command[16];       // "NODE_CONFIG"
    char     nodeId[16];        // target node (broadcast-safe; node matches its own NODE_ID)
    char     mothership_id[16];
    uint16_t configVersion;     // monotonic desired version
    uint8_t  targetState;       // 0=UNPAIRED, 1=PAIRED(reserved), 2=DEPLOYED(active), 3=STANDBY(paused)
    uint8_t  wakeIntervalMin;   // 1,5,10,20,30,60
    uint16_t syncIntervalMin;   // sync cadence (minutes)
    uint8_t  _pad[2];
    uint32_t syncPhaseUnix;     // sync anchor (unix seconds)
} node_config_message_t;         // 60 bytes
```

Confirmation reuses the existing `config_apply_ack_message_t` ("CONFIG_ACK",
40 bytes): `{ command, nodeId, appliedVersion, ok }`.

## Version rule (idempotency)

- The mothership bumps `configVersion` on **every** desired change (schedule
  change *or* unpair).
- The node applies a `NODE_CONFIG` only if `configVersion > appliedConfigVersion`
  (strict). On apply it sets `appliedConfigVersion = configVersion`, persists to
  NVS, and **echoes it in every snapshot** (`node_snapshot_v2.configVersion`).
- The mothership treats the node as converged when the echoed
  `configVersionApplied >= desiredConfigVersion`.

## Node behaviour on `NODE_CONFIG` (validated: target nodeId == NODE_ID, sender == bound mothership)

| `targetState` | Action |
|---|---|
| `DEPLOYED` (2) — active | Apply wake interval if changed; **clear `recordingPaused` (resume)** and re-arm the recording alarm A1 if it was paused; set version; persist; `CONFIG_ACK`. NODE_CONFIG governs the wake (A1) interval only — the sync (A2) schedule stays on the `SET_SYNC_SCHED` handover. |
| `STANDBY` (3) — paused | Set persisted `recordingPaused=true`; re-arm with **A2 (sync) only, no A1 (recording)** so the node still checks in but takes no samples; set version; persist; `CONFIG_ACK`. Remote seasonal pause; resume with `DEPLOYED`. |
| `UNPAIRED` (0) | Send `CONFIG_ACK(version, ok=1)` **first**, then wipe to unpaired (clear mothership MAC, `deployedFlag`, `recordingPaused`, disable alarms, persist, radio-hold) — the existing unpair path. |
| `PAIRED` (1) | Reserved (no-op for now). |

`recordingPaused` is persisted in `NodeConfigStoreRecord` so standby survives the
per-wake power-cycle; on boot a paused node arms A2-only and skips sampling. It is
distinct from the node lifecycle state — a paused node is still `DEPLOYED`.

## Mothership desired-state store (durable, survives sleep)

Extend the existing `node_dcfg` NVS namespace (`getDesiredConfig`/`setDesiredConfig`):
add `targetState` (key `t`, default **2 = DEPLOYED** so pre-existing nodes keep
running). Already stores `configVersion` (`v`), `wakeIntervalMin` (`w`),
`syncIntervalMin` (`s`), `syncPhaseUnix` (`p`).

## Config-mode actions → desired state (no imperative sends to deployed nodes)

- **Schedule / sync change on a DEPLOYED node:** bump `configVersion`, update
  `wake`/`sync`/`phase`, `targetState = DEPLOYED`. (No broadcast here — the sync
  window does it.)
- **Remove a DEPLOYED node:** `targetState = UNPAIRED`, bump `configVersion`.
  **Keep it in the registry** (shown as "removing…"); do not delete locally, do
  not send anything now.
- **Remove an awake node** (UNPAIRED/PAIRED, still listening in config mode):
  keep the existing immediate `sendUnpairToNode` + local `unpairNode` — that
  path works because the node is awake.
- **Deploy:** unchanged imperative `DEPLOY_NODE`, and also set desired
  `targetState = DEPLOYED` + version + schedule so the reconcile keeps it there.

## Sync-window reconcile (`handleSyncWake`, each window)

1. During the window, for every registered node whose desired `targetState` is
   `DEPLOYED` **or** `UNPAIRED`, broadcast `NODE_CONFIG` built from its durable
   desired config. (This replaces `broadcastWakeIntervalNow` +
   `broadcastSyncScheduleNow`.)
2. Snapshots received update `configVersionApplied` + `lastSeen` (already done).
3. Drain `CONFIG_ACK`s received this window (new: the sync receiver enqueues them
   alongside snapshots). For each ACK:
   - node's desired `targetState == UNPAIRED` and `appliedVersion >= desired` →
     **remove** the node from the registry + clear its desired config + persist.
   - `targetState == DEPLOYED` → mark converged.
4. A node that never ACKs stays listed as "removing" and keeps getting the
   broadcast every window — it is **never** removed on absence alone, so a node
   on a flaky link is never orphaned.

## Message-set simplification

| Before | After |
|---|---|
| `PAIR_NODE`, `DEPLOY_NODE` (in-hand) | unchanged |
| `SET_SCHEDULE`, `SET_SYNC_SCHED` | folded into `NODE_CONFIG` |
| `UNPAIR_NODE` (deployed) | folded into `NODE_CONFIG` (`targetState=UNPAIRED`) |
| `UNPAIR_NODE` (in-hand, awake) | still works via provisioning channel |
| UI "Stop" vs "Remove" | collapsed — both remove/unpair (only reliable halt) |

Old node handlers (`SET_SCHEDULE`/`SET_SYNC_SCHED`/`UNPAIR_NODE`) are **kept** for
back-compat during rollout; the mothership simply stops sending them.

## Flash order (must not brick field comms)

Reflash **all nodes first**, then the mothership:
- New node + old mothership → node still honours old commands (kept). ✓
- New mothership + old node → old node ignores `NODE_CONFIG` (unknown command)
  and keeps running its last schedule; no breakage, just no new control until it
  is reflashed. ✓
- New mothership + new node → full declarative reconcile. ✓

Plain flash (`-t upload`) preserves NVS (pairing/anchor/desired-config).
