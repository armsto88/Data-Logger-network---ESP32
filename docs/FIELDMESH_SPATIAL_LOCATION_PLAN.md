# fieldMesh Spatial / Location Plan

Status: Proposed
Date: 2026-06-27
Owners: fieldMesh firmware + Field UI + dashboard

## Overview

### Why
Ecological field data only becomes meaningful with geographic context. Sensor
readings need to be placed on a map, compared across sites, and analysed
spatially. Without coordinates, a reading is just a number; with coordinates it
becomes a point on a landscape that can be overlaid with vegetation maps,
tenure boundaries, catchments, and other sites.

### What
Add latitude/longitude to every node in the fieldMesh network:

- Stored in the node registry (NVS) on the mothership.
- Captured via the Field UI (phone GPS or manual entry).
- Included in every JSON upload to the cloud.
- Included in every CSV export.
- Rendered as markers on the dashboard map.

## Architecture

### Node location
- The mothership holds the authoritative location for each paired node in its
  NVS node registry, alongside the existing `lastReportedBatV`, deployment
  state, and config replay fields.
- Location is set once (at deployment) and rarely changes, so it is a
  configuration-time value rather than a per-snapshot telemetry value.

### Field UI
The Field UI (served by the mothership config portal) gains a "Location"
section on the node detail page with three capture options:

1. **Use phone GPS** (preferred for field use)
   - A button calls the JavaScript
     `navigator.geolocation.getCurrentPosition()` API.
   - The phone has a GPS receiver even with no internet connection, so this
     works on the captive portal while the user is standing next to the node.
   - The captured lat/lon is POSTed to the config server and written to NVS.
2. **Manual coordinate entry**
   - Two text fields (latitude, longitude) for typing coordinates from a
     handheld GPS, map, or site plan.
   - Useful when the node is inaccessible or the phone GPS is unavailable.
3. **Click on a map** (future / online only)
   - Embed a Leaflet + OpenStreetMap map in the config portal and let the user
     drop a pin.
   - **Limitation:** requires internet to load map tiles, so it will not work
     on the offline captive portal in the field. Noted as a Phase 4 / online
     convenience option, not a field-primary path.

The phone GPS option is the best for field use because the user is physically
at the node when deploying it.

### JSON upload
- The `status.nodes[]` array in the JSON payload uploaded to the cloud gains
  `latitude` and `longitude` fields per node entry.
- Nodes with no location set emit `null` (or omit the fields) so the schema
  stays backward compatible.

### Google Apps Script
- The Nodes sheet gains `Latitude` and `Longitude` columns.
- Existing rows are backfilled with blank values; new uploads populate them.

### CSV export
- The CSV header gains `latitude` and `longitude` columns.
- Values are decimal degrees to 6 decimal places, or blank when unset.

### Dashboard
- A map view (Leaflet.js + OpenStreetMap tiles) shows all nodes as markers.
- Marker clustering is used when many nodes are deployed.
- Each marker popup shows the node label, latest readings, battery voltage,
  and last-seen timestamp.
- Nodes without coordinates are listed in a sidebar "unplaced" group.

## Implementation phases

### Phase 1: Phone GPS capture + manual entry + NVS storage
- Add `latitude` / `longitude` to the `NodeInfo` struct.
- Add `lat` / `lon` keys to the `paired_nodes` NVS namespace.
- Add a "Location" section to the Field UI node detail page with the phone
  GPS button and manual entry fields.
- Add config-server endpoints to accept and persist the coordinates.

### Phase 2: Include lat/lon in JSON upload + Apps Script Nodes sheet
- Emit `latitude` / `longitude` in the `status.nodes[]` JSON array.
- Update the Google Apps Script to write the new columns into the Nodes
  sheet.

### Phase 3: Include lat/lon in CSV export
- Add the two columns to the CSV header and per-row output.

### Phase 4: Dashboard map view
- Add the Leaflet map view to the dashboard with markers, clustering, and
  popups.

## Technical details

### Data model
- `NodeInfo` struct gains:
  - `float latitude`  (default `NAN` = not set)
  - `float longitude` (default `NAN` = not set)
- Using `float` (32-bit) is sufficient for 6 decimal places (~0.11 m) and
  keeps the struct compact for NVS storage.

### NVS storage
- Namespace: `paired_nodes` (existing).
- New keys: `lat` and `lon` per node entry, stored alongside the existing
  battery and deployment-state fields.
- Unset nodes store `NAN` (or a sentinel) so a missing location is
  distinguishable from `(0, 0)`.

### Field UI geolocation
- `navigator.geolocation.getCurrentPosition()` is available in mobile
  browsers over HTTPS and over the captive portal, because the phone's GPS
  does not require internet.
- The config portal is served over HTTP on the captive portal; geolocation
  may require a secure context on some browsers. If blocked, fall back to
  manual entry. This is an open compatibility question to validate on the
  target field phones.

### Map embedding in the config portal
- Loading OpenStreetMap tiles requires internet, which is unavailable on the
  offline captive portal in the field.
- Therefore Phase 1 uses phone GPS + manual entry only; the embedded map is
  deferred to the online dashboard (Phase 4).

### Dashboard map
- Leaflet.js with OpenStreetMap raster tiles.
- `Leaflet.markercluster` for handling many nodes.
- Popup content: node label, latest sensor readings, battery voltage,
  last-seen time.

## Data format

- **Latitude:** decimal degrees, 6 decimal places (e.g. `-27.469771`).
- **Longitude:** decimal degrees, 6 decimal places (e.g. `153.025124`).
- **Coordinate system:** WGS84 (standard GPS).
- **Precision:** ~0.11 m at 6 decimal places — sufficient for ecological
  field plots.

## Privacy / security

- Coordinates are project-scoped and live inside the Supabase multi-tenant
  model; they are never exposed publicly without the owning tenant's consent.
- Exports include coordinates by default because they are essential for
  ecological interpretation. Users may strip the columns before sharing if a
  dataset must be anonymised.
- No coordinates are written to public-facing endpoints.

## Open questions

- Does mobile browser geolocation work reliably over the captive portal's
  HTTP context, or does the config portal need a self-signed TLS cert?
- Should location be editable per-deployment (one coordinate) or per-sample
  (for roving nodes)? Current plan: per-deployment only.
- Should the dashboard map support layer overlays (vegetation, tenure)? Out
  of scope for Phase 4 but worth noting for the roadmap.