# Backend / Frontend Handoff — Extended Spectral Data Ingest

**Effective:** 2026-07-04 (firmware v2.1+)  
**Status:** Ready to implement; no firmware changes required after initial node/mothership flash.

---

## Summary

Nodes with AS7341 spectral sensors now capture **5 additional data points per cycle** beyond the 8 visible light bands:
- **Wideband "Clear"** and **~910 nm NIR** photodiodes (raw counts)
- **Auto-exposure metadata:** gain multiplier, integration time, saturation flag

These unlock **immediate, calibration-free** light-quality metrics (blue/green/red fractions, red:blue ratio) and set up for PAR/DLI calibration. The data flows unchanged through the existing Supabase ingest endpoint — just new JSON keys.

---

## Data Contract: What the Backend Receives

### New JSON keys in `readings[]` (all nullable floats)

| Key | Type | Semantics | Range / Units |
|-----|------|-----------|---------------|
| `spectral_clear` | float | Wideband photodiode (raw ADC) | 0–65535 counts |
| `spectral_nir` | float | ~910 nm photodiode (raw ADC) | 0–65535 counts |
| `spectral_gain` | float | Auto-exposure gain applied **this read** | 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 |
| `spectral_integration_ms` | float | Integration time **this read** | ~50 ms typical |
| `spectral_saturated` | float | Measurement validity flag | 0 = valid, 1 = saturated/invalid |

Existing fields (unchanged):
- `spectral_415` … `spectral_680` (8 bands, raw counts)
- All other per-reading fields (battery, temp, humidity, soil, wind, aux)

### Key semantics

1. **Raw counts are meaningless without normalization.** Nodes run auto-exposure, so gain changes from row to row. **Always divide by gain × integration_ms before comparison or aggregation.**
2. **Saturation flag = 1 → do not trust that row's spectral data.** The firmware tried to lower gain and failed; measurements are lower-bounds only. Exclude from calibration and trending until resolved.
3. **All 5 keys can be `null`** (node without spectrometer, or failed read). Handle the same way as missing `spectral_415`.
4. **Gain values must be from the fixed ladder** {0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512}. Any other value is a decode error.

---

## Backend Implementation

### 1. Schema (Supabase)

Add 5 nullable numeric columns to the readings table:

```sql
alter table readings
  add column if not exists spectral_clear          double precision,
  add column if not exists spectral_nir            double precision,
  add column if not exists spectral_gain           double precision,
  add column if not exists spectral_integration_ms double precision,
  add column if not exists spectral_saturated      smallint;  -- 0/1, nullable
```

If the ingest function whitelists keys, add these 5 names. Older mothership firmware omits them; newer always sends them. The schema must accept both.

### 2. Normalization (mandatory before use)

For each reading and each band X in {415, 445, 480, 515, 555, 590, 630, 680, clear, nir}:

```sql
basic_X = spectral_X / (spectral_gain * spectral_integration_ms)
```

This removes auto-exposure variation so readings across time/gain are comparable. This is the AS7341 "basic counts" standard. **All downstream use (ratios, trends, calibration) must use `basic_*`, never raw.**

Guard: if `spectral_saturated = 1` OR any denominator is null/zero → `basic_*` is undefined → `NULL`.

### 3. Derived light-quality metrics (immediate value)

No calibration needed — valid today.

```sql
-- Normalised band grouping
blue  = basic_415 + basic_445 + basic_480
green = basic_515 + basic_555
red   = basic_590 + basic_630 + basic_680
vis   = blue + green + red

-- Dimensionless fractions (0–1)
spectral_blue_fraction  = blue  / vis
spectral_green_fraction = green / vis
spectral_red_fraction   = red   / vis

-- Plant light-quality index (dimensionless)
spectral_red_blue_ratio = red / blue       -- key metric for horticulture

-- Illumination / light-source descriptors
spectral_nir_ratio = basic_nir / (vis + basic_nir)   -- "warmth" / NIR richness
spectral_clear_level = basic_clear                    -- total brightness proxy
```

Store these as materialized columns or computed view columns. The fractions are valid immediately and high-value for growers.

### 4. PAR / PPFD / DLI (calibration-gated)

**Do not publish PPFD yet.** Reserve columns but leave null:

```sql
spectral_ppfd_umol_m2_s  double precision,  -- null until calibration exists
spectral_dli_mol_m2_day  double precision   -- null until calibration exists
```

Once a reference-quantum-sensor calibration campaign is complete (matching the assembled sensor head + diffuser), add:

```sql
spectral_ppfd_umol_m2_s = k * (weighted sum of basic bands)  -- k from cal
spectral_dli_mol_m2_day = Σ(PPFD × seconds) / 1e6            -- daily integral
```

### 5. Data quality rules

- **Exclude saturated rows** (`spectral_saturated = 1`) from any average/trend/calibration.
- **Normalise before cross-row comparison** — never chart raw spectral counts vs. time.
- **Treat all 5 keys independently nullable** — a reading can have bands but no gain (decode error).
- **Flag gain ladder violations** (value not in {0.5, 1, 2, …, 512}) as a data-quality alert.

---

## Frontend Implementation

### What to display

The dashboard should surface:

#### Light Quality (Real-time, no calibration needed)
- **Red:Blue Ratio** — animated gauge or sparkline
  - Threshold zones: shade (<0.8) | balanced (0.8–1.2) | red-rich (>1.2)
  - Growers optimize around 1.0–1.2 for vegetative growth
- **Blue / Green / Red fractions** — stacked bar or pie chart
  - Shows spectral composition at a glance
  - Helps detect lamp type (LED, HPS, natural, etc.)
- **NIR Ratio** — secondary card
  - High (>0.3) = warm/incandescent-rich; low (<0.15) = cool/LED

#### Sensor Health
- **Saturation flag** (0x on sparkline or dashboard badge)
  - Red icon if saturated; green if valid
  - Tooltip: "Sensor clipped; readings are lower-bounds"
- **Auto-gain value** (informational, small text)
  - Lets power users diagnose exposure swings
  - Typical range: 4× to 64× in field conditions
- **Measurement timestamp + integration time** (optional advanced view)
  - Shows `spectral_integration_ms` for diagnostic detail

#### Reserved (calibration-pending)
- **PPFD** — display container ready, render `null` or "Awaiting calibration"
- **DLI** — same; daily total renders once backend computes
- **Lux / CCT** — reserve space; lower priority

### UI/UX Guidelines

1. **Red:Blue Ratio is the marquee metric** — put it top-left or as the hero number. Growers care about this most for plant growth optimization.
2. **Group spectrometer data separately** from environmental (temp, humidity, soil). Use a collapsible card or tab.
3. **Show saturation clearly** — a red badge or triangle-alert icon; many users won't know what "SAT=1" means. Tooltip: "Measurement unreliable; sensor overexposed."
4. **Auto-gain and integration time are advanced.** Hide behind a "Diagnostics" toggle or link.
5. **Time-series charts:**
   - Red:Blue Ratio over 7 days (line chart; spot trends)
   - Spectral composition (stacked bar, daily avg)
   - Saturation occurrences (event log or heatmap)
6. **Alerts / rules** (optional, high-value):
   - "Red:Blue ratio below 0.7 for >2 hours" → suggest light adjustment
   - "Sensor saturation detected" → user may need to reduce gain manually (future FW feature)

### Data handling

- **Null / missing fields**: Don't crash; render as "–" or skip the card.
- **Gain ladder violations** (e.g., `spectral_gain = 3`): Display in red and log the incident; signal a decode error to ops.
- **Normalization:** Frontend should assume the backend sends `spectral_*_fraction` and `spectral_red_blue_ratio` already computed. If not, compute client-side as a fallback (but this is a backend job).
- **Timestamp alignment**: Spectral data arrives in the same snapshot as temp/humidity/soil, so they share the same `nodeTimestamp`. No cross-time interpolation needed.

---

## Timeline & Rollout

**Phase 1 (Now):**
- Mothership v2.1+ firmware flashed to all deployed units
- Nodes reflashed with AS7341 extended driver
- Data flows to Supabase with new keys (5 new null → actual values)

**Phase 2 (Backend, immediate):**
- Add schema columns (5 lines SQL)
- Compute `basic_*` normalization (view or generated columns)
- Compute light-quality fractions + red:blue ratio
- Deploy; test with live node data

**Phase 3 (Frontend, 1–2 weeks):**
- Add light-quality cards to dashboard
- Wire up Red:Blue Ratio as hero metric
- Add saturation badge + help text
- Test with live data; iterate on UX

**Phase 4 (Future, post-calibration):**
- Integrate reference-quantum-sensor calibration results
- Populate `spectral_ppfd_umol_m2_s` and `spectral_dli_mol_m2_day`
- Add PAR/DLI cards to dashboard

---

## What NOT to do

- ❌ **Do not publish PPFD without a calibration campaign.** "Raw counts" does not equal irradiance.
- ❌ **Do not use raw spectral counts in trends.** Always normalise by gain × integration_ms first.
- ❌ **Do not treat saturated readings as valid.** Exclude from averages and calibration.
- ❌ **Do not expose NDVI.** The sensor looks up (ambient light, not reflected). `(NIR − 680)/(NIR + 680)` is invalid.
- ❌ **Do not assume a "continuous spectrum."** The bands are broad (~26–52 nm); this is not a spectrograph.

---

## Questions?

- **"Can I use this for plant disease detection?"** Not yet. Requires a controlled reflectance setup and a model trained on your crops + environment. Start with red:blue ratio for growth optimization.
- **"What's the diffuser?"** A 38.6 mm optical element that controls how light is collected. It must be clean and unchanged to keep calibration valid. ams OSRAM recommends an achromatic diffuser.
- **"Can you measure chlorophyll?"** Not directly. Would need a 730 nm far-red channel and a validated model. Current bands are not sufficient.
- **"How often do nodes report?"** Default every 1 minute (data wake). Spectrometer auto-exposes each time, so gain can vary cycle-to-cycle. Always normalise.

