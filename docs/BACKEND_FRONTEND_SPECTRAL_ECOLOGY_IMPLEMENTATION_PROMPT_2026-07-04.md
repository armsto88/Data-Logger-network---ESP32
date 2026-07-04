# Prompt: extended AS7341 ingest and ecological light metrics

Copy everything below this line into the backend/frontend manager's task.

---

You are working in the FieldMesh dashboard/backend repository:

```text
C:\Users\thoma\Documents\FieldMesh\FieldMeshDashboard
```

The matching firmware repository is:

```text
C:\Users\thoma\Documents\Data-Logger-network---ESP32
```

Your task is to verify and complete the end-to-end handling of the AS7341's extended channels, then expose scientifically cautious ecological light metrics for upward-facing sensors deployed at plant height in a solar farm.

Do not begin by redesigning the UI. First trace a real or representative reading from the firmware JSON contract through ingest, database storage, provider flattening, and the current dashboard/chart helpers. Preserve raw evidence and existing behavior. Make the smallest safe contract fix, add focused tests, and then add derived products additively.

## Critical firmware detail: Clear and NIR are appended metadata

The eight visible bands are registered like normal sensor readings:

```text
spectral_415, spectral_445, spectral_480, spectral_515,
spectral_555, spectral_590, spectral_630, spectral_680
```

Clear, NIR, gain, integration time, and validity do **not** consume ordinary sensor-registry slots. The node reads them from the same AS7341 exposure as `SpectralMetadata` and appends them directly to the V2 key-value snapshot in `buildReadingsArray()`.

Trace these firmware locations:

1. Sensor acquisition:
   `node/firmware/v2/src/sensors/sensors_par_as7343.cpp`
   - `AS7341_CHANNEL_CLEAR`
   - `AS7341_CHANNEL_NIR`
   - `SpectralMetadata`
2. V2 snapshot append:
   `node/firmware/v2/src/sensors/sensors.cpp`
   - `buildReadingsArray()` appends five extra `{sensorId, value}` pairs after normal registry readings.
3. Stable IDs:
   `node/firmware/v2/shared/protocol.h`

| Sensor ID | Firmware meaning | Final JSON key |
|---:|---|---|
| 1109 | Clear broadband raw count | `spectral_clear` |
| 1110 | approximately 910 nm NIR raw count | `spectral_nir` |
| 1111 | gain multiplier used for this exposure | `spectral_gain` |
| 1112 | integration time in milliseconds | `spectral_integration_ms` |
| 1113 | saturated or invalid exposure flag | `spectral_saturated` |

4. Mothership decode and CSV:
   `mothership/firmware/v2/src/storage/flash_logger.cpp`
   - these are appended after `aux2`;
   - zero-based CSV columns are Clear 25, NIR 26, gain 27, integration time 28, saturation 29.
5. JSON construction:
   `mothership/firmware/v2/src/storage/json_payload.cpp`
   - the five final JSON names are exactly those in the table above;
   - they are ordinary nullable numeric keys inside each object in `readings[]` by the time HTTP JSON is sent.

Do not search for Clear/NIR as separate physical sensor slots or separate HTTP objects. They qualify the same AS7341 exposure and arrive in the same reading object.

V1 snapshots contain only `spectral[8]`; they cannot contain Clear, NIR, gain, integration time, or saturation. Older queued V1 observations must remain valid and should produce null extended fields.

## Confirmed naming mismatch in the current dashboard repository

The firmware contract and current dashboard contract disagree on two names:

| Meaning | Firmware JSON | Current database/frontend name |
|---|---|---|
| integration time | `spectral_integration_ms` | `spectral_atime` |
| invalid/saturated flag | `spectral_saturated` | `spectral_sat` |

Inspect these current dashboard files before editing:

- `supabase/migrations/202607040002_add_spectral_raw_channels.sql`
  - typed columns and RPC parsing currently use `spectral_atime` and `spectral_sat`;
  - Clear, NIR, and gain already use the same names as firmware.
- `supabase/functions/_shared/fieldmesh.ts`
  - the five extended fields are not in `KNOWN_NUMBER_FIELDS`;
  - the normalizer currently retains unknown scalar keys both at the reading's top level and in `extraMeasurements`.
- `frontend/src/lib/data-providers/supabase.js`
  - `readingFromRow()` starts from `row.payload` and merges `extraMeasurements` into the flattened reading.
- `frontend/src/lib/spectral.js`
  - current helpers read `spectral_nir` but use `spectral_sat` for validity.
- `frontend/src/lib/sensorMetadata.js`, `Dashboard.jsx`, and `Charts.jsx`
  - current UI metadata and diagnostics expect `spectral_atime` and `spectral_sat`.

This explains a deceptive state: the firmware values can be preserved in `payload`/`extraMeasurements`, while typed columns or UI helpers still return null because they ask for a different key.

## Required contract repair

Choose one canonical frontend/database vocabulary, but make ingest backward-compatible with both spellings. Prefer an explicit aliasing boundary rather than scattering fallbacks throughout components.

The least disruptive option is:

1. Accept both firmware and legacy aliases at ingest.
2. Populate the existing typed columns using:

```sql
spectral_atime = coalesce(
  nullif(v_reading->>'spectral_integration_ms', '')::double precision,
  nullif(v_reading->>'spectral_atime', '')::double precision
)

spectral_sat = coalesce(
  nullif(v_reading->>'spectral_saturated', '')::integer,
  nullif(v_reading->>'spectral_sat', '')::integer
)
```

3. Add all five actual firmware keys to the Edge Function's known numeric fields so they receive consistent finite-number validation.
4. At the provider boundary, expose one stable application shape. During migration it is acceptable to expose both aliases, derived from one value, so old and new components continue to work.
5. Keep the original payload unchanged for provenance/debugging.
6. Update generated database types if typed columns change.
7. Do not require extended fields: V1 and non-spectral nodes legitimately omit them.

Also verify that `mothership/firmware/v2/src/storage/upload_queue.h` has the same 30-column fallback header as `flash_logger.cpp`; the current firmware repository still contains an old 25-column fallback constant.

## Focused end-to-end acceptance test

Create a representative V2 reading fixture such as:

```json
{
  "datetime": "2026-07-04T12:00:00Z",
  "nodeId": "PAR_001",
  "seqNum": 42,
  "sensorPresent": 4,
  "qualityFlags": 0,
  "configVersion": 1,
  "spectral_415": 1800,
  "spectral_445": 2600,
  "spectral_480": 3100,
  "spectral_515": 4200,
  "spectral_555": 5100,
  "spectral_590": 4700,
  "spectral_630": 3900,
  "spectral_680": 2500,
  "spectral_clear": 12000,
  "spectral_nir": 6800,
  "spectral_gain": 4,
  "spectral_integration_ms": 50.04,
  "spectral_saturated": 0
}
```

Prove in focused tests that:

- ingest accepts this exact firmware shape;
- all five extended values survive normalization;
- Clear/NIR/gain reach their typed columns;
- the two long firmware names populate the selected typed aliases;
- `readingFromRow()` returns finite Clear, NIR, gain, integration, and validity values;
- a legacy fixture using `spectral_atime`/`spectral_sat` still works;
- a V1 fixture with no extended fields still works and yields nulls;
- saturated/invalid readings are stored but excluded from derived ecological metrics;
- null/zero gain or integration time does not produce Infinity or fabricated zero;
- a raw-count fixture scaled by gain produces the same Basic Counts after normalization.

If possible, run a live non-mutating query against one recently ingested V2 row and inspect, side by side:

```text
readings.spectral_clear
readings.spectral_nir
readings.spectral_gain
readings.spectral_atime
readings.spectral_sat
readings.extra_measurements
readings.payload
```

Report the exact boundary where any field disappears. Do not infer a firmware failure from the dashboard alone.

## Scientific rules for derived values

The sensor points upward and measures **incident light**. It does not observe vegetation reflectance.

Mandatory rules:

- Keep all ten raw channels visible/downloadable.
- Do not call raw counts irradiance, PAR, PPFD, lux, or photon fractions.
- Auto-gain makes raw values incomparable across exposures. For every channel:

```text
basic_X = raw_X / (spectral_gain * integration_time_ms)
```

- Return derived values as null when gain/time is absent or zero, the exposure is invalid/saturated, required bands are missing, or a configured gap limit is exceeded.
- Same-exposure channel ratios cancel common gain and integration time, but they remain **AS7341 response indices**, not calibrated photon fractions, because the filters differ in bandwidth, sensitivity, overlap, and NIR leakage.
- NIR is especially important as an out-of-band correction feature in the future PPFD model. It is not part of 400-700 nm PAR itself.
- Never calculate or label `(NIR-red)/(NIR+red)` as NDVI. An upward incident-light sensor cannot measure canopy NDVI.
- Do not use universal Red:Blue or NIR “good/bad” thresholds. Their ecological meaning depends on species, season, solar angle, clouds, optical calibration, and management.
- The current `PAR_PROXY` is relative arbitrary units. Keep it clearly labelled and never relabel it as PPFD.
- Publish PPFD and DLI only after the final assembled optical head has been calibrated against an appropriate reference using the versioned calibration procedure in:
  `sensors/as7341/SPECTRAL_SENSOR_CALIBRATION_PROTOCOL.md`.

## Ecologically useful products

Treat the light data as predictors describing the solar farm's microclimate mosaic, not as habitat suitability by themselves.

### Tier 1: available before absolute PPFD calibration

Use Basic Counts or within-exposure ratios and label every output as relative:

1. **Relative broadband light level**
   - `basic_clear` as a within-device light trend;
   - do not compare absolute levels across devices until unit-transfer calibration is complete.
2. **Open-reference light transmission**
   - pair each under-panel/edge/inter-row sensor with an unobstructed upward-facing reference sensor;
   - `relative_light = basic_clear_site / basic_clear_open` using time-aligned valid readings;
   - show percent reduction as `100 * (1 - relative_light)`;
   - this is generally more interpretable than an isolated raw count.
3. **Relative photoperiod/shade window**
   - operational day/night and shade periods based on a documented site-relative threshold;
   - report the threshold and label the result “operational”, not astronomical day length.
4. **Morning versus afternoon light balance**
   - integrate a relative light proxy separately before and after solar noon;
   - useful for fixed-tilt and tracking-array shade asymmetry.
5. **Shade-event duration and transition frequency**
   - continuous minutes below a configurable percentage of the simultaneous open reference;
   - number and duration of light/shade transitions;
   - useful for distinguishing persistent shade from moving-panel mosaics.
6. **Spatial light heterogeneity**
   - compare time-aligned open, inter-row, panel-edge, morning-shade, afternoon-shade, and full-shade nodes;
   - report median, interquartile range, coefficient of variation, and coverage count;
   - never mix unsynchronised samples under broken cloud without alignment/QC.
7. **Exploratory spectral-response indices**
   - AS7341 blue, green, and red response shares;
   - red:blue response index;
   - NIR response share and each site's difference from the simultaneous open reference;
   - show these in an “Exploratory light quality” section without habitat-quality thresholds.

### Tier 2: publish after PPFD calibration

These should become the main ecological light products:

1. **Instantaneous PPFD** in `umol m-2 s-1` with calibration version and QC.
2. **DLI** in `mol m-2 day-1`, integrated using actual timestamp intervals and an explicit maximum-gap rule.
3. **Daily PPFD summaries:** daytime mean, median, maximum, percentiles, and valid-data coverage.
4. **Morning and afternoon DLI** and their asymmetry.
5. **Open-reference DLI transmission:** `DLI_site / DLI_open` and daily PAR reduction.
6. **Hours within configurable PPFD bands.** Thresholds must be selected for a named species/guild or analysis, never hard-coded as universal habitat classes.
7. **Consecutive low-light duration** and shade-event distributions.
8. **Seasonal/cumulative photon exposure:** rolling 7-, 30-, and growing-season DLI totals with data coverage.
9. **Spatial DLI heterogeneity:** node/zone distribution, coefficient of variation, and a map or panel-layout profile.

Use numerical integration that respects irregular intervals. Do not bridge a gap larger than the configured limit. Report observed coverage so a low DLI is not confused with missing data.

## Combine light with the rest of FieldMesh

For habitat work, construct a versioned daily feature table by node and micro-patch. Candidate predictors include:

- calibrated DLI and PPFD distribution;
- relative light transmission and shade duration;
- air-temperature mean/min/max/range;
- relative humidity and derived vapour-pressure deficit;
- soil-temperature mean/range;
- soil-moisture mean, drought duration, and post-rain persistence;
- growing-degree days using a documented, species-appropriate base temperature;
- wind exposure;
- month/day-of-year and site/year effects;
- panel position, distance to panel/drip edge, row orientation, tracker/fixed state, and open-reference pairing;
- mowing, grazing, seeding, herbicide, and other management events;
- calibration version, firmware version, sensor/optics revision, and data coverage.

Store node placement as explicit ecological metadata, for example:

```text
OPEN_REFERENCE
INTER_ROW
PANEL_EDGE
FULL_SHADE
MORNING_SHADE
AFTERNOON_SHADE
DRIP_LINE
```

Do not infer these classes solely from one day's sensor values.

## Habitat suitability is a model outcome, not a sensor reading

Do not build a generic “habitat suitability score” from light alone. Select a biological response and collect ground truth at matching plots and dates. Appropriate targets include:

- named-species presence/absence, occupancy, abundance, or cover;
- plant species richness, Shannon diversity, or functional composition;
- vegetation height, green cover, biomass, and bare ground;
- flowering abundance, richness, and phenology;
- pollinator visitation, abundance, richness, or occupancy;
- management goals such as native-cover persistence or invasive-species risk.

For each target:

1. predefine the ecological question and spatial/temporal unit;
2. join biological surveys to sensor summaries without using future information;
3. include management, soil, landscape, and survey-effort covariates rather than attributing everything to light;
4. split validation by complete site, year, plot, or survey period as appropriate, not random neighbouring rows;
5. use interpretable baseline models before more complex ML;
6. report uncertainty, calibration range, missingness, class imbalance, and out-of-domain predictions;
7. distinguish association/prediction from causal evidence;
8. show the underlying predictors beside any model output.

The scientifically useful question is not “is this node good habitat?” It is closer to: “Given this site's shade, DLI, temperature, moisture, management, and season, how suitable is this micro-patch for the named species or ecological response, within the model's validated domain?”

## Frontend priorities

Keep raw spectra available, then add a compact **Light environment** section:

1. **Before calibration**
   - relative light versus open reference;
   - today's relative light exposure with coverage;
   - shade duration and morning/afternoon balance;
   - an exploratory spectral-response panel;
   - visible saturation/missing-metadata badges.
2. **After calibration**
   - current PPFD;
   - today's DLI and data coverage;
   - DLI relative to open reference;
   - shade hours;
   - 7- or 30-day DLI trend.
3. **Solar-farm comparison view**
   - time-of-day x micro-patch heatmap for relative light or PPFD;
   - daily DLI by node/zone;
   - spatial/panel-layout profile with the open reference clearly marked;
   - synchronized comparison with soil moisture, soil temperature, air temperature, and VPD.
4. **Research/export view**
   - raw ten channels plus gain, integration time, saturation, timestamps, calibration version, QC, and derived metrics;
   - clear definitions and units in tooltips/download metadata.

Avoid decorative gauges and unexplained traffic-light zones. Prefer trends, distributions, reference comparisons, and coverage indicators that help an ecologist judge the evidence.

## Research basis

Use these sources to guide interpretation, without copying their site-specific thresholds:

- AS7341 calibration and NIR correction: the supplied manufacturer note and papers under `sensors/as7341/`.
- A 2025 single-axis PV micro-patch study monitored PAR, DLI, photoperiod, temperature, humidity/VPD, wind, soil moisture, soil temperature, and vegetation outcomes across distinct shade zones: https://doi.org/10.3389/frsus.2025.1497256
- Spatial and temporal PPFD/DLI zones within agrivoltaics are useful for understanding crop/vegetation light availability: https://doi.org/10.1016/j.biosystemseng.2021.06.017
- Solar-farm microclimate studies compare under-panel, inter-panel, and external reference locations and relate PAR, temperature, humidity, and vegetation diversity: https://doi.org/10.3390/su14127493
- Partial shade can alter flowering phenology and floral abundance, but biological response depends on shade regime and ecosystem context: https://doi.org/10.1038/s41598-021-86756-4

## Deliverables

1. A short diagnosis showing where each of the five firmware fields currently lands.
2. The smallest backward-compatible ingest/provider contract fix.
3. Focused automated tests for firmware names, legacy aliases, V1 absence, nulls, saturation, and normalization.
4. Additive ecological derived-data helpers with documented units, calibration status, QC, and gap handling.
5. A compact frontend implementation that keeps raw channels and exposes the most decision-useful light-environment metrics.
6. A research-ready daily feature/export table for later habitat models.
7. Updated contract documentation naming the canonical fields and explicitly prohibiting uncalibrated PPFD and upward-looking NDVI.
8. A concise verification report with commands/tests run and any work that still requires live data, calibration, or biological field surveys.

Do not claim the task is complete merely because the fields appear in one component. Verify the path from an exact firmware-shaped fixture—or a live non-mutating row—all the way to the rendered frontend object.

---
