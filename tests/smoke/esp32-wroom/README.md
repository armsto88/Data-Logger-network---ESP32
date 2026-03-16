# ESP32-WROOM Smoke Tests

PlatformIO + C++ (Unity) smoke tests for early bring-up of a new ESP32-WROOM node.

## What these tests cover

- Build-time protocol/config sanity for WROOM target
- ESPNOW channel and default wake interval guardrails
- Protocol payload footprint guardrails (prevent accidental message bloat)

## Test files

- `tests/test_smoke_esp32_wroom/test_main.cpp`
	- Unity tests executed through PlatformIO `pio test`

## Run from repo root

Compile + run smoke tests on an ESP32-WROOM board:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" test -e esp32wroom-smoke
```

Compile-only (no upload):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" test -e esp32wroom-smoke --without-uploading
```

## Notes

- These are smoke tests, not full protocol conformance tests.
- Add additional `test_*.cpp` files in this folder as WROOM firmware matures.
