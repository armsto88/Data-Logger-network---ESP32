# Firmware Bring-Up Log

This log tracks firmware bring-up milestones, known-good settings, flash history, and verification outcomes.

## Current Status

- Date: 2026-04-18
- Mothership AP: Working
- Mothership Web UI: Working
- Discover Nodes: Working with fast refresh
- ESP-NOW: Working on shared channel
- BLE: Disabled intentionally

## Known-Good Configuration (2026-04-18)

### Mothership

- Environment: `esp32s3`
- Upload port: `COM9`
- AP mode: Enabled
- AP SSID pattern: `Logger` + `DEVICE_ID` (currently `Logger001`)
- AP channel: `11`
- ESP-NOW runtime: Enabled
- BLE GATT: Disabled
- Captive portal support: Enabled
- Discovery behavior: Burst scan + dashboard auto-refresh after discover

### Nodes

- Environment: `firmware/nodes/sensor-node` -> `esp32wroom`
- Upload port used during bring-up: `COM3`
- Shared ESP-NOW channel: `11`
- Node identity model: Auto-derived from MAC at runtime
- Per-node custom firmware IDs: Not required

## Bring-Up Decisions

1. Use shared RF channel for AP + ESP-NOW coexistence.
2. Keep BLE disabled for stability during this phase.
3. Use MAC-based identity to avoid duplicate logical ID collisions.
4. Keep user-friendly naming/assignment in UI metadata, separate from hardware identity.

## Flash History (Session)

### Mothership

- Flashed `esp32s3` to `COM9` multiple times while validating:
  - AP visibility
  - channel alignment
  - captive portal handling
  - discovery burst behavior
  - immediate dashboard refresh after discover

### Nodes (auto-ID firmware)

All flashed with standard `esp32wroom` firmware (no per-node ID build variants required):

- `D4:E9:F4:94:5D:C4`
- `D4:E9:F4:94:DF:54`
- `D4:E9:F4:92:F9:60`
- `D4:E9:F4:94:E3:8C`

## Verified Outcomes (2026-04-18)

- AP is visible and web UI is reachable.
- All 4 nodes can be discovered.
- Discover action refreshes UI quickly after scan.
- Mothership no longer merges nodes by duplicated logical ID when MAC differs.

## Discovery Validation Snapshot (2026-04-18 20:51 local)

- Mothership booted clean on AP channel 11.
- Fleet started empty (`paired/deployed=0`) and discovery was triggered.
- Unique nodes discovered during burst scan:
  - `ENV_945DC4` (`D4:E9:F4:94:5D:C4`)
  - `ENV_94E38C` (`D4:E9:F4:94:E3:8C`)
  - `ENV_92F960` (`D4:E9:F4:92:F9:60`)
  - `ENV_94DF54` (`D4:E9:F4:94:DF:54`)
- Repeated discovery packets from same MACs were handled without duplicate-node aliasing.
- `Fleet TIME_SYNC: no PAIRED/DEPLOYED nodes registered` was observed and is expected while all nodes are still UNPAIRED.
- Nodes later marked asleep in UI are expected behavior after no recent packets (idle timeout), not a fault.

## Pairing/Commissioning Validation (2026-04-18 20:58-21:03 local)

- One-node-at-a-time commissioning flow was used.
- Each node was discovered, assigned user ID/name, paired, and time-synced successfully.
- Final paired/deployed registry count reached 4 (`Saved 4 paired/deployed nodes to NVS`).

Final node mapping:

- `ENV_945DC4` (`D4:E9:F4:94:5D:C4`) -> user ID `001`, name `NODE 1`
- `ENV_92F960` (`D4:E9:F4:92:F9:60`) -> user ID `002`, name `NODE 2`
- `ENV_94DF54` (`D4:E9:F4:94:DF:54`) -> user ID `003`, name `NODE 3`
- `ENV_94E38C` (`D4:E9:F4:94:E3:8C`) -> user ID `004`, name `NODE 4`

Key validation lines observed for each node:

- `pairNode(...): PAIR_NODE=OK ... PAIRING_RESPONSE=OK`
- `TIME_SYNC -> <nodeId> ... : OK`

## Open Items

1. Optional: Re-enable BLE with coexistence-safe profile after AP/ESP-NOW soak period.
2. Optional: Add periodic health counters to UI (AP health, discovery success rate, node wake/contact age).
3. Run extended soak test (>= 1 hour) with all nodes powered.
4. Optional UI enhancement: Add a manual `Sync now` action per node and for fleet-wide use, so operators can force an immediate sync/refresh while viewing the dashboard.

## Suggested Entry Format for Future Updates

- Date/time:
- Firmware target(s):
- Ports used:
- Changes made:
- Flash result:
- Verification steps:
- Outcome:
- Follow-up actions:
