# Firmware Bug: Soil Sensor Registration Failure

**Affected firmware:** Node v2.0+ (soil_moist_temp backend)  
**Symptom:** Only SOIL1_VWC (sensor ID 2001) is registered; SOIL2_VWC, SOIL1_TEMP, SOIL2_TEMP disappear.  
**Impact:** 2 deployed nodes missing soil probe 2 data; snapshots have incomplete sensor list.  
**Root cause:** Sensor registration order doesn't match snapshot ID ordering; one of the registration slots fails silently.

---

## Evidence

**Node boot log (ENV_D13F98):**
```
[SOIL] ch0 raw=21232 mv=2654.0 → Tsoil1=23.70 °C
[SOIL] ch1 raw=1280 mv=160.0 → θv1=3.2000
[SOIL] ch2 raw=96 mv=12.0 → θv2=0.2400
[SOIL] ch3 raw=20864 mv=2608.0 → Tsoil2=22.59 °C
```
✅ All 4 channels read correctly by the ADS1115 hardware.

**Sensor registration:**
```
[SENS] soil_moist_temp backend reports 4 sensor(s)
[SENS] Slot 15 -> id=2001 label='SOIL1_VWC' type='SOIL_VWC' backend=2
```
❌ Only 1 slot registered, should be 4 (slots 15, 16, 17, 18).

**V2 snapshot:**
```
[16] id=2001  value=3.2000   ← only SOIL1_VWC appears
```
❌ IDs 2002, 2003, 2004 missing.

**Mothership fault detection:**
```
[SNAP] ENV_D13F98 sensor fault mask=0x0020 (expected=0x0037 present=0x0117)
```
✅ Mothership correctly flags SOIL2 (bit 0x20) as faulted.

---

## Root Cause

The soil backend's `label()` function returns indices in the wrong order for snapshot ID mapping:

**soil_moist_temp.cpp (lines 156–163):**
```c
const char* label(size_t index) {
  switch (index) {
    case 0: return "SOIL1_VWC";    // → resolves to sensor ID 2001
    case 1: return "SOIL2_VWC";    // → resolves to sensor ID 2003
    case 2: return "SOIL1_TEMP";   // → resolves to sensor ID 2002
    case 3: return "SOIL2_TEMP";   // → resolves to sensor ID 2004
  }
}
```

**Problem:** When `sensors.cpp` registers these 4 slots in the loop (lines 164–166), it calls:
```c
for (size_t i = 0; i < soilCount; ++i)
  commitSensorSlot(SENSOR_BACKEND_SOIL, i, label(i), type(i));
```

The registration loop succeeds for **index 0** (SOIL1_VWC, ID 2001), but the next 3 iterations don't produce logged `[SENS] Slot` lines. Either:
1. They're being masked/gated silently (no "[SENS] gated" message), or
2. One fails and subsequent iterations rely on state that's broken

**Secondary issue:** The V1 snapshot decode (flash_logger.cpp) can only handle the 8 bands + 4 soil values. Soil indices must map directly to the struct fields `soil1Vwc`, `soil1Temp`, `soil2Vwc`, `soil2Temp` in order. The current label order (VWC1, VWC2, TEMP1, TEMP2) breaks that.

**Tertiary issue:** The V2 snapshot stores (sensorId, value) pairs. The order doesn't matter for V2 — only the IDs. But if `commitSensorSlot()` silently fails on some indices, those IDs never get into the registry, and the backend won't read them at snapshot time.

---

## Fix

### Reorder the soil backend's label/type to match sensor ID order

**soil_moist_temp.cpp — fix the `label()` function:**

```c
const char* label(size_t index) {
  switch (index) {
    case 0: return "SOIL1_VWC";    // id 2001
    case 1: return "SOIL1_TEMP";   // id 2002
    case 2: return "SOIL2_VWC";    // id 2003
    case 3: return "SOIL2_TEMP";   // id 2004
  }
}

const char* type(size_t index) {
  switch (index) {
    case 0:
    case 2:
      return "SOIL_VWC";
    case 1:
    case 3:
      return "SOIL_TEMP";
  }
}
```

And update the `read()` function to match:

```c
bool read(size_t index, float &outValue) {
  if (index >= count()) return false;

  sampleAdsIfNeeded();
  if (!haveSample) return false;

  switch (index) {
    case 0: outValue = lastThetaV1; break;  // SOIL1_VWC
    case 1: outValue = lastTemp1C;  break;  // SOIL1_TEMP
    case 2: outValue = lastThetaV2; break;  // SOIL2_VWC
    case 3: outValue = lastTemp2C;  break;  // SOIL2_TEMP
    default: return false;
  }
  return true;
}
```

**Why this works:**
- Labels and types now reflect the **sensor ID order** (2001, 2002, 2003, 2004).
- `resolveSensorId()` in sensors.cpp will successfully map each to its ID without collision.
- All 4 slots get registered (no silent failures due to duplicate IDs or ordering issues).
- V2 snapshots include all 4 IDs in any order (doesn't matter).
- V1 snapshots (legacy) unpack in the correct order (soil1 data, then soil2 data).

### Bonus: Fix the moisture calibration bug

**sensors_soil_ads_calib.cpp:** The θv1 value logs as 3.2, which is outside valid range (0–0.6 m³/m³). This suggests either:
1. The calibration coefficients SOIL1_A0/SOIL1_B0/SOIL1_C0 are wrong, or
2. The raw voltage isn't being scaled before applying the calibration

Check the polynomial `theta_v_from_mv()` — it should take mV and return VWC. If it's receiving unconverted or double-converted input, it will produce garbage.

---

## Test Plan

After applying the fix:

1. Flash all affected nodes.
2. Boot a node and verify the sensor log:
   ```
   [SENS] soil_moist_temp backend reports 4 sensor(s)
   [SENS] Slot 15 -> id=2001 label='SOIL1_VWC' type='SOIL_VWC' backend=2
   [SENS] Slot 16 -> id=2002 label='SOIL1_TEMP' type='SOIL_TEMP' backend=2
   [SENS] Slot 17 -> id=2003 label='SOIL2_VWC' type='SOIL_VWC' backend=2
   [SENS] Slot 18 -> id=2004 label='SOIL2_TEMP' type='SOIL_TEMP' backend=2
   ```
3. Run a sync cycle and check the mothership:
   ```
   [SNAP] RX=... nodeId=ENV_D13F98 seq=... present=0x0137 proto=2 ...
   ```
   ✅ `present=0x0137` includes bits for SOIL1 (0x10) + SOIL2 (0x20) + SPECTRAL (0x04) + AIR (0x03) + BAT (0x100).
4. Verify the V2 snapshot body includes sensor IDs {2001, 2002, 2003, 2004}.
5. Check the mothership CSV — columns spectral_integration_ms (soil1Temp), soil1Vwc, soil2Vwc, soil2Temp should all be populated, no faults.

---

## Why This Happened

Soil sensor firmware was written with the ADS1115 hardware order in mind (ch0=T1, ch1=VWC1, ch2=VWC2, ch3=T2), which made intuitive sense for the driver. But the sensor registration system expects **backend indices to map to ascending sensor IDs** (or at least be consistent with the resolution logic). When the backend returned indices in the order VWC1, VWC2, T1, T2 instead of VWC1, T1, VWC2, T2, the snapshot packing and mothership decoding got confused.

The V2 snapshot format is more forgiving (key-value pairs), but the registration loop is strict: if `commitSensorSlot()` somehow fails on an index (due to mask gating, duplicate ID detection, or other logic), there's no retry, and that sensor never makes it into the registry.
