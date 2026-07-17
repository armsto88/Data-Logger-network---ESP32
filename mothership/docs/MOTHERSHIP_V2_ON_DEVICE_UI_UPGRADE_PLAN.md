# FieldMesh Mothership V2 On-Device UI Upgrade Plan

Date: 2026-07-17

Status: code-validated design and implementation plan. This document does not change firmware.

Scope: the captive portal served from `mothership/firmware/v2/src/config/config_server.cpp` at `http://192.168.4.1/`.

This plan was checked against the current V2 config server, node registry, snapshot decoding and logging paths, CSV schema, transmission settings, earlier Field UI redesign, 2026 UI style guide, and cloud dashboard design. The important current-state corrections are:

- the canonical `/stations`, `/station`, `/settings`, and compatibility routes already exist;
- `NodeInfo` already holds expected, last-present, and fault sensor masks, but it does not hold the actual latest sensor values;
- the current `/api/live` endpoint is deliberately RAM-only and should stay that way;
- `COMMON_CSS` and `COMMON_JS` are already named `PROGMEM` constants, although they still live in the 3,556-line config server file and add about 30 KB of inline text to every full page;
- the normal and config-mode snapshot handlers do not update all runtime sensor fields consistently;
- `/export` still redirects directly to the CSV, and the unused `buildDataStatusSectionHtml()` scans the whole file synchronously;
- manual upload is explicitly blocking, while its current form asks for JSON without using the async-form handler.

## 1. Executive summary

Keep the portal as a small multi-page application with real server routes, but give every page a consistent mobile bottom navigation, a wider two-column desktop layout, and one shared interaction model. The highest-value change is to surface trustworthy latest readings and short trends from a bounded RAM cache, using compact inline SVG rather than a chart library and keeping `/api/live` free of filesystem work. Station management, export, and settings should then be made consistently asynchronous and accessible, while manual upload moves to a separately testable background job. Refactor the monolithic file only after the visible behavior and data contracts are stable.

## 2. Information architecture

### 2.1 Keep server-rendered pages; do not turn the portal into a SPA

The current multi-page model is the right fit for this firmware:

- real URLs make browser Back, Refresh, captive-portal reopen, and no-JavaScript fallbacks predictable;
- each route can build only the data it needs, avoiding a large client-side state/router layer;
- a JavaScript view switcher would retain several pages' markup in RAM and would still need server endpoints for all mutations;
- captive-portal webviews are more reliable with ordinary links and forms than with a custom history/router implementation;
- the existing loading overlay already covers the short navigation gap.

Use JavaScript progressively inside each page for polling, charts, async forms, disclosure controls, and live updates. Do not use hash routing or intercept normal page navigation.

### 2.2 Route and page map

| Route | Page | Default content | Progressively disclosed content |
|---|---|---|---|
| `/` | Hub Overview | Attention summary, fleet counts, latest station readings, hub battery, storage, next collection, upload state, Finish & Start Recording | Set time when the RTC is already valid; firmware/build/network identifiers; support diagnostics |
| `/stations` | Stations | Compact cards for every station, attention-first ordering, latest headline reading, battery, sensor health, last contact | Add-station guide; technical station ID when a friendly name exists |
| `/station?id=...` | Station Detail | State, last reading time, latest sensor values, selected recent trend, sensor health, location/name/notes, lifecycle actions | Spectral channels, recent-readings table, identity editing for active stations, technical/config state |
| `/node-sensors?id=...` | Sensor Setup | Large toggle controls for fitted sensors and a clear save action | Short explanation of automatic versus explicit sensor selection if that distinction remains user-visible |
| `/export` | Data Export | File availability, size, first/latest reading times, recent preview, Download CSV action, phone save guidance | Schema/column explanation and technical storage details |
| `/settings` | Settings | Recording schedule, cloud connection, save state, storage summary | API-key replacement, QR string, endpoint/device identifiers, manual upload, support diagnostics |

The station and sensor pages are children of Stations rather than top-level navigation destinations. Keep the legacy route aliases during the upgrade.

### 2.3 Navigation

Use four persistent top-level destinations: Overview, Stations, Export, and Settings.

On phones:

- show a fixed bottom tab bar with four icon-and-text links;
- mark the current route with `aria-current="page"` and a shape/border as well as colour;
- make each target at least 48 px high and account for `env(safe-area-inset-bottom)`;
- add bottom padding to the document so the bar never covers the final action;
- keep Back in the page header on Station Detail and Sensor Setup;
- place Finish & Start Recording in a sticky action area just above the tab bar only on the Overview and Settings pages.

On tablet and desktop:

- render the same links as a compact horizontal navigation row below the header;
- do not introduce a hamburger menu for only four destinations;
- keep page-specific Back and Refresh actions in the header.

The connection indicator belongs in the header and must say `Connected`, `Updating`, `Connection interrupted`, or `Reconnecting`; its dot is supplementary rather than the only signal.

### 2.4 Progressive disclosure

Default views answer four field questions in this order:

1. Is something asking for attention?
2. Are stations recording and reporting?
3. What did they measure most recently?
4. What action should I take next?

Show by default:

- named station, lifecycle state, battery, configured sensor health, latest recorded time, and headline value;
- hub battery, storage pressure, upload result/backlog, and collection schedule;
- explicit pending states such as `Activation requested` or `Removal scheduled`;
- field-recoverable instructions next to an error.

Collapse or move to Advanced:

- MAC addresses, raw device IDs when a friendly name is present, firmware build, protocol/config versions, radio channel, and endpoint URL;
- API-key replacement and QR-string parsing after initial setup;
- manual upload because it is power-intensive and exceptional;
- RTC correction when the clock is valid;
- full spectral metadata, quality flags, and diagnostic counters.

Do not collapse active warnings, an invalid RTC, failed storage, a sensor fault, a pending lifecycle action, or a failed upload.

### 2.5 Page hierarchy

#### Hub Overview

Use the following order:

1. Header with Hub Overview title and connection state.
2. Conditional attention panel. Render it only when action is useful: invalid RTC, low hub battery, storage warning, upload failure/backlog, sensor fault, overdue station, or pending configuration.
3. Latest station readings. Show up to three cards, sorted by needs-attention first and then newest reading. Each card has one headline value, recorded time, sensor-health text, and a tiny recent trend when history exists. Add `View all stations` when the fleet is larger.
4. Fleet KPI row: Active, Connected, New. Counts remain useful, but they no longer occupy the primary visual position.
5. Two summary cards: Collection schedule and Cloud upload.
6. Finish & Start Recording.
7. About / Advanced.

Do not aggregate temperature, humidity, or soil readings across different stations into one fleet chart. That creates a scientifically ambiguous number and hides spatial differences.

#### Stations

Each card should fit comfortably in roughly 112-136 px on a phone:

- row 1: friendly name, lifecycle badge, chevron;
- row 2: large headline value and unit, with `Recorded 14:32` or a precise no-data state;
- row 3: battery, `6 sensors OK` or `1 sensor not reporting`, and schedule-aware last contact;
- optional 56 x 22 px sparkline at the trailing edge on wider cards.

Replace raw fallback IDs such as `ENV_...` in the main heading with `Unnamed station` and put the raw ID in secondary text. A newly found station should have a strong `Set up station` action.

#### Station Detail

Put observation before configuration:

1. status summary and latest recorded/received times;
2. latest sensor values grouped as Atmosphere, Soil, Wind, Light, Auxiliary, and Power;
3. one selected recent trend with metric buttons;
4. configured sensor health;
5. location and notes;
6. lifecycle actions;
7. Advanced technical details.

The sensor-configuration link remains a child page in the first implementation. Moving it into an in-page modal would add focus management and markup complexity without reducing a common field task.

#### Data Export

The page wraps, but does not replace, `/download-csv`. It should show:

- whether a data file exists;
- exact file size and storage usage;
- first and latest recorded times when the lightweight data index has them;
- a five-row recent preview with horizontal scrolling confined to the table;
- `Download CSV` with `Content-Disposition: attachment`;
- phone guidance: if the captive browser previews the file, use Share/Save to Files;
- a clear `No readings recorded yet` state rather than a 404 as the primary experience.

Do not add date-filtered export in this upgrade. Streaming a filtered CSV through the synchronous server requires a separate bounded parser and deserves its own design.

## 3. Data visualisation plan

### 3.1 Visualisation principles

- Prefer latest value, recorded time, unit, and data state over decorative graphics.
- Never render missing, invalid, or faulted values as zero.
- Do not show a trend arrow unless at least two valid samples for the same sensor exist.
- Do not show a sparkline as current when its newest point is stale.
- Use one scale and unit per mini-chart; avoid dual axes in the portal.
- Give every chart a text summary and recent-values table alternative.
- Preserve raw scientific values. Do not normalise sensor values unless the label explains the transformation.

### 3.2 Hub Overview visualisations

Use small, operational visualisations rather than a dashboard of large charts:

- **Latest station cards:** headline value plus a 12-point sparkline for the same metric. This is the primary measurement view.
- **Storage meter:** a horizontal progress bar with `42% used` text. Warn at an agreed threshold and show exact bytes below.
- **Upload backlog meter:** only when rows are waiting; use text and a short bar relative to an explicit warning threshold, not an unexplained percentage.
- **Fleet lifecycle counts:** retain three numeric tiles. Colour supports, but does not replace, the labels.
- **Attention count:** `2 items need attention` followed by specific links. Do not collapse battery, sensor, storage, and upload health into one vague score.

Recommended station-card headline priority until station-specific preferences exist:

1. air temperature;
2. soil probe 1 volumetric water content;
3. soil probe 2 volumetric water content;
4. wind speed;
5. PAR or spectral clear if the deployed protocol supplies it;
6. Aux 1, then Aux 2;
7. battery only as a final `Last report` fallback, because battery is already a health value.

The chosen sensor ID must be returned with the value so the frontend never guesses the unit.

### 3.3 Station Detail visualisations

#### Latest sensor groups

Use compact metric rows or tiles with:

- sensor name;
- value and unit;
- recorded time inherited from the snapshot;
- one of `Reporting`, `Not reported this cycle`, `Not reporting`, `Not configured`, or `Waiting for first reading`;
- an inline status icon with text.

Expected but absent sensors use the existing debounce semantics: one missing cycle is `Not reported this cycle`; `sensorFaultMask` after consecutive misses is `Not reporting`. Sensors outside `expectedSensorMask` are `Not configured`, not broken.

#### Selected recent trend

Show one chart at a time. Metric buttons are generated only for sensors present in the recent cache. Default to the card headline metric. The chart includes:

- 12 points on phones and up to 24 on wider screens;
- latest value, minimum, maximum, and recorded range as text;
- a simple polyline and optional point markers;
- a flat centre line when all values are equal;
- a gap rather than a zero when a sample lacks the selected sensor;
- a `Recent trend unavailable` state when fewer than two valid samples exist.

#### Spectral snapshot

Spectral channels are a wavelength profile, not a time-series substitute. Render the latest 415-680 nm channels as labelled vertical bars or a polyline across wavelength, with Clear and NIR shown separately. Show gain, integration time, and saturation in the expanded caption. If `spectral_saturated` is true, label the profile `Saturated - compare with caution`; do not silently scale it into a normal-looking chart.

#### Recent readings table

Show the five newest snapshots with a short, user-selected set of columns. Use semantic headers and keep a `View all columns in CSV` link. Do not render all 30 CSV columns on a phone.

### 3.4 Chart technology

Use DOM-created inline SVG for sparklines and the selected station trend.

Why SVG is the best fit:

- a polyline renderer, min/max scaling, and empty-state handling can stay under a few kilobytes of vanilla JavaScript;
- it is resolution-independent and does not need device-pixel-ratio canvas code;
- `<title>` and `<desc>` can describe the graphic;
- colours and dark mode can come from CSS variables;
- 12-24 points do not create a meaningful DOM burden;
- values can be set through attributes/text nodes without interpolating user strings into HTML.

Use CSS bars for storage and upload meters. Do not add Chart.js, a gauge library, canvas animations, radar charts, or a custom icon font.

### 3.5 Data path and cache design

The current code already decodes both wire formats to `DecodedSnapshot`, whose sparse readings use sensor IDs and allow up to 33 readings. Build on that contract.

```text
V1/V2 packet
    -> DecodedSnapshot
    -> shared applySnapshotToRuntime()
         -> NodeInfo contact/config/present/fault fields
         -> LatestSnapshotCache (one per registered station)
         -> RecentSnapshotRing (fixed global capacity)
    -> existing durable CSV logger

LatestSnapshotCache -> /api/live card summary
RecentSnapshotRing  -> /api/readings station detail/history
datalog.csv tail    -> incremental cache seed when config mode starts
```

Create one shared runtime-update helper and call it from both `processSnapshot()` in `main.cpp` and `processDecodedSnapshot()` in `espnow_config.cpp`. Today the normal path updates `lastSeen`, `lastSensorPresent`, and debounced faults, while the config path updates only timestamp/config/battery. Leaving that divergence would make the portal least reliable precisely while it is open.

Use fixed-size POD storage, not `String`, `std::map`, or unbounded vectors:

- `LatestSnapshotCache`: one entry per registered station, containing `char nodeId[16]`, recorded Unix time, received uptime/Unix when available, sequence, quality flags, sensor-present mask, reading count, and the sparse `(sensorId, float)` values.
- `RecentSnapshotRing`: a global compile-time capacity of 48 full sparse snapshots for the first implementation. With the current 33-reading maximum, expect roughly 14-18 KB after alignment; verify with `static_assert`/build output rather than relying on the estimate.
- `/api/readings` limit: default 12 and hard maximum 24.

A global ring gives the real three-station deployment useful depth without multiplying a per-station allocation by an unknown fleet maximum. If a larger supported fleet is confirmed, revise the capacity after measuring free and minimum-free heap.

### 3.6 Seeding recent data after a cold portal start

RAM is lost between field cycles, so a live-only cache would often show `No reading` even though `datalog.csv` contains valid data. Seed the ring from the CSV outside HTTP handlers:

1. At config-mode start, open the file and seek to a bounded tail window, initially 64 KB.
2. If the seek starts mid-row, discard that partial row.
3. Parse only a small number of rows per `configServerLoop()` iteration and yield back to DNS, HTTP, ESP-NOW, and discovery work.
4. Feed parsed rows through the same cache insertion function as live snapshots.
5. Publish a RAM-only cache state: `empty`, `indexing`, `ready`, or `error`.
6. Restart the seed safely if an upload purge replaces the data file while indexing.

Do not call `readCSVFile()`, `getCSVRecordCount()`, `flashGetCSVStats()`, or the dead `buildDataStatusSectionHtml()` from a request handler. Those paths read the whole file and can block the synchronous server or create a large `String`.

For the Export page, read the first data row directly after the header and obtain the latest row from the tail index. Exact record count is optional; label it `Calculating` until an incremental count is available rather than blocking the request.

### 3.7 API contracts

#### `/api/live`

Keep the endpoint RAM-only. Add only the card summary needed across the fleet:

```json
{
  "version": 42,
  "uptimeMs": 123456,
  "cacheState": "ready",
  "nodes": [
    {
      "id": "ENV_123456",
      "label": "North Hedge",
      "state": "DEPLOYED",
      "pending": false,
      "batV": 3.94,
      "lastSeenSec": 80,
      "expectedSensorMask": 263,
      "sensorPresentMask": 263,
      "sensorFaultMask": 0,
      "latest": {
        "recordedUnix": 1784305920,
        "readingVersion": 17,
        "sensorCount": 4,
        "headline": { "sensorId": 1001, "value": 22.1 }
      }
    }
  ]
}
```

Do not put the 12-24 point history for every station in this 650 ms/4 s poll.

Update the live fingerprint to include latest sequence/version, recorded time, `lastSensorPresent`, fault mask, pending target, and fields that change visible output. The current fingerprint omits `lastSensorPresent`, and the client only reconciles cards when `version` changes. For age text, return a stable contact timestamp/uptime anchor and update `x min ago` locally once per minute; do not force a JSON version every second.

#### `/api/readings?node=<id>&limit=<n>&sensor=<optional-id>`

Return only the bounded RAM ring. It must never scan LittleFS on demand.

- validate the station ID against the registry;
- clamp `limit` to 1-24;
- when `sensor` is supplied, return compact `(time, value, quality)` points for that metric;
- without `sensor`, return the latest full sparse snapshot plus up to five summary rows for the detail page;
- return `cacheState` and `historyTruncated` so the UI can be honest;
- send `Cache-Control: no-store`;
- use arrays of sensor ID/value pairs to avoid repeating long property names.

The Station Detail page fetches this endpoint once on load and again only when its `readingVersion` changes in `/api/live`.

### 3.8 No-data, fault, and stale-state rules

| Data condition | User-facing state | Visual treatment |
|---|---|---|
| Cache is still seeding | Loading recent readings | Small spinner; keep existing status content visible |
| Station is New and has never reported | Waiting for station setup | Neutral outlined state and Set up station action |
| Sensor is not in expected mask | Not configured | Muted text; no warning colour |
| Sensor is expected but no snapshot has ever been cached | Waiting for first reading | Neutral text with expected sensor name |
| Sensor missing from one snapshot | Not reported this cycle | Terracotta information state, not a hard fault |
| Sensor is in `sensorFaultMask` | Sensor not reporting | Warning icon, text, and troubleshooting link |
| Value is NaN/invalid or quality flag rejects it | Reading unavailable | Em dash plus reason where known; never `0` |
| Reading is older than its expected collection window plus grace | Last reading is overdue | Recorded time remains visible with overdue text |
| Station has valid data but no recent contact in this portal boot | Last contact unavailable | Do not infer freshness from reset `millis()` state |

Staleness must be schedule-aware. In interval mode, compare against the expected collection cadence and a documented grace period; in daily mode, compare against the next daily collection boundary. Do not use a fixed one-hour rule for every deployment.

## 4. Responsive layout plan

### 4.1 Layout widths and breakpoints

The current `.container` grows from 600 px to only 720 px. Replace the single narrow column with:

- `max-width: 1120px` for top-level pages;
- `max-width: 920px` for form-heavy child pages when a wider grid adds no value;
- 16 px page padding on phones, 20-24 px on tablet, and 24-32 px on desktop;
- one column below 768 px;
- two content columns from 768 px for Overview and Station Detail;
- a two-column station-card grid around 900 px, returning to one column when zoom/reflow makes cards too narrow.

Use CSS Grid with named utility classes. Avoid inline layout styles so breakpoints remain understandable.

### 4.2 Phone layout

- Keep body text at 16 px to avoid mobile-browser input zoom.
- Reduce nested card padding before reducing font size.
- Use one compact status row rather than four small labelled columns inside each station card.
- Keep the most important value and action above the fold.
- Confine horizontal scrolling to the recent-readings table; the page itself must not scroll sideways.
- Make destructive actions full-width only inside the Station actions section, separated from Save.
- Keep the header sticky, but shorten it so header plus bottom navigation do not consume excessive viewport height.
- Put primary actions in the lower half of the screen or sticky action area for thumb reach.

### 4.3 Desktop layout

Overview desktop grid:

- left, roughly two thirds: attention and latest station readings;
- right, roughly one third: hub health, schedule, and upload;
- fleet KPIs span the appropriate column rather than stretching three tiny tiles across the entire screen.

Station Detail desktop grid:

- left: latest measurements, trend, and sensor health;
- right: identity/location/settings and lifecycle actions;
- recent table spans both columns when expanded.

Settings desktop grid:

- recording schedule and cloud connection side by side;
- storage/manual upload below;
- do not make text inputs wider than a comfortable reading length.

### 4.4 Touch, pointer, and zoom behavior

- Minimum target: 44 x 44 px; bottom tabs and primary actions: 48-56 px.
- Maintain at least 8 px between unrelated touch targets.
- Keep the whole station card as a semantic link, but do not nest buttons inside that link.
- Add hover only inside `@media (hover:hover)` so touch users do not get sticky hover states.
- Preserve visible keyboard focus and add `scroll-margin-top` for focus targets below the sticky header.
- Verify 200% zoom and a 320 px viewport without loss of function.

## 5. UX flow improvements

### 5.1 One form contract

All mutation handlers should support the same contract:

- JavaScript path: `POST` form-encoded data with `ajax=1`; JSON response with `ok`, `message`, optional `fieldErrors`, optional `pending`, and optional `redirect`.
- No-JavaScript path: a styled result page built through `headCommon()` with a clear Back action.
- Validation failure: HTTP 400 with field-specific errors.
- Unknown station: HTTP 404.
- Conflict such as a second upload job: HTTP 409.
- Accepted background action: HTTP 202.
- Internal/hardware failure: HTTP 500 or 503 with a field-recoverable message.

Apply the async-form path to time setting, station save/action, sensor save, schedule forms, cloud settings, and manual upload start. The current station save returns a full result page, the sensor picker redirects, and several legacy fallbacks bypass shared styling.

Harden `sendAjaxResult()` or replace it with a small JSON response builder that escapes message content correctly. Keep user/device strings out of `innerHTML`; continue using `textContent` and DOM methods for live cards.

### 5.2 Feedback and pending state

- Keep the submit button label action-specific: `Saving station`, `Applying schedule`, `Searching`, `Starting upload`.
- Put the result beside the affected section as well as in the page live region.
- Keep errors until the user edits/retries; success can clear after several seconds but remains in the changed control state.
- Preserve scroll position and entered values on failure.
- Focus the first invalid field and link its error with `aria-describedby`.
- Do not reload the entire station list after a normal save; call `FM.bump()` and update the relevant text safely.

### 5.3 Optimistic versus confirmed changes

Use optimistic display only for values already accepted locally and not dependent on a sleeping station:

- friendly name, notes, and coordinates may update after the server confirms the NVS save;
- the selected global schedule may update after local persistence, but show `Reaches stations at the next collection` while configuration is pending;
- sensor selection may update after local persistence with the same pending explanation.

Do not optimistically show a station as Active, Paused, Resumed, or Removed. Those states depend on a later deployment/config acknowledgement. Show `Activation requested`, `Pause scheduled`, `Resume scheduled`, or `Removal scheduled`, and let `/api/live` replace the pending state when the real confirmation arrives.

### 5.4 Non-blocking manual upload

The final design is a background job, not a longer browser timeout.

Add a `ManualUploadJob` state machine with one active job:

```text
idle -> queued -> powering modem -> registering network
     -> uploading batch x/y -> shutting down modem
     -> succeeded | partial | failed
```

HTTP behavior:

1. `POST /manual-upload` validates settings, battery guard, storage, pending rows, and no existing job.
2. It creates the job and returns HTTP 202 immediately with a small job ID/status.
3. A dedicated FreeRTOS task runs the existing blocking modem sequence. It never calls `server.send()`.
4. `GET /api/upload-job` returns only a mutex-protected RAM snapshot: state, step label, batches attempted, readings sent, start time, and final error/result.
5. Settings polls this endpoint every 1 s while active and stops when complete or the page is hidden.
6. A second start returns HTTP 409 and the existing job state.

Required safety work:

- choose and measure a task stack based on the existing upload path's heap/stack use rather than accepting the default;
- serialize modem ownership;
- guard upload-queue cursor and LittleFS operations shared with snapshot logging/purge;
- define whether snapshots arriving during upload can append safely or must be queued briefly;
- make status strings fixed-size/bounded;
- keep the job result available until dismissed or the hub powers down;
- do not expose a fake percentage while network registration duration is unknown; show named stages and an elapsed time.

If the background task cannot be made safe in the same release, keep the existing blocking implementation behind Advanced and provide an honest indeterminate warning. A progress page cannot poll a synchronous server while the handler is blocked, so it is not a substitute for the job architecture.

### 5.5 Errors and attention states

Use calm, specific messages with the next action:

| Condition | Message pattern | Action |
|---|---|---|
| Poll interrupted | `Connection to the Hub was interrupted. Reconnecting.` | Automatic retry; retain last content |
| Station overdue | `North Hedge has not checked in since 14:20.` | Open station; show expected collection |
| Sensor fault | `Soil Probe 1 has not reported in two readings.` | Check cable/sensor; open sensor setup |
| Storage warning | `Storage is 82% full.` | Export CSV |
| Storage critical | `Storage is nearly full. New readings may be at risk.` | Export immediately; no automatic destructive clear |
| Upload authentication | `Upload was rejected. Check the API key.` | Open cloud settings |
| Upload network timeout | `The modem could not register on the network.` | Check antenna/coverage; retry |
| RTC invalid | `Hub time needs to be set before recording starts.` | Use browser time |
| Save timeout | `The Hub did not answer. Your changes may not have been saved.` | Keep values; retry |

Loaded content stays visible during background refresh. Do not replace an entire page with a spinner after initial load.

### 5.6 Empty states and onboarding

When no stations exist, Overview should replace normal fleet content with a three-step checklist:

1. Set Hub time, only if needed.
2. Put a station in pairing mode.
3. Find and set up the station.

The Stations page should use an expanded guided panel when the fleet is empty and a collapsed `Add station` control once stations exist. During discovery:

- show the current step and elapsed time;
- keep the instructions visible;
- on success, append and highlight the new station card, then offer `Set up station`;
- on no result, say what to check: power, pairing light, distance, and retry;
- do not report `No new stations` as a generic connection error.

Other empty states:

- no reading: `Waiting for the first reading from this station`;
- no history but a latest value: show the value and `A trend appears after two readings`;
- no CSV: show the export explanation with a disabled download action;
- cloud disabled: `Cloud upload is off. Local recording continues.`

### 5.7 Finish and power-down flow

After `/start` accepts the request, replace the page with a stable completion state:

- `Settings saved. The Hub is starting recording.`
- `You can disconnect from Logger001.`
- next expected collection time when available.

Stop the live poller before the AP disappears so the user does not see a false red reconnect error after a successful shutdown.

## 6. Visual polish

### 6.1 Typography

Keep the system font stack. It is already offline, fast, familiar on phones, and readable; embedding a font would spend flash and delay the first page for no operational benefit.

Use a restrained hierarchy:

- page title: 24 px, 700 weight;
- section title: 18 px, 650-700;
- primary measurement: 24-28 px with tabular numerals;
- body and form controls: 16 px;
- metadata: 13-14 px, never the only place critical information appears;
- line height: about 1.45-1.55 for prose and 1.2 for large metrics.

Use `font-variant-numeric: tabular-nums` for readings, time, voltage, and percentages.

### 6.2 Cards and surfaces

- Preserve the 10 px radius, warm paper background, sage primary, terracotta warning, and soft shadow.
- Use a border on every card; reserve stronger shadow/elevation for interactive cards and sticky navigation.
- Avoid stacking a shadowed card inside another shadowed card. Use dividers or tinted subpanels inside a section.
- Add a narrow status accent or icon only when it carries useful state.
- Keep generous whitespace between conceptual sections, but tighten repeated station cards.
- Replace one-off inline colours/styles with named CSS component classes and semantic tokens.

### 6.3 Status indicators

Every status combines:

- a small inline SVG icon or simple shape;
- a short text label;
- colour;
- optional supporting sentence for warnings.

Examples:

- check + `Reporting` + sage;
- clock + `Activation requested` + blue/neutral;
- pause + `Paused` + blue;
- warning triangle + `Sensor not reporting` + terracotta;
- x/circle + `Upload failed` + danger.

Do not use emoji as operational icons; rendering varies between captive browsers and the current source already shows signs of encoding-sensitive text.

### 6.4 Motion and micro-interactions

- 100-140 ms for hover/focus/tap feedback;
- 140-180 ms for toggles and badge changes;
- 180-240 ms for disclosure panels;
- highlight a newly discovered station once, then settle;
- never replay chart drawing on every poll;
- never pulse a healthy status continuously;
- add `@media (prefers-reduced-motion: reduce)` to remove non-essential animation and smooth scrolling.

### 6.5 Dark mode

Dark mode is worthwhile for night field work, but it should follow after the semantic tokens and accessibility baseline are complete. Implement it as a small `prefers-color-scheme: dark` token override, not a second set of component rules. Test warning/danger contrast, input fill, focus rings, charts, and the sticky bottom bar on real phones. Do not add a JavaScript theme toggle in the first release; a manual override can follow only if field users need it.

## 7. Accessibility fixes

Treat these as acceptance criteria for every phase, not a final cleanup:

1. Add `<html lang="en">` and a real escaped `<title>Page - FieldMesh</title>` in `headCommon()` and the captive landing page.
2. Add a visually hidden skip link before the header and a focusable `<main id="main-content">`.
3. Use semantic `<header>`, `<nav aria-label="Primary">`, `<main>`, `<section aria-labelledby>`, and `<footer>` landmarks.
4. Give every form control a unique `id`; bind every label with `for`. Current latitude, longitude, notes, identity, endpoint readouts, and several checkbox labels need correction.
5. Give help/error text an ID and connect it with `aria-describedby`; set `aria-invalid="true"` on invalid inputs.
6. Add `aria-pressed="true|false"` to every sensor toggle and update it with the visual class. Put the toggles in a labelled group. Preserve single-selection semantics for the wind backend.
7. Ensure hidden radio-card inputs have a visible focus style on the adjacent card, not only a checked style.
8. Use `role="status"`/`aria-live="polite"` for progress and successful saves; use `role="alert"` for errors that need immediate attention.
9. Make the discovery result announcement include the station count and next action. Do not move focus repeatedly during polling.
10. Mark decorative inline SVG with `aria-hidden="true"`; give icon-only controls an accessible name. Prefer icon plus visible text for primary actions.
11. Mark current navigation with `aria-current="page"` and do not rely on colour alone.
12. Give charts `<title>` and `<desc>`, and provide latest/min/max text plus a table alternative.
13. Give tables `<caption>`, `<th scope="col">`, and concise cells. Confine their horizontal scroll inside a labelled region.
14. Keep DOM order equal to visual reading order at desktop breakpoints; CSS Grid must not create a confusing screen-reader sequence.
15. Maintain 4.5:1 normal-text contrast, 3:1 essential graphic contrast, visible focus, 44 px targets, 200% zoom, and 320 px reflow.
16. Respect `prefers-reduced-motion` and do not animate live values for screen readers or sighted users.
17. On the shutdown success state, announce completion once and stop connection warnings.

VoiceOver/TalkBack validation flow:

1. Use the skip link and identify the page title.
2. Traverse primary navigation and hear the current page.
3. Read a station card as one understandable link with name, status, latest value, time, and attention state.
4. Submit a form, hear progress and result, and remain at the affected section.
5. Toggle sensors and hear pressed state.
6. Read the chart summary without needing the graphic.
7. Download data from a clearly named link.

## 8. Maintainability refactor

### 8.1 Lowest-risk sequence

Do not begin the UI upgrade with a 3,556-line file split. First stabilize the visible behavior and new API/cache contracts with focused tests. Then extract one responsibility at a time without changing routes or generated markup in the same commit.

`COMMON_CSS` and `COMMON_JS` are already separate `PROGMEM` strings and `headCommon()` already injects them. The next maintainability gain is moving them, page builders, and JSON builders into separate translation units, not merely renaming the constants.

Recommended eventual structure:

```text
mothership/firmware/v2/src/config/web/
  web_assets.h/.cpp       PROGMEM CSS, JavaScript, shared SVG fragments
  web_frame.h/.cpp        head, navigation, status region, footer, escaping
  web_pages.h/.cpp        pure page/section builders from view models
  web_api.h/.cpp          JSON response builders and validation helpers
  reading_cache.h/.cpp    latest cache, recent ring, incremental CSV seed
  upload_job.h/.cpp       background manual-upload job and RAM status

mothership/firmware/v2/src/config/config_server.cpp
  AP/DNS/server ownership, route registration, lightweight handlers, loop ticks
```

### 8.2 Rendering approach

Do not introduce a general-purpose token templating engine in this firmware pass. Repeated token replacement creates extra scans and transient `String` allocations, while condition-heavy station pages still need C++ logic.

Use:

- small semantic fragment functions such as `appendPageHeader`, `appendStatusBadge`, `appendFormField`, and `appendStationCard`;
- read-only view models (`HubViewModel`, `StationViewModel`, `SettingsViewModel`) populated once by the route handler;
- one `appendHtmlEscaped()` path for all user/device strings;
- `reserve()` based on measured output size;
- `F()`/`FPSTR()` for static literals;
- bounded JSON builders with `jsonEscapeLocal()` for every string;
- optional chunked `sendContent()` later if measured peak heap shows page-sized `String` assembly is a problem.

Keep data acquisition out of page builders. A builder must not open LittleFS, load NVS repeatedly, initialize the upload queue, or mutate the registry.

### 8.3 Asset delivery

Keep CSS/JS inline during the functional upgrade to preserve current captive-browser behavior. After validation, measure navigation transfer and heap:

- current common assets total roughly 30,304 source characters and are repeated in every full HTML response;
- if that repetition is material, serve versioned local routes such as `/assets/fieldmesh-v3.css` and `.js` directly from PROGMEM;
- keep HTML `no-store`, but give versioned assets a long cache lifetime;
- the assets remain fully offline and embedded in firmware;
- test iOS, Android, and Windows captive portals before removing the inline path.

Do not add a CDN, service worker, package manager, or runtime dependency.

### 8.4 Dead code and duplicated behavior

- Remove `buildDataStatusSectionHtml()` once `/export` uses the non-blocking cache/index; do not resurrect its full-file scan.
- Consolidate the normal and config-mode runtime snapshot updates.
- Consolidate lifecycle/status label mapping so server-rendered and live-created cards cannot drift.
- Consolidate sensor metadata (ID, label, unit, decimals, group, headline priority) in one small C++ table mirrored to JavaScript as generated/static JSON, rather than separate switch statements.
- Replace remaining `Node`/`Mothership` UI strings with `Station`/`Hub`; retain code/protocol names internally.
- Keep deployment acknowledgement, config acknowledgement, and local desired state distinct.

### 8.5 Test strategy and budgets

Add focused host/build tests for:

- HTML and JSON escaping;
- sensor metadata/unit/decimal mapping;
- headline selection;
- missing/fault/stale state mapping;
- live fingerprint changes for every visible field;
- ring capacity, wraparound, station filtering, and limit clamping;
- CSV tail parsing for legacy 25-column and current 30-column rows, partial first row, malformed row, empty file, and concurrent file replacement;
- upload-job state transitions and duplicate-start rejection;
- route result status codes and fallback behavior.

Create a development-only HTML fixture or extraction script for running the embedded CSS/JS through a desktop browser and basic syntax/accessibility checks. It must not become a runtime dependency.

Measure each phase against the current firmware:

- firmware binary size delta;
- free heap after boot and minimum free heap after visiting every page;
- largest page `String` allocation;
- `/api/live` bytes and response time with three stations;
- `/api/readings` bytes at the maximum limit;
- poll success during discovery, snapshot reception, CSV download, and upload;
- no filesystem access from `/api/live` or `/api/readings`;
- no dropped snapshot/ACK regression attributable to UI work.

Initial guardrails, to revise from measurements:

- common CSS + JS stays below 36 KB raw;
- recent-reading cache stays below 20 KB;
- three-station `/api/live` stays below 2 KB;
- a maximum `/api/readings` response stays below 12 KB;
- `/api/live` normally completes within 100 ms on the target board;
- minimum free heap regression is explicitly reviewed rather than accepted silently.

## 9. Implementation phasing

All phases preserve captive-portal probe routes, DNS behavior, legacy route aliases, existing CSV format, ESP-NOW protocol, NVS keys, and the adaptive `FM` polling cadence unless a phase explicitly tests a contract change.

Before Phase 1, capture a baseline from the live three-station deployment: screenshots at phone/desktop widths, compiled size, boot/free/minimum heap, page response sizes/times, `/api/live` payload, station states, latest CSV rows, discovery behavior, and a normal Finish & Start cycle.

### Phase 1 - Trustworthy latest readings on Overview and Stations

Highest-impact, bounded change:

- add shared snapshot-to-runtime updating for normal and config paths;
- add fixed latest cache and 48-entry recent ring;
- seed the cache incrementally from a bounded CSV tail;
- extend `/api/live` with cache state, reading version, recorded time, sensor count, and headline sensor/value;
- fix the fingerprint and local last-seen ageing;
- add latest value/time, sensor-health text, and a simple headline sparkline to Overview/Stations;
- implement the explicit no-data/fault states;
- add the accessibility semantics required by these new components.

Three-station milestone:

- one station shows a current air reading and trend;
- one station with a configured missing sensor says `Sensor not reporting` without replacing the last valid value with zero;
- one new/no-data station says `Waiting for station setup/first reading`;
- after rebooting into config mode, recent values repopulate from CSV without waiting for a new packet;
- `/api/live` performs no LittleFS/NVS/upload-queue work and remains responsive during discovery;
- current logging, ACK, and deployment behavior is unchanged.

Rollback boundary: cache/API additions and card UI are additive; disabling the new cache returns cards to existing battery/status fields without changing stored data.

### Phase 2 - Station insights and Data Export

- add `/api/readings` with hard limits;
- add grouped latest sensor values to Station Detail;
- add the selected SVG trend, spectral profile, and five-row semantic preview;
- add the real `/export` page with file/date/storage information and download guidance;
- change CSV disposition to attachment after testing phone behavior;
- remove the dead full-file status builder;
- finish first/latest-row indexing without request-time full scans.

Three-station milestone:

- each fitted sensor is correctly labelled and unit-formatted from real V1/V2 rows;
- spectral saturation/gain metadata is honest;
- missing values create gaps, not zeros;
- `/api/readings` rejects unknown stations and oversized limits;
- CSV download remains byte-for-byte identical;
- a full or malformed data file cannot freeze normal page polling.

### Phase 3 - Navigation, forms, onboarding, responsive layout, and terminology

- add the four-destination mobile bottom navigation and desktop navigation;
- widen desktop layout and compact phone station cards;
- convert all mutations to the shared async JSON contract while preserving styled fallbacks;
- implement inline validation and accurate pending lifecycle states;
- replace remaining user-facing Node/Mothership/Deploy wording with Station/Hub/Activate where protocol precision is not required;
- add first-run checklist, discovery guidance, empty/error states, and shutdown completion state;
- complete the full accessibility checklist and screen-reader flow.

Three-station milestone:

- complete add, name, sensor-configure, activate, pause, resume, and remove flows from a 360-390 px phone viewport;
- pending actions remain pending until the correct acknowledgement;
- every form succeeds without a full navigation when JavaScript is available and has a styled fallback when disabled;
- Back/Refresh, scroll position, focus, zoom, and bottom-bar safe area work on phone and desktop;
- the successful power-down flow does not end in a false reconnect error.

### Phase 4 - Non-blocking manual upload

- add `ManualUploadJob`, dedicated worker task, queue/filesystem/modem ownership guards, and RAM-only status endpoint;
- return HTTP 202 immediately;
- poll named stages and show elapsed time/result;
- prevent duplicate jobs and retain a useful failure message;
- keep the existing blocking path behind a compile-time rollback flag until field validation passes.

Three-station milestone:

- Overview, Settings, `/api/live`, DNS, and Station pages remain responsive throughout modem registration and six upload batches;
- incoming station data is either logged safely or deliberately queued with no loss;
- cursor advancement, purge, retry count, and partial-success semantics remain correct;
- network timeout, 400, 401, transient HTTP failure, low battery, no data, and duplicate click all produce the correct state;
- powering down is disabled or safely coordinated while a job is active.

### Phase 5 - Maintainability refactor and optional dark mode

- extract assets, frame, page builders, APIs, reading cache, and upload job one unit at a time;
- introduce view models and shared status/sensor metadata;
- remove duplicated inline styles and dead helpers;
- optionally switch to versioned local PROGMEM asset routes after captive-browser tests;
- add system dark-mode token overrides and reduced-motion coverage;
- update developer documentation and route/data-contract tables.

Three-station milestone:

- rendered pages and API fixtures remain semantically equivalent before/after each extraction;
- compiled size and minimum heap stay within the accepted budget;
- all captive-portal probes, canonical routes, legacy aliases, forms, charts, downloads, discovery, and power-down flow pass again;
- light/dark contrast and outdoor/night readability are checked on real phones.

## 10. Open questions

The plan includes defaults so implementation can proceed, but these decisions should be confirmed before the affected phase:

1. **Actual target environment.** The request identifies an ESP32-S3, but `mothership/firmware/v2/platformio.ini` currently uses `board = esp32dev`, a default environment named `mothership-v1-main`, and several V1 comments. Which build/partition file is the deployment authority? Default: resolve and record the actual S3 environment before accepting memory/flash measurements.
2. **Supported fleet size.** The live deployment has three stations, but no product maximum is stated. Default: a global 48-snapshot ring and one latest entry per registered station, with a 20 KB cache ceiling. Increase only after target-board heap measurement.
3. **Headline sensor priority.** Should every station use the default priority in Section 3, or should the user select a preferred metric per station? Default: fixed automatic priority in this upgrade; user-selectable preference is a later NVS/schema change.
4. **Time semantics and timezone.** Are node/Hub Unix timestamps UTC, local wall time stored as epoch, or mixed? The UI currently formats with UTC-style functions while labels imply local time. Default: label charts and exports `Hub time` and do not claim a timezone until the firmware contract is confirmed.
5. **Freshness grace.** What grace should be added after an expected interval/daily collection before `overdue` appears? Default: 15 minutes after the expected collection boundary, with schedule-aware logic and no fixed one-hour rule.
6. **Recent-history depth.** Is roughly 12-16 samples per station on the three-station deployment sufficient for field verification? Default: 48 global cached snapshots, 12 plotted on phones, 24 maximum from the API.
7. **Overview density.** Should Overview show all three current backyard stations or cap the list as the fleet grows? Default: show up to three, attention-first, then link to Stations.
8. **Bottom navigation.** Is the four-tab mobile bar acceptable inside the captive-portal webview, including the permanent vertical space cost? Default: yes below 768 px; use horizontal navigation above it.
9. **Manual upload concurrency.** May the upload worker run concurrently with incoming snapshot logging, or should config mode briefly queue snapshot writes while the upload queue purges/replaces the CSV? Default: allow append only after a filesystem/queue locking review; otherwise use a bounded snapshot queue and expose that state.
10. **Export statistics.** Is exact record count required, or are file size plus first/latest time sufficient? Default: do not block for an exact count; show it only after an incremental count completes.
11. **Dark-mode control.** Is following the phone's system theme enough? Default: system preference only, no portal-specific toggle in this release.
12. **Storage action.** Should the Export page eventually offer `Clear exported data`? Default: no destructive storage action in this upgrade. It needs a separate retention/confirmation/recovery policy.

Decisions already made by this plan:

- keep real multi-page routes rather than a SPA;
- keep all runtime assets local and embedded;
- use inline SVG/CSS rather than a chart library;
- keep `/api/live` RAM-only;
- separate live summary from bounded station history;
- distinguish desired, pending, acknowledged, and observed lifecycle/configuration states;
- do not read the whole CSV in an HTTP handler;
- do not present a fake progress percentage for manual upload.
