# fieldMesh Dashboard — Full-Stack Design Prompt

**Date:** 2026-06-26
**Status:** Design document for dashboard development
**Project:** fieldMesh — ESP32 environmental sensor network

## Project Overview

**fieldMesh** is a battery-powered environmental sensor network for ecological field research. ESP32 sensor nodes measure environmental data (air temperature, humidity, spectral light, soil moisture, soil temperature, wind speed/direction) at remote field sites and transmit it via ESP-NOW to a central "mothership" hub. The mothership logs data locally and uploads it to Google Sheets via an A7670G LTE Cat-1 cellular modem with SSL/TLS encryption.

The dashboard is the user-facing window into the fieldMesh system — visualizing sensor data, showing system configuration, and serving as a showcase piece with educational/explanatory content about the technology.

## System Architecture

```
Sensor Nodes (ESP32 + RTC + sensors)
    ↓ ESP-NOW (WiFi channel 11)
Mothership Hub (ESP32 + LTE modem + flash storage)
    ↓ HTTPS POST (SSL/TLS via A7670G)
Google Apps Script (serverless backend)
    ↓
Google Sheet (data store)
    ↓
fieldMesh Dashboard (web frontend)
```

## Data Source

Data arrives in Google Sheets as CSV rows with these columns:

| Column | Description | Unit | Example |
|---|---|---|---|
| datetime | Timestamp from node RTC | ISO 8601 | 2026-06-26T10:59:46 |
| nodeId | Node identifier | string | ENV_6C0AA0 |
| seqNum | Snapshot sequence number | integer | 1 |
| sensorPresent | Bitmask of active sensors | hex | 0x0137 |
| qualityFlags | Data quality indicators | hex | 0x0000 |
| configVersion | Node config version | integer | 1 |
| batVoltage | Battery voltage | volts | 4.12 |
| airTemp | Air temperature | °C | 28.05 |
| airHumidity | Relative humidity | % | 48.88 |
| spectral_415 | Light at 415nm | raw count | 1.0 |
| spectral_445 | Light at 445nm | raw count | 3.0 |
| spectral_480 | Light at 480nm | raw count | 2.0 |
| spectral_515 | Light at 515nm | raw count | 4.0 |
| spectral_555 | Light at 555nm | raw count | 9.0 |
| spectral_590 | Light at 590nm | raw count | 9.0 |
| spectral_630 | Light at 630nm | raw count | 6.0 |
| spectral_680 | Light at 680nm | raw count | 10.0 |
| windSpeed | Wind speed | m/s | 3.5 |
| windDir | Wind direction | degrees | 180.0 |
| soil1Vwc | Soil 1 volumetric water content | m³/m³ | 0.12 |
| soil1Temp | Soil 1 temperature | °C | 27.78 |
| soil2Vwc | Soil 2 volumetric water content | m³/m³ | 2.64 |
| soil2Temp | Soil 2 temperature | °C | 27.92 |
| aux1 | Auxiliary input 1 | varies | nan |
| aux2 | Auxiliary input 2 | varies | nan |

The system uses a flexible V2 key-value snapshot protocol — new sensor types can be added without changing the schema. The dashboard should handle unknown sensor columns gracefully (display them with auto-generated names like "sensor_5101").

---

## Backend Architecture

### Phase 1: Google Sheets as Backend (Start Here — Zero Cost)

Google Apps Script serves as the API layer. The existing `doPost()` already receives data from the mothership. Add `doGet()` endpoints for the dashboard:

**Apps Script endpoints:**

```
GET  /exec?action=getData&nodeId=ENV_6C0AA0&hours=24
     → Returns JSON array of recent readings for a node

GET  /exec?action=getLatest&nodeId=ENV_6C0AA0
     → Returns JSON object with most recent reading per node

GET  /exec?action=getAllLatest
     → Returns JSON array with latest reading from ALL nodes

GET  /exec?action=getNodes
     → Returns JSON array of all known nodes with metadata (ID, MAC, state, sensors)

GET  /exec?action=getSystemStatus
     → Returns JSON with mothership status (battery, storage, modem, sync schedule, last upload)

GET  /exec?action=getConfig
     → Returns JSON with current system configuration

POST /exec?action=updateConfig
     → Receives config changes (wake interval, sync interval, upload settings)
     → Stores in a "config" sheet for the mothership to pull on next sync

POST /exec?action=uploadData
     → Existing endpoint — receives CSV data from mothership (already working)
```

**Apps Script code structure:**

```javascript
function doGet(e) {
  var action = e.parameter.action;
  switch(action) {
    case 'getData': return getSensorData(e.parameter);
    case 'getLatest': return getLatestReading(e.parameter);
    case 'getAllLatest': return getAllLatestReadings();
    case 'getNodes': return getNodeList();
    case 'getSystemStatus': return getSystemStatus();
    case 'getConfig': return getConfig();
  }
  return ContentService.createTextOutput('Unknown action').setMimeType(ContentService.MimeType.TEXT);
}

function doPost(e) {
  var action = e.parameter.action;
  switch(action) {
    case 'updateConfig': return updateConfig(e.postData.contents);
    case 'uploadData': return appendSensorData(e.postData.contents);
  }
  return ContentService.createTextOutput('Unknown action').setMimeType(ContentService.MimeType.TEXT);
}

function getSensorData(params) {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Data');
  var nodeId = params.nodeId;
  var hours = parseInt(params.hours || '24');
  var cutoff = new Date(Date.now() - hours * 3600 * 1000);
  
  var data = sheet.getDataRange().getValues();
  var headers = data[0];
  var result = [];
  
  for (var i = 1; i < data.length; i++) {
    if (data[i][1] === nodeId) {
      var rowTime = new Date(data[i][0]);
      if (rowTime > cutoff) {
        var row = {};
        for (var j = 0; j < headers.length; j++) {
          row[headers[j]] = data[i][j];
        }
        result.push(row);
      }
    }
  }
  
  return ContentService
    .createTextOutput(JSON.stringify({success: true, data: result, count: result.length}))
    .setMimeType(ContentService.MimeType.JSON);
}

function getAllLatestReadings() {
  var sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName('Data');
  var data = sheet.getDataRange().getValues();
  var headers = data[0];
  var latestByNode = {};
  
  for (var i = 1; i < data.length; i++) {
    var nodeId = data[i][1];
    latestByNode[nodeId] = i;
  }
  
  var result = [];
  for (var nodeId in latestByNode) {
    var rowIdx = latestByNode[nodeId];
    var row = {};
    for (var j = 0; j < headers.length; j++) {
      row[headers[j]] = data[rowIdx][j];
    }
    result.push(row);
  }
  
  return ContentService
    .createTextOutput(JSON.stringify({success: true, data: result}))
    .setMimeType(ContentService.MimeType.JSON);
}
```

**Pros:** Zero hosting cost, no server to maintain, POST endpoint already working
**Cons:** Apps Script quotas (20,000 reads/day), no WebSocket, limited query capability, CORS restrictions

### Phase 2: Node.js Backend (For Showcase — Recommended)

**Stack:**
```
Node.js + Express
googleapis (Google Sheets API client for syncing)
better-sqlite3 (local cache for fast queries)
cors + helmet (security)
express-sse (Server-Sent Events for live updates)
node-cron (periodic sheet sync)
```

**Backend structure:**
```
backend/
├── server.js                # Express server entry point
├── sheets-sync.js           # Polls Google Sheets every 60s, caches to SQLite
├── routes/
│   ├── data.js              # GET /api/data/:nodeId?hours=24&sensor=air_temp
│   ├── latest.js            # GET /api/latest/:nodeId or /api/latest (all nodes)
│   ├── nodes.js             # GET /api/nodes (list with metadata)
│   ├── status.js            # GET /api/status (system status)
│   ├── config.js            # GET/POST /api/config (read/update configuration)
│   ├── export.js            # GET /api/export/:nodeId?format=csv
│   └── events.js            # GET /api/events (SSE stream for live updates)
├── db/
│   └── schema.sql           # SQLite schema matching the CSV columns
├── lib/
│   ├── sensorIds.js         # Sensor ID → column name mapping
│   └── thresholds.js        # Warning/critical thresholds per sensor
└── package.json
```

**Key API endpoints:**
```
GET  /api/data/:nodeId?hours=24&sensor=air_temp    → filtered time-series data
GET  /api/latest                                   → latest reading from all nodes
GET  /api/latest/:nodeId                            → latest reading from one node
GET  /api/nodes                                    → all nodes with metadata + status
GET  /api/status                                   → mothership system status
GET  /api/config                                   → current system configuration
POST /api/config                                   → update configuration (writes to config sheet)
GET  /api/export/:nodeId?format=csv                → download data as CSV
GET  /api/events                                   → SSE stream (pushes new readings as they arrive)
```

**Hosting:** Render.com or Railway.app free tier

---

## Frontend Architecture

### Tech Stack

```
React 18 + Vite           # Fast development, small production bundle
Tailwind CSS              # Responsive utility-first styling
Recharts                  # Time-series charts (composable, React-native)
React Query (TanStack)    # Data fetching, caching, auto-refresh
React Router              # Multi-page navigation
Lucide React              # Clean icon set
Framer Motion             # Smooth animations for showcase polish
date-fns                  # Date formatting and manipulation
```

### Page Structure

```
/                    → Dashboard (live data overview + current values)
/charts              → Time-series charts (selectable sensor, node, time range)
/nodes               → Node management (list, per-node detail, status)
/config              → System configuration (schedule, upload, modem settings)
/about               → fieldMesh architecture + educational showcase content
```

### Component Structure

```
src/
├── components/
│   ├── layout/
│   │   ├── Sidebar.jsx              # Navigation with fieldMesh logo
│   │   ├── Header.jsx               # Page title + connection status pill + refresh
│   │   └── Layout.jsx               # Responsive page wrapper
│   ├── charts/
│   │   ├── TimeSeriesChart.jsx      # Generic line chart (temp, humidity, battery)
│   │   ├── DualAxisChart.jsx        # Temp + humidity on dual Y-axes
│   │   ├── SpectralChart.jsx        # 8-wavelength stacked area or radar
│   │   ├── BatteryGauge.jsx         # Visual battery indicator with color zones
│   │   ├── WindCompass.jsx          # Wind direction compass rose + speed
│   │   └── SoilChart.jsx            # Soil moisture + temperature combined
│   ├── cards/
│   │   ├── SensorCard.jsx           # Current value + trend arrow + status color
│   │   ├── NodeCard.jsx             # Node ID, state, battery, last seen, sensors
│   │   ├── SystemStatusCard.jsx     # Mothership: battery, storage, modem, sync
│   │   └── StatCard.jsx             # Generic stat (node count, data points, uptime)
│   ├── config/
│   │   ├── ScheduleConfig.jsx       # Wake interval + sync interval selectors
│   │   ├── UploadConfig.jsx         # LTE upload enable + endpoint URL
│   │   └── NodeConfig.jsx           # Per-node settings (interval, sensors)
│   ├── educational/
│   │   ├── ArchitectureDiagram.jsx  # Animated data flow: nodes → mesh → cloud
│   │   ├── SensorExplainer.jsx      # Click sensor → what/why/how it works
│   │   ├── TechStack.jsx            # ESP32, RTC, ESP-NOW, LTE, SSL explained
│   │   └── DeploymentGuide.jsx      # Step-by-step field deployment
│   └── common/
│       ├── StatusPill.jsx           # Green/yellow/red status indicator
│       ├── TimeRangeSelector.jsx    # 1h / 6h / 24h / 7d / 30d buttons
│       ├── NodeSelector.jsx         # Dropdown to pick node(s)
│       └── LoadingSpinner.jsx       # Loading state
├── pages/
│   ├── Dashboard.jsx                # Main overview page
│   ├── Charts.jsx                   # Time-series exploration
│   ├── Nodes.jsx                    # Node management
│   ├── Config.jsx                   # System configuration
│   └── About.jsx                    # Educational showcase
├── hooks/
│   ├── useLatestData.js             # React Query hook for latest readings
│   ├── useTimeSeries.js             # React Query hook for chart data
│   ├── useNodes.js                  # React Query hook for node list
│   └── useSystemStatus.js           # React Query hook for system status
├── lib/
│   ├── api.js                       # API client (fetch wrapper)
│   ├── sensorMetadata.js            # Sensor ID → name, unit, description, thresholds
│   └── format.js                    # Date/number formatting utilities
├── App.jsx                          # Router + layout
└── main.jsx                         # Vite entry point
```

### Sensor Metadata (for display + educational content)

```javascript
const sensorMetadata = {
  1001: {
    name: "Air Temperature",
    shortName: "air_temp",
    unit: "°C",
    icon: "thermometer",
    color: "#ef4444",
    description: "Ambient air temperature measured by an SHT41 sensor",
    why: "Temperature drives plant growth, evapotranspiration, frost risk, and insect activity patterns",
    how: "The SHT41 uses a capacitive humidity sensor and a band-gap temperature sensor, calibrated at the factory to ±0.2°C accuracy",
    range: { min: -40, max: 85 },
    thresholds: { warning: 35, critical: 40 }
  },
  1002: {
    name: "Relative Humidity",
    shortName: "air_humidity",
    unit: "%",
    icon: "droplets",
    color: "#3b82f6",
    description: "Amount of water vapor in the air relative to maximum capacity",
    why: "Humidity affects fungal disease risk, plant transpiration, and human comfort in field conditions",
    how: "The SHT41 measures capacitance change in a polymer dielectric layer that absorbs water molecules",
    range: { min: 0, max: 100 },
    thresholds: { warning: 90, critical: 95 }
  },
  1101: {
    name: "Spectral Light (415nm)",
    shortName: "spectral_415",
    unit: "counts",
    icon: "sun",
    color: "#8b5cf6",
    description: "Violet/blue light intensity at 415nm wavelength",
    why: "Blue light drives chlorophyll absorption and phototropism in plants. Different wavelengths reveal plant health and canopy characteristics",
    how: "The AS7341 sensor uses 8 narrow-band photodiodes to measure light at specific wavelengths from 415nm to 680nm",
    range: { min: 0, max: 1000 },
    thresholds: {}
  },
  2001: {
    name: "Soil Moisture (Probe 1)",
    shortName: "soil1_vwc",
    unit: "m³/m³",
    icon: "waves",
    color: "#06b6d4",
    description: "Volumetric water content — volume of water per unit volume of soil",
    why: "Soil moisture drives irrigation scheduling, drought monitoring, and plant health assessment",
    how: "A capacitance probe measures dielectric permittivity of the soil, which correlates to water content. An ADS1115 ADC reads the analog voltage",
    range: { min: 0, max: 0.6 },
    thresholds: { warning: 0.1, critical: 0.05 }
  },
  4001: {
    name: "Battery Voltage",
    shortName: "bat_voltage",
    unit: "V",
    icon: "battery",
    color: "#10b981",
    description: "LiPo battery voltage — indicates remaining capacity and solar charge state",
    why: "Battery monitoring ensures nodes stay operational in the field and alerts when solar charging is insufficient",
    how: "An ESP32 ADC pin reads voltage through a resistor divider from the battery rail. The divider is on the switched VSYS rail so it doesn't drain the battery while the node is off",
    range: { min: 3.2, max: 4.2 },
    thresholds: { warning: 3.6, critical: 3.4 }
  }
};
```

---

## Dashboard Pages — Detailed Requirements

### Page 1: Dashboard (Main Overview)

**Purpose:** At-a-glance view of the entire fieldMesh network

**Layout:**
- Top row: 4 stat cards (Total Nodes, Deployed Nodes, Data Points Today, System Uptime)
- Second row: System status card (mothership battery, storage, modem, next sync)
- Main area: Grid of sensor cards (one per active sensor on selected node)
  - Each card shows: sensor name, current value + unit, trend arrow (up/down/flat), mini sparkline, status color
  - Click card → opens sensor explainer modal
- Node selector dropdown at top of main area
- Auto-refresh every 60 seconds
- Last updated timestamp with "stale" warning if >1 hour old

### Page 2: Charts (Time-Series Exploration)

**Purpose:** Deep-dive into historical data

**Controls:**
- Node selector (single or multi-select for comparison)
- Time range selector (1h / 6h / 24h / 7d / 30d)
- Sensor selector (which sensor to chart)

**Charts:**
- Air temp + humidity: dual-axis line chart
- Spectral: stacked area chart (8 wavelengths) or radar chart
- Soil: moisture + temp on dual axes, per probe
- Battery: line chart with warning/critical threshold lines
- Wind: speed line chart + direction compass (if data available)
- All charts: zoom, pan, tooltip with exact values, export as PNG

### Page 3: Nodes (Node Management)

**Purpose:** View and manage individual nodes

**Node list:**
- Table or card grid showing: node ID, friendly name, state (deployed/paired/unpaired), battery, last seen, sensor count, queue depth
- Sortable by any column
- Filter by state
- Click node → detail view

**Node detail:**
- Full metadata: MAC address, firmware version, config version, wake interval, sync interval
- Sensor inventory: list of active sensors with names
- Recent readings: mini table of last 5 snapshots
- Battery trend: mini chart
- Config controls (Phase 2): change wake interval, force sync, unpair

### Page 4: Config (System Configuration)

**Purpose:** View and modify system-wide settings

**Display:**
- Wake interval (current value + dropdown to change: 1/5/10/20/30/60 min)
- Sync interval (current value + explanation of 18× multiplier)
- Sync mode (interval vs daily)
- Upload enabled/disabled
- Upload endpoint URL
- Upload interval
- Last upload time + result
- Flash storage usage (used / total with progress bar)

**Controls (Phase 2 — read-only initially):**
- Each setting shows current value and a control to change it
- Changes write to the config sheet via POST /api/config
- Confirmation dialog for destructive changes (unpair, reset)
- Battery life impact estimate shown next to interval changes

### Page 5: About (Educational Showcase)

**Purpose:** Explain the fieldMesh system — this is the showcase piece

**Sections:**

1. **What is fieldMesh?**
   - One-paragraph summary
   - Use case: ecological consultants, town planners, environmental researchers monitoring remote sites

2. **System Architecture (animated diagram)**
   - Visual flow: Sensor Nodes → ESP-NOW → Mothership → LTE/SSL → Google Sheets → Dashboard
   - Hover each stage for explanation
   - Animated dots showing data flowing through the pipeline

3. **How It Works (interactive)**
   - "How does a node wake up?" → DS3231 RTC alarm → hardware power latch → ESP32 boot → sensor read → ESP-NOW transmit → power off
   - "How does data reach the cloud?" → Mothership receives → logs to flash → powers on LTE modem → SSL/TLS handshake → HTTPS POST → Google Apps Script → Google Sheet
   - "What makes it low-power?" → Full power cut (not deep sleep) between measurements, µA-level RTC-only current, weeks of battery life

4. **Sensor Library (clickable cards)**
   - Each sensor type: what it measures, why it matters, how it works, typical range
   - Use the sensorMetadata object above

5. **Technology Stack**
   - ESP32 microcontrollers (explain: dual-core, WiFi+BT, ultra-low-power)
   - DS3231 RTC (explain: precision timing, alarm-driven wake, µA current)
   - ESP-NOW protocol (explain: connectionless WiFi, no router needed, low latency)
   - A7670G LTE Cat-1 modem (explain: cellular data, SSL/TLS, 10Mbps down/5Mbps up)
   - V2 key-value snapshot protocol (explain: flexible sensor registration, future-proof)
   - Google Apps Script (explain: serverless, free, direct-to-Sheets)

6. **Field Deployment Guide**
   - Step 1: Install node at site (mount, connect sensors, insert battery)
   - Step 2: Power on mothership, enter config mode
   - Step 3: Discover nodes from web UI
   - Step 4: Pair and deploy nodes (set wake interval, sync schedule)
   - Step 5: Configure LTE upload (endpoint URL, enable transmission)
   - Step 6: Shut down mothership — it wakes automatically on RTC alarm
   - Step 7: Monitor from dashboard

7. **Key Design Decisions**
   - Why hardware power gating instead of deep sleep?
   - Why queue-first architecture (data never lost during connectivity gaps)?
   - Why key-value snapshots (add sensors without protocol changes)?
   - Why LTE instead of LoRa/Sigfox (higher bandwidth, SSL/TLS, standard SIM)?
   - Why Google Sheets (zero cost, collaborative, accessible from anywhere)?

---

## Design Preferences

- **Name:** fieldMesh
- **Logo concept:** Interconnected nodes forming a mesh pattern over a landscape/field outline
- **Color palette:** Earth tones — deep green (#1a5d3a), warm amber (#d4a017), sky blue (#4a90d9), slate (#37474f), cream background (#f8f6f0)
- **Typography:** Inter or system font for UI, monospace for technical/data display
- **Responsive:** Must work on phone (field use), tablet, and desktop (office use)
- **Theme:** Clean, professional, outdoor/field research aesthetic
- **Dark mode:** Optional but desirable for field use (bright screens attract insects)
- **Accessibility:** WCAG AA compliant, keyboard navigation, screen reader support
- **Performance:** First load < 3 seconds, charts render < 1 second, auto-refresh every 60s

## Deployment

- **Frontend:** Vercel or Netlify (free tier, auto-deploy from Git)
- **Backend Phase 1:** Google Apps Script (zero cost)
- **Backend Phase 2:** Render.com or Railway.app (free tier)
- **Data store:** Google Sheets (Phase 1) → SQLite cache (Phase 2) → optionally Supabase (Phase 3)