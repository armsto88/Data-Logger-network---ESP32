# Backend implementation prompt — AS7341 extended spectral ingest

**Context for the backend LLM/engineer.** The FieldMesh firmware was just extended to
capture more from the AS7341 spectral sensor than the 8 visible bands it used to send.
The mothership now posts five additional fields per reading to the Supabase ingest
endpoint. Your job is to (1) accept and store them, and (2) derive the light-quality /
PAR products they unlock. Nothing below requires a firmware change — the firmware side
is done and shipping these keys.

---

## 1. What the firmware now sends

The Supabase ingest RPC receives a `readings[]` array. Each reading object already
contained `spectral_415 … spectral_680` (8 bands, **raw ADC counts**). It now **also**
contains these keys (JSON numbers, or `null` when the sensor was absent that cycle):

| JSON key                  | Meaning                                   | Units / range              |
|---------------------------|-------------------------------------------|----------------------------|
| `spectral_clear`          | Wideband "Clear" photodiode               | raw ADC counts             |
| `spectral_nir`            | ~910 nm NIR photodiode                     | raw ADC counts             |
| `spectral_gain`           | Gain multiplier **actually applied** this read (auto-exposure) | 0.5, 1, 2, 4, … 512 |
| `spectral_integration_ms` | Integration time this read                 | milliseconds (~50 ms typ.) |
| `spectral_saturated`      | Saturation/validity flag                   | `0` = valid, `1` = saturated/invalid |

Important semantics:

- **`spectral_415 … spectral_680`, `spectral_clear`, `spectral_nir` are raw counts, NOT
  irradiance.** They are only comparable across readings after you normalise by gain and
  integration time (see §3). The firmware runs **auto-exposure**, so `spectral_gain` can
  differ from row to row for the same physical light level — normalisation is mandatory,
  not optional.
- **`spectral_saturated = 1` means do not trust the bands that cycle.** The firmware
  already tried to lower gain and could not bring the signal on-scale (or the measurement
  was flagged invalid). Treat normalised values from a saturated row as lower-bounds only,
  and exclude them from calibration/aggregation.
- All five keys can be `null` (node without a spectral head, or a failed read). Handle
  `null` the same way you already handle a `null` `spectral_415`.

The five keys map 1:1 to CSV columns the mothership logs locally
(`spectral_clear, spectral_nir, spectral_gain, spectral_integration_ms, spectral_saturated`),
appended after `aux2`. Field names are identical to the JSON keys.

---

## 2. Schema / storage changes

Add five nullable numeric columns to whatever table stores per-reading rows (the same
table that has `spectral_415` etc.):

```sql
alter table readings
  add column if not exists spectral_clear          double precision,
  add column if not exists spectral_nir            double precision,
  add column if not exists spectral_gain           double precision,
  add column if not exists spectral_integration_ms double precision,
  add column if not exists spectral_saturated      smallint;   -- 0/1, nullable
```

If the ingest function whitelists accepted keys, add the five names to that whitelist.
Unknown extra keys must not cause a 400 (older mothership firmware still omits them, and
newer firmware always sends them — the schema must accept both).

---

## 3. Derived values to compute (the actual point of this change)

Do these as generated columns, a view, or in a post-ingest transform — your call. All are
**per-reading** unless noted.

### 3.1 Normalised / "basic" counts (prerequisite for everything else)

For each band `X` in {415,445,480,515,555,590,630,680, clear, nir}:

```
basic_X = raw_X / (spectral_gain * spectral_integration_ms)
```

This removes the auto-exposure gain and integration differences so rows are comparable.
This is exactly the AS7341 "basic counts" definition. Use `basic_*` for all ratios,
trends, and any calibration — never the raw counts directly.

Guard: if `spectral_saturated = 1`, or `spectral_gain`/`spectral_integration_ms` is null
or 0, `basic_*` is undefined → store null.

### 3.2 Spectral light-quality fractions (firmware-free, high value)

From the normalised bands, group into blue/green/red and report fractions of visible total:

```
blue  = basic_415 + basic_445 + basic_480
green = basic_515 + basic_555
red   = basic_590 + basic_630 + basic_680
vis   = blue + green + red

blue_fraction  = blue  / vis
green_fraction = green / vis
red_fraction   = red   / vis
red_blue_ratio = red   / blue      -- classic plant light-quality index
```

These are dimensionless and need **no calibration** — they are valid immediately and are
the most useful new product for growers.

### 3.3 NIR / illumination descriptors

```
nir_ratio   = basic_nir / (vis + basic_nir)     -- light-source "warmth" / NIR richness
clear_level = basic_clear                        -- total-illumination proxy
```

`clear_level` is a good input for photoperiod / day-night detection (threshold on a time
series) and for a relative "how bright" trend before any PAR calibration exists.

### 3.4 PAR / PPFD and DLI — **calibration-gated, do not ship raw**

Do **not** publish a PPFD number until a reference-sensor calibration campaign has been
run on the assembled sensor head (diffuser included). Until then:

- Keep exposing the current uncalibrated `PAR_PROXY` (sum of raw bands) **clearly labelled
  as a proxy / arbitrary units**, if you already surface it.
- Reserve columns `ppfd_umol_m2_s` and `dli_mol_m2_day` but leave them null until a
  calibration coefficient exists. Structure so that when calibration lands you only add:
  ```
  ppfd = k * (weighted sum of basic bands)     -- k from reference-quantum-sensor fit
  ```
  and DLI is the daily time-integral: `DLI = Σ(PPFD * seconds) / 1e6`.

### 3.5 What NOT to compute

- **No NDVI.** The sensor looks up at ambient light (not reflected), so
  `(NIR − red)/(NIR + red)` is not valid NDVI. Do not expose an "NDVI" field.
- No absolute lux/CCT unless/until separately calibrated — lower priority.
- These bands are broad (~26–52 nm); do not present a "continuous spectrum".

---

## 4. Data-quality rules (apply at ingest or in the view)

1. If `spectral_saturated = 1` → exclude the row's bands from any average/calibration;
   still store the raw row (it is a legitimate "very bright" event).
2. Normalise (§3.1) before any cross-row comparison; never chart raw counts over time —
   auto-exposure makes them jump with gain.
3. Treat all five keys (and the bands) as independently nullable.
4. `spectral_gain` values are a fixed ladder {0.5,1,2,4,8,16,32,64,128,256,512}; a value
   outside that set indicates a decode error worth flagging.

---

## 5. Summary of contract

- **New accepted keys (all nullable numbers):** `spectral_clear`, `spectral_nir`,
  `spectral_gain`, `spectral_integration_ms`, `spectral_saturated`.
- **Must:** store them; accept payloads with or without them.
- **Should:** compute `basic_*`, the blue/green/red fractions + `red_blue_ratio`,
  `nir_ratio`, `clear_level`.
- **Later (calibration-gated):** `ppfd_umol_m2_s`, `dli_mol_m2_day`.
- **Never:** NDVI from this upward-looking geometry; PPFD from raw counts.
