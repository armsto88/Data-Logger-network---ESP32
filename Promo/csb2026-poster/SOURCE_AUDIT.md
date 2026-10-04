# Poster source audit

- **Audit date:** 2026-10-03
- **Firmware/hardware repository:** `Data-Logger-network---ESP32`, latest commit inspected `f546c70`
- **Dashboard/backend repository:** `FieldMeshDashboard`, latest commit inspected `4cf80d5`

This file separates what is supported by current implementation or published evidence from what is planned. Re-run the audit immediately before the print PDF is frozen because both repositories are active.

## Evidence hierarchy

1. Current source code and current bench records
2. Current tests and protocol contracts
3. Recent technical snapshots/manuscript drafts
4. Older design briefs and marketing copy

When these disagree, use the highest available level and retain uncertainty where hardware execution has not been observed.

## Claim map

| Proposed poster claim | Evidence | Status and safe wording |
|---|---|---|
| Solar-farm microclimates are heterogeneous and cross-study generalisation is constrained by small samples, sensor placement and study design. | Wiley review, DOI `10.1002/brv.70171`, especially abstract and methods/results. | **Supported.** The review synthesised 33 publications / 34 studies. |
| Nodes fully power off between scheduled wakes. | `docs/FIELDMESH_HARDWAREX_MANUSCRIPT_2026-07-26.md`; node power-control code in `node/firmware/src/main.cpp`. | **Supported.** Say “hard-powered off,” not deep sleep. |
| Nodes queue readings and transfer them during coordinated node-to-hub communication. | Node queue and protocol code; `mothership/firmware/v2/src/main.cpp`; commissioning notes in the HardwareX manuscript. | **Supported at prototype/bench level.** Do not imply long-duration field validation. |
| The FieldHub supports local browser-based discovery, naming, configuration and deployment. | `mothership/firmware/v2/src/config/config_server.cpp`; operation instructions in the HardwareX manuscript. | **Supported.** The local AP is WPA2-protected and the service UI is reached in a normal browser. |
| Poor internet does not stop local measurement collection. | Local node queue, FieldHub flash logger/upload queue, and optional backhaul architecture. | **Supported by design and bench evidence.** Prefer “can delay access without stopping local collection.” |
| The hub keeps a permanent local copy. | Current `mothership/firmware/v2/src/storage/sd_logger.cpp` implements a non-deleting microSD archive; `main.cpp` writes accepted snapshots to flash and SD where available. | **Conditional.** Say “local flash working store plus a non-deleting SD archive when the card is available.” Confirm current hardware validation before calling it permanent without qualification. |
| The dashboard shows latest conditions, trends, node comparisons and health context. | `frontend/src/pages/Dashboard.jsx`, `Nodes.jsx`, `Charts.jsx`, and FieldHub health components. | **Supported by implementation.** Use a current screenshot. |
| Users can export historical readings as CSV. | `frontend/src/pages/Export.jsx` and quick export in `Charts.jsx`. | **Supported by implementation.** |
| Cloud upload uses authenticated ingest and stores readings/status. | `supabase/functions/ingest-fieldmesh/index.ts`, shared normalisation code, and Supabase migrations. | **Supported by implementation and commissioning notes.** Avoid implying continuous coverage. |
| Air temperature, relative humidity, spectral channels and two soil channels are integrated. | Sensor firmware, snapshot schema, commissioning tables and dashboard metadata. | **Supported at prototype level.** Soil is uncalibrated voltage; spectral values are raw, not PAR/PPFD. |
| Wind speed is a current measurement. | Reed-cup wind pathway has standalone bring-up evidence; July manuscript says it was not yet exercised through the integrated queue → sync → CSV path. | **Amber.** Recheck before print. Until then say “wind-speed pathway” rather than validated integrated wind data. |
| Wind direction is available. | Schema/UI reserve direction, but current project documentation identifies it as in development. | **Not supported as a current capability.** Label planned/in development. |
| Consumer-grade sensors make dense replication feasible. | Project design rationale; no current cost-comparison dataset located. | **Rationale, not measured result.** Say “intended to make dense deployment more feasible,” not cheaper/best. |
| FieldMesh makes studies repeatable. | Common schema, sensor masks, intervals, deployment records and health metadata support consistency. | **Design goal.** Say “supports” or “makes chosen designs easier to reproduce,” not “guarantees standardisation.” |
| FieldMesh is field-ready or fully validated. | Calibration, drift, longer deployments and production refinement remain open. | **Do not claim.** Use “working prototype moving toward validated field use.” |

## Repository locations used

### Firmware, hardware and embedded workflow

- `docs/FIELDMESH_HARDWAREX_MANUSCRIPT_2026-07-26.md`
- `docs/FIELDMESH_SYSTEM_SNAPSHOT_HARDWAREX_2026-07-24.md`
- `node/firmware/src/main.cpp`
- `mothership/firmware/v2/src/main.cpp`
- `mothership/firmware/v2/src/config/config_server.cpp`
- `mothership/firmware/v2/src/storage/flash_logger.cpp`
- `mothership/firmware/v2/src/storage/upload_queue.cpp`
- `mothership/firmware/v2/src/storage/sd_logger.cpp`

### Frontend and backend

Repository root: `C:\Users\thoma\Documents\FieldMesh\FieldMeshDashboard`

- `frontend/src/pages/Dashboard.jsx`
- `frontend/src/pages/Nodes.jsx`
- `frontend/src/pages/Charts.jsx`
- `frontend/src/pages/Export.jsx`
- `frontend/src/components/about/ArchitectureDiagram.jsx`
- `frontend/src/lib/sensorMetadata.js`
- `frontend/src/lib/colors.js`
- `supabase/functions/ingest-fieldmesh/index.ts`
- `supabase/functions/_shared/fieldmesh.ts`
- `supabase/migrations/`

The current About-page `ArchitectureDiagram.jsx` still describes microSD archival as planned, while the newer firmware contains an SD archive implementation. Treat the firmware as newer implementation evidence, but confirm the hardware test state before final wording.

## External primary sources

- Armstrong et al. (2026), [A mosaic of microclimates: biodiversity outcomes and wildlife habitat potential in large-scale solar facilities](https://onlinelibrary.wiley.com/doi/10.1002/brv.70171), *Biological Reviews*.
- [CSB 2026 guidelines and key dates for oral and poster presenters](https://www.conference-solar-biodiversity.org/abstract-guidelines-scolarships/guidelines-and-key-dates-for-oral-and-poster-presenters).

## Pre-print audit checklist

- [ ] Confirm integrated wind status.
- [ ] Confirm microSD archive on the exact FieldHub hardware being presented.
- [ ] Replace demonstration data with real data or label it clearly.
- [ ] Confirm calibration language for soil and spectral channels.
- [ ] Confirm latest enclosure and production-hardware status.
- [ ] Confirm author list, affiliations, poster ID and QR destination.
- [ ] Confirm the 14 October submission deadline directly if the August sentence remains online.
