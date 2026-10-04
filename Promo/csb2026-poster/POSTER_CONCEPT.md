# Poster concept 01 — the system that connects the mosaic

## Recommendation

Build the poster around one visual argument:

> Solar farms contain many microhabitats. Comparable evidence therefore needs many repeatable monitoring points, connected by a field system that remains useful when power and connectivity are constrained.

This keeps the review paper as the scientific reason for the project while giving most of the page to the working system. It also avoids the feel of either a literature-review poster or a product advertisement.

## Two-minute reading path

1. **Why:** the review found a microclimate mosaic and inconsistent monitoring methods.
2. **Design brief:** distributed, repeatable, local-first and manageable.
3. **How:** nodes measure at many locations; one hub coordinates, stores and synchronises.
4. **Scientific honesty:** the platform is working, but sensor calibration and longer field validation remain essential.
5. **Use:** one deployment can support replicated research and management monitoring.

## A0 portrait wireframe

Use a 12-column grid with generous outer margins. The architecture and hypothetical deployment are the two dominant visuals.

```text
┌──────────────────────────────────────────────────────────────┐
│ MONITORING THE MOSAIC                          authors / logo │
│ One-sentence research-to-system thesis                      │
├───────────────────┬──────────────────────────────────────────┤
│ 1. THE GAP        │ 2. FIELDMESH ARCHITECTURE                │
│ Review finding    │ nodes → hub → local stores → cloud/UI    │
│ + design brief    │ OFF/ON/OFF and health-data callouts      │
├───────────────────┴──────────────────────────────────────────┤
│ 3. ONE SITE, MANY MICROHABITATS                              │
│ Full-width solar-farm deployment illustration               │
│ research replicates + management treatments → shared hub    │
├───────────────────┬───────────────────┬──────────────────────┤
│ 4. SENSORS +      │ 5. REPEATABILITY │ 6. STATUS            │
│ VALIDATION        │ within/across     │ working / next       │
├───────────────────┴───────────────────┴──────────────────────┤
│ If solar farms are mosaics, our monitoring must see them. QR│
└──────────────────────────────────────────────────────────────┘
```

Suggested vertical allocation:

- Header: 13–15%
- Gap + architecture: 25–28%
- Hypothetical deployment: 29–32%
- Validation + repeatability + status: 20–23%
- Footer: 7–9%

## Draft display copy

### Header

# Monitoring the mosaic

## FieldMesh for fine-scale environmental monitoring in solar farms

**Standfirst:** A review of 33 publications found that solar facilities create spatially heterogeneous microclimates, but comparisons are constrained by small samples, inconsistent sensor placement and different study designs. FieldMesh is a working prototype developed to make distributed, repeatable monitoring more practical.

Keep the standfirst to roughly 45 words in the final artwork.

### 1. The monitoring gap

**A single weather station cannot describe a mosaic.**

Solar-farm studies vary in sensor type, height, position, interval, spatial design and reporting. At the same time, dense field monitoring is constrained by cost, power, connectivity and maintenance.

Turn those constraints into four large design requirements rather than a paragraph:

**Distributed · Repeatable · Local-first · Manageable**

### 2. FieldMesh architecture

**Many measurement points. One coordinating field hub.**

The illustration should communicate three layers:

1. **Sensor nodes** — scheduled power-on, measure, queue, transmit, then complete power-off.
2. **FieldHub** — coordinates nodes, accepts readings into local storage, exposes the local field interface and manages backhaul.
3. **Browser system** — remote trends, node comparison, status and CSV export after synchronisation.

Use two small callout strips:

**OFF ━━━━━ ON: measure + queue/transmit ━ OFF ━━━━━ ON**

**Poor backhaul can delay access without stopping local collection.**

For final copy, avoid “data can never be lost.” The current implementation accepts a snapshot after LittleFS or SD storage succeeds; a non-deleting microSD archive is used when the card is available, with internal flash as the working store/fallback.

### 3. One site, many microhabitats

Make this the emotional centre of the poster: a clean oblique or plan-view solar farm with eight to twelve node markers.

Use two marker families:

- **Research replicates:** under-panel, row gap, panel edge and external reference.
- **Management monitoring:** grazing, mowing, restoration and vegetation treatments.

All markers lead to the same hub, data structure and health workflow.

**One monitoring framework. Two complementary uses.**

### 4. Accessible sensors, visible uncertainty

Show the current channels as five compact icons:

- air temperature and relative humidity
- visible spectral channels
- soil moisture signal and soil temperature at two depths
- wind speed pathway
- battery, reporting and sensor-health metadata

Then show the scientific pathway:

**Consumer-grade sensor → calibration → reference comparison → drift/uncertainty → interpretable data**

Important precision for the final poster:

- soil moisture is currently an uncalibrated probe voltage, not validated volumetric water content;
- the spectral head reports raw channels and is not yet calibrated as PAR/PPFD;
- reed-cup wind speed has standalone bring-up evidence, but integrated end-to-end status should be rechecked before submission;
- wind direction remains unimplemented/planned.

Suggested headline: **Low-cost sensors still need high standards.**

### 5. Repeatability, not one prescribed design

Use a simple two-level graphic:

**Within a site**

repeat sensors · intervals · placement rules · health metadata

↓

**Across sites**

repeat the chosen framework · preserve data structure · compare context

Copy:

> The aim is not one universal study design. It is to make a chosen design easier to reproduce and its limitations easier to see.

### 6. Current status

Use three visually distinct states rather than one undifferentiated feature list.

**Implemented in the current architecture**

- scheduled hard-power-off node cycle
- node-to-hub communication and queued transfer
- local browser-based setup
- local flash storage and SD archive pathway
- authenticated cloud ingest
- browser dashboard, comparisons and CSV export

**Evidence available**

- single- and three-node commissioning
- queueing and shared sync-window transfer
- power-cycle persistence
- cloud-path and remote-configuration commissioning records

**Still needed**

- calibration and reference comparisons
- drift and uncertainty assessment
- longer multi-week field deployments
- integrated wind verification and wind direction
- enclosure and production-hardware refinement

Headline: **The architecture is working. Establishing what its data can reliably support is the next step.**

### Footer

# If solar farms are mosaics, our monitoring systems need to see the mosaic.

Include the review citation, contact, affiliation and one QR code. The QR destination should contain project detail, not simply duplicate the poster.

## Visual system

Use the dashboard's established design language so the poster and screenshots feel like one project:

| Role | Colour | Use |
|---|---:|---|
| FieldMesh green | `#166534` | titles, node/hub system path, positive/implemented |
| Data blue | `#1D5FA7` | cloud, dashboard and information flow |
| Solar ochre | `#9A6700` | PV infrastructure and caution/validation |
| Warm off-white | `#F8F9FA` | page background |
| Charcoal | `#181C20` | body text |
| Muted grey | `#7C8794` | secondary labels and planned items |

Use purple only for research replicate markers and orange/ochre for management markers. Do not use a rainbow palette across the whole poster.

Suggested final-size type ranges:

- title: 90–110 pt
- subtitle: 42–54 pt
- section headings: 34–42 pt
- body: 24–28 pt
- captions/references: 18–21 pt

Check legibility by printing an A4 reduction and by viewing the PDF at 25% zoom.

## Alternative story angles

Keep these as backups, not simultaneous themes.

### A. Reliability-first

Lead with **Bad signal should delay data, not destroy it.** Strong for engineering audiences, but it underplays the review paper and the conference's biodiversity focus.

### B. Validation-first

Lead with **More measurement points, with uncertainty made visible.** Scientifically strong, but only use this once calibration/reference results can occupy a real results panel.

### C. Deployment-first

Lead with the hypothetical solar-farm map. This is visually immediate and may become the best final direction if strong field/hardware photos are unavailable.

## Asset priorities

1. Current node and FieldHub photographs on a neutral background.
2. A current dashboard screenshot with real or clearly labelled demonstration data.
3. A current local FieldHub setup screenshot.
4. The solar-farm deployment illustration.
5. The system architecture diagram.
6. A calibration/reference photo if available.

The repository currently contains older UI images and an archived system figure, but no clearly current poster-ready hardware/dashboard image set. Do not use the older images without checking them against the live interfaces.

## Decisions needed before artwork

- final affiliation and co-author list
- QR-code destination
- poster/abstract ID for submission metadata, even though it is not printed on the poster
- whether SD archival has completed on-device validation on the current FieldHub hardware
- latest integrated wind status
- which dashboard project can be safely shown
- whether management treatments should be grazing/mowing/restoration or a site-specific set

## Recommended build sequence

1. Lock the six-panel copy to roughly 450–600 total words.
2. Gather current screenshots and hardware photos.
3. Draw the architecture and deployment visuals in the same visual language.
4. Build the A0 composition and test at A4.
5. Run a final claim audit against both repositories.
6. Export a print PDF with embedded fonts and check the physical dimensions.
