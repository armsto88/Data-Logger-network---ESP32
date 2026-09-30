# Mothership Sync Rendezvous Design Note

This note defines the intended node↔hub sync rendezvous architecture for FieldMesh.

It is written as a design target, not as a claim that the deployed firmware already
behaves this way. The current 4-node deployment loses 31–38% of its sync windows and
cannot scale past roughly four nodes; this note explains why, and what should replace
the current mechanism.

Related notes:

- `MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md` — the power-gating and RTC-alarm
  architecture this rendezvous has to live inside.
- `MOTHERSHIP_STATUS_REPORTING_PLAN.md` — the status/telemetry surface the new
  instrumentation extends.

## 1. Purpose

The rendezvous is the only moment in the system where a node and the hub are
simultaneously powered. Both sides are hard power-gated and wake only on their own
DS3231 alarm. Neither can be summoned. If the two awake intervals do not overlap by
enough time to complete a handshake, the window is simply lost, and the node's data
for that period is lost with it once its buffer wraps.

The current implementation treats the rendezvous as a fixed 120-second window. It is
not. The effective aperture is a function of the hub's boot latency, it is not
measured anywhere, and it collapses to zero somewhere around 35 seconds of hub boot
time. This note:

- states what the code actually does, corrected against a fresh read
- identifies the aperture equation and why the observed failure signatures follow
  from it
- proposes a rendezvous that tolerates tens of seconds of jitter on both sides
- gives the capacity arithmetic as a function of fleet size N and says where it tops out
- stages the work into what can ship to the live deployment now versus what needs the
  protocol reworked
- specifies the instrumentation that would have made this a one-query diagnosis

## 2. What The Code Actually Does

This section is a corrected reading of the control flow. Three points differ from the
working assumption that motivated the investigation.

### 2.1 The hub's join phase closes at slot + 15 s — it does not open there

`runCoordinatedSyncWindow` (`mothership/firmware/v2/src/main.cpp:468`) computes:

```
joinWindowMs = clamp((toSlotSec + kJoinPostSlotSec) * 1000,
                     kJoinFloorMs = 15000, kJoinCapMs = 45000)
```

with `kJoinPostSlotSec = 15`, where `toSlotSec` is signed seconds from *now* to the
nearest slot boundary. The join loop then runs `while (millis() - syncStartMs < joinWindowMs)`
(`main.cpp:709`). So the join phase runs from the moment the hub reaches this function
until **slot + 15 s** — the `+15` is the *end* anchor, not a start offset.

The observed `join window=22000 ms` therefore means the hub reached the join loop at
slot − 7 s, and held the rendezvous open across `[slot − 7, slot + 15]`.

The node listens to `slot + SYNC_MARKER_GRACE_SEC` = slot + 25 s
(`node/firmware/src/main.cpp:2386-2400`). The overlap is therefore 22 seconds in that
sample, not 10. The aperture is still far too small and still far short of the nominal
120 s, but the shape of the problem is different from the `slot+15 → slot+37` reading.

### 2.2 There is no "all nodes reported" early exit in the coordinated path

The comment at `main.cpp:1672` describes an intelligent early shutdown once all
deployed nodes have reported. No such check exists in `runCoordinatedSyncWindow`. What
actually terminates the window early is the grant loop:

```
while (madeProgress && (int32_t)(grantStopMs - millis()) > 0)   // main.cpp:780
```

`madeProgress` goes false as soon as one full pass over `responders` finds every
responder released, empty, or twice-failed. The hub then releases everyone and returns,
abandoning whatever remains of its 105 s budget.

The distinction matters for the design: the hub does not close because it decided the
absentees were not coming. It closes because **it never knew they were expected** — the
roster is whatever joined during `joinWindowMs`, and the absentees were never in it.

### 2.3 Beaconing stops when the join phase closes

`broadcastSyncWindowOpen()` and `broadcastSyncSessionOpen()` are inside the join loop
only (`main.cpp:709-716`), at a 1000 ms cadence — not 5 s, and not for the whole
window. After `joinWindowMs` elapses, the hub is silent except for unicast grants and
releases to nodes already on the roster.

On the node side, the listen loop breaks only on `g_syncSessionOpenPending`
(`node/firmware/src/main.cpp:2413`). A node that does not hear a `SYNC_SESSION` frame
before `listenUntilUnix` prints `Sync marker not seen in listen window; flush skipped
this cycle` and goes straight to `finalizeWakeAndSleep`. There is no second attempt
within the period, and by design there is no fallback flush.

So the rendezvous is strictly one-shot on both sides, and the only frames that can
establish it are emitted during a window whose length the node cannot observe.

### 2.4 The node's wake time is exact; the hub's is not

`ds3231ArmSyncWake` (`node/firmware/src/main.cpp:596`) minute-aligns the phase, so
`nextSyncUnix` is always on a minute boundary and `wakeUnixRaw = nextSyncUnix - 60` is
too. The round-up at `main.cpp:656` is therefore a no-op for `SYNC_PRE_WAKE_SEC = 60`,
and the node wakes at exactly slot − 60 s every time. Any pre-wake lead below 60 s
rounds *up* to the slot itself — 60 really is the smallest expressible lead, as the
comment at `main.cpp:107` says.

The hub arms Alarm 1 with second resolution and a fixed 10 s lead
(`src/time/rtc_alarm.cpp:224`, `if (nextSyncUnix > 10) nextSyncUnix -= 10`). Its wake
instant is exact. What is not exact is how long it takes to *get to the rendezvous*.
Between `handleSyncWake()` at `main.cpp:1535` and `runCoordinatedSyncWindow` at
`main.cpp:1768` the hub performs, in order:

- NVS loads and schedule-transition detection
- `initSD()` — `main.cpp:1630`, retries and can block for seconds on a marginal card
- `initFlash()` — LittleFS mount, `main.cpp:1635`
- `uploadQueue.init()` and `emergencyPurgeIfFull()` — `main.cpp:1647-1648`, a
  compaction pass over the LittleFS upload queue
- `loadPairedNodes()`, `configInitRecordingIntervalControl()`, `deploymentBootstrap()`
- `initEspNowSyncOnly()`, `initSnapQueue(32)`
- building the per-node `NODE_CONFIG` vector

None of this is bounded, and the slow parts scale with how much data the hub is
holding. **This latency is the free variable in the rendezvous, and nothing records it.**

## 3. The Aperture Equation

Let `t_bootH` be seconds from the hub's alarm to it entering the join loop, and
`t_bootN` the node's boot-to-radio-live (~3–4 s: `delay(2000)` in setup, sensor init,
I2C at a 2 s per-transaction timeout).

- Node radio-live: `[slot − 60 + t_bootN, slot + 25]` ≈ `[slot − 56, slot + 25]`, ~81 s
- Hub beaconing: starts at `slot − 10 + t_bootH`, ends at `slot + 15`, unless the hub
  arrives after slot + 15, in which case the floor gives `[arrival, arrival + 15]`

The overlap is therefore:

```
O(t_bootH) = 25 − t_bootH        for t_bootH ≤ 25      (join ends slot+15, node ends slot+25)
           = 35 − t_bootH        for 25 < t_bootH < 35  (floor-clamped join, node ends slot+25)
           = 0                   for t_bootH ≥ 35
```

**The rendezvous aperture is 25 seconds minus the hub's boot time.** That is the whole
mechanism. The observed `join window=22000 ms` corresponds to `t_bootH = 3 s` and
`O = 22 s`, which works. A window in which the LittleFS purge, an SD retry, or a large
`uploadQueue.init()` scan adds 15–20 s gives `O = 2–7 s`, and a window that adds 35 s
gives nothing at all.

### 3.1 Why this explains every observed signature

This is the only candidate mechanism found that accounts for all four signatures
simultaneously, and it is consistent with the entire ruled-out list — which is
node-side without exception, and therefore could not have found a hub-side variable.

| Observation | Follows from the aperture equation |
| --- | --- |
| Bimodal 4-of-4 or 1-of-4, never graceful partial | `t_bootH` is **common-mode**: every node faces the same aperture in a given window. Either it is wide enough for the fleet or it is not. There is no per-node gradient to produce a partial. |
| The survivor rotates | HELLO timing is `coordinatedHelloJitterMs(sessionId)` (`node/firmware/src/main.cpp:2146`), an FNV hash of `NODE_ID` seeded with `sessionId`. The hub sets `sessionId = getRTCTime() ^ esp_random()` (`mothership/.../main.cpp:498`), **re-randomised every window**. In a 2–5 s aperture only the node that drew the smallest jitter that window gets a HELLO in — and which node that is is re-drawn each window. |
| Uniform 31–38% miss rate across all four nodes | Direct corollary: the jitter draw is uniform, so over 81 windows each node wins the narrow-aperture windows equally often. |
| Attendee lag 38–48 s on full *and* near-empty windows | Lag is drain position within the grant phase, which begins after the join phase regardless of roster size. It is unrelated to attendance, which is why it does not vary. |
| Every gap is exactly one window, never partial delivery | Roster membership is binary. A node is admitted and drained, or never admitted and flushes nothing. There is no path that delivers some records. |
| Worsening 7 → 11 → 10 misses over three days | `t_bootH` creeps as the hub's LittleFS fills and the upload queue grows, pushing more windows past the cliff. |

The flash-level test that ruled out the retention purge examined the hub's *flash
level*, on three data points, against miss count. The purge is only one of several
contributors to `t_bootH`, and flash level is a poor proxy for purge duration. That
test does not rule out the mechanism proposed here; §8 specifies the experiment that
does.

### 3.2 What the fix has to be

The aperture must stop being a function of hub boot latency. Three independent ways to
achieve that, all of which this design uses:

1. **Move hub boot latency in front of the slot** — raise the hub's pre-wake lead so
   boot finishes before the node is even listening.
2. **Stop the aperture from closing** — keep beaconing and keep accepting joiners for
   the whole window, not just the join phase.
3. **Give the node a second chance** — a conditional retry rendezvous so a pathological
   window costs a delay rather than a permanent hole.

## 4. Drain Rate: The Second Ceiling

Even with a perfect rendezvous, the fleet caps out on throughput. The measured cost is
**1.6–2.8 s per snapshot**, which is nowhere near radio-bound — a 132-byte ESP-NOW
frame is ~2 ms of airtime. The cost is in the node's flush loop.

`flushQueuedToMothership` (`node/firmware/src/main.cpp:1275`) does, per record:

1. `peekV2` the oldest record
2. `sendEspNowAndWait(..., 350)` — link-layer send plus callback
3. wait for a durable `SNAP_ACK`, up to `NODE_SNAPSHOT_ACK_TIMEOUT_MS = 900`
4. `local_queue::pop()` → `commitCandidate()` → `Preferences::putBytes` of the **entire
   ~3.6 KB queue blob** to an alternating A/B NVS slot (`storage/local_queue.cpp:512`)
5. `delay(5)`

Step 4 is the dominant term. Every single snapshot handed to the hub costs a full
3.6 KB NVS blob rewrite. That is the 1.6–2.8 s, and it is also a flash-wear problem:
at 18 snapshots per window and 16 windows per day, each node writes ~1 MB/day of NVS
purely as drain bookkeeping.

The grant machinery on top of this adds a round-trip per `kGrantQuota = 4` records.

### 4.1 The current capacity model

With `W = min(SYNC_WINDOW_MS, kCoordinatedWindowMs) = 105 s`, `J ≈ 22 s`, and a release
reserve of `min(3 + 1.6N, 30) s`:

```
C(N) = (W − J − reserve(N)) / r
```

At N = 4: reserve = 9.4 s, drain budget 73.6 s, `r ≈ 1.6–2.2` → **C ≈ 33–46 snapshots
per window**. Consistent with the observed 45–75 ceiling and with the observed
3 nodes × 21 records draining a full session.

Each node offers `B = kSyncFillK = 18` snapshots per window
(`src/config/config_server.cpp:59-61`, `computeAutoSyncMin(wake) = wake * 18`), so:

```
N_max = C / B ≈ 33/18 … 46/18 ≈ 2 … 3
```

The observed cap of ~4 is the optimistic end of that, helped by windows where nodes
carry less than a full 18. **The drain ceiling and the aperture failure are
independent; fixing one does not fix the other**, and the drain ceiling is the one that
makes the current design structurally unable to reach ten nodes even with a perfect
rendezvous.

## 5. Design Intent

The rendezvous should behave as a **scheduled, jitter-tolerant, time-division
appointment** rather than a race to be present during an unmeasured aperture.

Invariants the design must hold:

- **I1 — The aperture is independent of either side's boot latency.** No amount of hub
  or node startup work may narrow the overlap.
- **I2 — Attendance is known.** The hub knows which deployed nodes were expected and
  which did not appear, per window, and reports it.
- **I3 — A missed window costs latency, not data.** One miss must never produce a
  permanent hole.
- **I4 — Node radio-on time does not grow with fleet size.** Node energy is the scarce
  resource; hub awake time is not, comparatively.
- **I5 — Degradation is graceful.** Overload reduces resolution or delays delivery; it
  does not delete a contiguous span.
- **I6 — Nothing regresses.** Deployment-epoch stamping and reading attribution, the
  grant/quota fairness mechanism, the durable deployment-event outbox and its ack
  semantics, and the unpair/removal convergence path all keep working, including across
  a mixed-firmware fleet during rollout.

## 6. Mechanism Options — Argued

### 6.1 Widen the marker grace (node listens longer)

Raise `SYNC_MARKER_GRACE_SEC` from 25 to, say, 55, and lift the `now + 60 + 25` hard cap
so the node listens to slot + 55.

**For.** One node-side constant. Directly widens `O` by 30 s, taking the cliff from
`t_bootH ≥ 35` out to `t_bootH ≥ 65`. It requires no protocol change and no hub change,
so it works against the currently deployed hub.

**Against.** It is the most expensive fix per second of aperture bought, because it is
paid by every node on every window whether or not it is needed. See §7.4: the
rendezvous already dominates node energy, and +30 s is roughly +19% on the node's total
sync-wake budget. It also treats the symptom — the node compensating for the hub's
unbounded boot — rather than the cause. And it cannot help at all when `t_bootH`
exceeds the node's *pre-wake*, because the hub's join floor still only holds 15 s.

**Verdict.** Keep in reserve. Ship it only if §6.2 and §6.3 prove insufficient in the
field. It is the right emergency lever precisely because it is node-side and needs no
hub cooperation, which makes it the fallback if a hub rollout stalls.

### 6.2 Raise the hub's pre-wake lead

Change the hub's fixed 10 s lead in `armNextSyncAlarmPhase` (`src/time/rtc_alarm.cpp:224`)
to 45–60 s, so hub boot completes before the slot rather than eating into it.

**For.** One hub-side constant. It attacks the actual free variable. With a 45 s lead,
`t_bootH` up to 45 s is absorbed entirely and `O` returns to its structural maximum of
`slot+15 − (slot−56) = 71 s` for all realistic boot times. Hub energy is the cheap
resource: 35 extra seconds every 90 minutes is ~0.6% duty on a device with a
substantially larger power budget than the nodes.

**Against.** It does not *bound* anything; it buys headroom. A pathological boot (SD
card failing its retries, a large purge) can still exceed the lead. Alone it leaves the
system with the same cliff, just further away — and with no instrumentation, you would
not know you were approaching it again. It also slightly increases the risk that the
hub finishes its window and powers down before a straggler node has finished booting,
though the fixed `slot+15` join end makes that a non-issue in practice.

**Verdict.** Ship immediately, together with §6.3. It is the single highest
value-per-line change available, but it is headroom, not a guarantee.

### 6.3 Accept late joiners across the whole window

Move the beacons out of the join loop and into the grant loop as well, and call
`collectHellos()` throughout the window so `responders` can grow after the join phase
closes.

**For.** This is the change that makes the aperture *structural* rather than tuned.
It converts a 22 s aperture into the full 105 s window with **no node-side change at
all**, which means it improves the live fleet the moment the hub is reflashed, with no
staged node rollout and no wire-format change. It also removes the pathological case
entirely: a hub arriving at slot + 20 still catches every node that is listening at
slot + 21.

It composes with §6.2 rather than duplicating it: the pre-wake lead handles the common
case cheaply, and late-join handles the tail.

**Against.** Two real hazards.

First, **iterator invalidation**. The grant loop iterates `for (auto& responder : responders)`
(`main.cpp:781`) over a `std::vector`. Appending inside that loop invalidates the
iterator and the references. The loop must be converted to index-based iteration with a
re-read of `responders.size()` each pass. This is the one place in the change where a
mistake is a heap-corruption bug rather than a behaviour bug, and it must be reviewed as
such.

Second, **fairness**. A node joining at second 80 gets fewer grant rounds than one that
joined at second 5. That is correct and desirable — it should not preempt nodes already
mid-drain — but it must not be able to starve them either. The existing round-robin
already handles this: a late joiner enters the rotation and receives grants at the same
quota as everyone else for the remaining passes. The grant/quota fairness mechanism is
preserved, not bypassed, which satisfies I6.

A third, milder cost: beaconing for 105 s instead of 22 s adds ESP-NOW broadcast traffic.
At 1 Hz that is negligible airtime, but the beacon cadence in the grant phase should be
reduced to ~2 s so it does not contend with active dumps.

**Verdict.** Ship immediately. This is the core interim fix.

### 6.4 Staggered per-node offsets (time division)

Assign each node an integer minute offset `m_i`. Node `i` treats its slot as
`slot + m_i · 60`; the hub stays awake across all groups and serves group `m` during
minute `m`.

**For.** The DS3231's minute resolution, which has been an obstacle everywhere else in
this system, is exactly the right granularity here. It gives free, drift-immune time
division at 60 s steps with no negotiation, no clock synchronisation beyond the TIME_SYNC
the node already receives, and no extra node radio time. That last point is the decisive
argument: **staggering buys scale by spending hub awake time, which is cheap, rather than
node radio time, which is not** (I4). It is the only option here that scales sub-linearly
in node energy.

It also naturally pipelines. While group `m` is draining, group `m+1` is booting, so the
hub's drain phase and the nodes' boot latency overlap instead of serialising.

And it is backward compatible by construction: a node that does not understand
`slotOffsetMin` defaults to 0 and lands in group 0, which is precisely today's behaviour.

**Against.** It requires a wire-format change (a `slotOffsetMin` field in `DEPLOY_NODE`
and `SET_SYNC_SCHED`) and therefore a staged rollout, so it cannot help the live
deployment this week. It adds a genuinely new failure mode: a node whose offset
assignment is stale or divergent from the hub's view attends the wrong minute and misses
every window until corrected — which is a *worse* failure than today's because it is
persistent rather than probabilistic. That risk must be mitigated by (a) the hub
beaconing continuously across all group minutes, so a node in the wrong group still
finds a session, and (b) re-asserting the offset in every `SYNC_RELEASE`.

It also means the hub is awake for `⌈N/k⌉` minutes, which runs into
`kSyncSessionLimitMs = 300 s` (`main.cpp:68`) and the LTE upload that has to fit in the
same session.

**Verdict.** This is the scaling mechanism. Stage it after the interim fixes, and make
the hub's continuous cross-group beaconing a hard requirement so a mis-assigned offset
degrades to "joins the wrong minute but still syncs" rather than "dark forever".

### 6.5 Scale window length and quota with fleet size

Make `kCoordinatedWindowMs` and `kGrantQuota` functions of `deployedCount`.

**For.** Trivial, and clearly correct in direction — a 4-node window and a 30-node
window should not be the same length. Raising `kGrantQuota` from 4 to 8 halves the
grant round-trip overhead per record.

**Against.** Window length alone cannot fix the drain ceiling, because `r ≈ 2.2 s` is
dominated by the node's per-record NVS commit (§4). Scaling the window linearly in N
against a fixed per-record cost means hub awake time grows linearly in N, and you hit
`kSyncSessionLimitMs` at around N = 6 before you hit anything else. Quota scaling has a
subtler cost: a larger quota means a node holds the channel longer per grant, which
increases the blast radius of a node that stalls mid-grant, and `kGrantWindowMs = 9000`
must grow with it.

**Verdict.** Necessary but strictly secondary. Do it *after* the drain-rate fix (§6.6),
at which point it becomes genuinely effective. Doing it first would mask the real
bottleneck.

### 6.6 Fix the drain rate — batch the NVS commit

Add `local_queue::popN(uint8_t k)` that drops `k` records from the in-RAM blob and
commits **once**. The node pops per grant (or per grant window) rather than per record.

**For.** This is the highest-leverage change in the entire note. It attacks the term
that is 90% of `r`. Expected `r` after the change is `t_air + t_ack ≈ 0.1–0.2 s`, plus
one 3.6 KB commit amortised over the whole grant: at `Q = 8`, effective per-record cost
≈ `0.20 + 0.8/8 = 0.30 s`. That is a **7× throughput improvement**, taking C from ~35 to
~230 records per window at today's budget. It also cuts node NVS wear by the same factor,
which matters over a multi-year deployment.

It is node-side only, with no wire-format change, so it can roll out independently.

**Against.** It weakens the durability guarantee. Today, each record is removed from the
queue only after its own durable ACK is persisted, so a power loss mid-drain can at worst
re-send one record. With batched commits, a power loss after `k` acknowledged sends but
before the commit re-sends up to `k` records. That is a **duplicate**, not a loss, and the
hub already de-duplicates on `seqNum` — but the hub's de-duplication path must be verified
to actually cover this, not assumed. If it does not, the fix is to commit on the *grant*
boundary, where `DUMP_DONE` already forms a natural transaction point.

The safe ordering is: send all `k`, collect acks, commit, then send `DUMP_DONE`. A crash
before the commit replays the grant; a crash after it does not.

**Verdict.** Ship in the first protocol-adjacent stage, immediately after the interim
rendezvous fixes. Gate it on confirming hub-side `seqNum` de-duplication.

### 6.7 Node-side retry within the period

If the primary rendezvous produces no `SYNC_RELEASE`, the node arms A2 for
`slot + R` minutes and tries again. The hub, symmetrically, holds a retry window only
when attendance was incomplete.

**For.** This is the mechanism that satisfies I3 unconditionally. Everything else in this
note reduces the probability of a miss; only this one bounds the *consequence*. Its cost
structure is unusually good: **both sides pay only on failure**. The node's expected extra
radio time is `p_miss × 90 s`, which at today's 35% is a punitive +31 s per sync but at a
post-fix 1% is +0.9 s — the mechanism self-extinguishes as the primary path improves,
which is exactly the right shape for a safety net. And it is the only proposal here that
protects against hub boot pathologies not yet observed.

The hub side is cheap and clean because the hub already knows the roster: if
`responders.size() < deployedCount`, stay up until `slot + R + 1` min and keep beaconing.

**Against.** It is the most invasive change to the node's alarm state machine. A retry
wake is not on the period grid, so `nextSyncSlotUnix` (`node/firmware/src/main.cpp:1178`)
and `ds3231ArmSyncWake` both need an explicit "this is a retry wake" concept, persisted in
NVS so it survives the power cut. The `++slot` round-up at `main.cpp:1198` — which the
ruled-out list correctly cleared as a cause of the *current* failure — becomes genuinely
dangerous under retry semantics, because a retry wake sitting `R` minutes off-grid can
round to the wrong slot. That interaction needs explicit test coverage, not inspection.

There is also a thundering-herd risk: every absent node retries at the same instant, into
the same aperture, having already demonstrated that the aperture was too narrow. The retry
must therefore use a *wider* HELLO jitter than the primary, and the hub's retry window must
be generous.

**Verdict.** Worth the complexity, but last. Build it on top of a rendezvous that already
works, and on top of the instrumentation that can prove the retry is firing correctly.
Shipping it early would hide the primary defect behind a retry that succeeds.

### 6.8 Decouple buffer depth from `kSyncFillK`

`computeAutoSyncMin(wake) = wake * 18` forces `B = 18` at every wake interval, which sets
the required buffer depth. This conflates two unrelated decisions: how often the hub must
be awake (a hub-power and LTE-cost question) and how deep the node's buffer must be (a node
NVS-capacity question).

**For.** The coupling produces absurd corners. At `wake = 60`, `S = 1080 min` — an 18-hour
sync period, on a fleet that cannot survive a single miss. At `wake = 1`, `S = 18 min` —
16× more hub wakes and LTE sessions than necessary. Neither is a considered choice; both
are artefacts of a single constant. Making `S` independently settable, with a *derived
guard* (`S/wake ≤ B_safe`) rather than a derived value, keeps the safety property while
removing the absurdity.

**Against.** `kSyncFillK` currently guarantees the invariant "the buffer cannot overflow
between syncs", which is real and valuable. Replacing a guarantee with a validated setting
means the validation has to be right in three places — the hub config UI, the
`SET_SYNC_SCHED` path, and the node's own sanity check — or a bad combination reaches the
field and silently overflows. The current design is safe because it removes the choice.

**Verdict.** Do it, but implement the guard first and the freedom second. `B_safe` should
be ~45% of slab capacity in records, leaving 2× headroom so one missed window still cannot
overflow. Reject invalid combinations at the config server with an explicit message rather
than silently clamping.

### 6.9 Buffer depth itself

`kSlabCapacity = 3500` bytes (`node/firmware/src/storage/local_queue.cpp:16`) holds ~21
records at 132 B (48-byte header + 6 B × 14 readings). One missed window at `wake = 5`,
`S = 90` presents 36 records to 21 slots.

The binding constraint is `static_assert(sizeof(QueueBlobV2) < 4000)` — the NVS
single-key limit. The node has **no filesystem**: LittleFS/SPIFFS appear nowhere in
`node/firmware/src`. So the slab cannot simply be grown.

Options, in order of preference:

1. **Chained slabs** — `q_a0/q_b0`, `q_a1/q_b1`, … Each stays under the NVS key limit,
   A/B durability is preserved per slab, and — critically — the commit cost *drops*,
   because only the slab that changed is rewritten. This pairs with §6.6 rather than
   fighting it. Two slabs give ~42 records, i.e. `B_capacity ≈ 2.3 × B_nominal`.
2. **Shrink the record** — the 48-byte header is heavy for a 84-byte payload.
   Delta-encoding the timestamp against a per-slab base and dropping fields that are
   constant across a slab could recover ~15%. Useful, not sufficient alone.
3. **A raw flash partition on the node** — the largest win and the largest risk, and it
   introduces a storage layer the node does not currently have.

**Verdict.** Chained slabs plus header shrink. Target `B_capacity ≥ 2 × B_nominal` so a
single miss never overflows and decimation (§6.10) is reserved for genuine multi-window
outages.

### 6.10 Degrade by decimation rather than overwriting oldest

Today `enqueueV2` drops oldest and sets `QF_DROPPED`. Over one missed window that means
the newest 21 of 36 records survive and the oldest 15 are lost **contiguously** — the
observed permanent one-hour hole.

**For.** For environmental series — temperature, soil moisture, light — a 10-minute
cadence across the full hour is far more useful than a 5-minute cadence across half of it.
Gap-free series do not break downstream interpolation, resampling, or daily aggregates,
and a contiguous hole is the one artefact that cannot be repaired after the fact.

**Against.** It mutates history. A series that was uniform 5-minute cadence becomes
irregular, which the CSV schema and any cadence inference will misread — including
`inferredWakeIntervalMin`, which the hub derives from observed snapshot spacing
(`src/comms/espnow_manager.cpp:904`). It also costs a full slab compaction at the exact
moment the node is most time- and power-pressed. And it is irreversible: you cannot
un-decimate, whereas a contiguous hole at least honestly reports what is missing.

**Verdict.** Implement it, but as the **second-tier** policy, not the everyday one. Fix
buffer depth first (§6.9) so one miss never triggers it. Decimation is the floor for a
multi-window outage. Two requirements: mark decimated records with a new quality flag
alongside `QF_DROPPED`, and make cadence a per-record property downstream rather than a
per-node one, so the dashboard does not misreport a decimated node as having changed its
wake interval.

## 7. Arithmetic For N Nodes

### 7.1 Symbols

| Symbol | Meaning | Current value |
| --- | --- | --- |
| `N` | deployed nodes | 4 |
| `w` | wake/sample interval, min | 5 |
| `S` | sync period, min | 90 (= 18w) |
| `B` | snapshots buffered per node per window = `S/w` | 18 |
| `k` | nodes per stagger group | 1 (no staggering) |
| `G` | groups = `⌈N/k⌉` | 1 |
| `J` | join-phase length, s | 22 (variable) |
| `r` | seconds per snapshot drained | 1.6–2.8 |
| `Q` | `kGrantQuota` | 4 |
| `g` | per-grant round-trip overhead, s | ~0.3 |
| `W` | coordinated window budget, s | 105 |
| `L` | hub session limit, s | 300 |

### 7.2 Join capacity

With continuous beaconing (§6.3), a node needs one HELLO to land inside the window.
HELLO is retried every 1800 ms (`node/firmware/src/main.cpp:2485`), with initial jitter
uniform over `NODE_SYNC_HELLO_JITTER_MS = 1800`. With `k` nodes contending and an
effective vulnerable window `τ ≈ 5 ms`, per-attempt collision probability is
`p_c ≈ k·τ/1800`. At `k = 8`, `p_c ≈ 2%`; at `k = 20`, `p_c ≈ 6%`. Over `⌊J/1.8⌋`
attempts, per-node join failure probability is `p_c^⌊J/1.8⌋`, which is negligible for
`J ≥ 10 s` at any `k ≤ 20`.

**Join is not the binding constraint once the aperture is fixed.** The constraint it
imposes is only that `J` must cover the node arrival spread, which staggering bounds to
one minute per group.

The hard cap on concurrent joiners is the ESP-NOW peer table (20 unencrypted peers on
ESP32). The hub already adds peers on demand and deletes on release
(`main.cpp:764`, `main.cpp:837`), so the live peer count is `≈ k`, not `N`. **Keep
`k ≤ 12`** to leave headroom for recovery-deploy unicasts issued in the same window.

### 7.3 Drain time and window length

Per group:

```
T_group(k) = k·B·r + (k·B/Q)·g + ρ·k
```

where `ρ ≈ 0.3–1.2 s` is per-node release-ack cost. Total window:

```
W_required(N) = J + G·T_group(k) + reserve,     reserve = min(3 + 1.6·k, 30)
```

**Today** (`k = N`, no staggering, `r = 2.2`, `Q = 4`, `g = 0.3`, `B = 18`):

| N | `N·B` | drain | + J + reserve | vs W = 105 s |
| --- | --- | --- | --- | --- |
| 2 | 36 | 82 s | 110 s | over |
| 3 | 54 | 123 s | 154 s | over |
| 4 | 72 | 164 s | 196 s | over |

The model says the current design is *already* over budget at N = 2 when nodes carry a
full buffer. It survives at N = 4 only because most windows deliver partial buffers, and
because the grant loop's `madeProgress` exit lets it close as soon as the nodes it
actually got are drained. **That is the ~4-node cap, and it is not a soft one.**

**After the drain fix** (§6.6: `r = 0.20`, `Q = 8`, `g = 1.0` including one amortised
slab commit → effective 0.325 s/record):

| N | k | G | `k·B` | `T_group` | `W_required` | hub awake |
| --- | --- | --- | --- | --- | --- | --- |
| 4 | 4 | 1 | 72 | 28 s | 47 s | ~1.5 min |
| 8 | 8 | 1 | 144 | 55 s | 79 s | ~2 min |
| 16 | 8 | 2 | 144 | 55 s | 134 s | ~3 min |
| 30 | 8 | 4 | 144 | 55 s | 244 s | ~5 min |
| 40 | 8 | 5 | 144 | 55 s | 299 s | ~6 min |
| 72 | 9 | 8 | 162 | 62 s | 520 s | ~9.5 min |

`T_group` must fit inside the group's 60 s minute for the pipeline to stay in step:
`k·B·0.325 + ρ·k ≤ 55` gives `k ≤ 9` at `B = 18`.

### 7.4 Node power cost

Per sync wake, the node is radio-live for pre-wake (60 s) + grace (25 s) + boot (~4 s)
≈ 90 s, at roughly 100 mA in ESP-NOW RX:

```
E_sync ≈ 90 s × 100 mA = 2.5 mAh per sync
       × 16 syncs/day (S = 90 min) = 40 mAh/day
```

Against sample wakes: `288/day × ~10 s × 80 mA ≈ 6.4 mAh/day`.

**The rendezvous already costs the node six times more energy than all of its actual
sampling combined.** Three consequences that shape the design:

- Staggering (§6.4) is the only scaling mechanism that does not touch this number.
  Node radio time is `O(1)` in `N` under staggering and `O(1)` under widening too — but
  widening raises the constant for every node on every window, whereas staggering does not.
- Widening the grace by 30 s (§6.1) is +0.8 mAh/day (+2%) — small in absolute terms, which
  is why it stays available as an emergency lever, but it buys the least aperture per mAh.
- Conditional retry (§6.7) costs `p_miss × 2.5 mAh` per sync: **14 mAh/day at today's 35%
  miss rate, 0.4 mAh/day at 1%.** The retry is not a fixed tax; it is a mirror of how
  badly the primary path is doing.

There is also a real prize available here that is out of scope for this note: the 60 s
pre-wake exists only because DS3231 Alarm 2 is minute-resolution. Alarm **1** has second
resolution. Moving the node's sync wake from A2 to A1 — as the hub already does — would
cut the pre-wake from 60 s to ~10 s and take `E_sync` from 40 to ~18 mAh/day, more than
halving node rendezvous energy. A1 currently carries the data wake, so this needs the two
alarms multiplexed or the data wake moved to A2. Worth a follow-up note.

### 7.5 Where the design tops out

Binding constraints in order of which bites first:

1. **Hub session limit.** `kSyncSessionLimitMs = 300 s` must cover the rendezvous *and*
   the LTE upload. At `k = 8`, the rendezvous alone reaches 300 s at **N ≈ 40**. Raising
   the limit to 600 s pushes this to N ≈ 90 but eats hub battery and collides with the
   modem's own budget (`OTA_MIN_BUDGET_MS`, `maybeRunCloudOta` at `main.cpp:864`).
2. **Group drain inside 60 s.** `k ≤ 9` at `B = 18`. Larger `k` needs `B` reduced (shorter
   `S`), which increases `G` and hub awake time — a wash.
3. **ESP-NOW peer table**, 20 peers. Non-binding at `k ≤ 12`.
4. **LTE payload per window.** `N·B` records: 720 at N = 40. This becomes the dominant
   constraint before the rendezvous does if backhaul is metered.
5. **Node NVS capacity.** `B_capacity ≥ 2·B` requires chained slabs (§6.9) above `B ≈ 21`.

**Practical ceiling: ~40 nodes** per hub at `k = 8`, `B = 18`, `S = 90 min`, with the hub
session limit raised to ~420 s. **Theoretical ceiling ~72** before group drain and peer
table interact badly. Beyond 40, the correct answer is a second hub, not a longer window —
LTE payload and hub awake time both grow linearly while the rendezvous quality does not
improve.

## 8. Staged Implementation Plan

### Stage 0 — Interim, hub-only, ship to the live deployment now

No wire-format change. No node reflash. A mixed fleet is unaffected, because nodes see
only more beacons over a longer period. Every item is independently revertible.

**0a. Raise the hub pre-wake lead.** `src/time/rtc_alarm.cpp:224`, `10` → `45`. Also raise
the guard below it (`if (nextSyncUnix <= nowUnix + 5)`) proportionally. Absorbs boot
latency in front of the slot. One constant.

**0b. Anchor the join end to slot + 25 s.** `kJoinPostSlotSec` 15 → 25 (`main.cpp:485`),
matching the node's existing `SYNC_MARKER_GRACE_SEC`. Recovers 10 s of aperture the node
is already offering. Verify against `kJoinCapMs = 45000` — at a 45 s lead the computed
join is ~70 s, so **`kJoinCapMs` must rise to 75000** or the cap silently truncates the
gain. This is the easiest thing in Stage 0 to get wrong.

**0c. Keep beaconing and keep accepting joiners for the whole window.** The core fix.
- Move `broadcastSyncWindowOpen()` / `broadcastSyncSessionOpen()` into the grant loop at a
  2 s cadence in addition to the 1 s cadence during join.
- Call `collectHellos()` inside the grant loop.
- **Convert the grant loop at `main.cpp:781` from range-for to index-based iteration,
  re-reading `responders.size()` each pass.** Appending to `responders` while a range-for
  holds references into it is undefined behaviour. Treat this as the review gate for the
  whole stage.
- The `madeProgress` exit must not fire while the window still has budget and
  `responders.size() < deployedCount` — otherwise the hub still closes early on the
  absentees it is now capable of catching.

**0d. Defer the slow storage work past the window.** Move `emergencyPurgeIfFull()`
(`main.cpp:1648`) to after `runCoordinatedSyncWindow` returns. `initFlash()` must stay
before the window (snapshot persistence needs it), but the compaction pass does not.
`initSD()` can also move after, since nothing in the coordinated window writes to SD.

**Expected effect:** aperture goes from `25 − t_bootH` seconds to the full window, and
`t_bootH` itself drops. Miss rate should go to near zero at N = 4. If it does not, the
root-cause hypothesis in §3.1 is wrong and Stage 1 instrumentation will say why.

**0e. Ship the instrumentation with it** (§9). Do not ship 0a–0d blind — the whole point
is that the current failure was invisible for five days.

### Stage 1 — Confirm the mechanism

Before designing Stage 3 in detail, run the injected-latency experiment (§10.4). It
confirms or kills §3.1 in one afternoon on a bench hub and determines whether staggering
is solving the right problem.

### Stage 2 — Drain rate and capacity, node-side + hub constants

**2a.** `local_queue::popN(uint8_t)` with a single commit (§6.6). Verify hub-side `seqNum`
de-duplication first; if absent, add it. Commit on the grant boundary so `DUMP_DONE` is the
transaction point.

**2b.** `kGrantQuota` 4 → 8, `kGrantWindowMs` 9000 → 14000 (§6.5). Hub-only, but only
effective once 2a is deployed — sequence them.

**2c.** Scale `kCoordinatedWindowMs` with `deployedCount` rather than fixing it at 105 s,
bounded by `kSyncSessionLimitMs` minus the modem reserve.

**Expected effect:** `r` from ~2.2 s to ~0.3 s. Capacity from ~35 to ~230 records/window.
N_max from ~4 to ~12 with no protocol change.

### Stage 3 — Protocol: staggering and conditional retry

Requires a wire-format change and a versioned rollout.

**3a.** Add `slotOffsetMin` to `DEPLOY_NODE` and `SET_SYNC_SCHED`. Nodes default it to 0,
so an un-upgraded node lands in group 0 = current behaviour. Hub assigns offsets round-robin
at deploy time and re-asserts in every `SYNC_RELEASE`. **The hub must beacon continuously
across all group minutes** so a node with a stale offset still finds a session — this is
what keeps a mis-assignment a latency bug rather than a blackout.

**3b.** Conditional retry (§6.7). Node persists a `retryPending` flag in NVS and arms A2 for
`slot + R`. Hub holds the window open to `slot + R + 1` min when
`responders.size() < deployedCount`. Retry HELLO jitter widened to ~5 s to avoid a herd.
`nextSyncSlotUnix` needs explicit off-grid-wake handling; the `++slot` round-up at
`node/firmware/src/main.cpp:1198` needs test coverage under retry semantics.

**3c.** Raise `kSyncSessionLimitMs` to ~420 s and re-verify the modem budget interaction.

### Stage 4 — Buffer and degradation

**4a.** Chained NVS slabs (§6.9) to `B_capacity ≥ 2 · B_nominal`.

**4b.** Decouple `S` from `kSyncFillK` with a `S/w ≤ B_safe` guard enforced at the config
server, in `SET_SYNC_SCHED`, and on the node (§6.8).

**4c.** Decimation as the second-tier overflow policy, with a new quality flag and
per-record cadence downstream (§6.10).

### Held in reserve

**Widen `SYNC_MARKER_GRACE_SEC` to 55** (§6.1). Node-side, works against an un-upgraded
hub. Deploy only if Stage 0 underperforms in the field or a hub rollout stalls.

## 9. Instrumentation

The diagnosis took five days because nothing recorded a missed window.
`syncStale` stayed false and `staleMissCount` 0 throughout — correctly, since those are
hub-derived from *contact age* (`src/comms/espnow_manager.cpp:1321-1330`) and a node that
misses one window and returns is never stale by that definition. The signal that was needed
does not exist on either side.

### 9.1 Hub — per window, per node

A new `sync_window_attendance` record, uploaded with the batch:

| Field | Why |
| --- | --- |
| `slotUnix` | the slot this window served — the join key |
| `hubWakeUnix`, `hubJoinStartUnix` | **their difference is `t_bootH`, the missing variable** |
| `joinWindowMs`, `joinEndUnix` | the computed aperture, as the hub saw it |
| `deployedCount`, `responderCount` | attendance |
| `absentNodeIds[]` | **the record that does not exist today** — who was expected and did not appear |
| `windowClosedReason` | `all-drained` \| `budget-exhausted` \| `session-timeout` |
| `snapDrops` | `getSnapDropCount()` — already computed, never reported |

Per responder: `helloOffsetMs` (relative to `slotUnix`, so a node whose clock has diverged
shows up directly), `queueDepthAtHello`, `recordsDrained`, `queueDepthAtRelease`,
`grantsIssued`, `grantTimeouts`, `releaseConfirmed`.

`helloOffsetMs` and `hubBootLatencyMs` together are sufficient to reconstruct the aperture
for any window retrospectively, which is what was missing.

### 9.2 Node — in the snapshot header or per-window status

| Field | Why |
| --- | --- |
| `missedWindows` | consecutive sync wakes ending without `SYNC_RELEASE`. **The field whose absence cost five days.** Distinct from `staleMissCount`, which cannot see a single miss followed by recovery. |
| `lastSyncOutcome` | `released` \| `marker-no-session` \| `no-marker` \| `grant-timeout` \| `deadline` — the node already distinguishes all five in serial (`node/firmware/src/main.cpp:2504-2515`) and reports none of them |
| `listenStartOffsetSec`, `markerHeardOffsetSec` | relative to the node's *computed* slot; a per-node phase divergence becomes visible without a serial capture |
| `queueDroppedTotal` | `QueueStats.droppedDueToCapacity` — already tracked, never surfaced |
| `bootMs` | boot to radio-live, the node's half of the aperture equation |

### 9.3 Dashboard

A `sync_windows` table keyed `(mothership_id, slot_unix)` with `absent_node_ids text[]`,
`hub_boot_latency_ms`, `join_window_ms`, `window_closed_reason`. Follow the derivation
pattern already established in `202607240002_add_node_stale_rescue_signals.sql` — a
`BEFORE` trigger extracting from `raw_payload` — so the ingest RPC does not change.

The one query:

```sql
select slot_unix,
       hub_boot_latency_ms,
       join_window_ms,
       cardinality(absent_node_ids) as absent,
       absent_node_ids,
       window_closed_reason
from public.sync_windows
where mothership_id = 'M001'
  and slot_unix > extract(epoch from now() - interval '7 days')
order by slot_unix desc;
```

Plus an alert on any window with `absent > 0`, and a chart of `hub_boot_latency_ms` over
time — that is the leading indicator that would have flagged the "worsening" trend on day
one rather than day five.

### 9.4 A separate bug to check

`inferredWakeIntervalMin` reads 0 in production. It is only assigned at
`src/comms/espnow_manager.cpp:904`, from observed spacing between consecutive snapshots.
Snapshots arrive in bursts of up to 18 during a single window, so the spacing it observes
is drain spacing, not wake spacing. This is noted as suspected-broken, not diagnosed; it
should be checked while the surrounding telemetry is being extended, since anything
depending on it (including the stale-detection cadence fallback at
`espnow_manager.cpp:1321-1322`) is currently running on a fallback path.

## 10. Test Plan

### 10.1 Coverage gap in the existing bench environments

The named environments do **not** cover the rendezvous. Checked against
`mothership/firmware/v2/platformio.ini`:

| Env | `build_src_filter` | Covers rendezvous? |
| --- | --- | --- |
| `mothership-v2-test-deployment-epoch` | `tests/test_deployment_epoch.cpp` + `deployment_epoch.cpp` + `deployment_store.cpp` | No — no `main.cpp`, no ESP-NOW |
| `mothership-v2-test-upload-queue` | `tests/test_upload_queue.cpp` + registry/flash/queue/json | No |
| `mothership-v2-wipe-deploy-store` | `tests/wipe_deployment_store.cpp` | No — maintenance only |
| `esp32s3-mock-mothership-sync` | `node/firmware/tests/bringup_mock_mothership_sync.cpp` | Partially — a *node-side* mock hub on channel 1, `SYNC_INTERVAL_MIN=15`, `WIFI_ON_WINDOW_MS=12000`. Useful as a scaffold, not as a fleet emulator. |

The rendezvous arithmetic is currently untestable because it is inline in a ~400-line
function. **The refactor is a prerequisite for the test plan, not a nicety.**

### 10.2 New environments

**`mothership-v2-test-sync-rendezvous`** — extract the join-window arithmetic and the
roster/late-join logic into `src/comms/sync_rendezvous.{cpp,h}`, then:

```
build_src_filter = -<*>
  +<tests/test_sync_rendezvous.cpp>
  +<src/comms/sync_rendezvous.cpp>
```

Table-driven. For `t_bootH ∈ {0, 3, 10, 20, 30, 45, 60}` s × slot phase offsets ×
`deployedCount ∈ {1, 4, 8, 30}`, assert:
- computed aperture ≥ 40 s in every cell (the Stage 0 acceptance criterion)
- `kJoinCapMs` never truncates a computed join at the 45 s pre-wake lead
- late-joiner append preserves grant ordering and does not reorder existing responders
- a responder appended during grant iteration is reachable on the next pass — the
  index-iteration regression guard

**`esp32wroom-node-sync-listen`** — extract `nextSyncSlotUnix` and the listen-window
arithmetic into `src/time/sync_schedule.{cpp,h}`. Assert:
- listen end vs. slot for `SYNC_PRE_WAKE_SEC ∈ {60}` and grace `∈ {25, 55}`
- the A2 round-up at `main.cpp:656` for non-minute-aligned phases
- the `++slot` round-up at `main.cpp:1198` for on-grid, early, late, **and off-grid retry**
  wakes — the Stage 3b hazard
- idempotence: arming twice from the same state yields the same alarm bytes

**`esp32wroom-node-queue-drain`** — extends the existing `LOCAL_QUEUE_TESTING` hooks
(`storage/local_queue.h`). Assert:
- `popN(k)` leaves the same state as `k` × `pop()`
- crash-safety: interrupt the commit mid-write, confirm A/B recovery to the pre-`popN`
  state (replay, never loss)
- decimation preserves temporal coverage and sets the new quality flag
- chained-slab wrap across slab boundaries

### 10.3 Fleet emulation

Extend `bringup_mock_mothership_sync.cpp` into a **mock fleet**: one ESP32 presenting `K`
sequential HELLO identities with distinct `nodeId`s from a single radio, driving join
capacity and drain rate at `K = 8…30` without 30 boards.

State the limitation plainly: **one radio cannot reproduce true RF contention between
spatially separated nodes**, capture effect, or hidden-terminal behaviour. It validates
protocol and timing, not RF. Anything above `K = 12` needs real boards before it can be
trusted, and the peer-table headroom claim in §7.2 specifically needs a real multi-radio
check.

### 10.4 The confirming experiment (run before Stage 3)

Add `-D HUB_TEST_BOOT_DELAY_MS` to a bench-only environment, inserting a delay immediately
before `runCoordinatedSyncWindow`. On a bench hub against the real 4-node fleet, sweep
`0 → 45 s` in 5 s steps, one window each, and plot attendance against injected latency.

§3.1 predicts full attendance below ~15 s and a collapse to 0–1 attendees above ~25 s,
with the surviving node differing between windows. If attendance is flat across the sweep,
§3.1 is wrong and Stage 3 should not be designed until the real mechanism is found.

This is the cheapest high-information test in the plan and it should run first.

### 10.5 Regression guards (I6)

- `mothership-v2-test-deployment-epoch`, then `mothership-v2-wipe-deploy-store` — the
  ordering is mandatory and documented in `platformio.ini`: the epoch suite leaves fixture
  nodes and queued outbox events behind, and production firmware would upload them.
- `mothership-v2-test-node-config-control` — unpair/removal convergence.
- `mothership-v2-test-upload-queue` — retention and cursor behaviour, since Stage 0d moves
  the purge.
- Deployment-epoch stamping specifically: `deploymentBootstrap()` must still run before the
  window opens (`main.cpp:1661`), or snapshots stamp epoch 0. Stage 0d moves storage work
  *after* the window — `deploymentBootstrap` must **not** be moved with it.
- Outbox ack semantics and grant/quota fairness: assert in `test_sync_rendezvous` that a
  late joiner receives the same per-pass quota as an early one.

### 10.6 Known-red frontend suite

`Tests 21 failed | 259 passed` is expected and unrelated: `PROJECT_TIMEZONE` moved to
`Australia/Sydney` while the timezone suites still hardcode Berlin. Any change to that
count is a real signal; the count itself is not.

## 11. Risks To Avoid

### 11.1 Treating the 120 s window as the aperture

`SYNC_WINDOW_MS = 120000` describes how long the hub is *willing* to stay up. It has never
described how long a node can join. Conflating the two is what made the current defect
invisible.

### 11.2 Appending to `responders` while range-iterating it

The single most dangerous line in Stage 0. Range-for over a `std::vector` holds references
that `push_back` invalidates. Index-based iteration with a fresh `size()` each pass.

### 11.3 Shipping staggering before the aperture is fixed

Staggering assigns nodes to minutes. If the aperture is still boot-latency-dependent, a
mis-assigned or stale offset turns a probabilistic miss into a permanent blackout for that
node. Continuous cross-group beaconing is not optional.

### 11.4 Shipping the retry before the primary path works

A retry that usually succeeds will mask the primary defect and make the miss rate look
solved while doubling node radio energy. Fix the primary path, measure it, then add the
retry as insurance against what you have not seen.

### 11.5 Batching the NVS commit without verifying de-duplication

Batched commits convert a mid-drain power loss from "re-send one record" into "re-send up
to `k` records". That is safe **only** if the hub de-duplicates on `seqNum`. Verify it;
do not assume it.

### 11.6 Assuming the ruled-out list was wrong

It was not. Every hypothesis on it is node-side, and every one of them was correctly
eliminated. The reason none of them explained the data is that the free variable is on the
hub. The list is what makes that conclusion available.

## 12. Out Of Scope For This Note

- Moving the node's sync wake from DS3231 Alarm 2 to Alarm 1 to cut the 60 s pre-wake
  (§7.4). Large power win, needs the data wake relocated, deserves its own note.
- LTE backhaul sizing as `N·B` grows — see `MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md`.
- Multi-hub topologies, which are the correct answer above ~40 nodes.
- Encrypted ESP-NOW peering and its effect on the 20-peer table.
- Final constant values for `k`, `R`, and `B_safe`, which should be set from Stage 1 and
  Stage 2 measurements rather than chosen here.

## 13. Bottom Line

The rendezvous is not a 120-second window. It is `25 − t_bootH` seconds, where `t_bootH`
is the hub's boot latency — an unbounded, unmeasured, common-mode variable. That single
equation accounts for the uniform miss rate, the bimodal attendance, the rotating survivor,
and the worsening trend, and it explains why five days of node-side investigation could not
find it.

Three hub-only changes — a longer pre-wake lead, a join phase anchored to the node's own
grace deadline, and beacons that keep going for the whole window — remove the dependency
entirely and can ship to the live deployment without touching a single node.

The second ceiling is throughput, and it is not the radio. It is one 3.6 KB NVS blob
rewrite per snapshot on the node. Batching that commit is worth a 7× capacity improvement
for a node-side change with no wire-format impact.

Scaling past ten nodes then comes from time division on minute boundaries — which the
DS3231 gives for free, and which is the only mechanism here that buys scale with hub awake
time instead of node battery. That reaches ~40 nodes per hub. Beyond that, add a hub.

And none of it should ship without the attendance record. The design's most important
property is not that it works, but that when it stops working, one query says so.
