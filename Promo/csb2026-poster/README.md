# CSB 2026 conference poster

This directory is the working home for the FieldMesh poster for the Conference on Solar Energy and Biodiversity 2026.

## Current direction

- **Title:** Monitoring the mosaic
- **Subtitle:** FieldMesh for fine-scale environmental monitoring in solar farms
- **Format:** A0 portrait, 841 × 1189 mm
**Core story:** research gap → practical design requirements → FieldMesh architecture → validation pathway → research and management use

The recommended design direction is in [POSTER_CONCEPT.md](POSTER_CONCEPT.md). Claims and their source status are tracked in [SOURCE_AUDIT.md](SOURCE_AUDIT.md).

## Files

- `master_reference.md` — content-preserving import of `C:\Users\thoma\Downloads\fieldmesh_csb2026_poster_master_reference.md`. Treat this as the canonical brief.
- `POSTER_CONCEPT.md` — first layout, narrative and visual direction.
- `SOURCE_AUDIT.md` — evidence map across the firmware, frontend and backend repositories.
- `assets/README.md` — asset checklist and naming convention.

## Source repositories

- Firmware, hardware and embedded documentation: this repository, `Data-Logger-network---ESP32`.
- Dashboard frontend, Supabase backend and cloud schema: `C:\Users\thoma\Documents\FieldMesh\FieldMeshDashboard`.
- Research origin: [A mosaic of microclimates: biodiversity outcomes and wildlife habitat potential in large-scale solar facilities](https://onlinelibrary.wiley.com/doi/10.1002/brv.70171).

## Conference requirements checked 2026-10-03

The official presenter guidance specifies A0 portrait, PDF delivery, no required template and onsite printing. Its poster section and “Important Dates” list **14 October 2026** as the deadline. The same page contains an apparently stale sentence requesting posters by 20 August; confirm by email if there is any doubt.

Official guidance: <https://www.conference-solar-biodiversity.org/abstract-guidelines-scolarships/guidelines-and-key-dates-for-oral-and-poster-presenters>

## Working rules

1. Keep the master reference intact; develop new copy in separate files.
2. Prefer the current source code over older design documents when they disagree.
3. Label implemented, bench-tested, field-validated and planned capabilities differently.
4. Do not describe the nodes as sleeping: they are hard-powered off between scheduled wakes.
5. Avoid absolute data-loss claims. Describe what happens after a reading is accepted into local storage and state the SD-card condition where relevant.
6. Keep the poster readable in two to three minutes; detailed engineering belongs in conversation or the QR-linked material.
