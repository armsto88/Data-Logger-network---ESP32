# Sensor Calibration & Validation Documentation

This directory contains peer-review frameworks and calibration protocols for the FieldMesh data logger sensors, grounded in scientific literature on sensor accuracy, precision, and field validation.

## Documents

### 1. [CALIBRATION_VALIDATION_SCIENTIFIC_FOUNDATION.md](CALIBRATION_VALIDATION_SCIENTIFIC_FOUNDATION.md)
**Start here.** A survey of scientific principles for sensor calibration and validation.

**Covers:**
- Pre-deployment vs. post-deployment calibration strategies
- Accuracy vs. precision definitions
- Environmental sensor inter-comparison methods
- Sensor drift drivers (temperature, humidity, UV, chemistry, stress)
- In-situ recalibration algorithms for sensor networks
- Data quality & outlier detection
- Peer review checklist

**Key insight:** Laboratory calibration alone is insufficient. Field validation against reference standards is essential.

---

### 2. [SOIL_MOISTURE_CALIBRATION_PROTOCOL.md](SOIL_MOISTURE_CALIBRATION_PROTOCOL.md)
**For soil moisture sensors** (ADS1115 + capacitive probes).

**Phases:**
1. **Pre-deployment (lab):** Dry-down calibration curves for site-representative soil samples
2. **Field site survey:** Soil texture/density characterization; matching to lab curves
3. **On-site field calibration:** Gravimetric validation or reference sensor cross-check
4. **Drift monitoring:** Monthly checks; outlier filtering; temperature correction
5. **Publication:** Quality claims + supporting data

**Expected accuracy:** ±0.03–0.05 m³/m³ (3–5% VWC) with proper field calibration.

**Key finding from literature:** Field-calibrated sensors achieve **50% better accuracy** than lab-only calibration.

---

### 3. [SPECTRAL_SENSOR_CALIBRATION_PROTOCOL.md](as7341/SPECTRAL_SENSOR_CALIBRATION_PROTOCOL.md)
**For the AS7341 spectral sensor** (8 visible bands + Clear + NIR).

**Phases:**
1. **Dark current baseline:** Offset calibration across all 11 gain settings
2. **Linearity validation:** Test ADC response across full intensity range
3. **Gain validation:** Verify auto-gain algorithm maintains consistency
4. **Spectral response:** Color accuracy and cross-channel crosstalk
5. **Field deployment:** PAR proxy validation against calibrated reference
6. **Thermal stability:** Temperature coefficient measurement & correction
7. **Snapshot validation:** Rules for flagging saturated/invalid/low-signal data
8. **Multi-node cross-check:** Consistency across deployed units

**Expected accuracy:** PAR estimates ±100 µmol m⁻² s⁻¹ (R² >0.85 vs. reference meter).

---

## How to Use These Documents

### For Initial Deployment
1. Read **CALIBRATION_VALIDATION_SCIENTIFIC_FOUNDATION.md** (20 min)
2. Follow the appropriate sensor protocol:
   - Soil sensors → **SOIL_MOISTURE_CALIBRATION_PROTOCOL.md**
   - Spectral sensor → **SPECTRAL_SENSOR_CALIBRATION_PROTOCOL.md**
3. Use the **peer review checklists** at the end of each protocol

### For Data Quality Assurance
- Reference the **drift monitoring** and **outlier detection** sections during deployment
- Compare observed sensor behavior against expected performance ranges documented
- Re-calibrate if drift exceeds thresholds (typically 5% per 30 days)

### For Publishing or Sharing Data
- Fill in the **data reporting template** from the protocol you used
- Cite the scientific papers listed (links provided)
- Attach calibration curves, validation plots, and raw metadata

---

## Scientific Foundation & References

All protocols are based on peer-reviewed literature:

### General Calibration
- [Sensor Calibration Overview (ScienceDirect Topics)](https://www.sciencedirect.com/topics/earth-and-planetary-sciences/sensor-calibration)
- [Framework for Simulation of Sensor Networks — In Situ Calibration Algorithms (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC7472635/)
- [Self-Calibration Methods for Uncontrolled Environments (arXiv)](https://arxiv.org/pdf/1905.11060)

### Environmental Sensor Accuracy
- [EPA: Precision and Accuracy Study](https://www.epa.gov/sites/default/files/2014-08/documents/precisionandaccuracy.pdf)
- [Challenges and Opportunities in Calibrating Low-Cost Environmental Sensors (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC11175279/)
- [Low-Cost Aerosol Sensor Inter-comparison (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC5580827/)
- [Electrical Conductivity Sensor Accuracy Study (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC10159144/)

### Soil Moisture Sensors
- [Calibration and Field Validation of Smart Soil Moisture Monitoring (Harvard ADS)](https://ui.adsabs.harvard.edu/abs/2023EGUGA..2511699Y/abstract)
- [Laboratory Calibration and Field Validation — 5TE Sensor (PMC)](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC6928628/)
- [Designing and Field Calibration of Low-Cost Microcontroller-Based Sensors (Nature Scientific Reports)](https://www.nature.com/articles/s41598-024-81288-z)
- [Low-Cost Capacitive Soil Moisture Sensors for Irrigation (MDPI)](https://www.mdpi.com/1424-8220/25/2/343)
- [Calibration and Validation of Soil Water Reflectometers (Wiley)](https://acsess.onlinelibrary.wiley.com/doi/full/10.1002/vzj2.20190)

### Spectral/Optical Sensors
- [Dynamic Validation of Calibration Accuracy — Multi-Sensor Systems (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC11207938/)

### Environmental Drift
- [Environmental Stressors Causing Calibration Drift (Clarity.io Blog)](https://www.clarity.io/blog/which-environmental-stressors-most-often-trigger-calibration-drift-and-necessitate-shorter-calibration-intervals)

---

## Quick Reference: Calibration Timeline

| When | Action | Sensor Type | Evidence |
|------|--------|-------------|----------|
| **Before deployment** | Lab dry-down calibration | Soil | Calibration curve + R² |
| **Field site prep** | Soil characterization | Soil | Texture, bulk density, organic matter |
| **Week 1–2** | Baseline reference check | All | First gravimetric / reference measurement |
| **Weekly** | Auto-validation | Spectral | PAR proxy vs. reference meter (if available) |
| **Monthly** | Drift assessment | All | Re-measure reference samples or co-located sensor |
| **Quarterly** | Re-calibrate if drift >5% | All | Update coefficients; recapture baseline |
| **End of season** | Final accuracy report | All | Compare all readings to reference; summarize performance |

---

## Compliance & Quality Flags

Every sensor reading should be assigned a **quality flag**:

```
[VALID]              — all checks passed, use with confidence
[UNVALIDATED]        — not yet checked against reference, provisional
[DRIFT_WARNING]      — drift detected but <10%; monitor closely
[OUT_OF_RANGE]       — value physically impossible; reject
[SATURATED]          — sensor maxed out (especially spectral); data unreliable
[LOW_SIGNAL]         — noise-dominated; low confidence
[RECALIBRATED]       — fresh calibration applied; baseline reset
[REFERENCE_MISMATCH] — doesn't match reference sensor; investigate
```

Include these flags in your data export so downstream analysis knows data provenance.

---

## Next Steps

1. **Soil moisture:** Complete Phase 1 (lab calibration) before field deployment. Plan Phase 3 (field validation) for first 2 weeks on site.

2. **Spectral sensor:** Phase 1–2 can happen in a lab or bright room. Phase 5 (field validation) requires 1–2 weeks of co-location with a PAR meter (borrow one if needed).

3. **Air temperature/humidity (SHT41):** Low-cost, already factory-calibrated to ±2–3%. Skip extensive re-calibration; just verify no physical damage to sensor package before deployment.

4. **Wind (reed anemometer):** Validate against weather station wind data if available. Anemometer accuracy typically ±5% (factory spec); field comparison is optional.

---

## Questions or Improvements?

These protocols are **living documents**. As you complete calibrations and field validation, update them with:
- Actual results (photos of calibration setups, plots)
- Lessons learned (unexpected sensor behaviors, drift patterns)
- Improvements to procedures (what worked, what didn't)

Share findings with other deployments and the broader FieldMesh community.

---

**Document version:** 1.0  
**Date created:** 2026-07-04  
**Author:** Claude Code (based on peer-reviewed scientific literature)  
**Next review:** After first field season data is collected
