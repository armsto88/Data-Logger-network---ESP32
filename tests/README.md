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
