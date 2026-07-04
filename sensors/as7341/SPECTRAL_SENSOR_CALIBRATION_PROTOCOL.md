# AS7341 calibration, validation, and NIR integration protocol

**Sensor:** ams OSRAM AS7341, eight visible filters (F1-F8), Clear, and NIR

**Primary FieldMesh use:** outdoor photosynthetic photon flux density (PPFD/PAR), while retaining all raw channels

**Document status:** source review and implementation audit, 2026-07-04

## Executive conclusion

The AS7341 is not yet being used to its full potential. Its most defensible additional output is calibrated PPFD in `umol m-2 s-1`, followed by daily light integral (DLI) after time integration. Spectral balance or red/green/blue fractions can also be reported as carefully labelled indices. The sensor is not a spectrometer, cannot recover narrow spectral features reliably, and one upward-looking unit cannot produce vegetation indices such as NDVI without a second measurement of reflected light.

The present `PAR_PROXY` (the sum of F1-F8 raw counts) is only a brightness index. Counts change with gain, integration time, sensor-to-sensor variation, the diffuser, angle of incidence, and spectral composition. It must not be labelled PPFD.

The best calibration path supported by the supplied literature is:

1. freeze the complete optical head, including diffuser and enclosure;
2. retain F1-F8, Clear, NIR, applied gain, integration time, and validity for every exposure;
3. convert raw counts to gain/time-normalised basic counts and subtract measured offsets;
4. characterise linearity, saturation, angular response, temperature, repeatability, and unit-to-unit variation;
5. train a PPFD model using all ten channels against wavelength-resolved laboratory reference measurements;
6. validate on entire held-out spectral sweeps and then against an independent outdoor quantum sensor;
7. preserve raw data and calibration provenance beside every derived PPFD value.

If a monochromator laboratory is unavailable, a co-located quantum-sensor regression is useful, but it is an application- and spectrum-specific transfer calibration, not a universal spectral calibration.

## Source inventory and what each source contributes

### 1. ams OSRAM AN000633, *Calibration and Correction of Spectral Sensors*

Local file: [`caliberation_AS7341_AN000633_2-00.pdf`](caliberation_AS7341_AN000633_2-00.pdf)

This is the core manufacturer calibration note. Its important conclusions are:

- Raw readings must first be corrected for offset, gain, and integration time. The note calls the result **Basic Counts**.
- Filter overlap and out-of-band response mean that a channel is not a pure measurement at its nominal wavelength.
- F1-F8 have some NIR sensitivity. The NIR and Clear photodiodes can be used to estimate and remove this contamination.
- A diffuser is part of the calibrated optical system. Its spectral transmission and angular response cannot be ignored.
- Relevant disturbances include dark offset/noise, gain error, integration-time non-linearity, temperature, ageing, ambient light, and internal reflections.
- A stable calibration arrangement and a reference instrument covering approximately 350-1000 nm are recommended. The reference should ideally be about ten times more accurate than the required sensor result.
- A single scale factor works only close to the spectrum used to derive it. A regression matrix trained on representative, linearly independent spectra is more general.
- Individual-device calibration gives the best result. Batch calibration is a compromise; type or golden-device calibration has the largest residual unit error and should be combined with per-device scaling.

The note provides typical, not device-specific, NIR correction factors for F1-F8:

| Channel | F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Clear scaling | 1.48 | 1.87 | 1.85 | 1.92 | 1.79 | 2.00 | 1.92 | 2.32 |
| Typical NIR ratio | 4.47% | 5.51% | 5.31% | 6.15% | 3.91% | 7.01% | 5.31% | 4.28% |

These are suitable as a diagnostic starting point, not production calibration constants. The note explicitly warns that a typical correction can make an individual sensor worse, particularly under NIR-rich sources. Prefer a device-specific matrix or let a properly validated ten-channel PPFD model learn the correction.

### 2. AS7341 Evaluation Kit Manual UG000400

Local file: [`Datasheet_AS7341.pdf`](Datasheet_AS7341.pdf)

Despite its filename, this 66-page file is the **AS7341 Evaluation Kit Manual**, not the AS7341 silicon datasheet. The missing/deleted `AS7341.pdf` may have been the chip datasheet; do not treat this file as a complete register or electrical specification.

The manual reinforces that:

- directional ambient-light measurements require a diffuser;
- an approximately achromatic, volume-scattering Lambertian diffuser is preferable when source direction changes;
- the evaluation kit discusses Kimoto 100 PBU and OptSaver L-57 films, with L-57 providing the wider angular distribution;
- dirt, fingerprints, stress, reassembly, spacing, and enclosure changes alter the optical path and therefore invalidate calibration;
- calibration has two stages: correct offsets/drifts/device variation, then calibrate the complete device for the intended application;
- Clear and NIR may be included with the eight visible channels in a calibration matrix;
- derived lux, colour, or spectral values can be physically implausible when an inappropriate calibration matrix is used.

### 3. Bäumker et al. (2021), *A Novel Approach to Obtain PAR with a Multi-Channel Spectral Microsensor*

Local file: [`PAPERS/sensors-21-03390.pdf`](PAPERS/sensors-21-03390.pdf)

This paper demonstrates the basic viability of an AS7341 PPFD sensor:

- It used a fixed optical housing with an acrylic diffuser and glass dome, then calibrated the assembled head.
- Gain and integration time were fixed to avoid exposure-dependent coefficients: 100 ms, visible gain 4, NIR gain 1.
- Eight-band multiple linear regression against an LI-COR quantum sensor produced adjusted `R2 = 0.991` and RMSE near `16 umol m-2 s-1` in the reported dataset.
- A selected-channel model using F1, F3, F6, F8, and NIR improved the reported fit to adjusted `R2 = 0.994` and RMSE near `12 umol m-2 s-1`.
- Its outdoor calibration data were limited and biased toward lower PPFD, so the published coefficients should not be copied into FieldMesh.
- Its reduced light quality index shows that coarse red/green/blue fractions are possible, but overlapping broad filters bias the fractions, particularly green.

The paper is evidence that PPFD and broad light-quality indices are feasible, not that its regression coefficients are portable.

### 4. Klueppel et al. (2026), *Calibration of multi-spectral photosynthetically active radiation sensor*

Local file: [`PAPERS/1-s2.0-S0263224126004434-main.pdf`](PAPERS/1-s2.0-S0263224126004434-main.pdf)

This is the strongest and most directly applicable calibration study in the supplied set. It is the preferred model for FieldMesh:

- A quartz-tungsten-halogen source, monochromator, engineered diffuser, collimator, characterised beam splitter, and calibrated reference detector produced a uniform beam in a light-tight, blackened enclosure.
- Each AS7341 carried its final OptSaver diffuser during calibration.
- The authors swept 390-700 nm in 3 nm increments and 700-1100 nm in 5 nm increments. Five complete sweeps produced `5 x 185` observations containing wavelength, reference irradiance, all ten AS7341 channels, gain, and integration time.
- Measurements were corrected for detector area, beam-splitter transfer function, and the non-flat lamp spectrum, then converted from spectral irradiance to photon units.
- The ideal training target was zero outside 400-700 nm and photon response inside it.
- Partial least squares (PLS) regression used F1-F8, Clear, and NIR. Four whole sweeps were training data and the fifth was held out; cross-validation selected the latent-component count. Splitting complete sweeps avoids the leakage produced by randomly distributing adjacent wavelengths between train and validation sets.
- The final embedded model is computationally small: one coefficient per channel, applied to basic counts.
- Across four sensors, the full ten-channel model reproduced the ideal response with mean `R2 = 94.788%` (reported spread `+/-0.859%`). A visible-only model achieved only `79.858% +/-3.448%` because NIR contamination remained.
- Outdoor validation reported MAPE of 2.43% on a clear day and 7.36% under overcast conditions after excluding periods affected by moving shadows and unsynchronised sampling.
- Long-term effects of humidity, UV exposure, temperature, contamination, and ageing were not established.

The important NIR lesson is counter-intuitive: NIR is outside PAR, but its measurement is valuable because it tells the regression how much out-of-band response to subtract from the visible channels.

### 5. Rahman, Jamil, and Pearce (2025), *Open-Source Photosynthetically Active Radiation Sensor*

Local file: [`PAPERS/electronics-14-02225.pdf`](PAPERS/electronics-14-02225.pdf)

This paper provides a pragmatic field-transfer method:

- It used fixed AS7341 settings and regressed eight visible raw channels against an Apogee SQ-500SS reference.
- Separate calibrations were performed for grow lights, an agrotunnel, greenhouse, and agrivoltaic/outdoor conditions.
- The reported mean errors were roughly 1-5%, depending on environment.
- The fitted coefficients differ substantially between environments. This is evidence of spectral dependence, not a collection of universal constants.
- Clear and NIR were not included, so this approach does not explicitly correct out-of-band leakage.

This is the practical fallback when wavelength-resolved calibration is unavailable: co-locate with a good reference, sample the complete operational light range, fit locally, and validate on different days. A model trained under one grow lamp must not silently be applied to sunlight or a different lamp.

### 6. Liu et al. (2025), *Advances in Research and Application of Techniques for Measuring PAR*

Local file: [`PAPERS/remotesensing-17-01765.pdf`](PAPERS/remotesensing-17-01765.pdf)

This review places the device work in a broader measurement context. The recurring field errors are spectral mismatch, imperfect cosine response and Fresnel effects at high incidence angles, temperature and calibration drift, water ingress, contamination, and UV ageing. The review supports low-cost multi-spectral sensors, but calls for standardised procedures, cross-sensor comparisons, explicit uncertainty, and periodic laboratory and field checks. There is no universal reference-free shortcut.

### 7. Tonelli et al. (2025), *A Portable and Low-Cost Single Board Computer-Based Spectrophotometric Platform*

Local file: [`PAPERS/Advanced Sensor Research - 2025 - Tonelli - A Portable and Low-Cost Single Board Computer-Based Spectrophotometric Platform.pdf`](PAPERS/Advanced%20Sensor%20Research%20-%202025%20-%20Tonelli%20-%20A%20Portable%20and%20Low%E2%80%90Cost%20Single%20Board%20Computer%E2%80%90Based%20Spectrophotometric%20Platform.pdf)

This is a transmission/absorbance instrument rather than an outdoor PAR sensor, so its application coefficients are not transferable. Its useful metrology lessons are:

- lock the source, diffuser, sample, and sensor into repeatable geometry;
- measure source linearity before assuming detector linearity;
- operate comfortably below full scale (the paper used about 20%);
- quantify repeatability and multi-hour drift rather than relying on a single reading;
- use explicit dark and blank/reference subtraction;
- validate against an independent commercial reference and report detection limits where relevant.

It also illustrates the limitation of the AS7341 NIR channel: one broad NIR diode is a correction/intensity feature, not NIR spectroscopy.

## Quantities and equations

### Integration time and full scale

For the AS7341 settings used by the current V2 firmware:

```text
ATIME = 29
ASTEP = 599
t_int_ms = (ATIME + 1) * (ASTEP + 1) * 2.78 us / 1000
         = 50.04 ms

ADC_full_scale = min(65535, (ATIME + 1) * (ASTEP + 1))
               = 18000 counts
```

Therefore 60000 counts is not a meaningful near-saturation threshold for the current configuration. Use the actual full scale and the AS7341 `STATUS2` analogue/digital saturation and validity bits.

### Basic Counts

Use the manufacturer normalisation before comparing exposures:

```text
BasicCounts[ch] = RawCounts[ch] / (GainMultiplier * IntegrationTime)
CorrectedBasic[ch] = BasicCounts[ch] - BasicOffset[ch]
```

Integration time may be expressed in milliseconds or seconds, but the unit must be fixed in the calibration dataset, coefficient file, firmware, and backend. Coefficients trained with milliseconds are wrong by a factor of 1000 if applied with seconds.

Offset may depend on channel, device, gain, integration time, and temperature. If an offset is measured in raw counts, normalise it before subtraction in the same way as the sample.

### PPFD and DLI

A deployed linear or PLS model has the form:

```text
PPFD = intercept + sum(C[ch] * CorrectedBasic[ch]), ch = F1..F8, Clear, NIR
DLI  = sum(PPFD_i * delta_t_i) / 1,000,000
```

PPFD is in `umol m-2 s-1`, `delta_t` in seconds, and DLI in `mol m-2 day-1`. Do not compute DLI across long missing-data gaps without an explicit gap policy.

## Complete calibration procedure

### Phase 0 - define the measurand and acceptance criteria

Record before testing:

- primary measurand: hemispherical PPFD over 400-700 nm;
- expected range, for example darkness through full outdoor sun;
- desired uncertainty and maximum acceptable bias/RMSE;
- temperature, humidity, and deployment lifetime;
- sampling interval and DLI gap policy;
- whether calibration is individual-device, batch, or golden-device plus per-unit scale;
- model versioning and recalibration interval.

Do not invent universal pass thresholds such as `R2 > 0.95` or a fixed count floor. Derive acceptance limits from the scientific use case and quantify them against held-out data.

### Phase 1 - freeze and document the complete optical head

Calibrate the exact deployable assembly:

- AS7341 board and device serial number;
- diffuser manufacturer, material, thickness, lot, orientation, aperture, and distance from the die;
- window/dome material and thickness;
- enclosure geometry, seals, adhesives, and internal surface finish;
- final field orientation and levelling method.

Fit a clean diffuser permanently before calibration. Any later diffuser, window, air-gap, enclosure, adhesive, or assembly change creates a new optical system and requires at least revalidation, normally recalibration.

### Phase 2 - verify the acquisition contract

Every exposure used for calibration must store:

- raw F1-F8, Clear, and NIR counts;
- the numeric gain multiplier actually used;
- integration time, preferably ATIME and ASTEP as well as computed milliseconds;
- `STATUS2` validity and saturation state;
- device ID, timestamp, firmware version, optical-head revision, and calibration-model version;
- sensor temperature if it can be measured close to the AS7341;
- reference value and all reference corrections for calibration records.

Reject or separately label invalid/saturated exposures. A legitimate dark reading may be zero, so zero alone is not a sensor-failure test. Define low signal from measured dark noise and required signal-to-noise ratio, not a guessed Clear-count threshold.

### Phase 3 - dark offset, noise, and stability

1. Place the complete head in a truly light-tight, temperature-stable enclosure.
2. Warm the electronics for the normal stabilisation period.
3. For every gain/integration combination that auto-exposure can select, collect at least 30 repeated ten-channel exposures. More are preferable for tail behaviour.
4. Repeat on several devices and at representative low, nominal, and high temperatures.
5. Store mean/median offset, standard deviation, robust spread, and any time drift per channel and setting.
6. Repeat after the duration of a normal field measurement session to detect self-heating or drift.

The output is a versioned offset/noise table. It also supplies empirical limits of detection and the low-signal quality rule.

### Phase 4 - integration, gain, linearity, repeatability, and saturation

Use a stable, monitored broadband source with adjustable attenuation; a phone flashlight is not a calibration reference.

1. Hold irradiance and geometry fixed and sweep all permitted gain values. Compare offset-corrected Basic Counts.
2. Hold irradiance fixed and sweep permitted integration times. Compare corrected Basic Counts.
3. At a stable setting, vary reference irradiance from the noise floor to saturation using characterised neutral-density attenuation or a calibrated source.
4. Randomise or bracket intensity order to expose lamp drift and hysteresis.
5. Take repeated readings at every point and monitor the reference simultaneously.
6. Determine each channel's linear range, residual non-linearity, repeatability, saturation behaviour, and gain-step correction.

The AS7341 full scale depends on ATIME and ASTEP. Treat `STATUS2` saturation as invalid even if the reported count is below a convenient fraction of full scale. Select the auto-exposure target band only after these tests; leave headroom for transient sun and bright spectral peaks.

### Phase 5 - angular and cosine-response characterisation

PPFD is a hemispherical irradiance measurement, so a flat spectral response alone is insufficient.

1. Place the final head and a traceable cosine-corrected reference at the same point in a collimated beam.
2. Rotate from normal incidence toward the horizon in known angle steps and multiple azimuths.
3. Compare measured response with the ideal cosine law and the reference.
4. Repeat at several wavelengths or spectrally different sources because diffuser angular response can be wavelength-dependent.
5. Quantify levelling sensitivity and enclosure shadowing.

If angular error is too large, redesign the diffuser/head before performing the expensive absolute calibration.

### Phase 6A - preferred wavelength-resolved absolute calibration

Follow the Klueppel et al. design as closely as available equipment permits:

1. Use a stable broadband lamp feeding a calibrated monochromator in a light-tight, non-reflective enclosure.
2. Diffuse/collimate the output into a spatially uniform beam larger than both detector apertures.
3. Measure the AS7341 and calibrated reference simultaneously through a characterised beam splitter, or alternate them without moving geometry if simultaneous measurement is impossible.
4. Characterise wavelength accuracy, bandwidth, spatial uniformity, stray light, detector area, beam-splitter transfer, lamp/reference drift, and reference-detector responsivity.
5. Sweep at least 390-1100 nm. Use fine steps across 400-700 nm and adequate NIR points to identify leakage.
6. Acquire multiple complete, independent sweeps on each device. Record every correction and uncertainty contribution.
7. Convert reference irradiance to photon flux. Construct the ideal target response as photon response over 400-700 nm and zero outside this interval.
8. Offset-correct and convert every AS7341 channel to Basic Counts.
9. Fit PLS using all ten channels. Choose component count using cross-validation inside the training sweeps.
10. Hold out one or more **complete sweeps** and, for multi-device claims, complete devices. Do not randomly split adjacent wavelength rows.
11. Export the final intercept, ten coefficients, preprocessing definition, units, optical revision, device/batch scope, training range, validation metrics, and uncertainty.

PLS is useful during training because the overlapping channels are highly correlated. Deployment remains cheap: the validated model reduces to ten multiply-adds and an optional intercept.

### Phase 6B - practical field-transfer calibration

If wavelength-resolved facilities are unavailable:

1. Mount the final AS7341 head level beside a calibrated cosine-corrected quantum sensor, close enough to share the same sky view without mutual shading.
2. Synchronise clocks and sampling intervals. Faster sampling reduces errors from broken clouds and moving shadows.
3. Collect multiple independent days spanning clear sky, overcast, broken cloud, low and high solar elevation, shade, and the full PPFD range. If artificial lighting matters, include every lamp type and operating level.
4. Use offset-corrected ten-channel Basic Counts, not raw counts. Include Clear and NIR.
5. Split training and validation by complete day and lighting environment, never random individual rows.
6. Compare ordinary linear regression, regularised regression, and PLS. Prefer the simplest model whose held-out residuals show no important spectral, intensity, time-of-day, or temperature structure.
7. Report bias, MAE, RMSE, relative error over defined PPFD bands, residual plots, range, and reference-sensor uncertainty.

This model is valid only for the sampled optical head and spectral environments. A sunlight model is not automatically a horticultural-LED model.

### Phase 7 - device and batch transfer

For a multi-node deployment:

1. Fully calibrate several devices across production lots, not only one unusually good unit.
2. Co-locate all deployable heads under multiple stable spectra and intensity levels.
3. Decide from the residual distribution whether one global matrix is adequate.
4. If using a batch matrix, derive per-device dark offsets and per-channel or output scaling against the calibrated transfer standard.
5. Reserve complete devices for blind validation.
6. Store device ID, board lot, diffuser lot, calibration scope, date, model hash/version, and coefficients.

Pairwise correlation alone is insufficient: identically biased sensors can correlate perfectly. Compare each unit with the traceable reference and report absolute residuals.

### Phase 8 - independent field validation and uncertainty

Validate with data not used in model selection:

- different days and weather;
- dawn through midday and changing solar angle;
- the highest expected PPFD without clipping;
- representative temperatures and enclosure states;
- at least one independent reference instrument or recently calibrated transfer standard;
- several complete sensor heads.

Build an uncertainty budget including reference calibration, cosine mismatch, spectral mismatch/model residual, gain/time normalisation, repeatability, temperature, clock alignment, spatial mismatch, lamp/reference drift, unit transfer, and long-term drift. Report the range and conditions over which metrics apply.

### Phase 9 - ongoing field quality assurance

- Run an automated range/finite/status check on every exposure.
- Preserve raw counts even when a derived value is rejected.
- Inspect and clean the diffuser using a documented non-damaging procedure.
- Track Clear/basic-count response and cross-device residuals for abrupt optical changes.
- Re-co-locate a rotating subset with a reference after deployment and after enclosure service.
- Revalidate after firmware exposure changes, optical material changes, water ingress, UV yellowing, physical damage, or unexplained drift.
- Determine the recalibration interval from observed drift; the supplied papers do not establish a universal interval.

## What else can be extracted responsibly?

| Output | Feasibility | Calibration needed | Important limitation |
|---|---|---|---|
| Raw ten-channel spectrum | Available now | Gain/time/offset normalisation | Broad, overlapping filters; not a resolved spectrum |
| PPFD (400-700 nm) | Strong evidence | Ten-channel laboratory or field-reference model | Must be validated for optics and spectra |
| DLI | Straightforward after PPFD | Valid PPFD plus timestamp/gap QA | Accumulates bias and missing-data errors |
| Broad red/green/blue fractions | Feasible as indices | Cross-talk-aware definition and validation | Do not imply narrow-band spectral accuracy |
| Red:blue, red:far-red-like indices | Exploratory | Application-specific reference comparison | F8 is centred near 680 nm; this is not a standard 730 nm far-red sensor |
| CCT, lux, XYZ | Possible with separate matrices | Photometric/colorimetric reference calibration | Plant PPFD calibration does not provide these automatically |
| NIR level/leakage indicator | Useful | Gain/time/offset correction | One broad diode, not an NIR spectrum |
| NDVI or canopy reflectance | Not from this upward sensor alone | Incident and reflected geometry, red/NIR radiometric calibration | Requires at least a second optical view/reference |
| Narrow pigment/chemical identification | Generally unsuitable | Application-specific chemometrics | Too few broad bands; results are setup-dependent |

## NIR firmware audit

### What the current V2 code does correctly

The V2 sensor driver reads the Adafruit library's `AS7341_CHANNEL_NIR` after `readAllChannels()` and prints it in:

```text
[PAR] ... CLR=<count> NIR=<count> GAIN=<x> TINT=<ms> SAT=<0|1> ...
```

The V2 snapshot builder then adds these key-value readings:

| ID | Field |
|---:|---|
| 1109 | Clear raw counts |
| 1110 | NIR raw counts |
| 1111 | applied gain multiplier |
| 1112 | integration time in ms |
| 1113 | saturated/invalid flag |

The V2 mothership decoder accepts these IDs, the flash logger appends five corresponding CSV columns, and the JSON serializer maps `spectral_nir` and the other four fields into the upload payload. The nominal V2 chain is therefore implemented from sensor read through JSON construction.

### Most likely reasons NIR is not appearing

1. **The node is still running V1 firmware or sending a V1 snapshot.** `node_snapshot_t` has only `spectral[8]`. There is no place for Clear, NIR, gain, integration time, or saturation. The mothership deliberately renders the extended fields as missing for a V1 record. Both node and mothership need the compatible V2 path, and old records already queued as V1 cannot acquire NIR retrospectively.
2. **The cloud ingest/database still uses the old schema.** This repository proves JSON construction, not acceptance by the deployed endpoint. Confirm that the live endpoint accepts and stores all five nullable fields; the local handoff documents describe this as a required backend migration.
3. **A stale binary is flashed.** The current source prints NIR at the node and prints every V2 `{id,value}` pair. If those lines are absent, the running binary is not this source or the AS7341 did not initialise.
4. **NIR exists at the node but is lost after a specific boundary.** Inspect serial, then the mothership CSV, then the exact HTTP body in that order; do not diagnose from the dashboard alone.
5. **The optical stack attenuates NIR.** Some windows/diffusers transmit visible light differently from 910 nm. This can make NIR unexpectedly small, but it does not explain a missing/null field.

### Confirmed code issues to correct before calibration

- `mothership/firmware/v2/src/storage/upload_queue.h` still contains the old 25-column fallback `kUploadCSVHeader`, while the logger and JSON code use 30 columns. Normal uploads copy the actual on-disk header, so this is not the most likely cause of a missing NIR value. It is nevertheless a latent recovery/fallback contract bug and should be made identical to `flash_logger.cpp`.
- The auto-gain loop can change gain on its final permitted attempt and then exit without another acquisition. In that edge case the stored raw counts belong to the previous gain while the metadata reports the newly selected gain. That corrupts Basic Counts and any calibration. The loop must either reserve the final attempt for the post-change read or report the gain that produced the accepted sample.
- A failed direct `STATUS2` read currently leaves `status2 = 0`, which makes the sample invalid through missing `AVALID`, but the failure is not separately logged. Log/read-quality state explicitly during calibration.
- NIR is not included in the auto-exposure maximum. That may be acceptable for PAR because NIR is a correction channel, but an NIR-rich source can clip NIR while visible/Clear remain acceptable. Validate per-channel saturation and consider NIR when choosing or rejecting an exposure.

### Boundary-by-boundary diagnostic

Use one stable NIR-rich source such as incandescent/halogen illumination and one visible-dominant LED. Keep geometry fixed and avoid sunlight changes while tracing.

1. **Node sensor read:** require a `[PAR]` line with finite `NIR=<count>`. Compare source types; halogen should normally raise NIR relative to many visible LEDs.
2. **Node V2 snapshot:** require `NODE_SNAPSHOT2`, `protocolVersion=2`, and an entry `id=1110 value=<count>`. The expected spectral contribution is 13 readings: eight visible bands plus five extended fields.
3. **Mothership receive/decode:** require a V2 receive/decode log and confirm the record is not being converted through the fixed V1 struct before logging.
4. **On-device CSV:** inspect the real `/datalog.csv` header and a row. It must contain 30 columns, with `spectral_nir` at zero-based index 26.
5. **HTTP JSON:** capture a development upload and require a numeric `spectral_nir` key rather than missing/null.
6. **Backend/database:** query the stored row by node ID, sequence number, and timestamp. If the HTTP body is correct but the row is null/missing, the fault is the deployed ingest/schema.
7. **Dashboard:** only after storage is confirmed, trace API selection and display logic.

## Recommended implementation order

1. Flash matching current V2 node and mothership builds and run the boundary diagnostic above.
2. Align the upload fallback header and repair the auto-gain final-attempt metadata bug.
3. Add calibration-record fields for ATIME/ASTEP or guarantee and version the current 50.04 ms exposure definition.
4. Freeze and document the final diffuser/enclosure.
5. Run dark, linearity, gain/time, repeatability, saturation, and angular tests.
6. Arrange the wavelength-resolved ten-channel calibration if defensible cross-spectrum PPFD is required; otherwise perform the explicitly limited co-location calibration.
7. Store versioned coefficients and derived PPFD additively. Never remove or overwrite raw F1-F8, Clear, NIR, gain, time, and validity data.

## Calibration record template

```yaml
calibration_id:
model_version:
date_utc:
operator:
sensor_device_id:
sensor_board_lot:
firmware_commit:
optical_head_revision:
diffuser_material_lot_geometry:
window_enclosure_geometry:
reference_instrument_model_serial:
reference_calibration_date_uncertainty:
reference_corrections:
temperature_range_c:
gain_values:
integration_time_unit: ms
atime:
astep:
offset_table_version:
preprocessing_definition:
model_type:
intercept:
coefficients_f1_f8_clear_nir:
training_spectra_and_range:
train_validation_split_unit:
held_out_metrics:
angular_response_metrics:
field_validation_metrics:
known_limitations:
next_review_date:
```

## Final interpretation rule

Always distinguish these three levels in stored data and user interfaces:

1. **raw counts** - direct F1-F8, Clear, and NIR ADC outputs plus exposure metadata;
2. **corrected/basic counts** - offset-, gain-, and time-normalised instrument response;
3. **calibrated products** - PPFD, DLI, colour, or indices generated by a named, versioned model within its validated conditions.

That separation preserves the evidence, permits future recalibration, and prevents a convenient proxy from quietly becoming a scientific unit.
