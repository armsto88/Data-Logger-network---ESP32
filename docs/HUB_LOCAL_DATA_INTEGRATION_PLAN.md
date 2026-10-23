# FieldHub Local Data — Integration Plan (firmware, backend, dashboard)

Status: **proposed, nothing implemented.** Written 2026-10-23.

Audience: the owner, and engineers on this repo (hub firmware) and on
`armsto88/FieldMeshDashboard` (Supabase backend and React dashboard). The dashboard repo was
**read, not changed**, for this plan. Every statement about it cites a file there.

Related notes:

- `docs/HUB_LOCAL_SENSING_PLAN.md` — the firmware phases for collecting hub-local readings. This
  plan keeps its Phases 1–6 and replaces its Phase 7 and its "no registry visibility for the first
  release" decision (§4).
- `mothership/docs/MOTHERSHIP_LOCAL_SENSOR_COLLECTION_DESIGN_NOTE.md` — earlier design note.
- `mothership/docs/MOTHERSHIP_SD_PRIMARY_STORAGE_PLAN.md` — where the hub's rows are stored.
- `mothership/docs/MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` — shares the wake code (§11).
- `hardware/FieldHub_v2/revisions/2026-10-10-r6/BRINGUP_HARNESS.md` — what the r6 hardware does.
- Dashboard repo: `docs/FIELDHUB_ONBOARDING_AND_INSTALLATION.md`, `docs/PAYLOAD_REFERENCE.md`,
  `docs/node-deployment-epoch-firmware-handover.md`.

## 1. Summary

The hub should be able to record its own sensors and show them in the dashboard as one more
station, set up as an **optional step in commissioning**.

The r6 bring-up proved the hardware can read the sensors. The backend would store a reading that
carries a hub id. But the dashboard builds its station list from the `nodes` table, charts readings
by deployment, and only creates those records from `status.nodes[]` and `status.deploymentEvents`,
so a bare reading would be stored and then never shown (§3.5).

The plan makes the hub send the same three things a node sends: readings, a `status.nodes[]`
entry, and deployment events. **No database migration is needed for the first version.** The work
is in the hub firmware (capture, identity, a commissioning step), small guards in the edge
functions and the dashboard, and tests that run a hub-station payload end to end.

## 2. Requirements

- **Optional.** Off by default. A hub with the feature off behaves exactly as it does today, and a
  test proves it (§12, test 1).
- **Part of commissioning.** Offered as a skippable step in the hub's setup, and addable later.
- **Same path as other data.** Stored on the SD card and LittleFS, uploaded over LTE, ingested,
  charted. Nothing new on the wire beyond additive fields.
- **Scales.** One station and one deployment slot must fit beside 40 nodes (§3.2).

## 3. What exists today

Tags: **[verified]** read in code or measured; **[inferred]** follows from code but not run;
**[unverified]** not checked.

### 3.1 Hardware (FieldHub v2 r6, bring-up proven)

| Interface | Detail |
|---|---|
| I2C sensor ports CN17–CN20 | Through the mux at 0x71. Channel 0–3 is CN19, CN20, CN17, CN18. An SHT40 was read on every port. |
| Analogue CN15/CN16 | Four ADS1015 inputs, 3 mV per step. The sensor supply on pin 1 is 5 V only. |
| Reed (wind) | GPIO36. Counts correctly, but sees false edges when Wi-Fi or ADC1 activity changes (finding 9 in the bring-up guide). |
| Sensor power | The sensor rails are on whenever the hub is awake. Firmware cannot switch them or see a fault. |

### 3.2 Hub firmware

- **[verified]** Rows use the 35-column canonical header (`storage/csv_schema.h`). `processSnapshot()`
  writes them to LittleFS and the SD card.
- **[verified]** The deployment store is keyed by MAC plus node id, with 64 slots
  (`kMaxDeployNodes`) and a 16-event outbox (`config/deployment_store.h`). 40 nodes plus a hub
  station fit.
- **[verified]** A node's name, number and location are read by `nodeId` from NVS
  (`getNodeName()`, `getNodeUserId()`, `getNodeLatitude()` call `loadNodeMeta()`), not from the
  radio registry. A station can have them without being in `registeredNodes`.
- **[verified]** `resolveEpochForSample(nodeId, sampleUnix)` stamps each row with its deployment
  epoch (`config/deployment_epoch.cpp`).
- **[verified]** The hub's setup wizard (`handleSetupWizard()`) has seven steps: set up, connect to
  FieldMesh, dashboard control, clock, recording interval, review, done. The node deploy wizard
  (`handleStationSetupWizard()`) has four: name, sensors installed, location, deploy.
- **[verified]** Nothing collects hub-local readings yet. The sensor stack is not linked into the
  hub, and the production image is 90.2 % of its OTA slot (1,419,049 of 1,572,864 bytes).

### 3.3 Backend (`FieldMeshDashboard/supabase`)

- **[verified]** Ingest validation requires only a non-empty `nodeId` of up to 120 characters and
  a `deploymentEpoch` of 0–65535 (`functions/_shared/fieldmesh.ts`). `status.nodes` is limited
  to 256 entries.
- **[verified]** `readings` has no foreign key to `nodes`. It stores `node_id` as text, and the
  duplicate key is `(mothership_id, node_id, recorded_at, seq_num)`
  (`migrations/202606290001_create_fieldmesh_platform.sql`).
- **[verified]** `readings` has fixed columns for air temperature and humidity, spectral bands,
  wind, two soil probes, two aux channels and battery, plus `extra_measurements`. They cover every
  sensor the r6 hub can read.
- **[verified]** `nodes` rows are written only from `status.nodes[]`, and the whole entry is kept in
  `nodes.raw_payload` (`migrations/202607170001_add_firmware_control_plane.sql`). An extra field
  in an entry needs no migration.
- **[verified]** `node_deployments` rows come only from `status.deploymentEvents`
  (`apply_deployment_events`). Readings attach to a deployment by node, epoch and time
  (`resolve_reading_deployments`, `migrations/202607310001_add_node_deployments.sql`). A reading
  with epoch 0 attaches only to an *active* deployment of that node.
- **[verified]** A deployment number must be unique among active deployments in a project
  (`node_deployments_active_number_idx`).
- **[verified]** The project-wide recording-interval command is queued per FieldHub, not per node
  (`migrations/202607180003_add_recording_interval_commands.sql`).

### 3.4 Dashboard (`FieldMeshDashboard/frontend`)

- **[verified]** The station list comes from `nodes` (`getNodes()` in
  `src/lib/data-providers/supabase.js`). Charts use `getDeploymentData(deploymentId)`, and the
  latest value comes from `latest_readings`.
- **[verified]** The sensor bits in `src/lib/sensorMask.js` are AIR_TEMP, AIR_RH, SPECTRAL, WIND,
  SOIL1, SOIL2, AUX1, AUX2 and BATTERY. They match the hub's sensors.
- **[verified]** Pause and Resume on the Nodes page are gated by `canBatchControlNode()`
  (`src/lib/control.js`), and the documented prerequisite is a complete desired configuration
  reported by the node.

### 3.5 What happens with no change

**[inferred, confirm with the dry run in §11 phase A]** A hub reading uploaded as it stands is
accepted and stored. It has no `nodes` row, so it is not in the station list. It has no deployment,
so an epoch-0 reading attaches to nothing, and the charts that query by deployment do not show it.

## 4. Decision: the hub is a station

The hub appears in the dashboard as a station with a name, a number, a location and a deployment
that starts and ends, created in commissioning. This reverses the sensing plan's choice not to add
the hub to `status.nodes[]` for the first release. The rest of this plan depends on it.

Why: commissioning means naming and locating something and starting a deployment. The existing
deployment, attribution, chart and export machinery then works unchanged. The alternative, a hidden
data source, needs new backend views and a second path through every page.

What follows from it:

1. The hub station stays **out of `registeredNodes`**, so no radio logic (the sync roster, missing
   node, stale recovery, config replay) can see it. Its name, number and location use the same NVS
   functions, keyed by its `nodeId`.
2. It takes a **deployment slot** and writes deployment events like a node, so the backend creates
   its deployment record.
3. Its `status.nodes[]` entry is **added when the status JSON is built**, not stored in the
   registry.
4. It is **not counted in `status.fleet`**, which describes radio nodes.
5. Every command that targets nodes must **refuse** it in the first version (§7.6, §8).

## 5. Identity and data

| Item | Value |
|---|---|
| `nodeId` | `HUB_` plus the last six hex digits of the hub's factory MAC, for example `HUB_A1B2C3`. Fits the 15-character field and matches the MAC-derived node convention. |
| `mac` | The hub's own factory MAC. |
| Number and name | Chosen in the commissioning step, stored with `setNodeName()` and `setNodeUserId()`. The number joins the project's active-deployment number space, so the wizard must run the same collision check as the node wizard. |
| Marker | `isHub: true` and `hardwareRevision: "fieldhub-v2"` in the `status.nodes[]` entry. |
| `seqNum` | A persistent, monotonic counter in NVS that survives hard power cuts. |
| Time | The hub RTC, which the ingest trust check already handles (`v_clock_trusted`). The wizard requires a valid clock first. |
| Battery | The hub's own calibrated battery reading. |

Sensors map onto the existing columns and bits, using the same conversions the node sensor module
uses:

| Source | Columns | Bit |
|---|---|---|
| SHT40 on a port | `airTemp`, `airHumidity` | AIR_TEMP, AIR_RH |
| AS7343 on a port | `spectral_*` | SPECTRAL |
| Reed | `windSpeed` | WIND |
| ADC A0/A1 | `soil1Temp`, `soil1Vwc` | SOIL1 |
| ADC A2/A3 | `soil2Vwc`, `soil2Temp` | SOIL2 |
| Hub battery | `batVoltage` | BATTERY |

The ADC to soil mapping must be checked against how the hub's inputs are wired. **[unverified]**
No CSV column is added. The sensing design note already forbids it.

## 6. Wire contract (first version)

**Readings** are the same objects nodes send:

```json
{ "datetime": "2026-10-24T09:10:00Z", "nodeId": "HUB_A1B2C3", "seqNum": 1042,
  "sensorPresent": 259, "qualityFlags": 0, "configVersion": 0, "deploymentEpoch": 1,
  "batVoltage": 3.74, "airTemp": 16.8, "airHumidity": 67.0 }
```

(`259` is AIR_TEMP + AIR_RH + BATTERY.)

**`status.nodes[]`** gains one entry, built at upload time:

```json
{ "nodeId": "HUB_A1B2C3", "userId": "101", "name": "North Meadow hub",
  "mac": "AA:BB:CC:A1:B2:C3", "state": "DEPLOYED", "lastSeenUnix": 1792000000,
  "wakeIntervalMin": 10, "lastReportedBatV": 3.74,
  "hardwareRevision": "fieldhub-v2", "firmwareVersion": "0.1.0",
  "deploymentEpoch": 1, "deploymentStartedUnix": 1791990000, "deploymentEndedUnix": 0,
  "expectedSensorMask": 259, "sensorPresentMask": 259, "sensorFaultMask": 0,
  "isHub": true }
```

The mask field names follow `sensorMask.js`; confirm them against `PAYLOAD_REFERENCE.md`.
`lastSeenUnix` is the time of the latest capture. `deploymentEndedUnix` is `0` while active and
is emitted on every upload, as the deployment handover requires.

**`status.deploymentEvents`** carries the hub station's start and end events through the existing
durable outbox. The backend's acknowledgements and conflicts, such as a duplicate deployment
number, come back in the same response fields as for nodes.

**Optional `status.hubSensing`** (additive): enabled, interval, ports detected, last capture time
and any faults. It would be stored in `mothership_status` and ignored by older consumers.
Not needed for the first release.

## 7. Firmware plan

### 7.1 Capture

Follow `HUB_LOCAL_SENSING_PLAN.md` Phases 1–6: the shared sensor module, a second RTC alarm, a
persisted configuration, capture into the normal row path, and the wake dispatch. The bring-up
adds three constraints:

- **Port map.** The hub maps mux channels to ports as CN19, CN20, CN17, CN18. The node's code
  hard-codes the SHT40 on channel 0 and the AS7343 on channel 1, which are CN18 and CN17 on the
  node but CN19 and CN20 on the hub. The channel must be a per-board setting.
- **Reed.** Sample it with the radio in a steady state, ignore edges for a short time after any
  Wi-Fi or ESP-NOW start or stop, and do not read the battery ADC during the window.
- **Alarm flags.** A shutdown must confirm both RTC alarm flags are clear, or the next alarm armed,
  before releasing the power hold (bring-up finding 2).

Rows are written through the single append function in the SD plan.

### 7.2 Station identity and deployment

- Create the station's deployment slot from the hub MAC and `HUB_xxxxxx`.
- Starting a hub deployment calls the same start path as a node (epoch, start time, outbox event).
  Ending it sets `deploymentEndedUnix`.
- Whether the deployment store API can be driven for a station that is not in `registeredNodes`
  must be confirmed first. **[unverified]** (§11 phase A).

### 7.3 Status

`buildNodesStatusJson()` appends the hub entry when the feature is on and a deployment exists.
`status.fleet` stays radio-only.

### 7.4 Commissioning workflow in the hub UI

1. **Setup wizard.** A new optional step after *Recording interval*: "Does this FieldHub have
   sensors of its own?" with **Skip** and **Set up**. The clock step comes first, so the station
   always starts with a valid time. Skip stores nothing.
2. **Hub station wizard**, shaped like the node deploy wizard:
   1. *Name and number* — with the deployment-number collision check.
   2. *Sensors installed* — a live scan of the four I2C ports shows what answered. The analogue
      channels and the reed cannot be detected, so the operator declares them. A **Test now**
      button reads each chosen sensor once and shows the value. This reuses the bring-up scan and
      read code.
   3. *Location.*
   4. *Deploy* — sets the collection interval, starts the deployment and arms the second alarm.
3. **Later.** The same wizard is reachable from Settings, with *End deployment* beside it.
4. The Review step lists "Hub sensors: on or off".

### 7.5 Configuration and safety

- New NVS keys: enabled flag, collection interval, sensor mask, next-collection time.
- A compile flag and a runtime enable, as in the sensing plan, so a disabled build is unchanged.
- The collection interval comes from the existing allowed list (1, 5, 10, 20, 30, 60 minutes) and
  is separate from the node recording interval. The existing variable `gWakeIntervalMin` is the
  *node* interval and must not be reused.

### 7.6 Refusing node commands

The command dispatcher must answer an `INVALID` outcome for node commands that name the hub
station, and the project-wide interval command must not touch it. Because the station is not in the
registry, this should follow naturally. It needs a test, not an assumption.

## 8. Backend plan (`FieldMeshDashboard/supabase`)

No migration in the first version. Work items:

1. **Confirm acceptance.** Add a case to `supabase/tests/ingest.test.sql` (or a new
   `hub_station.test.sql`, in the style of `deployments.test.sql`) that ingests a hub-station
   payload and asserts: a `nodes` row with `isHub` in `raw_payload`; a `node_deployments` row with
   the name, number and location; readings attached to that deployment; a `latest_readings` row.
2. **Duplicate and ordering behaviour.** Assert that re-uploading the same rows is harmless. The
   hub's SD plan relies on this (`(mothership_id, node_id, recorded_at, seq_num)` is the key).
3. **Command guards.** `enqueue-control-command`, `enqueue-control-batch` and the recording-interval
   functions must reject a node id that carries `isHub`, with a clear error. The documented
   prerequisite (a complete desired configuration) probably excludes it already **[inferred]**.
   Make it explicit.
4. **Number collisions.** A hub station number competes with node numbers in
   `node_deployments_active_number_idx`. The conflict already comes back in
   `deploymentEventConflicts`; test it.
5. **Documentation.** `PAYLOAD_REFERENCE.md` documents `isHub`, the hub-station semantics and the
   optional `hubSensing`. A short handover note beside the existing
   `node-deployment-epoch-*-handover.md` files gives the firmware contract.
6. **Check** that `export-readings` returns hub-station readings. **[unverified]**

## 9. Dashboard plan (`FieldMeshDashboard/frontend`)

1. `nodeFromRow` carries an `isHub` flag, read from `raw_payload.isHub` and falling back to the
   `HUB_` prefix.
2. **Nodes page:** a "FieldHub" badge; no checkbox, Pause or Resume for the station
   (`canBatchControlNode()` returns false for it); liveness from the age of its last upload; the
   sensor inventory comes from the masks with no change.
3. **Charts and the dashboard:** no logic change if the deployment row exists. Verify with a test
   and a screenshot. The label comes from `nodeIdentity()`.
4. **FieldHubs page:** optionally link a hub card to its station.
5. **Docs and About page:** add the optional step to the "Install equipment" part of
   `FIELDHUB_ONBOARDING_AND_INSTALLATION.md`.
6. **Tests:** `Nodes.test.jsx` and `control.test.js` cases for a station; `lib/data-providers/supabase.test.js`
   for `isHub`.

## 10. The user journey

1. *At home:* register and provision the FieldHub as today.
2. *On site, local portal:* the setup wizard offers "Hub sensors". The operator plugs sensors into
   the ports, presses **Set up**, sees the live scan and a **Test now** reading, names the station,
   places it and presses **Deploy**.
3. *Later:* the hub records at its interval, and the readings upload with the next sync.
4. *Dashboard:* the hub appears as a "FieldHub" station with a chart and sensor inventory. Ending
   its deployment in the local portal ends it in the dashboard after the next upload.

## 11. Work plan and order

| Phase | Where | What | Gate |
|---|---|---|---|
| A. Spikes, no behaviour change | both | (1) Run a hub-station payload through the existing ingest tests. (2) Confirm the deployment store can be driven for a station outside the registry. (3) Measure the flash the sensor libraries add. | Written results; §3.5 confirmed or corrected |
| B. Backend and dashboard first | dashboard repo | §8 items 1–5, §9 items 1–3 and 6 | Harmless to ship: nothing sends a hub station yet. Tests green |
| C. Firmware core | this repo | §7.1 behind the flag, no UI | Bench: rows captured, flag-off baseline identical |
| D. Firmware identity | this repo | §7.2, §7.3, §7.6 | Bench hub uploads to a staging project: station, deployment, chart present |
| E. Commissioning UI | this repo | §7.4 | A fresh hub commissioned end to end |
| F. Field acceptance | both | §12 | All tests pass on a hub with real nodes |

**Rollout order is dashboard first, firmware second.** An older dashboard would show the hub
station as a node with Pause and Resume buttons, so the dashboard must know about `isHub` before
any firmware sends it. This is the reverse of the provisioning flow, where the firmware has to lead
(`FIELDHUB_ONBOARDING_AND_INSTALLATION.md`).

**Sequencing with the other hub work.** Phases C and D change the wake and alarm code that the
rendezvous work (Stage 0) also changes. Do the rendezvous Stage 0 first, as the sensing design
note advises. Phases A and B touch neither and can start now. The SD plan's write path is the
function phase C writes through, so build its phase 1 (the storage seam) before or with phase C.

## 12. Acceptance tests

1. **Flag off.** Hub wake timing, payload and rows are identical to today. Compare against a
   captured baseline.
2. **Commissioning.** A fresh hub with an SHT40 on a port: the scan finds it, **Test now** shows a
   plausible value, Deploy starts the station.
3. **Capture.** Rows carry `HUB_xxxxxx`, epoch 1 and the right columns, at the chosen interval.
4. **Backend.** After an upload to a staging project: a `nodes` row, a `node_deployments` row with
   the chosen name, number and location, readings attached to the deployment, and a
   `latest_readings` row.
5. **Dashboard.** The station appears with a FieldHub badge, a chart and its sensor inventory, and
   with no Pause or Resume control.
6. **Redeploy.** Ending and starting again gives epoch 2 and a separate series.
7. **Commands.** A crafted node command naming the hub station is rejected by the edge function
   and, if it reaches the hub, by the firmware.
8. **Coexistence.** With two radio nodes and the station on, sync attendance matches the baseline
   from the rendezvous sweep.
9. **Outage.** With LTE off for a day, rows are kept and upload in order afterwards.
10. **Power cut during a capture.** No duplicate sequence number, no lost deployment event.

## 13. Risks

- **Image size.** The image is 90.2 % full before the sensor libraries are added. Measure in
  phase A.
- **Wake-code collision** with the rendezvous work (§11).
- **Number collisions.** The station's number shares the project's active-deployment number space.
- **Reed noise** and **unqualified soil probes** on r6: the reed false edges can produce small
  phantom wind values, and soil probes are limited to a 5 V supply. The wizard should label the
  analogue channels "5 V, unqualified" until the next hardware revision.
- **Clock.** A hub with an invalid RTC would stamp invalid times. The wizard requires a valid clock.
- **Backend assumptions.** §3.5 and the `export-readings` check are not yet confirmed.

## 14. Questions for the owner

1. **Confirm the hub is a station** (§4). Everything above depends on it.
2. **Sensor scope for the first version.** I2C ports, reed and battery for certain. Include the
   analogue soil channels now, labelled unqualified, or wait for the next revision?
3. **Remote control.** Is "no dashboard control of the hub station" right for the first version?
   Enabling it or changing its interval from the dashboard would need a new command and a
   backend change.
4. **Who changes the dashboard repo?** I can prepare the backend and dashboard work as a branch in
   `FieldMeshDashboard` for review, if you want that.

## 15. Out of scope

Dashboard control of the hub station, new database columns, new CSV columns, ultrasonic wind,
hardware changes (the next-revision notes cover those), and any change to the radio protocol.
