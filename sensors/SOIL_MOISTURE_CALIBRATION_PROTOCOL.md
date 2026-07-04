# Soil Moisture Sensor Calibration Protocol

**Target sensors:** ADS1115 + capacitive moisture & temperature probes (VWC + temp)  
**Scientific basis:** [Field Calibration Study (Nature)](https://www.nature.com/articles/s41598-024-81288-z), [5TE Validation (PMC)](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC6928628/)

---

## Phase 1: Pre-Deployment Calibration (Lab)

### Equipment Needed
- Reference soil samples (3–5 varieties: clay, loam, sandy loam, sand)
- Oven (105°C capable)
- Scales (±0.1g precision)
- Your ADS1115 + sensor setup (at target mounting angle)
- Deionized water
- Temperature-controlled environment (±2°C)

### Procedure: Dry-down Calibration Curve

1. **Baseline (dry):** Oven-dry soil at 105°C for 24h. Record **zero-point ADC reading** for each sensor at each soil type.
   
2. **Moisture increments:** Add water in steps (e.g., 5%, 10%, 15%, ... 35% volumetric water content).
   - **Method:** Gravimetric calculation: `VWC = (mass_water / mass_soil) × (soil_density / water_density)`
   - For each step, equilibrate soil in container for 30 min, then record ADC output + temperature.
   
3. **Temperature sweep:** At each moisture level, measure ADC output at 15°C, 25°C, 35°C. Record T with each reading.

4. **Fit calibration model** (example for one soil type):
   ```
   VWC = a₀ + a₁×ADC + a₂×ADC² + a₃×T + a₄×ADC×T
   ```
   Use least-squares regression to find coefficients.

### Deliverable
- One calibration curve per soil type (plot ADC vs. VWC, residuals shown)
- Fit uncertainty (R², RMSE in m³/m³)
- Temperature correction coefficients

---

## Phase 2: Field Site Survey (Before Deployment)

### Soil Sample Collection
1. At deployment site, collect **3–5 undisturbed soil cores** from the top 10–20 cm (sensor depth).
2. For each core, measure:
   - **Texture** (% sand/silt/clay by sedimentation or triangle diagram)
   - **Bulk density** (gravimetric method on core: dry mass / core volume)
   - **Organic matter** (loss on ignition method, or estimate from color)

3. **Classify soil type** using USDA triangle. Cross-reference to your lab calibration samples.

### Objective
Determine which lab calibration curve best matches your deployment soil. If no close match, plan an **on-site calibration** (see Phase 3).

---

## Phase 3: On-Site Field Calibration

### Setup
Deploy sensor in natural soil at target location. Use a **reference method** to ground-truth sensor readings.

### Option A: Gravimetric Validation (Low-cost, manual)
**Every 2–3 weeks**, collect soil samples adjacent to sensors:
1. Extract soil core at sensor depth
2. Weigh wet core immediately (prevent evaporation)
3. Oven-dry, re-weigh
4. Calculate true VWC
5. Record your sensor ADC reading at time of sampling
6. Plot **sensor reading vs. true VWC** — should align with lab curve; if not, adjust coefficients

**Timeline:** Collect 8–10 paired samples over the deployment season (wet and dry periods).

### Option B: Dual-Sensor Cross-validation (Higher confidence)
If budget allows, co-deploy a **calibrated reference sensor** (e.g., 5TE, CS655) alongside your ADS1115.
- Record both outputs daily for 2 weeks
- Plot correlation scatter: (your sensor) vs. (reference)
- R² should be >0.95; if <0.90, recalibrate

### Option C: TDR Soil Moisture Probe (Gold standard)
Measure soil volumetric water content with a TDR probe at sensor location once per month. Highest accuracy but expensive.

---

## Phase 4: Data Processing & Drift Monitoring

### Real-time Outlier Rejection
1. **Range check:** Reject VWC outside [0.0, 1.0]
2. **Rate filter:** Reject changes >0.15 m³/m³ per 60-min interval (unless heavy rain)
3. **Cross-sensor check:** If operating multiple nodes, flag if one deviates >0.1 from peers for >3 hours

### Drift Detection
Every 30 days:
1. Plot VWC from all nodes over the last month
2. Look for gradual trends (gain drift) or step changes (calibration shift)
3. If any sensor drifts >5% from baseline, perform gravimetric validation or re-calibrate

### Temperature Compensation
Before reporting VWC:
```
VWC_corrected = VWC_raw + T_correction_factor × (T_measured - 25°C)
```
Ensure all reported data includes temperature at time of measurement.

---

## Phase 5: Publication/Peer Review

### Claims to Support

**Claim:** "Soil moisture measurements are accurate to ±X m³/m³"

**Supporting evidence:**
- ✅ Lab calibration curves (one per soil type, R² values shown)
- ✅ Field site soil characterization (texture, bulk density)
- ✅ Gravimetric validation data (N samples, correlation plot)
- ✅ Drift assessment (monthly readings over deployment period)
- ✅ Temperature correction applied? Quantified?

### Data Reporting Template

```
Soil Moisture Measurement Quality

Sensor: ADS1115 + [model soil probe]
Deployment site: [location], soil type = [loam/clay/etc.]
Calibration method: Lab curve (soil type matched) + field validation
Reference standard: Gravimetric sampling (N=10 over 3 months)

Accuracy:
  - Lab calibration RMSE: ±0.031 m³/m³
  - Field validation (vs. gravimetric): R²=0.93, RMSE=0.035 m³/m³
  - Operational accuracy: ±0.05 m³/m³ (at 95% confidence)

Drift monitoring:
  - Baseline (day 1): [ADC reading at known reference point]
  - 30-day drift: ±2%
  - 90-day drift: ±5%
  - Recalibration applied: [date]

Temperature compensation: Yes, T-dependent model fitted
Range: 10–40°C, correction factor = [value] per °C
```

---

## Troubleshooting

### Problem: Lab calibration doesn't match field data
**Cause:** Soil type mismatch or field soil compaction/structure differs.  
**Fix:** Collect undisturbed core from deployment site, repeat lab dry-down on that exact soil.

### Problem: Temperature effects are large (>10% VWC error)
**Cause:** Sensor dielectric response is T-dependent.  
**Fix:** Fit higher-order T correction (quadratic) or use soil-specific T coefficients from literature.

### Problem: Sensor drifts >10% over a month
**Cause:** Sensor aging, electrical drift in ADS1115, or salt accumulation in soil.  
**Fix:** Check reference gravimetric sample at same time. If reference is stable, re-calibrate ADC. If both drift, suspect soil salinity change.

### Problem: Two identical sensors in same soil read differently
**Cause:** Manufacturing variation, different soil contact, or cable capacitance.  
**Fix:** Swap sensor probes in soil; if readings swap, it's sensor variation. Apply **individual calibration** to each unit.

---

## References & Further Reading

- [Calibration and Field Validation of Soil Water Content and Salinity Measurements Using the 5TE Sensor](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC6928628/)
- [Designing and Field Calibration of Low-Cost Microcontroller-Based Soil Moisture Sensors](https://www.nature.com/articles/s41598-024-81288-z)
- [Calibration of Low-Cost Capacitive Soil Moisture Sensors for Irrigation Management](https://www.mdpi.com/1424-8220/25/2/343)

---

## Compliance Checklist

Before publishing soil moisture data:

- [ ] Lab calibration curves generated (R² >0.90)
- [ ] Field soil type identified and matched to lab curve
- [ ] Gravimetric validation completed (≥8 paired samples)
- [ ] Drift monitored monthly; all drifts <5% over 90 days
- [ ] Temperature compensation model fitted and tested
- [ ] Outlier/rate-of-change filters documented
- [ ] Measurement uncertainty quantified (RMSE in m³/m³ or %)
- [ ] Metadata logged: sensor ID, firmware version, calibration date, reference used
- [ ] Quality flags assigned: [VALID], [DRIFT_WARNING], [UNVALIDATED]

---

**Document status:** 2026-07-04  
**Next step:** Spectral sensor calibration protocol (AS7341 PAR)
