# Project Guidelines

## Scope
- This repo combines firmware, hardware, and design documentation for an ESP32-based sensor-node and mothership system.
- Keep repo-specific behavior here; do not add a root `AGENTS.md` alongside this file.

## Layout
- `mothership/` contains mothership firmware source and design docs.
- `node/` contains node firmware, tests, shared headers, and design docs.
- `docs/` contains shared/cross-system design notes, roadmap, and workflow docs.
- `hardware/` contains PCB, CAD, and simulation assets.
- `include/` contains shared headers (if used).

## Working Conventions
- Put new mothership-specific design notes in `mothership/docs/`.
- Put new node-specific design notes in `node/docs/`.
- Put new shared/cross-system design notes in `docs/` unless they are tightly coupled to a hardware asset folder.
- Preserve the repo's existing documentation tone: separate confirmed decisions from open questions and proposed work.
- Prefer small, focused updates over broad rewrites.
- Do not create duplicate planning docs when an existing mothership or node design note already owns the topic.

## Validation
- For code changes, prefer a narrow PlatformIO build or other task scoped to the touched area.
- For documentation-only changes, at minimum check for editor-detected errors and broken local references when practical.
- If a change affects workflow or repo structure, update documentation accordingly.

## Common Repo Tasks
- Mothership work usually touches `mothership/docs/`, `mothership/firmware/src/`, and shared scheduling or storage notes.
- Node hardware work usually touches `node/docs/`, `node/firmware/`, and `hardware/` assets.
- PCB and enclosure planning should keep manufacturability, serviceability, and field power constraints explicit.

## References
- See `CONTRIBUTING.md` for repository layout and contribution rules.
- See `docs/README.md` for the main documentation index.

## Agent Workflow
- The active repo-scoped workflow lives under `.github/`.
- The intended user-facing entrypoint is `orchestrator`.
- Keep the archived legacy files in `docs/legacy-agent-workflow/` treated as reference material unless deliberately migrated into `.github/agents/`.
- See `.github/README.md` for the active workflow index.