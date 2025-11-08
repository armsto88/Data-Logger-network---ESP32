# Data Logger Network — ESP32 Mothership and Sensor Nodes

This repository contains a small sensor-network system built using ESP-NOW:

- Mothership: ESP32-S3 running a web UI, handling discovery, pairing, deployment, logging to SD, and persisting paired nodes.
- Sensor nodes: ESP32-C3 devices that auto-discover the mothership, pair, receive deployment commands, and send sensor data.

This README documents the current state of the code, how to build and test it, and known issues.

## Project status (2025-11-08)

- **All schedule/interval logic has been fully removed** from both mothership and node firmware. The only time sync is RTC on deploy.
- **UI reverted to classic style** for clarity and user preference.
- **'Undeploy' (revert to paired) feature is implemented**: You can now revert a deployed node to the paired state from the web UI. Node stops transmitting when reverted.
- **All code builds and uploads cleanly** for both mothership (ESP32-S3) and node (ESP32-C3) using PlatformIO.
- **Web UI and backend are consistent and minimal**: Only core flows (discovery, pair, deploy, unpair, revert) are present. No schedule, alarm, or interval logic remains.
- **Serial output and protocol are clean**: No schedule/interval/alarm messages are sent or displayed.

## Repository layout

```text
Data-Logger-network---ESP32/
├── platformio.ini                # Mothership PlatformIO envs (esp32s3)
├── Readme.md                     # This file
├── BrainStorm.md                 # Notes / ideas
├── src/                          # Mothership source
│   ├── main.cpp                  # Web UI, WiFi, routes
│   ├── espnow_manager.cpp/.h     # ESP-NOW logic, persistence
│   ├── sd_manager.cpp/.h         # CSV logging helper
│   ├── rtc_manager.cpp/.h        # RTC helper
│   └── config.h                  # Pin and build-time config
└── nodes/
    ├── README.md                 # Node-level docs
    └── air-temperature-node/     # Example node project (esp32c3)
```

## Key concepts and protocols

- Communication uses ESP-NOW (no WiFi AP client traffic required).
- The mothership runs in AP+STA mode and uses a fixed pairing channel (channel 1) for discovery/pairing to improve reliability.
- Message types: discovery, pairing request/response, deploy, time sync, sensor data, UNPAIR.
- Paired nodes are stored in NVS via Preferences and re-added as peers on startup.

## Quick build & upload (PlatformIO)

From the repository root (PowerShell examples):

Build and upload the mothership (ESP32-S3):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32s3 -t upload --upload-port COM4
```

Build and upload the example node (ESP32-C3):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\nodes\air-temperature-node -e esp32c3 -t upload --upload-port COM7
```

Open serial monitors for both devices (set correct COM ports and 115200 baud) while testing pairing/deploy flows.

## How to test pairing, deploy, unpair, and revert

1. Put mothership online (flash and open the web UI; web server runs on the AP interface).
2. Flash and power the node(s). Watch node serial for discovery messages.
3. On the mothership web UI, click 'Discover' then 'Pair selected nodes' for any discovered nodes. The UI will send both a pairing command and an RNT-style compact reply so nodes that expect that format will auto-add the mothership.
4. After a successful pair, deploy the node with the 'Deploy' action. The node should set its RTC, change to DEPLOYED state, and start sending sensor data.
5. To unpair remotely, use the Unpair UI. The mothership will send a best-effort `UNPAIR_NODE` command and then remove the peer and persist the change locally. The UI now shows per-node send status (Remote UNPAIR sent/failed) and local unpair status.
6. To revert a deployed node to paired, use the 'Revert to Paired' button in the deployed node list. The node will stop transmitting and return to the paired state.

## Known issues and caveats

- ESP-NOW is sensitive to WiFi interface/channel state; the code forces channel 1 for pairing and binds ESP-NOW peers to the STA interface to avoid ESP_ERR_ESPNOW_IF. If you see send failures, confirm both devices use channel 1 during pairing.
- The remote unpair is best-effort: if the node is asleep or out of range, the mothership will still remove the peer locally and persist the change. When the node wakes and discovers no mothership, it will continue discovery.

## Troubleshooting tips

- If pairing fails with esp-now send errors, open serial on both devices and ensure the `📨 send_cb to ... status=OK/FAIL` lines are shown. Try increasing delays in `espnow_manager.cpp` around peer add/send.
- If nodes don't persist pairing, check that NVS (Preferences) writes succeed on the mothership (Serial logs show saved counts).
- If you change board hardware or MAC addresses, update preloaded `mothershipMAC` in node code or use discovery flow.

## Next steps (planned)

- Add better UI feedback (AJAX, progress bar) and per-node logs
- Add secure pairing/encryption option for ESP-NOW peers
- Add more node types and a shared protocol header

---

**Session summary (2025-11-08):**
- Removed all alarm, schedule, and interval logic from both mothership and node firmware, including all UI and protocol fields.
- Added 'undeploy' (revert to paired) feature to the web UI and backend; node stops transmitting when reverted.
- Cleaned up all serial output and protocol messages for clarity.
- Reverted UI to classic style for user preference.
- All code builds and uploads cleanly for both boards.
