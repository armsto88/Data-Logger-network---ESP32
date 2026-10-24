# FieldHub Own-Sensor Data — Integration Plan (firmware, backend, dashboard)

Status: **proposed, nothing implemented.** Written 2026-10-23, revised 2026-10-24 after the owner
decided that the hub is never a node (§4).

Audience: the owner, and engineers on this repo (hub firmware) and on
`armsto88/FieldMeshDashboard` (Supabase backend and React dashboard). The dashboard repo was
**read, not changed**, for this plan. Every statement about it cites a file there.

Related notes:

- `docs/HUB_LOCAL_SENSING_PLAN.md` — the firmware phases for collecting hub-local readings. This
  plan keeps its Phases 1–6 and replaces its Phase 7.
- `mothership/docs/MOTHERSHIP_LOCAL_SENSOR_COLLECTION_DESIGN_NOTE.md` — earlier design note.
- `mothership/docs/MOTHERSHIP_SD_PRIMARY_STORAGE_PLAN.md` — where the hub's rows are stored.
- `mothership/docs/MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` — shares the wake code (§11).
- `hardware/FieldHub_v2/revisions/2026-10-10-r6/BRINGUP_HARNESS.md` — what the r6 hardware does.
- Dashboard repo: `docs/FIELDHUB_ONBOARDING_AND_INSTALLATION.md`, `docs/PAYLOAD_REFERENCE.md`.

## 1. Summary

The new FieldHub can read its own sensors. When the operator turns that option on during setup, the
hub records them and sends them with its normal uploads. In the dashboard they appear as **the
FieldHub's own data**, on the FieldHub's card, not as a node.

The backend already stores a reading from any id, and the existing sensor columns cover what the
hub can read, so **no database migration and no new CSV column are needed** for the first version.
The work is in the hub firmware (capture and a setup option), a place on the FieldHub card to show
the data, and a small filter so the data never appears as a node card.

## 2. Requirements

From the owner, 2026-10-23 and 2026-10-24:

- The hub is **never a node**. It stays a hub. Only the new hub has its own sensors, and it
  transmits their data **if the option is selected**.
- The option belongs in **commissioning**, and must be optional. A hub with it off behaves exactly
  as it does today, and a test proves it (§12, test 1).
- The data takes the same path as everything else: SD card and LittleFS, LTE upload, ingest,
  dashboard.
- **First version leaves out the analogue (soil) inputs.** They wait for the next hardware
  revision, which gives the probes the supply they need.

## 3. What exists today

Tags: **[verified]** read in code or measured; **[inferred]** follows from code but not run;
**[unverified]** not checked.

### 3.1 Hardware (FieldHub v2 r6, bring-up proven)

| Interface | In the first version |
|---|---|
| I2C sensor ports CN17–CN20 (mux 0x71; channel 0–3 is CN19, CN20, CN17, CN18) | **Yes.** An SHT40 was read on every port. |
| Reed (wind) on GPIO36 | **Yes**, with the radio-quiet sampling rule in §7.1. It sees false edges when Wi-Fi or ADC1 activity changes. |
| Hub battery | **Yes.** Calibrated; reads about 1.2 % high against a meter. |
| Analogue CN15/CN16 (ADS1015) | **No.** The 5 V sensor supply cannot power the soil probes. Next revision. |

The sensor rails are on whenever the hub is awake. Firmware cannot switch them or see a fault.

### 3.2 Hub firmware

- **[verified]** Rows use the 35-column canonical header (`storage/csv_schema.h`).
  `processSnapshot()` writes them to LittleFS and the SD card.
- **[verified]** A row's name, number and location columns are read from NVS by `nodeId`
  (`getNodeName()`, `getNodeUserId()`, `getNodeLatitude()` call `loadNodeMeta()`). For an id with
  nothing stored they return empty, so a hub row has blank values there.
- **[verified]** `resolveEpochForSample(nodeId, sampleUnix)` stamps each row with a deployment
  epoch. For an id with no deployment it gives 0.
- **[verified]** The hub's setup wizard (`handleSetupWizard()`) has seven steps: set up, connect to
  FieldMesh, dashboard control, clock, recording interval, review, done.
- **[verified]** Nothing collects hub-local readings yet. The sensor stack is not linked into the
  hub, and the production image is 90.2 % of its OTA slot (1,419,049 of 1,572,864 bytes).

### 3.3 Backend (`FieldMeshDashboard/supabase`)

- **[verified]** Ingest validation requires only a non-empty `nodeId` of up to 120 characters and a
  `deploymentEpoch` of 0–65535 (`functions/_shared/fieldmesh.ts`).
- **[verified]** `readings` has no foreign key to `nodes`. It stores `node_id` as text, with its
  `mothership_id`, and the duplicate key is `(mothership_id, node_id, recorded_at, seq_num)`
  (`migrations/202606290001_create_fieldmesh_platform.sql`). Ingest also writes a
  `latest_readings` row for any id.
- **[verified]** `readings` has fixed columns for air temperature and humidity, spectral bands, wind
  and battery, among others. They cover the sensors in §3.1.
- **[verified]** A reading with epoch 0 and no active deployment for its id is simply stored without
  a deployment (`resolve_reading_deployments`, `migrations/202607310001_add_node_deployments.sql`).
- **[verified]** `mothership_status` has a free `raw_payload` JSON column for the status object
  (`migrations/202606290001_create_fieldmesh_platform.sql`), so an additive status key needs no
  migration.
- **[verified]** `export-readings` filters by node id only when the caller asks for specific nodes,
  so an unfiltered project export includes every id (`functions/export-readings/index.ts`).

### 3.4 Dashboard (`FieldMeshDashboard/frontend`)

- **[verified]** The node list, the Dashboard cards and the Charts node picker all come from the
  `nodes` table (`getNodes()` in `src/lib/data-providers/supabase.js`; `pages/Dashboard.jsx`;
  `pages/Charts.jsx`). A reading whose id is not in `nodes` is not shown by any of them.
- **[verified, one exception]** `Dashboard.jsx`, `Charts.jsx` and `Nodes.jsx` each build their list
  from `nodes` when there are any, and otherwise from the latest readings:
  `nodes.length > 0 ? nodes : allLatest.map(...)`. A project with no nodes yet would show the hub's
  data id as a node on all three pages.
- **[verified]** `getData(nodeId, hours, ..., projectId)` reads `readings` by project and id with
  no deployment needed, and the sensor bits in `src/lib/sensorMask.js` cover the hub's sensors.
- **[verified]** The FieldHubs page (`pages/Motherships.jsx`) renders one card per hub from
  `motherships` and `mothership_status`.

### 3.5 What happens with no change

**[inferred, confirm with the test in §11 phase A]** A hub reading uploaded as it stands is stored
and ignored by every node-centric view, with the one exception above. Nothing shows it. That is
why the dashboard needs a place for it.

## 4. Decision: the hub's data is the hub's data

The hub is a hub. Its own sensor readings are the **FieldHub's data**: they belong to the hub's
record, are shown on the FieldHub's card, and never become a node, a node card, a deployment or a
member of the fleet.

The earlier draft of this plan made the hub a node-like "station" with a name, number, location and
deployment. That is dropped. It is also a simpler design:

| Dropped | Replaced by |
|---|---|
| A deployment slot, epochs and start/end events for the hub | Epoch 0. The hub's data is one continuous series per FieldHub registration |
| A `status.nodes[]` entry and an `isHub` marker | A small `status.hubSensing` object in the status, stored in `mothership_status.raw_payload` |
| A node-style deploy wizard with name, number and location | One optional step in the hub's setup |
| Number collisions with node deployments | Nothing to collide with |
| Guards so node commands refuse the hub | Nothing to refuse: it is not a node |
| Counting the hub in `status.fleet` | Not counted |

The reading still needs an id, because ingest and the duplicate key require one. That id is a
**data label**, not a node: `HUB_` plus the last six hex digits of the hub's MAC.

## 5. Identity and data

| Item | Value |
|---|---|
| Data id | `HUB_` plus the last six hex of the hub's factory MAC, for example `HUB_A1B2C3`. The dashboard derives the same id from `motherships.mac_address`, so no column is needed to find the data. |
| Epoch | 0 |
| Name, number, location columns | Blank. The FieldHub's name lives on its card. |
| `seqNum` | A persistent, monotonic counter in NVS that survives hard power cuts. |
| Time | The hub RTC; the ingest trust check already handles a bad clock. The setup step requires a valid clock first. |
| Battery | The hub's own calibrated battery reading. |

| Source | Columns | Bit |
|---|---|---|
| SHT40 on a port | `airTemp`, `airHumidity` | AIR_TEMP, AIR_RH |
| AS7343 on a port | `spectral_*` | SPECTRAL |
| Reed | `windSpeed` | WIND |
| Hub battery | `batVoltage` | BATTERY |

No CSV column is added.

## 6. Wire contract (first version)

**Readings** are the same objects nodes send, with the data id and epoch 0:

```json
{ "datetime": "2026-10-24T09:10:00Z", "nodeId": "HUB_A1B2C3", "seqNum": 1042,
  "sensorPresent": 259, "qualityFlags": 0, "configVersion": 0, "deploymentEpoch": 0,
  "batVoltage": 3.74, "airTemp": 16.8, "airHumidity": 67.0 }
```

(`259` is AIR_TEMP + AIR_RH + BATTERY.)

**`status.hubSensing`** is a new, additive object. Older consumers ignore it:

```json
{ "enabled": true, "dataId": "HUB_A1B2C3", "intervalMin": 10,
  "sensors": ["air_temp", "air_rh", "wind"], "lastCaptureUnix": 1792000000,
  "faultMask": 0 }
```

There is no `status.nodes[]` entry and no deployment event for the hub.

## 7. Firmware plan

### 7.1 Capture

Follow `HUB_LOCAL_SENSING_PLAN.md` Phases 1–6: the shared sensor module, a second RTC alarm, a
persisted configuration, capture into the normal row path, and the wake dispatch. The bring-up adds
three constraints:

- **Port map.** The hub maps mux channels to ports as CN19, CN20, CN17, CN18. The node's code
  hard-codes the SHT40 on channel 0 and the AS7343 on channel 1, which are CN18 and CN17 on the
  node but CN19 and CN20 on the hub. The channel must be a per-board setting.
- **Reed.** Sample it with the radio in a steady state, ignore edges for a short time after any
  Wi-Fi or ESP-NOW start or stop, and do not read the battery ADC during the window.
- **Alarm flags.** A shutdown must confirm both RTC alarm flags are clear, or the next alarm armed,
  before releasing the power hold (bring-up finding 2).

Rows are written through the single append function in the SD plan.

### 7.2 Outside the radio paths

The hub's data is never in `registeredNodes`, never in the deployment store and never in the sync
roster, so no radio logic (sync window, missing-node, stale recovery, config replay) can see it.
Nothing about the node protocol changes.

### 7.3 Status

`status.hubSensing` is added to the status JSON when the option is on. `status.nodes` and
`status.fleet` are unchanged.

### 7.4 Commissioning option

One new, skippable step in the setup wizard, after *Recording interval* and before *Review*:

1. "Does this FieldHub have sensors of its own?" with **No** and **Yes**. No stores nothing.
2. Yes shows a live scan of the four I2C ports (what answered, at which port), a checkbox for the
   reed, and a **Test now** button that reads each chosen sensor once and shows the value. This
   reuses the bring-up scan and read code.
3. The operator picks the collection interval from the existing list (1, 5, 10, 20, 30, 60
   minutes) and saves. Saving arms the second alarm.

The clock step comes first, so a recorded reading always has a valid time. The Review step lists
"Hub sensors: on or off". The same page is reachable later from Settings, with a way to turn it
off.

### 7.5 Configuration and safety

- New NVS keys: enabled flag, collection interval, sensor selection, next-collection time.
- A compile flag and a runtime enable, as in the sensing plan, so a disabled build is unchanged.
- The collection interval is separate from the node recording interval. The existing variable
  `gWakeIntervalMin` is the *node* interval and must not be reused.

### 7.6 Transmission

Hub rows go through the same upload queue as node rows, so they are sent exactly when the data
destination is FieldMesh (or a custom HTTPS receiver) and stay on the hub when the destination is
local storage only. There is no separate transmit switch (§14, question 1).

## 8. Backend plan (`FieldMeshDashboard/supabase`)

No migration in the first version. Work items:

1. **Confirm acceptance.** Add a case to `supabase/tests/ingest.test.sql` (or a new
   `hub_data.test.sql`, in the style of `deployments.test.sql`) that ingests hub readings with
   `status.hubSensing` and asserts: the readings are stored with epoch 0 and no deployment; a
   `latest_readings` row exists; no `nodes` row and no `node_deployments` row appear;
   `mothership_status.raw_payload` carries `hubSensing`.
2. **Repeat uploads.** Assert that re-uploading the same rows is harmless. The hub's SD plan relies
   on this.
3. **Unknown ids in commands.** Confirm that `enqueue-control-command` and the batch function reject
   an id with no `nodes` row, so a crafted command for `HUB_…` cannot be queued. **[unverified]**
4. **Export.** Confirm the unfiltered project export includes hub rows, with the id readable as the
   hub's. **[inferred from §3.3]**
5. **Documentation.** `PAYLOAD_REFERENCE.md` documents `status.hubSensing` and the `HUB_` data id.

## 9. Dashboard plan (`FieldMeshDashboard/frontend`)

1. **A helper** `isHubData(nodeId)`, true for `HUB_` ids, in `src/lib/nodeIdentity.js`. It is used
   to keep these ids out of node views.
2. **The fallback in three pages.** In `Dashboard.jsx`, `Charts.jsx` and `Nodes.jsx`, the code that
   builds the list from the latest readings when a project has no nodes must skip hub data ids.
   One shared helper that all three call is better than three copies.
3. **FieldHub card.** On the FieldHubs page, a "Hub sensors" section appears when `hubSensing` is
   present: the latest values, the last capture time, the interval, any fault, and a small trend.
   It reads `getLatest(dataId)` and `getData(dataId, hours)`, where `dataId` is derived from the
   hub's MAC.
4. **Charts and Nodes pages** are unchanged: they list nodes, and the hub is not one.
5. **Export** includes the rows already. Optionally label the hub id.
6. **Docs.** Add the optional step to the "Install equipment" part of
   `FIELDHUB_ONBOARDING_AND_INSTALLATION.md`.
7. **Tests:** a test for each of the three pages that a hub-only project shows no node; a FieldHubs
   card test; `supabase.test.js` for the derived data id.

## 10. The user journey

1. *At home:* register and provision the FieldHub as today.
2. *On site, local portal:* during setup, answer **Yes** to "Does this FieldHub have sensors of its
   own?", see which ports answered, press **Test now**, pick an interval and save.
3. *Later:* the hub records at its interval and the rows upload with the next sync, if the data
   destination is FieldMesh.
4. *Dashboard:* the FieldHub's card shows a "Hub sensors" section with the latest readings and a
   trend. Nodes are listed exactly as before.

## 11. Work plan and order

| Phase | Where | What | Gate |
|---|---|---|---|
| A. Spikes, no behaviour change | both | (1) Run hub readings through the existing ingest tests and confirm §3.5. (2) Measure the flash the sensor libraries add. | Written results |
| B. Dashboard and backend | dashboard repo | §8 items 1–5, §9 items 1–3 and 7 | Safe to ship first: nothing sends hub data yet. Tests green |
| C. Firmware capture | this repo | §7.1 behind the flag, no UI | Bench: rows captured; flag-off baseline identical |
| D. Firmware status and setup option | this repo | §7.3, §7.4 | A fresh hub commissioned end to end |
| E. Field acceptance | both | §12 | All tests pass on a hub with real nodes |

**Order.** An older dashboard would store the data and ignore it, except in a project with no
nodes yet, where it would show an odd card. So the dashboard filter should ship first, but the order
is flexible.

**Sequencing with the other hub work.** Phases C and D change the wake and alarm code that the
rendezvous work (Stage 0) also changes. Do the rendezvous Stage 0 first, as the sensing design note
advises. Phases A and B touch neither and can start now. The SD plan's append function is the one
phase C writes through, so build its storage seam (phase 1) before or with phase C.

## 12. Acceptance tests

1. **Flag off.** Hub wake timing, payload and rows are identical to today. Compare against a
   captured baseline.
2. **Setup.** A fresh hub with an SHT40 on a port: the scan finds it, **Test now** shows a
   plausible value, and saving turns the option on.
3. **Capture.** Rows carry the `HUB_` id, epoch 0 and the right columns, at the chosen interval.
4. **Backend.** After an upload to a staging project: readings stored without a deployment, a
   `latest_readings` row, `hubSensing` in the status, and no `nodes` or `node_deployments` row.
5. **Dashboard.** The FieldHub card shows the Hub sensors section. The node list, the Dashboard
   cards and the Charts picker are unchanged, including in a project that has no nodes yet.
6. **Destination.** With the destination on local storage only, no hub rows are sent; with FieldMesh,
   they are.
7. **Coexistence.** With two radio nodes and the option on, sync attendance matches the baseline
   from the rendezvous sweep.
8. **Outage.** With LTE off for a day, rows are kept and upload in order afterwards.
9. **Power cut during a capture.** No duplicate sequence number and no corrupt row.

## 13. Risks

- **One continuous series.** With no deployment, a hub moved to a new site is not separated from its
  old site automatically. Today's way out is to archive the FieldHub and register it again, which
  starts a new record (§14, question 2).
- **No location** for the hub's data: the FieldHubs page has no location field today.
- **Image size.** The image is 90.2 % full before the sensor libraries are added. Measure in
  phase A.
- **Wake-code collision** with the rendezvous work (§11).
- **Reed noise.** The false edges can produce small phantom wind values; §7.1 reduces them.
- **Clock.** A hub with an invalid RTC would stamp invalid times; the setup step requires a valid
  clock.
- **Backend assumptions.** §3.5 and §8 items 3 and 4 are not yet confirmed.

## 14. Questions for the owner

1. **Transmission switch.** Hub rows are sent whenever the data destination sends everything else.
   Do you also want a separate switch that keeps the hub's own data local while nodes upload?
2. **Site and location.** The hub's data is one continuous series with no location. Is that right
   for the first version, with "archive and register again" as the way to start a new series? A
   location on the FieldHub record would be a small later addition.
3. **Dashboard repo.** When you are ready, I can prepare §8 and §9 as a branch in
   `FieldMeshDashboard` for your review. Not before.

## 15. Out of scope

The analogue (soil) inputs, until the next hardware revision. Deployments, names, numbers and
locations for hub data. Dashboard control of the hub's sensors. New database columns, new CSV
columns, ultrasonic wind, hardware changes (the next-revision notes cover those), and any change to
the radio protocol.
