# fieldMesh — Frequently Asked Questions

**Date:** 2026-06-28
**Audience:** Ecologists, field practitioners, and anyone deploying or operating a fieldMesh network
**Companion docs:**
- `docs/FIRMWARE_FUNCTIONALITY_FOR_ECOLOGISTS.md` — full end-to-end workflow
- `docs/concept_overview.md` — system architecture
- `docs/FutureRoadMap.md` — roadmap
- `docs/FIELDMESH_USER_ONBOARDING_BRIEF.md` — multi-tenant onboarding plan

---

## Getting Started

### Q: What do I need in the field to run a fieldMesh network?
A mothership unit, one or more sensor nodes, and a phone or laptop with WiFi. No internet connection is required for local operation. The mothership creates its own WiFi network (e.g. `Logger001`) and serves a web dashboard at `http://192.168.4.1/`. Nodes communicate with the mothership over ESP-NOW on the same WiFi channel.

### Q: How do I connect to the mothership dashboard?
1. Power on the mothership.
2. On your phone or laptop, join the WiFi network named `Logger001` (password: `logger123`).
3. A captive portal should open automatically on most phones. On a laptop, open a browser and go to `http://192.168.4.1/`.

### Q: Do I need to set the time on the mothership?
Yes — at least once. The mothership's DS3231 RTC is the time reference for all scheduling and logging. On first boot, use the "Set time" panel on the home page (tap "Use browser time" for convenience). The RTC is coin-cell backed, so time survives power loss after that.

### Q: What if I forget the WiFi password?
The default is `logger123`. The SSID is `Logger` + the device ID (e.g. `Logger001`). If the device ID has been changed, it will be shown in the About / Advanced panel on the home page.

---

## Nodes

### Q: How do I add a new node?
1. Power on the node.
2. Press and hold the pair button on the node for ~3 seconds until the green light appears.
3. On the mothership dashboard, tap **Find New Nodes** (or open the Nodes page and use the "Add New node" panel).
4. The node appears in the list as "New" (unpaired). Tap it to assign an ID, name, and activate it.

### Q: What is the difference between "New", "Connected", and "Active"?
- **New (Unpaired):** The mothership has seen the node but has no association. It is not collecting data.
- **Connected (Paired):** The node is registered to this mothership but not yet actively sampling.
- **Active (Deployed):** The node is sampling on its configured interval, queuing data, and syncing with the mothership.

### Q: How long does a node's battery last?
That depends on the wake interval and battery capacity. A node wakes, reads sensors in 2–3 seconds, and powers down with the radio off. At a 5-minute interval, a typical 18650 LiPo lasts several weeks. The dashboard shows each node's last reported battery voltage (green ≥ 3.9 V, orange 3.5–3.9 V, red < 3.5 V).

### Q: What happens if a node misses a sync window?
Nothing is lost. The node retains its queued readings (up to 24 records) and tries again at the next sync slot. If the queue fills, the oldest record is dropped and flagged with quality flag `0x0001` so the gap is visible in the CSV.

### Q: Can I change a node's recording interval without redeploying?
Yes. The recording interval is a global setting (Settings page). When you change it, the mothership pushes the new interval to all connected and active nodes at their next sync. Per-node overrides are planned for future firmware.

### Q: How do I remove a node from service?
Open the node's detail page, choose "Remove node", and confirm. The mothership sends an UNPAIR command and clears the node's ID, name, and notes. You will need to re-pair it with the pair button if you want to add it back later.

---

## Data & Storage

### Q: Where is my data stored?
Locally on the mothership in `/datalog.csv` (LittleFS flash on V1/V2, SD card on older production hardware). You can download it directly from the dashboard via the **Export Data** button.

### Q: What format is the CSV in?
Wide format — one row per node per wake cycle, with all sensor channels in columns. Sensors not fitted on a given node appear as `NaN`. See `FIRMWARE_FUNCTIONALITY_FOR_ECOLOGISTS.md` §7 for the full column list.

### Q: What is the difference between `ms_datetime` and `node_unix`?
`ms_datetime` is when the mothership received and logged the record. `node_unix` is when the node actually measured the sample. The difference is the buffering delay (expected and correct). Use `node_unix` for ecological analysis of measurement timing.

### Q: What does quality flag `0x0001` mean?
The record was preceded by a queue overflow — a gap may exist in the data stream before this row. All other records have `0x0000` (clean).

### Q: How much data can the mothership hold?
V1/V2 motherships use LittleFS flash (typically ~1 MB usable). The dashboard shows storage used as a percentage. At a 5-minute interval with 5 sensors, a node produces ~150 bytes/row; 30 nodes at 5-min intervals generate ~1.3 MB/day. If flash fills, older data is not automatically purged — download and clear periodically for long deployments.

---

## Cloud Upload

### Q: How do I enable cloud upload?
Go to **Settings → Cloud connection**, check "Enable cloud upload", and enter your API key (format `fm_xxxxxxxx`). You can also paste a QR code string of the form `url|key` to set both endpoint and key at once. Save, then use "Finish & Start Recording" to apply.

### Q: What happens if the upload fails?
The mothership increments a retry counter and tries again at the next scheduled collection. Failed data stays in the upload queue — it is not lost. The dashboard shows a "Last upload failed" status indicator. Check antenna connection, signal strength, and API key validity.

### Q: Can I trigger an upload manually?
Yes, if "Allow manual upload" is enabled in advanced settings. A "Upload now" button appears on the Settings page. This powers on the LTE modem and transmits immediately — it takes 30–60 seconds and draws extra power.

### Q: Does cloud upload replace local storage?
No. LittleFS `/datalog.csv` remains the durable local record. Cloud upload is an additional egress path. If the modem or network is unavailable, data is still logged locally and queued for the next upload attempt.

---

## Scheduling

### Q: How is the sync interval determined?
The sync interval is auto-derived from the wake interval: `syncMin = wakeMin × 18`. This keeps the node queue at ~75% fill at sync time, with a 6-wake headroom before any overflow risk. You cannot set the sync interval directly — change the wake interval and sync follows.

| Wake interval | Auto sync interval |
|---|---|
| 1 min | 18 min |
| 5 min | 90 min |
| 10 min | 3 h |
| 30 min | 9 h |
| 60 min | 18 h |

### Q: Can I set a daily upload time instead of interval-based?
Yes. In Settings, switch the upload schedule to "Daily" and set a time (HH:MM). The mothership will upload once per day at that time. This is useful when LTE airtime cost is a concern.

### Q: What happens when I change the wake interval?
The mothership broadcasts the new interval to all connected and active nodes. Each node updates its NVS and re-arms its RTC alarm. The change takes effect at the node's next wake — no redeploy required.

---

## Power & Reliability

### Q: What happens if the mothership loses power?
All settings (wake interval, sync mode, node registry, API key) are stored in NVS and survive power loss. The DS3231 RTC is coin-cell backed, so time is preserved. On power restoration, the mothership resumes operation automatically. Nodes continue sampling independently — they don't need the mothership to be awake for each measurement.

### Q: What happens if a node loses power?
The node's state (paired/deployed, wake interval, mothership MAC) is in NVS and survives power loss. The DS3231 RTC is coin-cell backed. On power restoration, a deployed node reloads its state and resumes sampling automatically.

### Q: Do nodes need the mothership to be awake to measure?
No. Nodes wake on their own RTC alarm, read sensors, queue data, and power down — all without radio activity. The mothership is only needed during sync windows to receive the queued data.

---

## Future Plans

### Q: Will there be user accounts and a cloud dashboard?
Yes. The roadmap includes a multi-tenant platform on Supabase: users sign up, create projects, register motherships, and view isolated data in a real-time dashboard. See `FIELDMESH_USER_ONBOARDING_BRIEF.md`.

### Q: Will more sensor types be supported?
Yes. The roadmap targets an 8-port plug-and-play node where each port can be mapped to any sensor type (soil, PAR, wind, thermocouple, etc.) via a "Port Mapper" UI. Calibration coefficients will be manageable per-sensor and pushed wirelessly. See `FutureRoadMap.md` §2–4.

### Q: What about the ultrasonic anemometer?
An on-board ultrasonic anemometer is in development with PCB, mechanical, and OpenFOAM simulation assets. It will slot into the port-mapper system as a wind sensor backend. See `hardware/ultrasonic_anemometer/`.

### Q: Will OTA firmware updates be supported?
Yes — over ESP-NOW or WiFi AP, pushable to single nodes or the whole fleet. See `FutureRoadMap.md` §5.

### Q: Is there a native mobile app planned?
The roadmap includes a native app with real-time data, project management, and QR-based device provisioning. See `NATIVE_APP_FULL_CONCEPT_AND_SCHEMAS.md`.

---

## Troubleshooting

### Q: A node shows "n/a" for battery — why?
The node hasn't sent a battery reading yet. Battery voltage is reported in the first data packet after deploy. Wait for the next sync window or check that the node is powered and in range.

### Q: "Find New Nodes" didn't find my node — what do I check?
- Ensure the node is powered on and in pairing mode (green light after holding the pair button for 3 seconds).
- Ensure the node is within ESP-NOW range (typically ~100 m line-of-sight, less through vegetation/walls).
- Try pressing Find New Nodes again — the mothership sends 3 discovery bursts.

### Q: The dashboard clock is wrong — how do I fix it?
Open the "Set time" panel on the home page and tap "Use browser time", then "Set Time". This syncs the mothership RTC to your phone/laptop clock. All nodes will pick up the corrected time at their next sync.

### Q: My upload keeps failing — what should I check?
1. Verify the API key is correct (Settings → Cloud connection shows last 4 digits).
2. Check that the LTE antenna is connected and the SIM is active.
3. Check battery voltage — the modem won't power on below the minimum battery threshold (default 3700 mV).
4. Look at the retry count on the dashboard — if it's climbing, the modem is trying but failing to register on the network.

### Q: The captive portal doesn't open on my phone — what now?
Open a browser manually and navigate to `http://192.168.4.1/`. Some Android phones suppress captive portals for networks without internet — this is normal and doesn't affect functionality.