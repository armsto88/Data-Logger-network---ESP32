# Sensor Node Projects

This folder contains example sensor node projects that communicate with the mothership via ESP-NOW. The primary example included is the air-temperature node (ESP32-C3).

## Layout

```
nodes/
├── shared/                   # (optional) shared protocol headers
├── air-temperature-node/     # Example ESP32-C3 node
└── (other nodes...)          # Future node types
```

## air-temperature-node (ESP32-C3)

This example node (`nodes/air-temperature-node`) is a simple air-temperature sender. It:

- Broadcasts discovery requests when UNPAIRED.
- Accepts pairing commands and pairing responses from the mothership.
- Responds to `DEPLOY_NODE` to set RTC, schedule, and enter DEPLOYED state.
- Handles a best-effort `UNPAIR_NODE` command to clear its stored mothership MAC and return to UNPAIRED.

### Quick build & upload (PlatformIO)

From the repository root (PowerShell):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d .\nodes\air-temperature-node -e esp32c3 -t upload --upload-port COM7
```

Adjust the COM port as needed.

### Node configuration

- Node ID: `TEMP_001` (edit `src/main.cpp` to change)
- Default wake interval: 5 minutes
- The node stores a preloaded `mothershipMAC` in RTC memory for testing—discovery/pairing will overwrite it when the node finds the mothership.

### How pairing & unpairing works

- Discovery: node broadcasts a `DISCOVER_REQUEST`; the mothership responds with a discovery response.
- Pairing: node sends/receives pairing messages. The mothership sends both a textual `PAIR_NODE` command and an RNT compact reply; the node stores the mothership MAC and marks itself PAIRED.
- Deploy: mothership sends `DEPLOY_NODE` with time and schedule; node sets RTC, records schedule, and becomes DEPLOYED.
- Unpair: mothership will send a best-effort `UNPAIR_NODE` command and also remove the peer locally. The node's code listens for `UNPAIR_NODE`, clears its stored mothership MAC, sets state to UNPAIRED, and re-enters discovery.

### Known issues

- Undeploy (moving a node from DEPLOYED back to PAIRED/UNPAIRED) is not implemented yet. If you need an 'undeploy' action, we can add it so the mothership sends a specific `UNDEPLOY_NODE` command and the node transitions back and optionally clears scheduled behavior.
- ESP-NOW channel/interface sensitivity: pairing uses channel 1 and peers are bound to the STA interface to avoid ESP_ERR_ESPNOW_IF errors. If you see send failures, ensure devices are on channel 1 during pairing and check serial logs for `esp_err` text.

### Troubleshooting

- Check serial logs on both node and mothership. The mothership prints `📨 send_cb to <MAC> status=OK/FAIL` for delivery status. Use those lines to determine whether commands reached the node.
- If the node doesn't persist pairing, ensure the mothership saved paired nodes to NVS (mothership serial shows "✅ Saved X paired nodes to NVS").

If you'd like, I can add a small test harness or scripts to simulate many nodes for load testing.