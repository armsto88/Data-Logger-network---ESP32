# FieldMesh — Deployment Log & Communication Test Plan

> **Status:** Living document. Updated as the deployment runs and test results come in.
>
> **Purpose:** Track the current real-world deployment, record what has been achieved, and plan the next round of communication tests between the mothership and nodes.

---

## 1. Current Deployment

| Field | Value |
|---|---|
| **Location** | Backyard |
| **Nodes deployed** | 3 |
| **Mothership** | 1 (on-site) |
| **Deployed since** | Early July 2026 (≈ 2 weeks as of 2026-07-17) |
| **Wake interval** | *(fill in — e.g. 5 min)* |
| **Sync interval** | *(fill in — auto-derived or manual)* |
| **Sensors active per node** | *(fill in — e.g. SHT41 air, AS7341 light, ADS1115 soil ×2, battery)* |
| **Cloud upload** | *(fill in — LTE active? manual? not yet?)* |
| **Dashboard** | *(fill in — Google Sheets / local only?)* |

### Node inventory

| Node ID | Friendly name | MAC / serial | Sensors | Battery | Notes |
|---|---|---|---|---|---|
| 1 | *(fill in)* | *(fill in)* | | | |
| 2 | *(fill in)* | *(fill in)* | | | |
| 3 | *(fill in)* | *(fill in)* | | | |

---

## 2. What Has Been Achieved So Far

### Confirmed working

- ✅ Three nodes deployed and running continuously for approximately two weeks.
- ✅ Reliable scheduled wake → sensor read → ESP-NOW sync → mothership logging over the full period.
- ✅ No data loss reported across the fleet during normal operation.
- ✅ Mothership receiving and logging snapshots from all three nodes.

### Previous bench validation (pre-deployment)

From `docs/FIRMWARE_AND_HARDWARE_NOTES.md` and `docs/MULTI_NODE_VALIDATION_2026-05-05.md`:

- ✅ Single-node end-to-end pipeline (sensor → queue → sync → CSV) validated.
- ✅ Three-node concurrent sync-window flush validated on bench.
- ✅ ESP-NOW range characterised: 1 m LOS ~98.5% ACK, 30 m LOS ~99.7% ACK, 100 m obstructed ~84–87% ACK.
- ✅ Queue persistence, RTC recovery, and sync-window miss handling validated on bench.
- ✅ 4-node fleet test matrix (FN-1 through FN-8) designed; some run on bench.

### What the backyard deployment adds

- ✅ First long-duration real-world run (≈ 2 weeks continuous).
- ✅ Multi-node reliability confirmed outside the bench environment.
- ✅ Real environmental conditions (temperature swings, humidity, overnight) sustained without intervention.

---

## 3. Communication Use Cases to Test

These are the scenarios most likely to expose weaknesses in the mothership ↔ node communication path that normal backyard operation won't surface on its own. Each is drawn from the bench test matrix in `FIRMWARE_AND_HARDWARE_NOTES.md` but adapted for real-world field conditions.

### 3.1 Range and obstruction

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-1 | Move one node behind a solid wall | NLOS / obstruction degradation | Relocate one node behind a brick wall or metal shed, 10–30 m from the mothership. Leave for several sync windows. | Node still delivers snapshots; any missed windows queue locally and flush on next successful window. No permanent data loss. |
| C-2 | Maximum range test | ESP-NOW link budget at distance | Move one node to the edge of the property (fence line, far corner). Monitor ACK rate and queue depth over 1–2 hours. | Node either maintains delivery or queues and catches up; no silent drop. Record approximate distance and obstruction count. |
| C-3 | Node in a metal enclosure | RF shielding worst case | Place one node inside a metal box or shed. | If delivery fails, node queues and flushes when brought back into range. Confirms queue-then-recover behaviour. |

### 3.2 Sync-window failures

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-4 | Mothership powered off during sync window | Node queue-while-offline | Power off the mothership just before a scheduled sync window. Leave off for 2–3 hours (several missed windows). Power back on. | All three nodes queue locally during outage. After mothership returns, next sync window flushes all queued snapshots. No gaps in CSV beyond the queue capacity. |
| C-5 | Mothership reboot mid-sync-window | FN-7 in the field | Reboot the mothership while a sync window is open and nodes are flushing. | Nodes listen for the full window, log "sync marker not seen", re-arm, and flush on the next window. No data loss. |
| C-6 | Extended mothership downtime (full day) | Queue capacity over long outage | Leave the mothership off for 8–12 hours with nodes waking at their normal interval. | Queue accumulates without overflow (or, if overflow occurs, oldest records drop cleanly and the event is logged). All recoverable data flushes on next sync. |

### 3.3 Node-side recovery

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-7 | Node battery pulled mid-cycle | Power-loss recovery (SN-9) | Remove the battery from one node during a wake/sync cycle. Re-insert after 10 minutes. | Node reloads state from NVS, resumes schedule, and continues sending. No duplicate or missing sensor record at the boundary. |
| C-8 | RTC coin cell removed | RTC loss + TIME_SYNC recovery (SN-6) | Remove the DS3231 coin cell from one node, then power-cycle it. | Node logs "RTC lost power", sets `rtcSynced=0`, requests TIME_SYNC from mothership on next sync, and re-aligns within 2 sync cycles. |
| C-9 | Node left in pairing mode, then re-deployed | Re-pair flow in the field | Unpair one node via the web UI, let it sit for a few cycles, then re-pair and re-deploy. | Node stops sending after unpair, resumes after re-deploy. Other two nodes unaffected throughout. |

### 3.4 Schedule and config changes

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-10 | Change wake interval mid-deployment | SET_SYNC_SCHED propagation (FN-2) | From the web UI, change the wake interval (e.g. 5 min → 10 min) while all three nodes are deployed. | All three nodes receive the new schedule within 1–2 sync windows, re-arm alarms, and resume at the new cadence. `configVersion` increments for all nodes. |
| C-11 | Add a 4th node while 3 are deployed | Discovery + pairing with active fleet (FN-6) | Power on a new node while the existing three are running. | New node is discovered, paired, and deployed with the same sync interval and phase as the fleet. Existing nodes unaffected. |
| C-12 | Sync interval shortened to stress flush | Simultaneous flush collision (FN-8) | Temporarily set sync interval equal to wake interval so all nodes flush every cycle with potentially deep queues. | All snapshots land in the CSV. Any delivery failure leaves the queue non-empty for retry. No permanent data loss over a 1-hour run. |

### 3.5 Coexistence and interference

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-13 | Wi-Fi captive portal active during sync window | AP + ESP-NOW coexistence on channel 11 | Connect a phone/laptop to the mothership's Wi-Fi and browse the dashboard during a sync window. | Node sync flush completes normally. Dashboard remains responsive. No sync window failure or dropped node. |
| C-14 | Config button wake during sync window | Manual wake + scheduled wake interaction | Press the config button on the mothership during a scheduled sync window. | Mothership handles both triggers without corrupting the sync schedule. Next scheduled sync window fires on time. |
| C-15 | 2.4 GHz interference | Wi-Fi/Bluetooth congestion | Run a heavy Wi-Fi transfer or Bluetooth device nearby during a sync window. | No sustained sync failure. Any missed snapshots queue and flush on the next window. |

### 3.6 Stale node recovery

| # | Scenario | What it tests | How to run it | Pass criteria |
|---|---|---|---|---|
| C-16 | Node misses 3+ sync windows | Stale-assist TIME_SYNC (STALE_MISS_THRESHOLD=3) | Power off one node for long enough to miss 3+ sync windows. Power it back on. | Mothership detects the node as stale and sends a stale-assist TIME_SYNC. Node re-aligns within 2 sync cycles. No manual intervention needed. |

---

## 4. How to Log Results

For each test, record:

```
### C-X: <scenario name>
**Date:** YYYY-MM-DD
**Nodes involved:** which nodes
**Duration:** how long the test ran
**Result:** PASS / FAIL / PARTIAL
**Observations:** what actually happened
**CSV evidence:** row count before/after, any gaps
**Serial log highlights:** key log lines
**Follow-up:** any issues found, next steps
```

Add results under a new "Test Results" section below as they come in.

---

## 5. Test Results

*(No field results logged yet. Add entries here as tests are run.)*

---

## 6. Open Questions

- What is the real-world ESP-NOW range from the mothership in this backyard (with walls, fences, vegetation)?
- Does the upload queue survive an overnight mothership outage without overflow at the current wake interval?
- How does the fleet behave when a 4th node is added — is discovery reliable with 3 nodes already active?
- Does the captive portal reliably coexist with sync-window traffic under real Wi-Fi conditions?

---

## Related documents

- `docs/FIRMWARE_AND_HARDWARE_NOTES.md` — bench test matrix (SN-1 through SN-10, FN-1 through FN-8), ESP-NOW range data
- `docs/MULTI_NODE_VALIDATION_2026-05-05.md` — original three-node bench validation
- `docs/FIELDMESH_OVERVIEW.md` — system overview
- `docs/concept_overview.md` — node lifecycle and dashboard behaviour