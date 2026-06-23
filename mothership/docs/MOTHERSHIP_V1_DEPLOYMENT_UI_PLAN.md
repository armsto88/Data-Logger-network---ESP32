# Mothership V1 Deployment UI Plan

Date: 2026-06-23

Status: validated planning document. This is not an implementation patch.

This plan consolidates:

- the original Mothership V1 deployment UI review;
- the revised external LLM plan supplied by the user;
- a code-grounded validation pass against the current V1 mothership and shared node protocol files.

The core conclusion is unchanged: the deployment flow is fundamentally sound, but the UI needs to make state confidence explicit. The operator must be able to tell the difference between a configuration the mothership wants, a command that was sent, a command that was acknowledged by a node, live data from this session, last-known data, and an estimate.

The current system works and has been validated through bring-up and sync tests. All implementation should remain staged, additive, and defensive.

## Source files checked

- `mothership/firmware/v1/src/config/config_server.cpp`
- `mothership/firmware/v1/src/config/node_registry.h`
- `mothership/firmware/v1/src/config/node_registry.cpp`
- `mothership/firmware/v1/src/comms/espnow_config.h`
- `mothership/firmware/v1/src/comms/espnow_config.cpp`
- `mothership/firmware/v1/src/main.cpp`
- `mothership/firmware/v1/src/time/rtc_alarm.h`
- `mothership/firmware/v1/src/time/rtc_alarm.cpp`
- `node/firmware/shared/protocol.h`

## Validation of the revised external plan

The revised plan is directionally correct. The following claims were confirmed:

- All current web UI HTML, CSS, and JavaScript are embedded in `config_server.cpp`.
- `NodeInfo` currently includes deployment/config fields such as `state`, `deployPending`, `stateChangePending`, `pendingTargetState`, `configVersionApplied`, `lastReportedBatV`, `wakeIntervalMin`, `inferredWakeIntervalMin`, `lastSeen`, and `isActive`.
- `NodeDesiredConfig` includes `configVersion`, `wakeIntervalMin`, `syncIntervalMin`, and `syncPhaseUnix`.
- `/ui-status` exposes fleet-level pending counts, but not `pendingToDeployed`.
- The CSS already defines `chip--cfg-pending` and `chip--cfg-ok`, but the node manager currently does not render config chips.
- `computeNextWakeIsoLocal()` can append `, est`, but `handleNodesPage()` truncates the string with `substring(0, 8)`, which hides the estimate marker.
- `broadcastTimeSyncAll()` exists, but there is no UI route for it.
- `deploySelectedNodes(std::vector<String>)` already supports multiple node IDs, but the current UI only drives one node at a time.
- `/upload-status` already exists and should be reused for polling upload state.
- `node_status_message_t` already includes `rescueMode`.
- `lastSeen` is currently based on `millis()` and is not an absolute persisted timestamp, so restored nodes can look fresher than they really are.

The following corrections were made to the external plan:

1. `deployPending` is already set to `true` in `deploySelectedNodes()`.
   - The critical missing piece is UI rendering, not necessarily setting the flag again in `handleNodeConfigSave()`.
   - Future routes should continue to call `deploySelectedNodes()` rather than duplicating deployment-state mutation.

2. Deployment acknowledgement and config acknowledgement should remain separate.
   - `DEPLOY_ACK` or a deployed `NODE_HELLO` can clear deployment pending.
   - `CONFIG_ACK` should mark config as applied.
   - Do not automatically treat `CONFIG_ACK` as proof of deployment unless the protocol contract is explicitly changed.

3. Rescue mode is present in the protocol but not yet stored in `NodeInfo`.
   - A `RESCUE` chip cannot be rendered from current UI data without adding a small runtime field to the registry and updating it from `NODE_STATUS`.
   - Persisting rescue state across reboots is useful but can wait until Phase 2.

4. Phase 1 is not purely UI-only if it includes RTC validity fields, rescue mode, physical labels, or `pendingToDeployed`.
   - This document splits Phase 1 into:
     - Phase 1A: UI-only clarity and safety changes.
     - Phase 1B: tiny backend/status additions that still avoid protocol changes.

5. PWA support should be described carefully.
   - A manifest and mobile web-app metadata are useful.
   - A true service-worker-backed PWA should not be assumed on `http://192.168.4.1`.
   - No service worker should be added for V1.

## Current deployment workflow

1. Mothership wakes into config/AP mode.
2. Web UI serves dashboard, node manager, node config, and upload settings.
3. User discovers nodes.
4. Mothership registers discovered nodes as unpaired or known.
5. User pairs or deploys nodes.
6. Mothership sends ESP-NOW pairing, deployment, time-sync, and config messages.
7. Nodes later acknowledge deployment/config or report state through `NODE_HELLO`, `NODE_STATUS`, or snapshots.
8. User shuts down mothership.
9. On RTC sync wake, mothership opens a sync window, receives node snapshots, writes them to flash, optionally uploads, then arms the next RTC alarm.

This is a good field architecture. The UI should now make each step's certainty visible.

## State model

### Universal confidence labels

Every important field should carry one of these labels where useful:

| Label | Meaning |
| --- | --- |
| Desired | Stored mothership target config |
| Sent | Mothership attempted to transmit a command |
| Acknowledged | Node explicitly confirmed receipt/application |
| Live | Reported during the current config session |
| Last-known | Persisted or previously observed value |
| Estimated | Derived by mothership, not confirmed by node |
| Unknown | Not available, not reported, or lost after reboot |
| Pending | Waiting for node to wake/respond/apply |
| Stale | Known node, no useful recent contact |
| Rescue | Node reported rescue/recovery mode |

### Node deployment states

| State | Chip label | Source condition | Backend support |
| --- | --- | --- | --- |
| Unpaired | Unpaired | `state == UNPAIRED` | Existing |
| Paired | Paired | `state == PAIRED && !stateChangePending` | Existing |
| Deploy requested | Requested... | `state == DEPLOYED && deployPending` | Existing flag; UI rendering needed |
| Deployed confirmed | Deployed | `state == DEPLOYED && !deployPending` | Existing, but confirmation semantics should be shown carefully |
| Reverting to paired | Reverting... | `stateChangePending && pendingTargetState == PENDING_TO_PAIRED` | Existing |
| Unpairing | Unpairing... | `stateChangePending && pendingTargetState == PENDING_TO_UNPAIRED` | Existing |
| Deploy pending target | Deploying... | `stateChangePending && pendingTargetState == PENDING_TO_DEPLOYED` | Existing enum; fleet JSON missing count |
| Rescue mode | RESCUE | Latest `NODE_STATUS.rescueMode == 1` | Needs `NodeInfo` runtime field |
| Stale | Stale | No live/current-session contact after threshold | Needs absolute timestamp or session flag |

Recommended new CSS classes:

```css
.chip--state-requested{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00;animation:chip-pulse 1.4s ease-in-out infinite}
.chip--state-rescue{border-color:#e879f9;background:#fdf4ff;color:#701a75}
.chip--state-stale{border-color:#d1d5db;background:#f3f4f6;color:#6b7280}
@keyframes chip-pulse{50%{opacity:.55}}
```

### Config states

| State | Chip label | Source condition | Backend support |
| --- | --- | --- | --- |
| Config synced | Synced | `configVersionApplied == desired.configVersion && desired.configVersion > 0` | Existing |
| Config pending | Pending | `configVersionApplied < desired.configVersion` | Existing |
| Config unknown | Unknown | `configVersionApplied == 0` | Existing |
| Config stale | Stale config | Config pending after at least one or two sync windows | Needs timestamp/window tracking |

Recommended CSS additions:

```css
.chip--cfg-unknown{border-style:dashed;border-color:#d1d5db;color:#6b7280}
.chip--cfg-stale{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00}
```

Important rule: config state and deployment state are related but not identical. A node can be deployed but running an old config, or config-synced but still lacking an explicit deployment acknowledgement depending on message ordering.

### Wake-time confidence

| Condition | Display | Meaning |
| --- | --- | --- |
| Firm/current-session contact | `14:22` | Based on a live session anchor |
| Estimated | `~14:22` | Derived from interval and last-known timing |
| Unknown | `n/a` | No safe interval/time basis |

Fix required: do not truncate `computeNextWakeIsoLocal()` with `substring(0, 8)`. Extract the display time and estimate flag separately.

Example rendering logic:

```cpp
bool isEst = nextWake.indexOf("est") >= 0;
String timeOnly = (nextWake.length() >= 5) ? nextWake.substring(0, 5) : "n/a";
```

Recommended CSS:

```css
.chip--wake-firm{border-color:#bcd1bd;color:#2f4f35}
.chip--wake-est{border-style:dashed;color:var(--sub)}
```

### Interval source labels

The current interval value can come from several sources. The UI should say which.

| Source | Label suffix | Meaning |
| --- | --- | --- |
| Desired | `.set` | Stored target config for the node |
| Global | `.global` | Inherited from fleet/global wake interval |
| Observed | `.obs` | Reported by node |
| Default | `.default` | Firmware fallback |

Recommended CSS:

```css
.src-tag{font-size:.62rem;color:var(--sub);margin-left:3px}
```

## Revised UI structure

### 1. Mothership status panel

Show:

- firmware version and build;
- RTC time;
- RTC validity: valid, time invalid, absent/unknown;
- next sync time;
- RTC alarm status when available;
- AP status;
- ESP-NOW channel;
- flash mounted and usage;
- upload enabled/disabled;
- pending upload rows/bytes;
- phase anchor health when available.

Current support:

- most identity/schedule/upload fields already exist in `/ui-status`;
- RTC validity and alarm health need additional status fields;
- phase-anchor health needs an explicit status field.

### 2. Connection status pill

Add a sticky header pill:

- Connected;
- Reconnecting;
- Offline.

Drive it from a heartbeat poll against `/ui-status`.

The current JavaScript has async form handling and dashboard KPI refresh support, but not a persistent connection pill or offline form guard.

### 3. Deployment/site details

Add a human deployment metadata panel.

Suggested fields:

- project name;
- site name;
- deployment ID;
- operator name;
- deployment date/time;
- expected recovery date;
- timezone;
- latitude;
- longitude;
- physical location notes;
- habitat/plot notes;
- deployment purpose.

Recommended storage:

- new NVS namespace: `deploy`;
- keep `siteId` and `deploymentId` aligned with transmission settings, but make the deployment panel the operator-facing source.

### 4. Node discovery and pairing

Node cards should show:

- deployment chip;
- config chip;
- wake-time chip;
- interval with source tag;
- battery chip;
- live/last-known/unknown source where relevant.

Add:

- NEW badge for nodes discovered during the current session;
- clear distinction between restored-from-registry nodes and nodes heard in this config session.

### 5. Node configuration page

Restructure into explicit blocks.

Identity:

- physical label;
- MAC;
- firmware node ID;
- user node ID;
- display name;
- node type;
- notes.

Desired config:

- wake interval;
- sync interval or daily mode;
- sync phase;
- desired config version.

Node-confirmed config:

- applied config version;
- pending version delta;
- last `CONFIG_ACK`;
- last `DEPLOY_ACK`;
- last `NODE_HELLO`.

Live/last-known status:

- battery;
- queue depth;
- node RTC timestamp;
- last contact;
- rescue mode;
- data source label.

Actions:

- deploy;
- stop/keep paired;
- unpair/forget;
- resend config;
- sync time to node.

### 6. Deployment schedule

Make this a dedicated panel.

Show:

- global node wake interval;
- sync mode;
- sync interval or daily sync time;
- next mothership sync;
- per-node next wake estimate;
- schedule source: desired, acknowledged, observed, estimated, unknown.

Schedule change warning:

> Changing the wake interval updates the desired config for N nodes. Sleeping nodes will not apply this until their next wake. Some nodes may be out of sync until they acknowledge the new config.

### 7. Readiness checklist

This is the most important new field-deployment UI.

| Check | OK | Warning | Blocker |
| --- | --- | --- | --- |
| RTC set and valid | Valid | Time uncertain | Blocks deployment |
| RTC alarm armed | Verified | Not checked | Blocks safe shutdown |
| Next sync time known | Known | Estimated | Unknown |
| Phase anchor loaded | Loaded | Fallback | Lost |
| Flash mounted | Mounted | - | Failed |
| Flash free space | >= 20% | < 20% | < 5% |
| Upload config, if enabled | Complete | Untested | Missing endpoint/settings |
| At least one node confirmed deployed | Confirmed | Pending | None |
| Deployed nodes config-synced | Synced | Pending | Stale |
| Contact seen this session | All | Some missing | - |
| Node battery | Good | Low | Critical |
| Rescue-mode nodes | None | - | Active rescue |

Shutdown should show this checklist. If blockers exist, shutdown should require an explicit second confirmation such as:

> 2 blockers: RTC not set, 1 node deployment unconfirmed. Shut down anyway?

### 8. Storage and comms

Improve the existing storage/upload section with:

- flash mounted chip;
- flash usage bar;
- free bytes;
- CSV record count;
- CSV file size;
- last confirmed record timestamp;
- upload enabled chip;
- pending rows;
- pending bytes;
- last upload time;
- retry count;
- last upload result/error text.

Reuse `/upload-status` for polling.

## Critical safety fixes before broader UX work

These should be first because they reduce field mistakes without changing the ESP-NOW protocol.

### Fix 1: Two-step shutdown confirmation

Current problem:

- Shutdown is a regular form button in the quick-action grid.
- It is close to routine actions.
- A thumb mis-tap can end the config session.

Proposed improvement:

- Move shutdown out of the quick-action grid.
- Use a two-step confirm button with a short countdown.
- Show readiness blockers before final shutdown.

Backend support:

- UI/JS only for the first pass.
- Later: include readiness summary from `/ui-status`.

Change type:

- UI only initially.

Benefit:

- Prevents accidental power-down during field setup.

### Fix 2: RTC-unset blocking banner

Current problem:

- RTC invalid/unset can appear as a plain clock value.
- Operators can attempt deployment without a valid scheduling clock.

Proposed improvement:

- Render a full-width danger banner when RTC time is invalid or unavailable.
- Disable deploy action when RTC is invalid and explain why.

Backend support:

- `initRTC()` already distinguishes `RTC_OK`, `RTC_PRESENT_TIME_INVALID`, and `RTC_ABSENT`.
- `/ui-status` should expose RTC status, not just `rtcUnix`.

Change type:

- Small backend status addition plus UI.

Benefit:

- Prevents invalid node timestamps and unsafe schedules.

### Fix 3: Preserve estimated wake confidence

Current problem:

- `computeNextWakeIsoLocal()` includes `est`, but node manager truncates the string and hides it.

Proposed improvement:

- Parse the next-wake text into time and confidence.
- Render estimated wake with `~HH:MM` and a dashed/muted chip.

Backend support:

- Existing data is enough.

Change type:

- UI rendering only.

Benefit:

- Operators stop treating estimates as confirmed wake times.

### Fix 4: Render deploy-pending state

Current problem:

- `deploySelectedNodes()` sets `node.state = DEPLOYED` and `node.deployPending = true`.
- The node manager currently renders all `DEPLOYED` nodes as simply "Deployed".

Proposed improvement:

- Render `Requested...` while `deployPending == true`.
- Render `Deployed` only once pending is cleared by a deployment-confirming message.

Backend support:

- Existing `deployPending` field is enough.
- Do not duplicate deploy state mutation in the UI handler.

Change type:

- UI rendering only.

Benefit:

- Avoids false confidence after a command send but before node acknowledgement.

## Implementation plan

### Phase 1A: UI-only safety and clarity

No protocol changes. Avoid changing sync flow.

- Add two-step shutdown confirmation.
- Move shutdown out of the quick-action grid.
- Add RTC-unset/invalid warning banner using existing `rtcUnix` as a first approximation.
- Fix next-wake truncation and render estimated wake distinctly.
- Render deploy state using `deployPending`.
- Render config chips using existing desired/applied config fields.
- Add interval source suffixes.
- Add stronger consequence text for Stop and Unpair.
- Add a connection pill driven by `/ui-status` heartbeat.
- Disable destructive form submissions when the heartbeat has marked the UI offline.
- Add pending fleet KPI card from existing `/ui-status.fleet.pending`.
- Keep mobile touch targets at least 44 px.
- Add `touch-action: manipulation` and `user-select: none` on action buttons.
- Add high-contrast mode using a body class and `localStorage`.

Acceptance checks:

- `pio run -e mothership-v1-main` still compiles.
- No route semantics changed.
- No ESP-NOW protocol changes.
- Existing sync flow remains unchanged.

### Phase 1B: Minimal backend/status additions

Still no protocol changes.

- Extend `/ui-status` with:
  - RTC status enum: `ok`, `time_invalid`, `absent_or_uninit`;
  - `pendingToDeployed`;
  - phase-anchor health if available;
  - config-session remaining time if available.
- Add runtime-only `rescueMode` field to `NodeInfo` and update it from `NODE_STATUS.rescueMode`.
- Render `RESCUE` chip when the field is true.
- Add physical label field in node metadata.
- Add NVS helpers for physical label.
- Add per-node JSON endpoint if needed for live chip updates.

Acceptance checks:

- Existing node registry load/save remains backward compatible.
- New NVS fields are additive and optional.
- Nodes with no physical label render exactly as before except for the new UI layout.

### Phase 2: Persistence and operator context

- Persist absolute `lastSeenUnix` when RTC is valid.
- Persist last confirmed config version.
- Persist last battery reading.
- Persist last node RTC timestamp.
- Persist last queue depth.
- Persist rescue mode until a later non-rescue status clears it.
- Add deployment metadata namespace `deploy`.
- Link deployment metadata with upload `siteId` and `deploymentId`.
- Add event log ring buffer in LittleFS.
- Add dashboard event-log panel.
- Add bulk deploy for all paired nodes.
- Add sync-time-to-all route using existing `broadcastTimeSyncAll()`.
- Add upload progress polling around `/upload-status`.
- Request `navigator.wakeLock` during manual upload as a best-effort browser feature.

Acceptance checks:

- Registry remains loadable if old NVS entries lack the new fields.
- Event log writes are bounded and cannot fill flash unexpectedly.
- Manual upload page remains usable even if browser wake lock is unavailable.

### Phase 3: Protocol and storage expansion

These changes need more careful compatibility planning.

- Add node firmware version to `NODE_HELLO` or `NODE_STATUS`.
- Add node reset reason.
- Add richer battery/health flags.
- Add sensor inventory/config acknowledgement.
- Add explicit node next-wake report.
- Add deployment manifest export.
- Differentiate RTC-present-time-invalid from RTC-absent in UI and status JSON.

Acceptance checks:

- Shared protocol versioning is explicit.
- Older node firmware remains compatible or fails visibly.
- Mothership can display "unknown" for fields older nodes do not send.

### Phase 4: Advanced field UX

- Map preview when the phone has internet.
- Coordinate-only fallback when connected only to the mothership AP.
- Offline deployment manifest export.
- QR-code deployment summary.
- Per-node install checklist.
- Field technician mode with fewer controls and larger targets.
- Recovery wizard for stale/rescue nodes.
- Multi-mothership correlation fields.

## New data fields

### Additive node metadata

| Field | Storage | Phase | Notes |
| --- | --- | --- | --- |
| Physical label | NVS node metadata | 1B | Human label written on enclosure |
| Last seen Unix | NVS paired node metadata | 2 | Only update when RTC valid |
| Last contact source | Runtime/NVS | 2 | Live, restored, sync snapshot, status, hello |
| Last confirmed config version | NVS | 2 | Separate from desired version |
| Last deploy ACK time | NVS | 2 | Useful for readiness |
| Last config ACK time | NVS | 2 | Useful for readiness |
| Rescue mode | Runtime first, NVS later | 1B/2 | From `NODE_STATUS` |
| Last node RTC Unix | NVS | 2 | From HELLO/STATUS/snapshot |
| Last queue depth | NVS | 2 | From HELLO |
| Last battery | Existing partial/NVS expansion | 2 | Already partly persisted |

### Deployment metadata

| Field | Storage | Phase |
| --- | --- | --- |
| Project name | `deploy` NVS | 2 |
| Site name | `deploy` NVS | 2 |
| Deployment ID | `deploy` NVS plus upload settings alignment | 2 |
| Operator name | `deploy` NVS | 2 |
| Deployment timestamp | `deploy` NVS | 2 |
| Expected recovery date | `deploy` NVS | 2 |
| Timezone | `deploy` NVS | 2 |
| Latitude | `deploy` NVS | 2 |
| Longitude | `deploy` NVS | 2 |
| Habitat/plot notes | `deploy` NVS | 2 |
| Deployment purpose | `deploy` NVS | 2 |

## Field failure scenarios and UI response

### RTC battery dead or time lost

Firmware already detects invalid RTC time. The UI should:

- show a blocking banner;
- explain that the RTC chip may have lost power;
- block deployment until time is set;
- allow safe shutdown only with explicit override.

### Node enters rescue mode

Protocol already carries `rescueMode`. The UI should:

- show `RESCUE` chip;
- show a node-page warning;
- explain that re-pairing/config restoration may be needed;
- mark readiness as blocked while rescue is active.

### Two operators on the AP

V1 should not try to implement full sessions/locking.

Add an advisory:

> Only one device should configure the mothership at a time. Multiple simultaneous sessions may conflict.

### Config timeout while operator is mid-form

The heartbeat should:

- turn the connection pill red/offline;
- disable destructive forms;
- avoid silent form submission failure.

If a session-remaining endpoint is added, show a countdown in the header during the last two minutes.

### ESP-NOW send succeeds but node never ACKs

The UI should:

- show `Requested...`;
- keep config/deployment pending visible;
- after one or two sync windows, mark the node as unconfirmed/stale;
- include the node in readiness warnings.

### Manual upload takes a long time

The UI should:

- warn that upload may take 30-60 seconds;
- poll `/upload-status`;
- avoid auto-refresh;
- display progress stages if exposed by backend;
- keep the phone awake where supported.

## Mobile-first notes

The existing mobile CSS foundation is good. Additions should stay small and consistent with the current visual language.

Recommended:

- keep `.btn--sm` at 44 px minimum height as a base rule, not only under 480 px;
- add `touch-action: manipulation` to buttons and node cards;
- add `user-select: none` to action buttons;
- add high-contrast mode for outdoor sunlight;
- add a bottom action bar for the safest frequent actions only, such as Discover and CSV download;
- keep shutdown out of the bottom action bar;
- use `viewport-fit=cover` if safe-area handling is added;
- add mobile web-app metadata and manifest only as a convenience hint;
- do not add a service worker for V1.

## What not to do yet

- Do not redesign ESP-NOW protocol in Phase 1.
- Do not change sync scheduling logic as part of UI cleanup.
- Do not make `CONFIG_ACK` mean deploy confirmed unless the protocol is changed and documented.
- Do not rely on browser PWA installability on the mothership AP.
- Do not make shutdown impossible when blockers exist; allow explicit override for recovery/maintenance.
- Do not hide unknown values behind optimistic defaults.

## Recommended next step

Implement Phase 1A first:

1. two-step shutdown;
2. RTC warning banner;
3. next-wake estimate display fix;
4. deploy-pending chip rendering;
5. config chips;
6. interval source labels;
7. heartbeat connection pill;
8. stronger destructive-action copy.

This gives the largest field-safety improvement with the least risk to the working sync flow.

Then implement Phase 1B only after Phase 1A compiles and the existing sync/config flow still works.

