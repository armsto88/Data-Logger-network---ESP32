# Scripts

This folder is for utility scripts used during development, bring-up, and validation.

## Intended subfolders

- `scripts/flash/` - flashing and monitor helpers
- `scripts/test/` - host-side test runners and quick checks
- `scripts/data/` - data conversion or export helpers

## Script conventions

- Prefer PlatformIO-first scripts and Python wrappers (`.py`) for portability.
- Keep scripts idempotent where possible.
- Do not hardcode COM ports; accept them as parameters.
- Add a short usage header at the top of each script.

## First candidates to add

- `scripts/test/smoke_espnow.py`
- `scripts/test/check_rtc_sync.py`
- `scripts/flash/upload_node.py`
