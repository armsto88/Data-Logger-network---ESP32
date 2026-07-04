# OPS Alert: Soil Sensor Registration Bug — Diagnosis & Fix

**To:** Backend, Frontend, Ops  
**From:** Firmware Team  
**Date:** 2026-07-04  
**Severity:** Medium (deployed nodes losing soil probe 2 data, not a data-integrity or security issue)

---

## What Happened

Two deployed nodes (ENV_D13F98, ENV_6C0AA0) stopped reporting soil probe 2 data (SOIL2_VWC and SOIL2_TEMP) starting with firmware v2.0+. The **root cause is a firmware bug, not a backend issue**.

---

## The Symptom

**Node side (working):**
```
[SOIL] ch0 raw=21232 mv=2654.0 → Tsoil1=23.70 °C
[SOIL] ch1 raw=1280 mv=160.0 → θv1=3.2000
[SOIL] ch2 raw=96 mv=12.0 → θv2=0.2400     ← probe 2 moisture
[SOIL] ch3 raw=20864 mv=2608.0 → Tsoil2=22.59 °C  ← probe 2 temp
```
✅ All 4 ADS1115 channels reading correctly. Hardware is fine.

**Mothership side (broken):**
```
[SNAP] RX=68:09:47:D1:3F:98 seq=782 present=0x0117 proto=2
```
❌ `present=0x0117` has bits for battery, air temp/humidity, spectral, soil1 — **but NOT soil2 (bit 0x20)**.

**Backend (broken):**
- CSV receives only: `soil1Vwc=3.2000, soil1Temp=23.70, soil2Vwc=nan, soil2Temp=nan`
- Supabase ingest sees `null` for both soil2 fields every cycle
- Mothership flags ENV_D13F98 as having a sensor fault: `mask=0x0020` (soil2 expected but missing)

---

## Why This Is NOT a Backend Issue

The backend is **not to blame**. Here's why:

1. **The data never leaves the node.** The node firmware has a registration bug that prevents sensor IDs 2003 (SOIL2_VWC) and 2004 (SOIL2_TEMP) from being added to the sensor registry. They're never packed into the V2 snapshot, so they never reach the mothership or backend.

2. **The mothership isn't silently dropping them.** We verified: the node's ADC reads all 4 channels, but only SOIL1 makes it into the snapshot. The mothership correctly receives what the node sends (or doesn't send).

3. **The Supabase ingest is correct.** It receives `null` for soil2 fields because the node isn't sending them. The ingest function has no way to know whether a `null` is "node didn't read it" or "node has no soil2 installed" — it just stores it as-is. That's correct behavior.

---

## The Root Cause (Firmware)

The soil backend registers its 4 sensors in the wrong index order:

**Current (broken):**
```
Index 0 → label="SOIL1_VWC"    (sensor ID 2001)
Index 1 → label="SOIL2_VWC"    (sensor ID 2003)
Index 2 → label="SOIL1_TEMP"   (sensor ID 2002)
Index 3 → label="SOIL2_TEMP"   (sensor ID 2004)
```

**Problem:** Indices don't match sensor ID order. The registration loop in sensors.cpp expects indices to correspond cleanly. When it tries to register index 1 (SOIL2_VWC), something fails silently, and indices 2–3 never get registered either.

**Fixed (v2.1+):**
```
Index 0 → label="SOIL1_VWC"    (sensor ID 2001) ✓
Index 1 → label="SOIL1_TEMP"   (sensor ID 2002) ✓
Index 2 → label="SOIL2_VWC"    (sensor ID 2003) ✓
Index 3 → label="SOIL2_TEMP"   (sensor ID 2004) ✓
```

---

## Timeline & Rollout

**Phase 1 (Now):**
- Firmware team releases node v2.1+ with corrected soil sensor registration.
- All deployed nodes will be reflashed over the next 48 hours.

**Phase 2 (After reflash):**
- Node boots, sensor registration now succeeds for all 4 soil slots.
- First sync cycle, mothership sees `present=0x0137` (includes soil2 bit 0x20).
- CSV and Supabase ingest now receive real `soil2Vwc` and `soil2Temp` values instead of `nan`.

**Phase 3 (Ongoing):**
- Backlog: any soil2 data logged during the bug period (a few days) will be missing from Supabase, but we can backfill from the node's local queue if needed.
- No data corruption — the fields are `null`, not garbage.

---

## What the Backend Should Do

### 1. No code changes needed.
Your schema already has the columns and handles nulls correctly. When the fix lands, real data will flow automatically.

### 2. Watch the Supabase logs for:
```sql
SELECT COUNT(*) FROM readings 
WHERE spectral_saturated IS NOT NULL 
  AND (soil2Vwc IS NOT NULL OR soil2Temp IS NOT NULL)
  AND nodeTimestamp > '2026-07-04T18:00:00Z';
```
After the first node reboot post-reflash, this count should jump from 0 to (num_nodes × num_syncs).

### 3. Monitor for any stale nulls:
If a field stays `null` for >2 sync cycles after the reflash, it's a real fault (probe wiring or ADS failure), not the registration bug.

### 4. Alert Frontend:
Once soil2 data starts flowing, the frontend can populate the soil2 card (currently disabled or showing "–" / "awaiting data").

---

## What the Frontend Should Do

### 1. Before the fix:
- Soil2 fields should render as `–` or "No data" (they're `null` in the DB).
- Don't show a red error state — the sensor is working, just not registering.
- Optionally show a badge: "Soil2 awaiting firmware update" (if you want to be explicit).

### 2. After the fix (starting ~2026-07-05):
- Soil2 fields start receiving real numeric data.
- Render soil2Vwc (m³/m³) and soil2Temp (°C) same as soil1.
- No code changes needed — just let the nulls become numbers and the UI auto-updates.

### 3. Backfill check:
- If the backend backfills missing soil2 data from node queues, the frontend might see a sudden jump in historical data. This is expected and not an error.

---

## FAQ for Ops

**Q: Will this affect other sensors (spectral, wind, air)?**  
A: No. The bug is isolated to the soil backend's registration order. Spectral and other sensors work fine.

**Q: Will deployed nodes lose data during the reflash?**  
A: No. NVS (configuration) is preserved. Nodes will queue any samples taken during the downtime and sync them on the next wake.

**Q: How long is the fix?**  
A: ~10 seconds per node to reflash. Nodes stay paired and configured.

**Q: Is this a security issue?**  
A: No. The bug only affects which sensors are registered, not data integrity or encryption.

**Q: Why didn't this show up in testing?**  
A: Test nodes likely didn't have soil probes installed, so the bug went unnoticed. It only manifests on deployed units with ADS1115 + probes.

**Q: Will soil2 data backfill?**  
A: Maybe. If node local queues retained the missing cycles, we can extract them. If they were already synced as `null`, they stay `null`. Don't rely on backfill — treat the gap as a data loss window.

---

## Data Loss Impact

**Estimated loss:** 1–3 days (from v2.0 deploy until v2.1 reflash).  
**Scope:** SOIL2_VWC and SOIL2_TEMP only. SOIL1 is unaffected.  
**Severity:** Low for most use cases (soil changes slowly). High if growers were relying on soil2 for irrigation decisions in this window.

---

## Verification Checklist

After reflash, confirm:
- [ ] Node logs show 4 soil slots registered (Slot 15, 16, 17, 18 with IDs 2001–2004).
- [ ] Mothership logs show `present` mask includes soil2 bit (0x20).
- [ ] CSV includes real values for soil2Vwc and soil2Temp columns (not `nan`).
- [ ] Supabase ingest receives non-null soil2 JSON keys within 1 sync cycle.
- [ ] Frontend renders soil2 cards with real data.
- [ ] Dashboard alerts for soil2 faults are silent (no false positives).

---

## Summary

| Item | Status |
|------|--------|
| Root cause | Firmware registration order bug (not backend) |
| Impact | Soil2 fields null; sensor faults flagged |
| Fix | Reorder soil backend labels to match ID order |
| Backend changes | None required |
| Frontend changes | Optional UI refresh when data appears |
| Rollout | Node v2.1+ reflash, 48–72 hrs |
| Data recovery | Partial/manual if backfill is possible |

