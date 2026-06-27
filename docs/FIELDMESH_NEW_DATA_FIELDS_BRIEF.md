# New Data Fields Available from the Mothership

**Date:** 2026-06-27
**To:** Dashboard frontend team
**From:** Firmware team
**Purpose:** Documentation of new data fields available in the Google Sheets backend for dashboard consumption

---

## Overview

The mothership firmware has been updated with new data fields in the JSON upload. The Google Apps Script backend writes these to the appropriate sheets. Here's what's available and how to use it.

---

## 1. `projectStarted` (Status sheet)

**What:** A Unix timestamp marking when the mothership was first powered on. Set once in NVS on first boot, never overwritten. Represents the start of the project/deployment.

**Where:** Status sheet, key = `projectStarted`, value = Unix timestamp (e.g. `1782493200`)

**How to use in the dashboard:**
```javascript
// Convert to readable date
const projectDate = new Date(status.projectStarted * 1000);
// Display: "Project started 14 Jun 2026" or "Running for 14 days"
const daysRunning = Math.floor((Date.now() - projectDate) / 86400000);
```

**Dashboard placement:** Project header or settings page — "Project started: 14 Jun 2026 (14 days ago)"

---

## 2. `deployedSinceUnix` (Nodes sheet)

**What:** A Unix timestamp marking when each node was activated (state changed to DEPLOYED). Reset to 0 if the node is stopped and re-deployed. Stored per-node in NVS on the mothership.

**Where:** Nodes sheet, column = `deployedSinceUnix`, value = Unix timestamp or 0

**How to use in the dashboard:**
```javascript
// Per-node deployment duration
if (node.deployedSinceUnix > 0) {
  const deployDate = new Date(node.deployedSinceUnix * 1000);
  const daysDeployed = Math.floor((Date.now() - deployDate) / 86400000);
  // Display: "Deployed 3 days ago" or "Since 14 Jun 2026"
}
```

**Dashboard placement:** Node cards, node detail page, node table — "Deployed: 3 days" or "Since 14 Jun"

---

## 3. `latitude` and `longitude` (Nodes sheet)

**What:** GPS coordinates for each node, captured via the phone's GPS when the user stands next to the node in the field. Decimal degrees, WGS84, 6 decimal places (~0.11m precision). Empty if not set.

**Where:** Nodes sheet, columns = `latitude` and `longitude`

**How to use in the dashboard:**
```javascript
// Show on a map (Leaflet + OpenStreetMap)
if (node.latitude && node.longitude) {
  L.marker([node.latitude, node.longitude])
    .addTo(map)
    .bindPopup(`${node.name}<br>Battery: ${node.lastReportedBatV}V<br>Temp: ${latestReading.airTemp}°C`);
}

// Calculate distance between nodes
function distance(lat1, lon1, lat2, lon2) {
  const R = 6371e3; // Earth radius in meters
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a = Math.sin(dLat/2) ** 2 + Math.cos(lat1*Math.PI/180) * Math.cos(lat2*Math.PI/180) * Math.sin(dLon/2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
}
```

**Dashboard placement:**
- Map view with markers for each node
- Node detail page showing coordinates with a "View on map" link
- Node table with location column
- Data export including coordinates

---

## 4. `batVoltage` (Status sheet) — mothership battery

**What:** The mothership's own battery voltage (not a node's). Read from the ADC on the mothership board.

**Where:** Status sheet, key = `batVoltage`, value = float in volts (e.g. `3.75`)

**How to use:**
```javascript
// Mothership battery health
const batV = status.batVoltage;
const batPct = Math.max(0, Math.min(100, ((batV - 3.3) / (4.2 - 3.3)) * 100));
// Display: "Mothership battery: 3.75V (50%)"
```

---

## 5. `lastUploadResult` (Status sheet)

**What:** Result of the last cloud upload attempt.

**Where:** Status sheet, key = `lastUploadResult`, value = string: `"success"`, `"failed"`, or `"pending"`

**How to use:**
```javascript
// Upload health indicator
const result = status.lastUploadResult;
// success → green dot "Upload working"
// failed → red dot "Last upload failed"
// pending → grey dot "No upload yet"
```

---

## Removed field

**`uptime`** has been removed from the Status sheet. It was derived from `millis()` which resets every wake cycle, making it meaningless. Use `projectStarted` instead for deployment duration.

---

## API endpoints (unchanged)

All existing GET endpoints work the same:
- `?action=getSystemStatus` — returns all Status sheet key/values including the new fields
- `?action=getNodes` — returns all Nodes sheet rows including lat/lon/deployedSinceUnix
- `?action=getData&nodeId=X&hours=24` — returns readings (unchanged)
- `?action=getAllLatest` — returns latest reading per node (unchanged)

---

## Summary of changes

| Field | Sheet | Type | Status | Dashboard use |
|-------|-------|------|--------|---------------|
| `projectStarted` | Status | Unix timestamp | **New** | "Project started X days ago" |
| `deployedSinceUnix` | Nodes | Unix timestamp | **New** | "Node deployed X days ago" |
| `latitude` | Nodes | float | **New** | Map markers, location display |
| `longitude` | Nodes | float | **New** | Map markers, location display |
| `batVoltage` | Status | float | **New** | Mothership battery gauge |
| `lastUploadResult` | Status | string | **New** | Upload health indicator |
| `uptime` | Status | — | **Removed** | Use `projectStarted` instead |

---

## Nodes sheet column order (updated)

```
nodeId | userId | name | mac | state | lastSeenUnix | wakeIntervalMin | 
inferredWakeIntervalMin | lastReportedBatV | configVersion | notes | 
isActive | deployPending | stateChangePending | pendingTargetState | 
latitude | longitude | deployedSinceUnix
```

The Nodes sheet now has 18 columns (was 15). If you have an existing Nodes sheet, delete it and run `setup()` in the Apps Script editor to recreate it with the correct headers.