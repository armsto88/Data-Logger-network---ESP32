# Tests

This folder is reserved for repeatable validation assets when new hardware arrives.

## Scope

- Host-driven smoke tests
- Protocol validation checks
- CSV logging verification
- Hardware bring-up checklists (test artifacts only)

## Suggested structure

- `tests/smoke/` - short sanity checks for core paths
- `tests/protocol/` - packet and state-machine checks
- `tests/hardware/` - fixture notes and acceptance steps
- `tests/fixtures/` - static sample payloads/logs for regression checks

## Notes

- Keep tests deterministic and fast where possible.
- Store expected outputs close to each test.
- If a test depends on real hardware, state assumptions clearly.

## Current Smoke Suite

- `tests/test_smoke_esp32_wroom/test_main.cpp`

These tests target early bring-up for a new ESP32-WROOM node.

Quick examples from repo root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" test -e esp32wroom-smoke
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" test -e esp32wroom-smoke --without-uploading
```
