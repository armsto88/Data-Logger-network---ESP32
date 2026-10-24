# Rendezvous Stage 0: follow-up plan after two code reviews

**Date:** 2026-10-24
**Status:** Approved by the owner, 2026-10-24, except section 3.5 (still open).
Section 3.1 ships on its own first (branch `fix/stage0-purge-order`); sections
3.2 to 3.5 are not written yet.
**Context:** Stage 0 (`MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` section 8) is
merged on `main` as PR #7 and bench-confirmed (`SYNC_RENDEZVOUS_BENCH_RUNBOOK.md`,
"Second run"). A first review found defects. A patch for them (`632b456`,
branch `fix/sync-stage0-review`, never pushed) was reviewed again and was only
partly right. This plan replaces that patch with one designed change.

## 1. Where things stand

**Verified correct by both reviews:** the Stage 0 rendezvous itself (45 s
pre-wake, join held to slot + 25 s, early close on a full roster, 20 s late-join
grace, drain budget from the join end, `[SYNC-STAT]`), with no iterator
invalidation or time-arithmetic faults. From `632b456`, reverting Stage 0d (the
retention purge back before the window) and the multi-port logger fixes.

**`main` is not safe for a field hub yet.** It still carries Stage 0d, which can
leave the purge without the free space it needs (section 3.1). No field hub runs
it: the demo hub is on the pre-Stage 0 build and HUB-R6-01 is a bench board.

**Defects still open:**

| # | Defect | Origin |
| --- | --- | --- |
| D1 | `CONFIG_ACK` is trusted by node ID; the sender's MAC is discarded, so any nearby radio can confirm an unpair and the hub erases the node | Pre-existing |
| D2 | One config burst to more than 32 nodes can still push a one-off unpair ACK out of the 32-deep drop-oldest queue | `632b456`, incomplete |
| D3 | The per-window ACK list grows with every distinct unvalidated node ID, without bound, at O(n²) cost | `632b456`, new |
| D4 | An ACK arriving between the last harvest and `deinitEspNowSync()` is deleted unread | Narrowed by `632b456`, not closed |
| D5 | Releases ignore the session deadline: the release reserve is capped at 30 s, and about 2.3 s per node can add minutes on a large roster | Pre-existing structure, partly capped by `632b456` |
| D6 | A config-mode shutdown cannot tell a served slot from an unserved one, so the imminent-slot wake can repeat a served slot: an empty ~80 s window, possibly an extra upload, and a legacy grace cycle consumed twice | `632b456` |
| D7 | If shutdown overruns the 6 s imminent-slot alarm, the alarm holds power on and the running firmware never re-arms | `632b456`, low |
| D8 | Daily sync mode: switching to it, or changing the daily time, leaves hub and nodes on different times of day with no recovery | Pre-existing; removed by decision below |

## 2. Decisions

- **Daily sync mode is removed from the firmware** (owner, 2026-10-24). Interval
  mode is the only schedule. This removes D8 and the daily alarm path.
- **Open, owner to decide:** the release policy past the session deadline
  (section 3.5).

## 3. Changes

### 3.1 Keep from `632b456`

- Retention purge before the window, gated exactly as in `196bed5`.
- Logger: `[CONFIG]`/`[DEPLOY]` dropped as hub markers; partial last line kept.

### 3.2 Remove daily mode (hub)

Where it lives: `config/config_server.cpp` (mode and time handlers, NVS, UI
presets, next-sync text), `config/config_server.h`, `main.cpp` (window phase
selection, alarm arming at sync end and at config shutdown),
`comms/espnow_manager.cpp` (`computeNextDailySyncPhaseUnix`, deploy and
`SET_SYNC_SCHED` sending `syncMin = 0`), `time/rtc_alarm.*` (`armDailyAlarm`),
`storage/json_payload.h`.

- **Migration.** `SYNC_MODE_DAILY` is enum value **0**, so a stored or zeroed
  `sync_mode` currently means daily. On boot, any stored value other than
  interval is rewritten as interval, keeping the stored interval and anchor, and
  logged once. The `sync_hh`/`sync_mm` keys are ignored, then erased. `sync_day`
  (`gLastSyncBroadcastEpochDay`) is checked for any interval-mode use before it
  is removed.
- **No path can send `syncIntervalMin = 0`** in `DEPLOY_NODE`, `SET_SYNC_SCHED`,
  `NODE_CONFIG` or `SYNC_RELEASE`. The hub refuses to send a zero interval.
- **UI:** the daily preset and the daily-time field are removed. `/set_sync_time`
  is removed or answers "not supported". A request for daily mode is refused,
  never silently turned into something else.
- **Upload payload:** `status.syncMode` stays and is always `"interval"`;
  `status.syncDailyTime` stays and is always `""`. The reporting contract is
  additive-only, and the dashboard is not changed from this repo.
- **Node:** the node's daily branch (`syncIntervalMin == 0`) becomes dead once no
  hub sends 0. It is removed in the next node firmware batch, together with the
  pre-wake race fix, so nodes are reflashed once. Until then the node keeps
  accepting 0, which no hub will send.

### 3.3 `CONFIG_ACK` path, designed once

Requirements:

- **R1.** An ACK counts only if its node ID **and** sender MAC match one registered
  node (D1).
- **R2.** Memory is bounded by the registry (at most 64 nodes), whatever arrives
  over the air (D3).
- **R3.** No legitimate ACK is lost at any fleet size up to the registry limit
  (D2).
- **R4.** Nothing is lost at teardown (D4).

Design:

- The ACK queue slot carries the sender MAC, like `SyncHelloSlot`.
- Depth is set from the registry limit plus margin, and the hub harvests
  **between NODE_CONFIG sends inside a burst** as well as in the existing loops.
  With one ACK per node per send, the queue can never hold more than one
  burst's worth.
- The harvest runs in the main task and authenticates against
  `registeredNodes` by MAC and ID. Unmatched ACKs are counted and dropped. The
  result is one slot per registered node, so memory is bounded and there is no
  search over unvalidated IDs.
- Teardown: stop intake first (unregister the receive callback, which
  `deinitEspNowSync()` already does as its first step, split out as its own
  call), then the final harvest, then reconcile, then free the queues.

### 3.4 Remember the served slot (D6, D7)

- At the end of every sync wake, persist the slot just served (`last_slot`).
- A config-mode shutdown wakes early for the imminent slot only if that slot is
  not `last_slot`; otherwise it arms the next one, as after a sync. This also
  stops the double legacy-grace decrement.
- The imminent-slot alarm moves from now + 6 s to now + 10 s. The 45 s lead
  leaves room. A missed alarm with power held on is noted in section 5 rather
  than engineered around.

### 3.5 Session deadline and releases (D5), owner decision

A release is what gives a node its clock and schedule, so skipping it risks
the node drifting. Two options:

- **(a) Recommended.** Releases stay mandatory. The release reserve scales with
  the roster (the 30 s cap is removed), so grants stop early enough for every
  release to fit inside the limit in normal operation. Only a window that starts
  at or past the deadline can overrun, by at most about 2.3 s per responder.
  This is documented and visible in `[SYNC-STAT]`.
- **(b) Hard limit.** Releases stop at the deadline. Unreleased nodes time out
  on their own session timer, keep their data, and keep the schedule they
  already have. Simpler bound, but those nodes miss that window's clock
  correction.

## 4. Order and verification

1. New branch from `main`. Carry over section 3.1 from `632b456`.
2. Section 3.2 (daily removal), then 3.3, 3.4 and 3.5. One commit each.
3. Build both envs, then one review against this plan's requirements, not
   against the previous patch.
4. Bench, HUB-R6-01 with two spare nodes, multi-port logger:
   - short sweep at 0, 35, 65 and 70 s injected delay (Stage 0 unchanged);
   - config shutdown about 30 s before an unserved slot: the hub wakes and runs it;
   - config shutdown just after an early-closed window: no second window;
   - unpair a node during a window with the other node absent: confirmed and removed;
   - a hub with `sync_mode` stored as daily (0): boots as interval, logs the
     migration, schedule unchanged.
   A spoofed ACK from a spare board (R1) is optional.
5. One PR to `main`.

Before that, and independent of it, the section 3.1 changes go to `main` on
their own as a small PR (owner, 2026-10-24), so `main` stops carrying Stage 0d
while the rest is done. Step 1 above is then already done.

## 5. Not in scope

- The node pre-wake race (runbook, "The misses at 30 s are a node bug"): next
  node firmware batch.
- Persisting or uploading `[SYNC-STAT]`.
- A hardware fail-safe for an RTC alarm that fires while the hub is still
  powered (D7, residual).
- Rendezvous Stages 1 to 4.
