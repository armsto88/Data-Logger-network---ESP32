# fieldMesh Dashboard — Interactive "About" Section Design

**Date:** 2026-06-26
**Status:** Design document (planning only — no code changes)
**Project:** fieldMesh — ESP32 environmental sensor network
**Companion to:** `docs/FIELDMESH_DASHBOARD_DESIGN.md`

---

## 1. Overview

### 1.1 Purpose

The "About" section is the educational and showcase layer of the fieldMesh dashboard. While the rest of the dashboard shows *live data*, the About section explains *how the system works* — from a 30-second plain-English summary for an ecologist, down to a hex-dump packet decoder for a firmware developer.

It serves three goals:

1. **Onboard non-technical users** (ecologists, town planners, site valuers) so they trust the data they see elsewhere in the dashboard.
2. **Satisfy curious technical users** who want to understand the architecture without reading firmware source.
3. **Showcase the engineering** for developers, collaborators, and funding reviewers who want to see the depth of the system.

### 1.2 Audience

| Audience | Technical level | What they want | Layer |
|---|---|---|---|
| Ecologists / field researchers | Non-technical | "What does this system measure and why should I trust it?" | Layer 1 |
| Town planners / site valuers | Non-technical | "Is this real data? Where does it come from?" | Layer 1 |
| Technical curious (IT-literate) | Intermediate | "How do the pieces fit together?" | Layer 2 |
| Developers / collaborators | Advanced | "Show me the protocol, the firmware modules, the packet bytes." | Layer 3 |

### 1.3 Design philosophy

- **Progressive disclosure** — start with a single animated diagram and one sentence. Reveal complexity only when the user asks for it.
- **Visual-first** — every concept leads with a diagram, animation, or interactive widget. Text supports the visual, never replaces it.
- **Generic housing** — the system has no final enclosure yet. All hardware visuals use stylised board outlines (rectangles, component footprints, callout lines) — never 3D enclosure renders. *[Confirmed decision]*
- **Mobile-friendly** — collapsible sections, touch-draggable sliders, responsive SVG that scales. The ecologist in the field will open this on a phone.
- **No heavy external dependencies** — the About page is a static, self-contained page. All animations are CSS/SVG/JS. No Three.js, no WebGL, no external diagram libraries. *[Confirmed decision]*
- **Honest about state** — the About section shows the real system as it exists today, including open questions and known limitations. It does not over-promise.

---

## 2. Information Architecture

### 2.1 Top-level structure

The About page is a single scrollable page with a sticky layer-switcher at the top. The user never navigates away — they change depth.

```
┌─────────────────────────────────────────────────────┐
│  fieldMesh — About                          [≡]     │
│                                                     │
│  ┌─────────┐ ┌─────────┐ ┌──────────┐ ┌──────────┐ │
│  │ Layer 1 │ │ Layer 2 │ │ Layer 3  │ │ Simulator│ │
│  │ What is │ │ How does│ │ Show me  │ │ Mini-System│ │
│  │ this?   │ │ it work?│ │ everything│ │ Simulator │ │
│  └─────────┘ └─────────┘ └──────────┘ └──────────┘ │
│                                                     │
│  [Active layer content scrolls here]                │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### 2.2 The three layers + simulator

| Section | Tab label | Primary audience | Default open? |
|---|---|---|---|
| Layer 1 | "What is this?" | Ecologists, non-technical | ✅ Yes (default) |
| Layer 2 | "How does it work?" | Technical / curious | No |
| Layer 3 | "Show me everything" | Tech heads / developers | No |
| Mini-Simulator | "Try it yourself" | All levels | No (accessible from all layers) |

### 2.3 How they connect

```
Layer 1 (animated overview)
    │
    │ "Want more detail?" →
    ▼
Layer 2 (architecture + timeline + simulator controls)
    │
    │ "Show me the raw protocol" →
    ▼
Layer 3 (PCB viewer + hex decoder + firmware graph)
    ▲
    │ "Run a scenario" ← (available from all layers)
    │
Mini-System Simulator (shared interactive component)
```

The Mini-System Simulator is a **shared component** — it appears as an embedded widget in Layer 1 (simplified, auto-running) and as a full-control panel in Layer 2 and Layer 3. It is not a separate page; it is the same React component rendered at different detail levels. *[Confirmed decision]*

### 2.4 Navigation model

- **Sticky tab bar** at top — click to switch layers. Remains visible during scroll.
- **Within each layer** — content flows top-to-bottom in collapsible cards.
- **Cross-layer links** — each layer ends with a "Go deeper" button that switches to the next layer and scrolls to the relevant sub-section.
- **Simulator launch** — a floating "▶ Run Sync Cycle" button is always visible in the bottom-right corner on all layers. Clicking it opens the simulator in a modal or scrolls to the embedded instance.

---

## 3. Layer 1 — "What is this?"

### 3.1 Audience

Ecologists, town planners, site valuers — people who need to trust the data but do not care about ESP32 pin assignments.

### 3.2 Visual layout

```
┌─────────────────────────────────────────────────────┐
│  fieldMesh in 30 seconds                            │
│                                                     │
│  [Animated system diagram — full width]             │
│                                                     │
│  Sensors → Node → Mothership → Cloud → Dashboard    │
│                                                     │
│  "Environmental sensors in the field measure        │
│   temperature, moisture, and light. A central hub   │
│   collects the data and sends it to the cloud       │
│   over cellular. You see it here."                  │
│                                                     │
│  [▶ Run a sync cycle]  [Tell me more →]             │
│                                                     │
├─────────────────────────────────────────────────────┤
│  What does it measure?                              │
│                                                     │
│  [Sensor icon grid — 6 cards]                       │
│  🌡 Air Temp   💧 Humidity   ☀ Light                │
│  🌱 Soil Moist. 🌡 Soil Temp  🌀 Wind               │
│                                                     │
│  (Click any card → plain-English explainer popover) │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Why trust it?                                      │
│                                                     │
│  • Each reading has a timestamp from an atomic      │
│    clock on the node.                               │
│  • Data is stored locally before upload — nothing   │
│    is lost if the cell signal drops.                │
│  • Battery and signal health are tracked per node.  │
│                                                     │
├─────────────────────────────────────────────────────┤
│  [▶ Run a sync cycle — animated demo]               │
│  [How does it work? →  (goes to Layer 2)]           │
└─────────────────────────────────────────────────────┘
```

### 3.3 Animated system diagram

**Visual:** A horizontal SVG scene with five stages, left to right:

| Stage | Visual | Label |
|---|---|---|
| 1 | Stylised sensor icons (thermometer, droplet, sun) attached to a small board outline | "Sensors" |
| 2 | Small rectangular node board outline with antenna zigzag | "Node" |
| 3 | Larger rectangular mothership board outline with antenna + small modem rectangle | "Mothership" |
| 4 | Cloud shape with sheet icon inside | "Cloud (Google Sheets)" |
| 5 | Browser window outline with mini chart | "Dashboard" |

**Animation sequence (auto-plays on page load, loops every 15s):**

1. **Sensor reading** (0–3s): Sensor icons pulse. A small data dot appears at each sensor.
2. **Node capture** (3–5s): Data dots flow into the node board. Node label flashes "Reading complete".
3. **ESP-NOW transfer** (5–8s): A radio-wave arc animates from node to mothership. Data dots travel along the arc. Mothership label flashes "Received".
4. **LTE upload** (8–11s): A cellular-tower icon appears between mothership and cloud. Data dots travel up to the cloud. Cloud label flashes "Stored".
5. **Dashboard update** (11–14s): Data dots flow from cloud to browser window. Mini chart updates with a new point.
6. **Rest** (14–15s): All elements settle. Loop.

**Interaction:**
- Click any stage → popover with a 2-sentence plain-English explanation (see Section 9 for content).
- Hover any stage → stage highlights and shows a one-line tooltip.
- "Run a sync cycle" button → replays the animation from step 1 with a slightly slower pace and stage labels visible.

**Technical approach:**
- Pure SVG + CSS animations (`@keyframes` for pulse, `stroke-dashoffset` for data-dot travel along paths).
- JS only for the "Run sync cycle" button (restart animation by toggling a CSS class) and popover positioning.
- SVG `viewBox` scales responsively. On mobile, stages stack vertically with vertical data-dot travel.

### 3.4 Sensor icon grid

Six cards, one per sensor category. Each card shows:
- Icon (Lucide: `thermometer`, `droplets`, `sun`, `waves`, `thermometer`, `wind`)
- Sensor name (plain English)
- One-line description

Click → popover with:
- **What it measures** (1 sentence)
- **Why it matters** (1 sentence, ecology/planning context)
- **How the sensor works** (1 sentence, non-technical)

Content sourced from the `sensorMetadata` object in the dashboard design doc. *[Confirmed: reuse existing sensorMetadata]*

### 3.5 "Why trust it?" card

Three bullet points, no interactive elements. Plain English. See Section 9 for exact text.

### 3.6 Layer 1 → Layer 2 transition

Bottom of Layer 1:
- "How does it work?" button → switches to Layer 2, scrolls to architecture diagram.
- "▶ Run a sync cycle" floating button → opens simulator in simplified mode (auto-run, no controls).

---

## 4. Layer 2 — "How does it work?"

### 4.1 Audience

Technical-curious users — IT-literate ecologists, data people, anyone who wants the architecture without firmware source.

### 4.2 Visual layout

```
┌─────────────────────────────────────────────────────┐
│  How fieldMesh works                                │
│                                                     │
│  [Interactive architecture diagram — full width]    │
│   (click any component to see details)              │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Component detail cards (appear on click)           │
│                                                     │
│  [ESP32]  [DS3231 RTC]  [A7670G Modem]              │
│  [Sensors]  [LittleFS]  [NVS Queue]                 │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Sync protocol timeline (Gantt-style)               │
│                                                     │
│  [Gantt chart showing wake → capture → sync →       │
│   upload cycle across multiple nodes]               │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Sync simulator                                     │
│                                                     │
│  [Sliders: node interval, sync interval, node count]│
│  [Battery level indicator]                          │
│  [▶ Run simulation — animated timeline]             │
│                                                     │
├─────────────────────────────────────────────────────┤
│  [Show me everything →  (goes to Layer 3)]          │
└─────────────────────────────────────────────────────┘
```

### 4.3 Interactive architecture diagram

**Visual:** A larger, more detailed version of the Layer 1 diagram. Same five-stage horizontal flow but each stage is a clickable card showing internal components.

```
┌──────────┐    ┌──────────────────┐    ┌─────────────────────────┐    ┌──────────┐    ┌───────────┐
│  Sensors │    │      Node        │    │      Mothership         │    │  Cloud   │    │ Dashboard │
│          │    │                  │    │                         │    │          │    │           │
│ SHT41    │───▶│ ESP32            │───▶│ ESP32                   │───▶│ Google   │───▶│ Web UI    │
│ AS734x   │    │ DS3231 RTC       │    │ DS3231 RTC              │    │ Sheets   │    │           │
│ ADS1115  │    │ NVS Queue        │    │ LittleFS / SD           │    │ Apps     │    │           │
│ VH-5     │    │ LiPo + Solar     │    │ A7670G LTE Modem        │    │ Script   │    │           │
│          │    │                  │    │ LiPo + Solar             │    │          │    │           │
└──────────┘    └──────────────────┘    └─────────────────────────┘    └──────────┘    └───────────┘
                 ESP-NOW (ch 11)              HTTPS + SSL/TLS
```

**Interaction:**
- Click any component (ESP32, DS3231, A7670G, Sensors, LittleFS, NVS Queue) → detail card slides in from the right (or expands below on mobile).
- Active component highlights in the diagram.
- Connection lines between stages are labelled with the protocol (ESP-NOW ch 11, HTTPS/SSL).

### 4.4 Component detail cards

Each card has: name, role, key specs, "what it does in fieldMesh", and a fun fact.

#### ESP32
- **Role:** Main processor on both node and mothership
- **Specs:** Dual-core 240MHz, 520KB SRAM, 4MB flash, WiFi/BLE
- **In fieldMesh:** Runs the sensor reading loop, manages the RTC alarm wake cycle, handles ESP-NOW radio, and (on mothership) drives the LTE modem
- **Fun fact:** The ESP32 costs about $4 and uses less power than a dim LED when asleep

#### DS3231 RTC
- **Role:** Real-time clock with alarm
- **Specs:** ±2ppm accuracy, coin-cell backup, I2C, alarm interrupt pin
- **In fieldMesh:** Wakes the node on a schedule (every 1–60 min). Keeps time even if the main battery dies. The mothership syncs its time to nodes during deployment
- **Fun fact:** The RTC drifts less than 1 minute per year. It has a built-in crystal oscillator that is temperature-compensated

#### A7670G LTE Modem
- **Role:** Cellular uplink for the mothership
- **Specs:** LTE Cat-1 (10Mbps down / 5Mbps up), SSL/TLS, AT command interface, UART
- **In fieldMesh:** Sends accumulated data to Google Sheets via HTTPS POST. Only the mothership has one — nodes are radio-only
- **Fun fact:** The modem draws ~1.8A peak during transmission. The mothership battery and boost circuit are sized for this

#### Sensors
- **Role:** Environmental measurement
- **Specs:** SHT41 (temp ±0.2°C, RH ±1.8%), AS734x (8-channel spectral 415–680nm), ADS1115 (16-bit ADC for soil probes), VH-5 (capacitive soil moisture)
- **In fieldMesh:** Read once per wake cycle. Each reading is tagged with a sensor ID and packed into a key-value snapshot
- **Fun fact:** The spectral sensor can see light across 8 wavelengths — the same bands that plants use for photosynthesis

#### LittleFS / SD
- **Role:** Local data storage on the mothership
- **Specs:** LittleFS (wear-levelled flash filesystem) or SD card, CSV format
- **In fieldMesh:** Every received snapshot is logged to a CSV file before upload. If the cell signal drops, data accumulates locally and uploads when connectivity returns
- **Fun fact:** The CSV file uses a dynamic column system — new sensor types automatically get new columns without reformatting the file

#### NVS Queue
- **Role:** Non-volatile storage queue on the node
- **Specs:** ESP32 NVS partition, circular byte slab (~3500 bytes), A/B slot redundancy
- **In fieldMesh:** Stores snapshots between wake cycles if the node can't reach the mothership. Survives power loss. Each record is acknowledged cumulatively — no duplicates on retry
- **Fun fact:** The queue uses two alternating slots (A and B) so a power cut during a write never corrupts the last good copy

### 4.5 Sync protocol timeline (Gantt-style)

**Visual:** A horizontal Gantt chart showing a typical 30-minute window with 3 nodes and 1 mothership.

```
Time →  00:00        00:05        00:10        00:15        00:20        00:25        00:30
        │            │            │            │            │            │            │
Node 1  ├─Wake─┤     │            ├─Wake─┤     │            ├─Wake─┤     │            │
        │ Cap  │     │            │ Cap  │     │            │ Cap  │     │            │
        │ Sync │     │            │ Sync │     │            │ Sync │     │            │
        │ Sleep├─────┤            │ Sleep├─────┤            │ Sleep├─────┤            │
        │            │            │            │            │            │            │
Node 2  │            ├─Wake─┤     │            ├─Wake─┤     │            ├─Wake─┤     │
        │            │ Cap  │     │            │ Cap  │     │            │ Cap  │     │
        │            │ Sync │     │            │ Sync │     │            │ Sync │     │
        │            │ Sleep├─────┤            │ Sleep├─────┤            │ Sleep├─────┤
        │            │            │            │            │            │            │
Node 3  ├─────Wake─────┤     │    ├─────Wake─────┤     │    ├─────Wake─────┤     │
        │     Cap      │     │    │     Cap      │     │    │     Cap      │     │
        │     Sync     │     │    │     Sync     │     │    │     Sync     │     │
        │     Sleep────┤     │    │     Sleep────┤     │    │     Sleep────┤     │
        │            │            │            │            │            │            │
Mothership │  Listen   │  Listen   │  Listen   │  Listen   │  Listen   │  Upload   │
        │            │            │            │            │            │  (LTE)    │
```

**Interactive controls:**
- Slider: node wake interval (1, 5, 10, 20, 30, 60 min)
- Slider: number of nodes (1–8)
- Slider: sync interval (how often the mothership uploads via LTE)
- Toggle: "Show upload window" — highlights the LTE upload Gantt bar

**Animation:** Click "Play timeline" → a vertical playhead sweeps left-to-right. As it crosses each wake bar, the corresponding node card in the diagram pulses. When it hits the upload window, the mothership→cloud connection animates.

**Technical approach:**
- SVG or CSS-grid Gantt. Bars are absolutely-positioned divs with percentage widths based on interval/time.
- Playhead is a CSS-animated vertical line (`transform: translateX`).
- JS computes bar positions from slider values.

### 4.6 Sync simulator (Layer 2 mode)

See Section 6 for full spec. In Layer 2, the simulator shows:
- Node count, interval, and sync interval sliders
- Battery level per node (visual gauge)
- Queue depth per node (bar indicator)
- "Run Sync Cycle" button → animates one full cycle
- "Auto-run" toggle → continuous simulation

### 4.7 Layer 2 → Layer 3 transition

Bottom of Layer 2:
- "Show me everything" button → switches to Layer 3, scrolls to PCB viewer.

---

## 5. Layer 3 — "Show me everything"

### 5.1 Audience

Developers, firmware collaborators, technical reviewers. People who want to see the wire protocol and the firmware module graph.

### 5.2 Visual layout

```
┌─────────────────────────────────────────────────────┐
│  The full technical picture                         │
│                                                     │
│  [Tab bar: PCB Viewer | Protocol | Firmware | Hex]  │
│                                                     │
├─────────────────────────────────────────────────────┤
│  PCB Viewer                                         │
│  [Stylised board outline with component callouts]   │
│  [Click component → callout with pin/role info]     │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Protocol Deep Dive                                 │
│  [V2 snapshot structure diagram]                    │
│  [Field-by-field breakdown table]                   │
│  [ESP-NOW sync window timeline]                     │
│  [RTC alarm schedule visualiser]                    │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Firmware Architecture                              │
│  [Module dependency graph — interactive]            │
│  [Click module → role, dependencies, files]         │
│                                                     │
├─────────────────────────────────────────────────────┤
│  Hex Dump Decoder                                   │
│  [Paste hex bytes → decoded fields]                 │
│  [Live packet decoder with sample packets]          │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### 5.3 Interactive PCB viewer

**Visual:** A stylised 2D board outline (top view) for the node and mothership. Not a 3D render — a clean, labelled schematic-style outline. *[Confirmed: generic board outlines only, no enclosure renders]*

**Node board outline:**
```
┌─────────────────────────────────────────┐
│                                         │
│   ┌──────────┐        ┌──────────┐     │
│   │ ESP32    │        │ DS3231   │     │
│   │ WROOM    │        │ RTC      │     │
│   └──────────┘        └──────────┘     │
│        │                    │           │
│   ┌──────────┐   ┌──────────┐          │
│   │ PCA9548A │   │ ADS1115  │          │
│   │ I2C Mux  │   │ ADC      │          │
│   └──────────┘   └──────────┘          │
│                                         │
│   ┌──────────┐   ┌──────────┐          │
│   │ SHT41    │   │ AS734x   │          │
│   │ Temp/RH  │   │ Spectral │          │
│   └──────────┘   └──────────┘          │
│                                         │
│   ┌──────────┐        ┌──────────┐     │
│   │ LiPo     │        │ Solar    │     │
│   │ Conn     │        │ Conn     │     │
│   └──────────┘        └──────────┘     │
│                                         │
│   ○ ○ ○ ○  (soil probe connectors)     │
│                                         │
└─────────────────────────────────────────┘
```

**Mothership board outline:**
```
┌─────────────────────────────────────────┐
│                                         │
│   ┌──────────┐        ┌──────────┐     │
│   │ ESP32    │        │ DS3231   │     │
│   │ WROOM    │        │ RTC      │     │
│   └──────────┘        └──────────┘     │
│                                         │
│   ┌──────────┐   ┌──────────┐          │
│   │ A7670G   │   │ MT3608   │          │
│   │ LTE Modem│   │ Boost    │          │
│   └──────────┘   └──────────┘          │
│                                         │
│   ┌──────────┐        ┌──────────┐     │
│   │ SD Card  │        │ LiPo     │     │
│   │ Slot     │        │ Conn     │     │
│   └──────────┘        └──────────┘     │
│                                         │
│   ┌──────────┐        ┌──────────┐     │
│   │ Solar    │        │ Status   │     │
│   │ Conn     │        │ LEDs     │     │
│   └──────────┘        └──────────┘     │
│                                         │
└─────────────────────────────────────────┘
```

**Interaction:**
- Toggle between "Node board" and "Mothership board".
- Click any component → callout panel appears with:
  - Component name and package
  - I2C address (where applicable: DS3231=0x68, PCA9548A=0x71, ADS1115=0x48, SHT41=0x44)
  - Pin assignments (from `FIRMWARE_AND_HARDWARE_NOTES.md`: SDA=18, SCL=19, PWR_HOLD=IO23, battery sense=IO35, etc.)
  - Role in the system
- Hover → highlight component and show connection lines to related components (e.g., clicking ESP32 highlights all I2C bus connections).

**Data source:** Pin assignments from `docs/FIRMWARE_AND_HARDWARE_NOTES.md` Section 1.8 run logs and `node/docs/NODE-PCB-OVERVIEW.md`. *[Open question: confirm pin map is current — see Section 11]*

**Technical approach:**
- SVG with `<rect>` components and `<line>` callout connectors.
- Each component is a `<g>` group with `data-component` attribute.
- JS handles click → shows callout panel (HTML overlay positioned relative to SVG element).

### 5.4 Protocol deep dive

#### V2 snapshot packet structure

**Visual:** A byte-layout diagram showing the 48-byte header and variable-length body.

```
Offset  0       16      32      40      46  47  48       54      60
        │       │       │       │       │   │   │        │       │
        ├───────┴───────┴───────┴───────┴───┴───┴────────┴───────┤
        │ command     │ nodeId      │ nodeTs │seq│cnt│qf│cv│pv│r│ Reading 1   │ Reading 2  │ ...
        │ [16 bytes]  │ [16 bytes]  │ [4]   │[4]│[2]│[2]│[2]│[1]│[1]│ [6 bytes]  │ [6 bytes]  │
        │ "NODE_SNAP  │ "ENV_6C0AA0 │       │   │   │   │   │   │  │ sensorId   │ sensorId  │
        │  SHOT2\0.." │ \0.."       │       │   │   │   │   │   │  │ + float    │ + float   │
        └─────────────┴─────────────┴───────┴───┴───┴───┴───┴───┴───┴────────────┴───────────┘

Header (48 bytes):
  command[16]       "NODE_SNAPSHOT2"     identifies V2 protocol
  nodeId[16]        "ENV_6C0AA0"         node identifier string
  nodeTimestamp     uint32               unix seconds from node RTC
  seqNum            uint32               snapshot sequence number
  sensorCount       uint16               number of KV readings following
  qualityFlags      uint16               bitfield (QF_DROPPED, etc.)
  configVersion     uint16               node config version
  protocolVersion   uint8                = 2
  reserved          uint8                = 0

Body (sensorCount × 6 bytes, packed):
  sensorId          uint16               SENSOR_ID_* constant
  value             float (4 bytes)      sensor reading

Max: 48 + (33 × 6) = 246 bytes (ESP-NOW 250-byte limit)
Typical: 48 + (17 × 6) = 150 bytes
```

**Interaction:**
- Hover any field in the diagram → highlights and shows description tooltip.
- Click a field → detail panel with:
  - Field name, type, size, offset
  - Description
  - Example value
  - Valid range / constraints

**Field breakdown table:**

| Offset | Field | Type | Bytes | Description |
|---|---|---|---|---|
| 0 | command | char[16] | 16 | "NODE_SNAPSHOT2" — identifies V2 protocol |
| 16 | nodeId | char[16] | 16 | Node identifier, e.g. "ENV_6C0AA0" |
| 32 | nodeTimestamp | uint32 | 4 | Unix timestamp from node DS3231 RTC |
| 36 | seqNum | uint32 | 4 | Monotonic snapshot sequence number |
| 40 | sensorCount | uint16 | 2 | Number of key-value readings in body |
| 42 | qualityFlags | uint16 | 2 | Bitfield: QF_DROPPED, sensor status, RTC confidence |
| 44 | configVersion | uint16 | 2 | Node config version (for desired-state sync) |
| 46 | protocolVersion | uint8 | 1 | Always 2 for V2 |
| 47 | reserved | uint8 | 1 | Unused, set to 0 |
| 48+ | readings | Reading[sensorCount] | 6 each | `{uint16 sensorId, float value}` packed |

**Sensor ID reference table (subset):**

| Sensor ID | Name | Unit |
|---|---|---|
| 1001 | Air Temperature | °C |
| 1002 | Relative Humidity | % |
| 1101–1108 | Spectral 415–680nm | counts |
| 1301 | PAR | µmol/m²/s |
| 2001 | Soil 1 VWC | m³/m³ |
| 2002 | Soil 2 VWC | m³/m³ |
| 2101 | Soil 1 Temp | °C |
| 2102 | Soil 2 Temp | °C |
| 4001 | Battery Voltage | V |
| 5001+ | Reserved for future port sensors | — |

#### ESP-NOW sync window timeline

**Visual:** A zoomed-in timeline showing a single sync window — the few seconds when a node wakes and exchanges data with the mothership.

```
Node wake
│
├── 0ms:    DS3231 alarm fires → PWR_HOLD asserted (IO23 high)
├── 50ms:   ESP32 boots, loads state from NVS
├── 200ms:  Sensors initialised (I2C mux, SHT41, AS734x, ADS1115)
├── 500ms:  Sensor read complete → snapshot built
├── 550ms:  Snapshot enqueued to NVS (local durability)
├── 600ms:  ESP-NOW send → mothership
├── 650ms:  Mothership receives → SNAPSHOT_ACK
├── 700ms:  Node receives ACK → pops queue, advances ack pointer
├── 750ms:  Node checks for config updates (desired-state pull)
├── 800ms:  Node rearms DS3231 alarm for next interval
├── 850ms:  Node releases PWR_HOLD → power off
│
Total awake time: ~850ms
```

**Interaction:** Click any step → detail popover explaining what happens and why.

#### RTC alarm schedule visualiser

**Visual:** A 24-hour clock face with alarm markers at the configured interval.

```
        12
    11      1
  10          2
  9            3   ← alarm at :03, :08, :13, :18...
  8            4      (5-minute interval)
  7          5
    6      4
        5
```

**Controls:**
- Slider: wake interval (1, 5, 10, 20, 30, 60 min)
- Display: number of wake events per 24h, estimated battery life

### 5.5 Firmware architecture — module dependency graph

**Visual:** An interactive directed graph showing firmware modules and their dependencies.

```
                    ┌──────────┐
                    │  main.cpp│
                    └────┬─────┘
                         │
          ┌──────────────┼──────────────┐
          │              │              │
          ▼              ▼              ▼
    ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ sensors/ │  │ storage/ │  │ espnow/  │
    │ sensors  │  │ local_   │  │ espnow_  │
    │ .cpp     │  │ queue    │  │ manager  │
    └────┬─────┘  └────┬─────┘  └────┬─────┘
         │              │              │
         ▼              ▼              ▼
    ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ I2C mux  │  │ NVS      │  │ WiFi     │
    │ PCA9548A │  │ partition│  │ ESP-NOW  │
    └────┬─────┘  └──────────┘  └──────────┘
         │
    ┌────┴────────┬──────────┐
    │             │          │
    ▼             ▼          ▼
┌────────┐  ┌────────┐  ┌────────┐
│ SHT41  │  │ AS734x │  │ ADS1115│
└────────┘  └────────┘  └────────┘
```

**Interaction:**
- Click any module → detail panel with:
  - Module name and file path
  - Role (1 sentence)
  - Dependencies (which modules it calls)
  - Dependents (which modules call it)
  - Key functions / entry points
- Hover → highlight connected edges.

**Mothership firmware graph (separate toggle):**

```
                    ┌──────────┐
                    │  main.cpp│
                    └────┬─────┘
                         │
       ┌────────────┬────┴────┬────────────┐
       │            │         │            │
       ▼            ▼         ▼            ▼
  ┌──────────┐┌──────────┐┌──────────┐┌──────────┐
  │ espnow/  ││ flash_   ││ upload_  ││ config_  │
  │ manager  ││ logger   ││ queue    ││ portal   │
  └────┬─────┘└────┬─────┘└────┬─────┘└────┬─────┘
       │           │           │            │
       ▼           ▼           ▼            ▼
  ┌──────────┐┌──────────┐┌──────────┐┌──────────┐
  │ WiFi     ││ LittleFS ││ A7670G   ││ WebServer│
  │ ESP-NOW  ││ / SD     ││ Modem    ││ (AP)     │
  └──────────┘└──────────┘└──────────┘└──────────┘
```

*[Open question: confirm module names match current firmware tree — see Section 11]*

### 5.6 Hex dump decoder

**Visual:** A two-panel tool — left is a hex input, right is a decoded field table.

```
┌─────────────────────────┬────────────────────────────┐
│  Hex input              │  Decoded fields            │
│                         │                            │
│  4E 4F 44 45 5F 53 4E   │  command:      NODE_SNAP   │
│  41 50 53 48 4F 54 32   │                SHOT2       │
│  00 00 45 4E 56 5F 36   │  nodeId:       ENV_6C0AA0  │
│  43 30 41 41 30 00 00   │  nodeTimestamp: 2026-06-26 │
│  ...                    │                10:59:46    │
│                         │  seqNum:       1           │
│  [Load sample packet]   │  sensorCount:  17          │
│  [Decode]               │  qualityFlags: 0x0000      │
│                         │  configVersion: 1          │
│                         │  protocolVersion: 2        │
│                         │                            │
│                         │  Readings:                 │
│                         │   [1001] Air Temp: 28.05°C │
│                         │   [1002] Humidity: 48.88%  │
│                         │   [1101] Spectral 415: 1.0 │
│                         │   ...                      │
└─────────────────────────┴────────────────────────────┘
```

**Features:**
- **Load sample packet** button — fills the hex input with a real captured V2 packet (150 bytes).
- **Decode** button — parses the hex and populates the decoded fields table.
- **Manual entry** — user can paste their own hex bytes.
- **Byte highlighting** — hovering a decoded field highlights the corresponding bytes in the hex input.
- **V1/V2 auto-detect** — the decoder checks the command string. "NODE_SNAPSHOT" → V1 (124-byte fixed struct). "NODE_SNAPSHOT2" → V2 (variable length).
- **Error handling** — if the hex is too short, truncated, or has an invalid command, show a clear error message.

**Sample packets (embedded in the page):**
1. Typical V2 snapshot (17 readings, 150 bytes)
2. Minimal V2 snapshot (1 reading, 54 bytes)
3. V1 snapshot (124 bytes, for comparison)
4. Corrupted packet (truncated, for error demonstration)

**Technical approach:**
- Pure JS `DataView` for parsing.
- No external hex editor library — custom hex display using a `<pre>` with monospace font.
- Byte highlighting via `<span>` wrapping each byte pair.

---

## 6. Mini-System Simulator — Full Spec

### 6.1 Purpose

A miniature working model of the real fieldMesh system. The user can configure a fleet of nodes, press "Run Sync Cycle", and watch the entire data flow animate — from sensor reading through to dashboard update. It shows battery drain, queue buildup, and sync timing in real time.

### 6.2 Placement

- **Layer 1:** Simplified version — auto-running, no controls. Just the animation with a "Run Sync Cycle" button.
- **Layer 2:** Full controls — all sliders and toggles visible.
- **Layer 3:** Same as Layer 2 but with an additional "Show packet bytes" toggle that reveals the hex encoding of each snapshot as it travels.
- **Floating button:** Always accessible from any layer. Opens the simulator in a modal overlay.

### 6.3 Visual layout (full mode)

```
┌─────────────────────────────────────────────────────┐
│  Mini-System Simulator                              │
│                                                     │
│  ┌─ Controls ──────────────────────────────────┐   │
│  │                                              │   │
│  │  Nodes:        [━━●━━] 3                     │   │
│  │  Wake interval: [━━●━━] 5 min                │   │
│  │  Sync interval: [━━●━━━━] 30 min             │   │
│  │  Upload:       [ON]                          │   │
│  │  Sim speed:    [━━●━━] 1x                    │   │
│  │                                              │   │
│  │  [▶ Run Sync Cycle]  [Auto-run]  [Reset]    │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  ┌─ Node fleet ────────────────────────────────┐   │
│  │                                              │   │
│  │  Node 1   🔋 92%  📶 OK   Queue: 0   ●Deployed│   │
│  │  Node 2   🔋 87%  📶 OK   Queue: 2   ●Deployed│   │
│  │  Node 3   🔋 78%  📶 Weak  Queue: 5   ●Deployed│   │
│  │                                              │   │
│  │  [+ Add node]  [− Remove node]              │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  ┌─ Animation stage ───────────────────────────┐   │
│  │                                              │   │
│  │  [Sensors] → [Node] → [Mothership] → [Cloud]│   │
│  │                                              │   │
│  │  (animated data dots, same as Layer 1 but    │   │
│  │   driven by simulator state)                 │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  ┌─ Mothership status ─────────────────────────┐   │
│  │                                              │   │
│  │  🔋 85%   📶 LTE OK   Storage: 1,240 rows   │   │
│  │  Next upload: 12 min   Last upload: 18 min   │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  ┌─ Event log ─────────────────────────────────┐   │
│  │  10:59:46  Node 1 woke, captured 17 readings│   │
│  │  10:59:47  Node 1 → Mothership (ESP-NOW)    │   │
│  │  10:59:47  Mothership ACK → Node 1          │   │
│  │  11:00:02  Node 2 woke, captured 17 readings│   │
│  │  11:00:03  Node 2 → Mothership (ESP-NOW)    │   │
│  │  11:00:03  Mothership ACK → Node 2          │   │
│  │  11:15:00  Mothership → Cloud (LTE upload)  │   │
│  │  11:15:05  Cloud confirmed 34 new rows      │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### 6.4 Controls

| Control | Type | Range | Default | Effect |
|---|---|---|---|---|
| Nodes | Slider | 1–8 | 3 | Number of simulated nodes |
| Wake interval | Slider | 1, 5, 10, 20, 30, 60 min | 5 | How often each node wakes and reads sensors |
| Sync interval | Slider | 5, 10, 15, 30, 60 min | 30 | How often the mothership uploads to cloud |
| Upload toggle | Switch | On/Off | On | Whether LTE upload is enabled |
| Sim speed | Slider | 0.5x, 1x, 2x, 5x | 1x | Animation speed multiplier |
| Run Sync Cycle | Button | — | — | Runs one full wake→capture→sync→upload cycle |
| Auto-run | Toggle | On/Off | Off | Continuously runs cycles at sim speed |
| Reset | Button | — | — | Resets all state to defaults |

### 6.5 Simulated state

The simulator maintains an in-memory model:

```javascript
simState = {
  nodes: [
    {
      id: 1,
      battery: 92,        // percent, drains per wake cycle
      signal: 'OK',       // OK | Weak | Lost (randomised, affects queue)
      queueDepth: 0,      // unsynced snapshots
      deployed: true,
      lastWake: null,     // sim timestamp
    },
    // ...
  ],
  mothership: {
    battery: 85,
    lteSignal: 'OK',
    storageRows: 0,
    lastUpload: null,
    nextUpload: 30,       // minutes until next upload
  },
  cloud: {
    rowsReceived: 0,
  },
  simTime: 0,             // minutes elapsed in simulation
  eventLog: [],
}
```

### 6.6 Simulation logic

**Per wake cycle (per node):**
1. Node wakes (battery drains ~0.3% per wake).
2. Captures 17 readings (typical sensor set).
3. Snapshot enqueued to NVS (queueDepth++).
4. ESP-NOW send attempt:
   - Signal OK → mothership receives, ACK sent, queueDepth-- (cumulative ack).
   - Signal Weak → 85% chance of success. On failure, queueDepth stays.
   - Signal Lost → queueDepth stays. Snapshot remains in NVS for next cycle.
5. Node sleeps.

**Per sync interval (mothership):**
1. If upload is ON and LTE signal is OK:
   - Upload all accumulated rows to cloud.
   - Cloud confirms row count.
   - Event log entry.
2. If upload is OFF or LTE is down:
   - Rows accumulate in mothership storage.
   - Event log: "Upload skipped — LTE offline".

**Battery model:**
- Each node wake: -0.3% battery.
- Each ESP-NOW send: -0.1% battery.
- Solar recharge: +0.05% per sim minute (daytime assumed).
- Mothership LTE upload: -1.0% battery.
- When battery < 20%: node shows warning. Below 10%: node stops waking.

**Queue buildup scenario:**
- If a node's signal is "Lost" for multiple cycles, queueDepth grows.
- When signal returns, all queued snapshots sync in a burst (queue drains rapidly).
- This demonstrates the NVS queue's durability guarantee.

### 6.7 Animation

The animation stage reuses the Layer 1 SVG diagram but is driven by simulator events rather than a fixed loop:
- When a node wakes → its sensor icons pulse, data dots flow to node.
- When ESP-NOW send succeeds → radio arc animates node→mothership.
- When upload fires → cellular tower animates, dots flow mothership→cloud→dashboard.
- When a send fails → red X appears on the radio arc, data dot returns to node queue.

### 6.8 Event log

A scrolling text log at the bottom of the simulator. Each line is timestamped (sim time). Colour-coded:
- Green: successful send/ack
- Blue: upload event
- Yellow: queue buildup / signal weak
- Red: send failure / battery low

### 6.9 "Show packet bytes" toggle (Layer 3 only)

When enabled, each data dot that travels from node to mothership shows a tooltip with the hex encoding of the snapshot it represents. Clicking the dot opens the hex decoder (Section 5.6) pre-filled with that packet's bytes.

---

## 7. Visual Style

### 7.1 Colour palette

Inherited from the main dashboard design (`docs/FIELDMESH_DASHBOARD_DESIGN.md`). The About section uses the same tokens for consistency.

| Token | Hex | Use |
|---|---|---|
| `--fm-bg` | `#0f172a` | Page background (dark mode) |
| `--fm-surface` | `#1e293b` | Card backgrounds |
| `--fm-border` | `#334155` | Borders, dividers |
| `--fm-text` | `#f1f5f9` | Primary text |
| `--fm-text-muted` | `#94a3b8` | Secondary text |
| `--fm-accent` | `#3b82f6` | Primary accent (links, active states) |
| `--fm-green` | `#10b981` | Success, healthy, online |
| `--fm-yellow` | `#f59e0b` | Warning, stale, weak signal |
| `--fm-red` | `#ef4444` | Error, critical, offline |
| `--fm-purple` | `#8b5cf6` | Spectral / special highlight |

*[Confirmed: reuse dashboard palette. Open question: does the dashboard support light mode? If so, mirror it here — see Section 11]*

### 7.2 Sensor colours

Reused from `sensorMetadata` in the dashboard design doc:
- Air temp: `#ef4444` (red)
- Humidity: `#3b82f6` (blue)
- Spectral: `#8b5cf6` (purple)
- Soil moisture: `#06b6d4` (cyan)
- Soil temp: `#f97316` (orange)
- Wind: `#14b8a6` (teal)
- Battery: `#10b981` (green)

### 7.3 Icons

Lucide React icon set (already in the dashboard stack). Key icons:
- `thermometer` — temperature
- `droplets` — humidity/moisture
- `sun` — light/PAR
- `waves` — soil moisture
- `wind` — wind
- `battery` / `battery-low` — battery
- `wifi` — ESP-NOW / radio
- `radio` — LTE modem
- `cloud` — cloud upload
- `cpu` — ESP32
- `clock` — RTC
- `database` — storage
- `activity` — sync cycle

### 7.4 Animation approach

- **CSS `@keyframes`** for all looping animations (pulses, glows, data-dot travel).
- **CSS `transition`** for state changes (card expand, tab switch, slider feedback).
- **`requestAnimationFrame`** for the simulator playhead and continuous animations.
- **SVG `stroke-dashoffset`** animation for data dots travelling along paths.
- **No Framer Motion required** for the About section — the animations are simple enough for pure CSS. *[Confirmed decision: keep the About section dependency-light, even though the main dashboard uses Framer Motion]*
- **`prefers-reduced-motion`** — all animations respect this media query. If set, animations are replaced with static states and a "Step through" button. *[Confirmed decision]*

### 7.5 Typography

- Headings: same as dashboard (system font stack, bold weights).
- Body: 16px base, 1.6 line-height for readability.
- Monospace: for hex dumps, packet fields, and code-like content. `font-family: 'SF Mono', 'Fira Code', 'Consolas', monospace`.
- Layer labels: small caps, muted colour.

### 7.6 Responsive behaviour

| Breakpoint | Behaviour |
|---|---|
| Desktop (>1024px) | Full layout. Architecture diagram horizontal. Simulator controls side-by-side with animation. |
| Tablet (640–1024px) | Architecture diagram horizontal but smaller. Simulator controls stack above animation. |
| Mobile (<640px) | Architecture diagram stacks vertically (sensors → node → mothership → cloud → dashboard, top to bottom). All cards full-width. Simulator controls in a collapsible panel. Hex decoder panels stack. PCB viewer scales to fit, callouts appear as bottom-sheet modals. |

**Touch interactions:**
- Sliders: native `<input type="range">` (touch-friendly).
- Component click: on mobile, tapping a component opens a bottom-sheet detail panel instead of a side panel.
- Layer switching: tabs become a horizontal scroll strip on mobile.

---

## 8. Technical Implementation Notes

### 8.1 Framework alignment

The About page is part of the existing React dashboard (`src/pages/About.jsx`). It uses the same stack:
- React 18 + Vite
- Tailwind CSS
- Lucide React (icons)

However, the About page intentionally avoids heavy dependencies:
- **No Framer Motion** — CSS animations only. *[Confirmed]*
- **No Recharts** — the About section's charts (Gantt, timeline) are custom SVG/CSS.
- **No external diagram libraries** (no Mermaid, no react-flow) — diagrams are hand-built SVG. *[Confirmed]*

### 8.2 Component structure

```
src/pages/About.jsx                    # Page shell, layer switcher, routing

src/components/about/
├── LayerSwitcher.jsx                  # Sticky tab bar
├── Layer1Overview.jsx                 # "What is this?"
│   ├── AnimatedSystemDiagram.jsx      # SVG + CSS animation, 5-stage flow
│   ├── SensorIconGrid.jsx             # 6 sensor cards with popovers
│   └── TrustCard.jsx                  # "Why trust it?" static card
├── Layer2Architecture.jsx             # "How does it work?"
│   ├── ArchitectureDiagram.jsx        # Clickable component diagram
│   ├── ComponentDetailCard.jsx        # Slide-in detail panel
│   ├── SyncTimeline.jsx               # Gantt-style timeline
│   └── SimulatorPanel.jsx             # Embedded simulator (full controls)
├── Layer3DeepDive.jsx                 # "Show me everything"
│   ├── PcbViewer.jsx                  # SVG board outline + callouts
│   ├── ProtocolDeepDive.jsx           # V2 packet structure diagram
│   ├── FirmwareGraph.jsx              # Module dependency graph
│   └── HexDumpDecoder.jsx             # Hex input + decoded output
├── MiniSystemSimulator.jsx            # Shared simulator component
│   ├── SimulatorControls.jsx          # Sliders, toggles, buttons
│   ├── SimulatorStage.jsx             # Animated SVG driven by sim state
│   ├── NodeFleetPanel.jsx             # Per-node status rows
│   ├── MothershipStatus.jsx           # Mothership status card
│   └── EventLog.jsx                   # Scrolling event log
└── data/
    ├── sensorMetadata.js              # Reused from dashboard (import)
    ├── protocolSpec.js                # V2 field definitions, sensor IDs
    ├── pcbPinMap.js                   # Component → pin/I2C address data
    ├── firmwareModules.js             # Module graph nodes and edges
    └── samplePackets.js               # Pre-built hex sample packets
```

### 8.3 Data sources

| Data | Source | Static or dynamic? |
|---|---|---|
| Sensor metadata | `src/lib/sensorMetadata.js` (existing) | Static (import) |
| V2 protocol spec | `docs/V2_SNAPSHOT_MIGRATION_PLAN.md` → hardcoded in `protocolSpec.js` | Static |
| PCB pin map | `docs/FIRMWARE_AND_HARDWARE_NOTES.md` → hardcoded in `pcbPinMap.js` | Static |
| Firmware module graph | Firmware source tree → hardcoded in `firmwareModules.js` | Static |
| Sample hex packets | Constructed from V2 spec → hardcoded in `samplePackets.js` | Static |
| Simulator state | In-memory React state (`useReducer`) | Dynamic (client-side only) |

**No API calls** — the About section is entirely static. It does not fetch live data from the backend. This keeps it fast, offline-capable, and deployable as a static page. *[Confirmed decision]*

### 8.4 Simulator state management

The simulator uses a `useReducer` with a single state object (see Section 6.5). The reducer handles actions:

| Action | Payload | Effect |
|---|---|---|
| `TICK` | `{ minutesElapsed }` | Advance sim time, trigger due wake cycles and uploads |
| `RUN_CYCLE` | `{ nodeId }` | Manually trigger one node's wake cycle |
| `RUN_UPLOAD` | — | Manually trigger mothership upload |
| `SET_NODES` | `{ count }` | Add/remove nodes |
| `SET_INTERVAL` | `{ wakeMinutes }` | Update wake interval |
| `SET_SYNC_INTERVAL` | `{ syncMinutes }` | Update upload interval |
| `TOGGLE_UPLOAD` | — | Enable/disable LTE upload |
| `SET_SPEED` | `{ multiplier }` | Set animation speed |
| `RESET` | — | Reset to defaults |

A `useEffect` with `setInterval` drives `TICK` actions when auto-run is enabled.

### 8.5 Hex decoder implementation

Pure JS, no dependencies:

```javascript
// Pseudocode — not implementation code
function decodePacket(hexBytes) {
  const view = new DataView(hexBytes.buffer);
  const command = readString(view, 0, 16);

  if (command.startsWith('NODE_SNAPSHOT2')) {
    return decodeV2(view);
  } else if (command.startsWith('NODE_SNAPSHOT')) {
    return decodeV1(view);
  } else {
    return { error: 'Unknown command string' };
  }
}

function decodeV2(view) {
  return {
    command: readString(view, 0, 16),
    nodeId: readString(view, 16, 16),
    nodeTimestamp: view.getUint32(32, true),  // little-endian
    seqNum: view.getUint32(36, true),
    sensorCount: view.getUint16(40, true),
    qualityFlags: view.getUint16(42, true),
    configVersion: view.getUint16(44, true),
    protocolVersion: view.getUint8(46),
    reserved: view.getUint8(47),
    readings: readReadings(view, 48, sensorCount),
  };
}
```

### 8.6 Performance

- All SVG diagrams are lightweight (<50 elements each).
- Animations use CSS transforms and opacity (GPU-accelerated).
- The simulator's `setInterval` runs at 1–5 ticks/second depending on speed — negligible CPU.
- No network requests, no large JSON payloads.
- Total page weight target: <100KB (excluding React/Tailwind, which are already loaded).

### 8.7 Accessibility

- All interactive elements are keyboard-navigable (`tabindex`, `aria-label`).
- Layer switcher uses `role="tablist"` / `role="tab"` / `role="tabpanel"`.
- Diagrams have `aria-label` descriptions.
- Colour is never the sole indicator — icons and text labels accompany all status colours.
- `prefers-reduced-motion` disables all animations (see Section 7.4).

---

## 9. Content Outline — Plain English Text

### 9.1 Layer 1 content

#### Hero text
> **fieldMesh in 30 seconds**
>
> Environmental sensors in the field measure temperature, moisture, light, and wind. A central hub collects the data wirelessly and sends it to the cloud over a cellular connection. You see the results here on this dashboard.

#### Stage popovers (animated diagram)

**Sensors:**
> Small sensors mounted on poles or in the soil measure the environment — air temperature, humidity, light levels, soil moisture, and wind. Each sensor is read every few minutes.

**Node:**
> Each sensor cluster is connected to a small battery-powered computer (a "node"). It wakes up on a schedule, reads the sensors, and sends the data by radio to the central hub. Then it goes back to sleep to save battery.

**Mothership:**
> The mothership is the central hub. It stays on, listens for data from all the nodes, and stores everything on its own memory card. Once it has collected enough data, it sends it to the cloud.

**Cloud:**
> Data is uploaded to a Google Sheet over a secure cellular connection. This is the permanent record — safe even if something happens to the field equipment.

**Dashboard:**
> This is where you are now. The dashboard pulls data from the cloud and shows you charts, current values, and the health of every node in the field.

#### Sensor card content (6 cards)

**Air Temperature**
> *What:* How warm or cold the air is, in degrees Celsius.
> *Why:* Temperature drives plant growth, frost risk, and insect activity.
> *How:* A digital sensor (SHT41) measures temperature to within 0.2°C.

**Humidity**
> *What:* How much water vapour is in the air, as a percentage.
> *Why:* High humidity can mean fungal disease risk. It affects how plants transpire.
> *How:* The same SHT41 sensor measures a change in capacitance caused by moisture.

**Light (Spectral)**
> *What:* Light intensity at 8 different colours (wavelengths), from violet to red.
> *Why:* Plants use specific colours of light for photosynthesis. Measuring them tells us about canopy health.
> *How:* An AS7341 sensor has 8 light filters, each tuned to a specific wavelength.

**Soil Moisture**
> *What:* How much water is in the soil, as a fraction of volume.
> *Why:* Soil moisture drives irrigation decisions and drought monitoring.
> *How:* A capacitance probe in the soil measures how well the soil conducts electricity — more water means higher capacitance.

**Soil Temperature**
> *What:* How warm or cold the soil is, in degrees Celsius.
> *Why:* Soil temperature affects seed germination, root growth, and microbial activity.
> *How:* A temperature sensor built into the soil probe.

**Wind**
> *What:* Wind speed and direction.
> *Why:* Wind affects evaporation, pollination, and spray drift. It also indicates exposure.
> *How:* An ultrasonic anemometer measures wind by timing sound pulses between transducers.

#### "Why trust it?" card
> **Why you can trust this data**
>
> - **Every reading is timestamped.** Each node has its own atomic clock (RTC) that keeps time even when the battery is flat. You always know exactly when a measurement was taken.
> - **Nothing is lost.** Data is saved on the node before it's sent. If the radio link drops, the data waits and retries. If the cell tower is down, the mothership holds everything until it comes back.
> - **Health is monitored.** Battery voltage, signal strength, and last-seen times are tracked for every node. You can see at a glance if something needs attention.

### 9.2 Layer 2 content

#### Architecture diagram intro
> **How the pieces fit together**
>
> fieldMesh has four main parts: sensor nodes, a mothership hub, a cloud spreadsheet, and this dashboard. Click any component in the diagram to learn what it does.

#### Sync timeline intro
> **The sync cycle**
>
> Nodes wake up on their own schedule, read sensors, and radio the data to the mothership. The mothership listens continuously and uploads to the cloud on its own schedule. Adjust the sliders to see how timing changes the flow.

#### Simulator intro
> **Try it yourself**
>
> This is a miniature version of the real system. Add nodes, change the wake interval, toggle the cell upload, and press "Run Sync Cycle" to watch the data flow. Notice how the queue builds up when signal is weak, and how it drains when the connection returns.

### 9.3 Layer 3 content

#### PCB viewer intro
> **The hardware**
>
> These are stylised board layouts for the node and mothership — not to scale, but showing where the key components sit. Click any component for its I2C address, pin assignments, and role.

#### Protocol deep dive intro
> **The V2 snapshot protocol**
>
> Every time a node wakes, it builds a "snapshot" — a compact packet of sensor readings. The V2 format uses a 48-byte header followed by 6-byte key-value pairs. This design lets new sensor types be added without changing the format.

#### Firmware graph intro
> **Firmware architecture**
>
> The firmware is organised into modules with clear boundaries. Sensors, storage, and radio are separate layers. Click any module to see its role, dependencies, and the files it lives in.

#### Hex decoder intro
> **Packet decoder**
>
> Paste hex bytes from a captured ESP-NOW packet and see the decoded fields. Try the sample packets first — one is a typical 17-sensor snapshot, one is a minimal single-sensor packet, and one is a V1 packet for comparison.

---

## 10. Interaction Map

### 10.1 Layer 1 interactions

| Element | Trigger | Action |
|---|---|---|
| Animated diagram stage | Hover | Stage highlights, tooltip shows one-line label |
| Animated diagram stage | Click | Popover with 2-sentence plain-English explanation |
| "Run a sync cycle" button | Click | Animation restarts from step 1, slower pace, stage labels visible |
| Sensor icon card | Click | Popover with what/why/how text |
| "How does it work?" button | Click | Switch to Layer 2, scroll to architecture diagram |
| Floating "▶" button | Click | Open Mini-System Simulator in simplified auto-run mode |

### 10.2 Layer 2 interactions

| Element | Trigger | Action |
|---|---|---|
| Architecture diagram component | Click | Detail card slides in from right (bottom-sheet on mobile) |
| Architecture diagram component | Hover | Component highlights, connection lines emphasised |
| Sync timeline slider (wake interval) | Change | Gantt bars reposition, wake count per 24h updates |
| Sync timeline slider (node count) | Change | Gantt rows add/remove |
| Sync timeline slider (sync interval) | Change | Upload Gantt bar repositions |
| "Play timeline" button | Click | Playhead sweeps left-to-right, nodes pulse as playhead crosses wake bars |
| Simulator "Run Sync Cycle" button | Click | One full cycle animates: wake → capture → sync → (upload if due) |
| Simulator "Auto-run" toggle | Toggle | Continuous simulation at selected speed |
| Simulator sliders | Change | Sim state updates, animation reflects new config |
| Simulator node row | Click | Highlights that node in the animation stage |
| "Show me everything" button | Click | Switch to Layer 3, scroll to PCB viewer |

### 10.3 Layer 3 interactions

| Element | Trigger | Action |
|---|---|---|
| PCB viewer board toggle | Click | Switch between node board and mothership board |
| PCB component | Click | Callout panel with pin map, I2C address, role |
| PCB component | Hover | Component highlights, related connections emphasised |
| Protocol field in byte diagram | Hover | Field highlights, tooltip with description |
| Protocol field in byte diagram | Click | Detail panel with type, offset, example, constraints |
| Firmware graph module | Click | Detail panel with role, dependencies, dependents, files |
| Firmware graph module | Hover | Connected edges highlight |
| Firmware graph board toggle | Click | Switch between node firmware and mothership firmware graph |
| Hex decoder "Load sample packet" | Click | Fills hex input with selected sample |
| Hex decoder "Decode" button | Click | Parses hex, populates decoded fields table |
| Hex decoder hex byte | Hover | Highlights corresponding decoded field |
| Hex decoder decoded field | Hover | Highlights corresponding hex bytes |
| Hex decoder sample selector | Change | Loads different sample packet |

### 10.4 Mini-System Simulator interactions

| Element | Trigger | Action |
|---|---|---|
| Nodes slider | Change | Add/remove nodes from fleet |
| Wake interval slider | Change | Update node wake frequency |
| Sync interval slider | Change | Update mothership upload frequency |
| Upload toggle | Toggle | Enable/disable LTE upload in sim |
| Sim speed slider | Change | Adjust animation speed |
| "Run Sync Cycle" button | Click | Execute one full cycle for all due nodes |
| "Auto-run" toggle | Toggle | Start/stop continuous simulation |
| "Reset" button | Click | Reset all state to defaults |
| "+ Add node" / "− Remove node" | Click | Adjust fleet size by one |
| Node fleet row | Click | Highlight node in animation stage |
| "Show packet bytes" toggle (Layer 3) | Toggle | Data dots show hex tooltip on hover |
| Data dot (with bytes toggle on) | Click | Open hex decoder pre-filled with that packet's bytes |
| Event log row | Hover | Highlight related animation element |

---

## 11. Open Questions and Decisions Needed

### 11.1 Needs user decision

1. **Light mode support** — The dashboard design doc specifies a dark colour palette. Does the dashboard (and therefore the About section) need a light mode toggle? If yes, the About section's colour tokens need light-mode variants. *Status: open.*

2. **Simulator realism vs simplicity** — The current simulator model is simplified (e.g., battery drain is a fixed percentage per cycle, signal quality is randomised). Should it use more realistic models (e.g., solar irradiance by time of day, actual ESP-NOW range data from the 1m/30m/100m test results in `FIRMWARE_AND_HARDWARE_NOTES.md`)? *Status: open — recommend starting simple and adding realism if users request it.*

3. **PCB pin map accuracy** — The pin assignments in the PCB viewer are sourced from bring-up logs in `FIRMWARE_AND_HARDWARE_NOTES.md` (SDA=18, SCL=19, PWR_HOLD=IO23, battery=IO35, etc.). Are these current for the V3 node board? The `node/docs/NODE_V3_OVERVIEW.md` may have updates. *Status: open — needs verification against V3 hardware.*

4. **Firmware module names** — The module dependency graph uses inferred module names (sensors/, storage/, espnow/, flash_logger, upload_queue, config_portal). Should these match the exact file/folder names in the current firmware tree? *Status: open — needs a pass over `node/firmware/src/` and `mothership/firmware/` to confirm.*

5. **Config portal mini-versions** — The task mentions showing config portal pages (status, sync settings, LTE upload) as mini-versions. Where should these appear? Options: (a) as a sub-section in Layer 2, (b) as a separate tab in Layer 3, (c) embedded in the simulator as "what the operator sees." *Status: open — recommend option (c), embedded in the simulator as a mock-up of the mothership's AP portal.*

6. **CSV data format as interactive table** — The task mentions showing the CSV format as an interactive table. Where should this appear? Options: (a) in Layer 2 as part of the architecture diagram (cloud stage detail), (b) in Layer 3 alongside the protocol deep dive. *Status: open — recommend Layer 3, as a bridge between the protocol spec and the hex decoder.*

### 11.2 Confirmed decisions (summary)

| Decision | Status |
|---|---|
| Generic board outlines only, no 3D enclosure renders | ✅ Confirmed |
| No heavy external dependencies (no Three.js, Mermaid, react-flow) | ✅ Confirmed |
| No Framer Motion in About section (CSS animations only) | ✅ Confirmed |
| Mini-System Simulator is a shared component across all layers | ✅ Confirmed |
| About section is entirely static — no API calls | ✅ Confirmed |
| `prefers-reduced-motion` disables all animations | ✅ Confirmed |
| Reuse existing `sensorMetadata` from dashboard | ✅ Confirmed |
| Reuse dashboard colour palette | ✅ Confirmed |
| Progressive disclosure — Layer 1 is default | ✅ Confirmed |

### 11.3 Implementation risks

1. **SVG diagram complexity on mobile** — The architecture diagram has many components and connection lines. On a 360px-wide screen, it may become cramped. Mitigation: stack vertically on mobile, use bottom-sheet modals for detail panels.

2. **Simulator state grows with node count** — At 8 nodes with auto-run, the event log and animation could become busy. Mitigation: cap event log at 50 lines (scrolling), limit visible animation to the most recent event per node.

3. **Hex decoder edge cases** — V1 packets are 124 bytes fixed. V2 packets are variable. Corrupted or truncated input could crash the parser. Mitigation: wrap all parsing in try/catch, validate `sensorCount` against available bytes before reading body.

4. **Firmware graph staleness** — If the firmware tree changes, the hardcoded module graph in `firmwareModules.js` will be out of date. Mitigation: add a comment in the data file pointing to the source files it was derived from, and note the last-updated date.

---

## 12. References

| Source | Path | Used for |
|---|---|---|
| Dashboard design | `docs/FIELDMESH_DASHBOARD_DESIGN.md` | Page structure, sensor metadata, colour palette, component conventions |
| V2 snapshot protocol | `docs/V2_SNAPSHOT_MIGRATION_PLAN.md` | Packet structure, NVS queue spec, sensor IDs |
| Firmware & hardware notes | `docs/FIRMWARE_AND_HARDWARE_NOTES.md` | Pin assignments, I2C addresses, ESP-NOW range data, bring-up details |
| Concept overview | `docs/concept_overview.md` | Node lifecycle, config portal pages, CSV format, sensor stack |
| Node comm workflow | `docs/SONNET_HANDOFF_NODE_COMM_WORKFLOW.md` | Sync protocol, desired-state pull model, wake cycle timing |
| Node local storage contract | `node/docs/NODE-LOCAL-STORAGE-CONTRACT-V1.md` | NVS queue structure, A/B redundancy, cumulative ACK model |
| Node V3 overview | `node/docs/NODE_V3_OVERVIEW.md` | V3 hardware changes (pin map verification — open) |
| Mothership power & wake | `mothership/docs/MOTHERSHIP_POWER_AND_WAKE_DESIGN_NOTE.md` | Mothership battery, wake architecture (reference) |
| LTE backhaul concept | `mothership/docs/MOTHERSHIP_LTE_BACKHAUL_CONCEPT.md` | A7670G modem details, upload flow (reference) |