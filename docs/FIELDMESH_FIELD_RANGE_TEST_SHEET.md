# FieldMesh Field Range Test Recording Sheet

**Date:** _______________  **Operator:** _______________

**Mothership / FieldHub:** MAC _______________  firmware build _______________
**Nodes tested:** MAC _______________ / _______________ / _______________

**Sync window:** 105 s budget, 15–45 s join, 9 s grant, 3 snapshot retries, 2 grant failures → skip.
**Listen window (node):** 60 s for marker, then session deadline from mothership.

---

## What to watch on the serial monitor

### Mothership (115200 baud) — one complete sync produces these lines:

```
[SYNC] join window=Xms (anchored to slot+Xs, deployed=N)
[SYNC] roster +<nodeId> queue=N           ← HELLO received from each node
[SYNC] FW_CAPS <nodeId> v<ver> hw=<hw>    ← firmware identity (if built)
[SYNC] rendezvous closed: responders=N deployed=M
[SYNC] grant node=<id> id=N quota=N reportedQueue=N
[SNAP] RX=<mac> nodeId=<id> seq=N present=0xNN bat=X.XXV airT=X.X airH=X.X
[SNAP-ACK] <id> seq=N persisted=N proto=N send=OK     ← durable ack sent
[SYNC] done node=<id> sent=N remaining=N status=N
[SYNC] release node=<id> sent=N confirmed=N remaining=N
[SYNC] coordinated window complete: responders=N drops=N
[SYNC] NODE_CONFIG broadcasts prepared: N node(s)
[SYNC] CONFIG_ACK converged: <id> v<N> (ACTIVE/STANDBY)
[SYNC] Sync window closed
```

**Failure indicators (mothership):**
```
[SYNC] grant timeout node=<id> failures=N        ← node didn't respond to grant
[SYNC] grant send failed node=<id> failures=N
[SNAP] Flash logging failed                       ← storage issue, not radio
```

### Node (115200 baud) — one complete sync produces:

```
📶 Sync wake: joining coordinated mothership session
📶 Sync listen window until <time> (target=<time>, grace=Xs)
[EVENT] SYNC_WINDOW_OPEN marker phaseUnix=N
[SYNC] session open id=N join=Xms window=Xs
👋 NODE_HELLO sent: cfgV=N wakeMin=N qDepth=N : direct=OK bcast=OK
[SYNC] grant=N quota=N window=Xms queue=N
[DUMP] ... (only on failure — link failure / ACK timeout)
📤 queue flush done: sent=N pending=N
[ACK] durable SNAPSHOT_ACK matched seq=N
[SYNC] DUMP_DONE session=N grant=N sent=N remaining=N status=N
[SYNC] RELEASE_ACK session=N applied=N sync=N phase=N remaining=N grace=N
💤 [FINALIZE] Power cut scheduled – reason: <reason>
```

**Failure indicators (node):**
```
[SYNC] session N ended without RELEASE; queue retained=N    ← missed release
⚠️ Sync marker not seen in listen window; flush skipped      ← no marker heard
[DUMP] seq=N attempt=N/N link failure queue=... status=N     ← radio loss mid-flush
[DUMP] seq=N attempt=N/N durable ACK timeout                 ← mothership didn't ACK
```

---

## RSSI measurement (separate from production sync)

The production sync path does **not** log ESP-NOW RSSI. To get dBm at each distance, flash the range-test sketches to a TX and RX node:

```
pio run -e esp32wroom-range-tx  -t upload --upload-port COMx   # TX node
pio run -e esp32wroom-range-rx  -t upload --upload-port COMy   # RX node
```

Record the RSSI the RX sketch prints. Then reflash production firmware (`esp32wroom`) for the reliability runs.

---

## Per-distance recording table

Copy this block for each distance / environment. Run at least 10–20 syncs per block.

### Block: Distance _____ m  |  Environment: ____________________

**Line of sight:** clear / light veg / dense veg / building / terrain  
**FieldHub antenna height:** _____ m  **orientation:** vertical / tilted / panel  
**Node antenna height:** _____ m  **orientation:** _______________  
**Vegetation/obstruction notes:** ____________________________________  
**Enclosure placement:** ____________________________________________  
**RSSI (range sketch):** _____ dBm  (measured separately)  
**Nodes competing this window:** _____  

| # | Time | Full sync? | HELLO RX | Snapshot RX | SNAP-ACK persisted | CONFIG_ACK | Retries | DUMP_DONE | Session dur (s) | Notes |
|---|------|-----------|----------|-------------|---------------------|------------|---------|-----------|-----------------|-------|
| 1 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 2 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 3 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 4 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 5 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 6 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 7 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n-a|         | Y / N     |                 |       |
| 8 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 9 |      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 10|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 11|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 12|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 13|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 14|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 15|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 16|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 17|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 18|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 19|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |
| 20|      | Y / N     | Y / N    | Y / N       | Y / N               | Y / N / n/a|         | Y / N     |                 |       |

**Block summary:**
- Successful full syncs: ___ / ___  ( ___ % )
- Consecutive missed syncs (max run): ___
- Snapshots removed before durable ACK? Y / N   (check `[DUMP] retaining seq=N` — if seen, snapshots were NOT removed before ack, which is correct)
- Config round-trips complete? Y / N / n/a
- Full fleet finished within sync window? Y / N
- Battery (mothership `[SNAP] bat=`): _____ V
- Notes: ________________________________________________________

---

## Progressive test sequence

### 1. Close-range baseline (e.g. 5–10 m, clear LOS)
- Establish that the protocol works end-to-end at close range.
- Confirm: HELLO → grant → snapshot → SNAP-ACK → DUMP_DONE → RELEASE → CONFIG_ACK all appear.
- Record 10 syncs as Block A.

### 2. Increase distance until failures begin
- Suggested steps: 25 m, 50 m, 75 m, 100 m, 150 m, 200 m.
- At each step: quick RSSI sweep (range sketch), then 10–20 production syncs.
- Note the distance where the first missed sync, first retry, or first `[DUMP] link failure` appears.

### 3. Boundary probing (outward and inward)
- Around the failure boundary, run 20 syncs at +10 m and −10 m.
- The "repeatable worst-case range" is the furthest distance where ≥95% of 20 syncs complete fully.

### 4. Realistic vegetation and obstruction
- Repeat the boundary distance under:
  - light vegetation (grass / scattered shrubs)
  - dense vegetation (hedge / trees)
  - building wall (one room / through wall)
  - terrain (slight hill / depression)
- Record each as a separate block.

### 5. Multi-node at deployment density
- Place 3 nodes at the boundary distance (or slightly inside).
- Run 20 syncs with all 3 waking in the same window.
- Check: does every node get a grant? Does any node starve? Does the fleet finish within 105 s?
- Record: responders vs deployed per sync (`[SYNC] rendezvous closed: responders=N deployed=M`).

### 6. Boundary node across multiple wake cycles
- Leave one node at the boundary distance running on its normal wake interval.
- Check it across at least 3–5 wake cycles (not just one session).
- Record: did it converge every time? Any consecutive misses?

---

## Provisional "reliable" definition

A distance/environment is **reliable** if ALL of:
- ≥ 95% complete sync success (HELLO → snapshot → persisted ACK → DUMP_DONE)
- No repeated consecutive misses (max 1 miss in a row)
- No snapshots removed before durable acknowledgement (`[DUMP] retaining seq=N` on failure is correct; queue should never drop silently)
- Configuration round trips complete (CONFIG_ACK converged if a NODE_CONFIG was broadcast)
- The full fleet finishes within the 105 s sync window

**Published deployment range = 70–80% of the measured worst-case repeatable reliable range.**

---

## Results → output guidance

After the test, fill in this table and send it back:

| Environment | Reliable distance (m) | RSSI (dBm) | Success % | Max consecutive misses | Fleet fits window? | Recommended FieldHub spacing (m) |
|-------------|----------------------|------------|-----------|------------------------|--------------------|----------------------------------|
| Clear LOS   |                      |            |           |                        |                    |                                  |
| Light veg   |                      |            |           |                        |                    |                                  |
| Dense veg   |                      |            |           |                        |                    |                                  |
| Building    |                      |            |           |                        |                    |                                  |
| Terrain     |                      |            |           |                        |                    |                                  |

Plus record:
- Recommended FieldHub mounting height: _____ m
- Warning signs another FieldHub is required: (consecutive misses > 1, RSSI < _____ dBm, fleet exceeds window)