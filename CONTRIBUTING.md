# Contributing

## Repository layout

- `firmware/mothership/src/` - mothership firmware source
- `firmware/nodes/` - sensor node firmware projects and bring-up scripts
- `include/` - shared headers for mothership firmware
- `hardware/` - PCB, CAD, and simulation assets
- `docs/` - high-level design, roadmap, and debug notes
- `data/` - collected or example datasets

## Organization rules

- Keep the repository root minimal: only key entry files and top-level folders.
- Put new markdown docs in `docs/` unless tightly coupled to a subproject.
- Place hardware-specific docs beside hardware assets when practical.
- Add a local README when a folder's purpose is not obvious.

## Naming and style

- Use lowercase, underscore-separated names for new scripts and docs where practical.
- Keep filenames descriptive and stable.
- Avoid moving source files without updating references in docs.

## Before opening a PR

- Build the target firmware (`mothership` or node) at least once.
- Update documentation if folder structure or workflows changed.
