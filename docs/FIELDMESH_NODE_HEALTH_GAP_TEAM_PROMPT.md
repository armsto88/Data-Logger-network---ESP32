# Discussion prompt: node-health / silent-node gap, and a rich SD-card mirror

Hand this to the backend team (or their LLM) as a **discussion starter, not a
build ticket** — backend is busy, there's no urgency, and nothing here is
implemented yet on either side. The goal is to agree on a schema/design
*now* so firmware work (including a rich local SD log) can proceed without
having to redo it once backend has bandwidth. It is self-contained — you
should not need to open the firmware repo to weigh in.

---

## System context

A fleet of solar ESP32 **nodes** report sensor readings to a **mothership**
(ESP32 + LTE) over ESP-NOW at each sync window. The mothership logs readings
to local flash (LittleFS, `/datalog.csv`) and periodically POSTs a JSON
payload — `{meta, readings[], status}` — to the Supabase `ingest-fieldmesh`
function. Full payload field reference:
`docs/FIELDMESH_PAYLOAD_REFERENCE.md`. Table destinations per that doc:
`readings[]`→`readings`, `status.nodes[]`→`nodes`, `meta`+`status`
(flattened)→`mothership_status`, `meta`→`sync_sessions`.

**The mothership will soon get an SD card** on the next board revision (it's
also earmarked as the relay cache for node-firmware OTA — see
`docs/FIELDMESH_CLOUD_OTA_BENCH_STATUS_2026-07-19.md` §"Node OTA", which is
blocked on the same hardware). This prompt is about a *second*, independent
use for that card: a rich local data mirror, discussed below.

## The gap we found (evidence, not guesswork)

We audited whether an operator looking back 6 months of data could answer
"when did a node stop logging, and why?" Answer: **only the "when" is
answerable, and only by inference — the "why" is essentially never
captured.** Specifics:

- **The CSV/readings stream is sensor data only.** 30 columns
  (`mothership/firmware/v2/src/storage/csv_schema.h:17-24`), two thin health
  bitmasks (`sensorPresent`, `qualityFlags`), no row type for "no data
  because X."
- **The mothership already computes real missed-sync and recovery-reason
  signals — and then discards them before upload:**
  - `syncStale` / `staleMissCount`, the actual missed-sync detector
    (`mothership/firmware/v2/src/comms/espnow_manager.cpp:1329-1330`), is
    RAM-only — it is **not** in `buildNodesStatusJson()`
    (`mothership/firmware/v2/src/config/node_registry.cpp:785-871`), so it
    never reaches the `status.nodes[]` payload or the `nodes` table.
  - `NODE_STATUS` rescue/recovery pushes (`rtcSynced`, `rescueMode` —
    `mothership/firmware/v2/src/comms/espnow_manager.cpp:678-731`), the one
    message type that explicitly carries *why* a node needed recovery, is
    `Serial.printf`'d only (line 724) and never persisted or uploaded.
- **`nodes` is upsert-only (per our docs), i.e. current-state, not
  history.** `lastSeenUnix`, `sensorFaultMask`, `recordingPaused`, `state`
  only ever reflect the *most recent* upload — there's no record of *when*
  any of those changed.
- **`mothership_status`'s write mode is ambiguous in our own docs** —
  `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md:604-605` says "upserted," but
  the DDL a few hundred lines earlier (`:344-369`) shows a `BIGSERIAL` PK
  with no unique constraint on `device_id` — consistent with plain
  INSERT/history instead. We can't resolve this from the firmware repo;
  need backend to say which it actually is.
- **`sync_sessions` is the one confirmed append-only table**, but it's one
  row per mothership *upload event*, not per node — it can show the
  mothership itself went dark, not which node(s) were silent during a
  session that did happen.

**Bottom line today:** the only way to find a silent node 6 months back is
noticing a timestamp gap in `readings` for that `node_id`. The only causal
signal that survives at all is `qualityFlags`/`QF_DROPPED` — and that only
fires on rows that *do* arrive (local queue overflow), saying nothing about
an offline, dead-battery, paused, or mothership-down node.

## Why the SD card changes the calculus

Once the mothership has local SD storage, there's no reason the *local*
record should be thinner than what the cloud could theoretically hold. The
proposal: design one shared data model for "node health over time," and
mirror it in both places —

- **Locally on SD**, so a technician who pulls the card (or a mothership
  that's been offline for months) has a complete, self-contained health
  history, not just raw readings.
- **In the cloud**, so the dashboard can answer "when/why did node X go
  quiet" without an operator eyeballing timestamp gaps.

Same field names, same event shape, in both places — so whatever the
dashboard eventually renders is exactly what a technician sees reading the
card directly, and nothing has to be re-designed twice.

## Proposed design (for discussion — not locked in)

### 1. A new append-only "node health event" stream
Distinct from `readings[]` (which is a sample every wake) — this is *only*
written on a state transition, so it stays small even over 6 months:

| Field | Notes |
|---|---|
| `nodeId` | same identifier as `readings`/`nodes` |
| `eventType` | `NODE_WENT_STALE`, `NODE_RECOVERED`, `SENSOR_FAULT_START`, `SENSOR_FAULT_CLEAR`, `STATE_CHANGED` (UNPAIRED/PAIRED/DEPLOYED), `RECORDING_PAUSED`, `RECORDING_RESUMED`, `RESCUE_MODE_ENTERED`, `RESCUE_MODE_EXITED`, `LOW_BATTERY_OBSERVED` |
| `observedAtUnix` | mothership RTC time the *mothership* noticed the transition — not necessarily the exact instant it happened on the node, since silence is only detectable at the next expected contact. Worth being explicit about this "first observed, not exact" semantic so nobody over-trusts the timestamp precision. |
| `detail` | small free-form JSON — e.g. `{"staleMissCount":3}`, `{"from":"DEPLOYED","to":"UNPAIRED"}`, `{"lastReportedBatV":3.31}` |

This is firmware wiring existing-but-discarded state (`syncStale`,
`NODE_STATUS` reasons, the existing `state`/`recordingPaused`/
`sensorFaultMask` transitions already tracked in `NodeInfo`,
`mothership/firmware/v2/src/config/node_registry.h:29-84`) into a stream
that actually gets kept, not new detection logic.

### 2. Cloud side: a `node_health_events` table (sketch, open to backend's preferred shape)
Append-only, one row per event above, keyed by `mothership_id` (derived
server-side from the API key, same as every other table) + `nodeId` +
`observedAtUnix`. This is the table that would let a dashboard render "node
went silent on 2026-03-14, no contact for 11 days, resumed 2026-03-25" —
something genuinely not answerable from any existing table today.

### 3. Resolve the `mothership_status` ambiguity intentionally
Whichever it actually is (upsert vs. insert), we'd like it decided on
purpose rather than left contradictory between two docs — if it's meant to
be a live "current state" table, a lightweight history mechanism (even just
a periodic snapshot table, doesn't need to be every upload) would close the
same "when did diagnostics/battery/signal start degrading" question for the
mothership itself, mirroring what item 1-2 do for nodes.

### 4. SD-card local mirror (firmware-side, doesn't block on backend)
Rather than only logging `readings`, the SD card would carry:
- The existing 30-column readings CSV, unchanged.
- A periodic full status/health snapshot (the same shape as the `status{}`
  block already built for upload — `nodes[]`, `fleet{}`, `diagnostics{}`,
  `modem{}` — see `docs/FIELDMESH_PAYLOAD_REFERENCE.md` §3), so the card has
  the full picture even across a stretch with no connectivity.
- The new node-health-event stream from item 1.

Because this reuses the exact same JSON field names as the upload payload,
a technician's SD pull and the cloud dashboard would show the same
vocabulary — nothing new to learn twice.

**One thing to verify before finalizing this (firmware-side, not a backend
question):** does a failed upload currently lose that session's `status`
snapshot entirely, or is it queued/retried the way `readings` rows are
(`upload.pendingRows`/`pendingBytes` in the payload reference suggest
readings are queued — unconfirmed whether `status` gets the same
treatment)? If status snapshots are currently lost on a failed upload, the
SD mirror would fix that as a side effect, which is worth confirming is
actually a real gap before we invest firmware time in it.

## Questions for backend, whenever there's bandwidth

1. Confirm `nodes`: genuinely upsert-only with no history, or is there
   already a mechanism (audit table, WAL-based history, etc.) we don't know
   about? (Worth asking plainly — this whole prompt exists because our
   internal audit couldn't rule out "already implemented," and the ingest
   Edge Function source isn't in the firmware repo.)
2. Which is `mothership_status` actually — upsert or insert-per-upload? The
   two docs disagree.
3. Any appetite / better existing pattern for a `node_health_events`
   (or similar) append-only table? Open to backend's preferred shape over
   the sketch above — the firmware side just needs to know the field names
   and event vocabulary to target.
4. Anything already planned on the dashboard roadmap that would make this
   moot (e.g. a generic audit-log table that already exists for other
   entities and could be reused for nodes)?

## What this prompt is *not* asking for

- No SD hardware exists yet — this is schema/design discussion only, timed
  so firmware can start on the local-mirror format as soon as the board
  lands, without redesigning it later.
- No implementation commitment or deadline. Reply whenever.
- Not asking backend to build `node_health_events` right now — just to
  weigh in on shape/feasibility so firmware's local format (which doesn't
  block on backend) stays compatible with wherever the cloud side ends up.

## Suggested next step

Firmware can prototype the SD-card local mirror format independently — it
has value on its own (a technician pulling the card already gets a richer
picture than today) even before backend picks this up. The ask of backend
is just to sanity-check the shape above (or propose a better one) so the
two don't diverge.

---

## UPDATE (firmware, 2026-07-24): first three health fields shipped — names locked

Responding to backend's ask for exact field names: firmware has now
**implemented** the first slice of the "why did a node go quiet" signals and
added them to the existing `status.nodes[]` objects (additive — same objects
that already populate the `nodes` table, so they land in
`node_status_history.raw_payload` automatically, no backend change needed to
capture them). Locked field names and shapes:

| Field | Type | Meaning |
|---|---|---|
| `syncStale` | bool | Node has missed its expected contact cadence — `true` once it has missed ≥3 expected wakes **or** has been silent ≥24h, whichever comes first. Computed mothership-side each sync wake from `lastSeen` age vs. the node's desired sync/wake cadence. |
| `staleMissCount` | int (0–255) | How many expected wake cycles the node has missed (saturates at 255). `0` = seen this cycle; a rising value across check-ins = a node drifting silent. |
| `rescueMode` | bool | The node's own self-reported rescue flag from its latest `NODE_STATUS` — `true` means "this node rebooted ≥3 times within 20s while unpaired and is beaconing for re-adoption." Transient/live (clears the moment the node is re-paired/deployed), not a sticky historical flag. |

**When to add typed columns:** worth doing before anyone queries "which
nodes went stale in the last 6 months" against JSONB at scale — recommend
`sync_stale bool`, `stale_miss_count int2`, `rescue_mode bool` alongside
`sensor_fault_mask` in a follow-up migration. No urgency; the `raw_payload`
capture works today regardless.

### Correction to the original gap report above

The gap write-up earlier in this doc said these signals were "computed and
then discarded before upload." That was **half wrong** and worth flagging so
nobody trusts the old file:line citations: the missed-sync detector and the
rescue-mode read it pointed at lived in
`mothership/firmware/v2/src/comms/espnow_manager.cpp`, which turned out to be
**excluded from the production build** (`platformio.ini`, the
`mothership-v1-main` env drops that file) — so in the *shipping* firmware
these signals didn't exist at all, not "existed but weren't uploaded." They
have now been implemented for real in the live files
(`config/node_registry.cpp` + `comms/espnow_config.cpp`), not merely wired
through.

### Scope note

This is **detection + reporting only.** The dead-code reference also had a
"stale-assist" auto-retry (re-push schedule + re-send time-sync to a node
once it's flagged stale); that was deliberately **not** included in this
slice and remains a possible follow-up. Nothing here changes node firmware —
the node already sent everything required; this was a mothership-only change.

The richer event-transition stream and SD-card mirror (§1–4 above) are still
the plan and still don't block on backend — these three fields are just the
cheapest first win that answers real "why" questions today.
