# Contributing

## Repository layout

- `mothership/` - mothership firmware source and docs
- `node/` - sensor node firmware, tests, shared headers, and docs
- `hardware/` - PCB, CAD, and simulation assets
- `docs/` - shared/cross-system design, roadmap, and debug notes

## Organization rules

- Keep the repository root minimal: only key entry files and top-level folders.
- Put new markdown docs in `mothership/docs/` or `node/docs/` unless they are genuinely cross-system, in which case use `docs/`.
- Place hardware-specific docs beside hardware assets when practical.
- Add a local README when a folder's purpose is not obvious.

## Naming and style

- Use lowercase, underscore-separated names for new scripts and docs where practical.
- Keep filenames descriptive and stable.
- Avoid moving source files without updating references in docs.

## Agent Workflow

- The active repo-scoped AI workflow lives under `.github/`.
- `orchestrator` is the intended user-facing coordinator.
- If workflow docs change, update `.github/` agent definitions and `README.md` together.

## Before opening a PR

- Build the target firmware (`mothership` or node) at least once.
- Update documentation if folder structure or workflows changed.
