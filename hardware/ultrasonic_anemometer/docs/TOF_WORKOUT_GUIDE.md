# TOF Workout Guide (Ultrasonic Anemometer)

This guide maps your current geometry (`FreeCAD/MACRO.py`) to the firmware timing math.

## 1) What to read from the macro output

Run the macro and use the `---- TOF Flat-Roof Check ----` section. The key values are:

- `Path length (pod->roof->pod)` = acoustic distance $L$ for one direction (A->B)
- `Beam projection (|sin tilt|)` = projection factor $p$ from axis wind to along-beam wind
- `No-wind TOF (A->B)` = expected nominal time at the chosen temperature assumption
- Sample timing rows = expected $t_{AB}$ / $t_{BA}$ split vs wind speed

## 2) Core equations to implement in firmware

Use bidirectional measurements on each axis pair:

- Measure $t_{AB}$ and $t_{BA}$ in seconds.
- Let $L$ be path length in meters.
- Let $p = |\sin(\theta)|$ where $\theta$ is pod tilt.

Along-beam wind component:

$$
\nu = \frac{L}{2}\left(\frac{1}{t_{AB}} - \frac{1}{t_{BA}}\right)$$

Axis wind component:

$$U_{axis} = \frac{\nu}{p} = \frac{L}{2p}\left(\frac{1}{t_{AB}} - \frac{1}{t_{BA}}\right)$$

Speed-of-sound estimate from the same pair:

$$c = \frac{L}{2}\left(\frac{1}{t_{AB}} + \frac{1}{t_{BA}}\right)$$

Use this $c$ for health monitoring and temperature consistency checks.

## 3) Practical measurement flow

1. Trigger TX burst A->B.
2. Timestamp start and first valid RX edge at B (gated window).
3. Repeat B->A.
4. Collect at least 16-64 samples per direction and median-filter.
5. Compute $U_{axis}$ using reciprocal formula above.
6. Repeat for the orthogonal axis pair and rotate into global wind vector.

## 4) Zero-wind and scaling calibration

1. **Zero calibration (no airflow):**
   - Capture long runs per axis.
   - Store timing bias or wind bias offsets.
2. **Gain calibration (fan + reference meter):**
   - Fit $U_{true} = a\,U_{meas} + b$ per axis.
3. **Temperature check:**
   - Compare inferred $c$ vs $331.3 + 0.606T$.
   - Flag outliers (rain splash, false echo, saturation).

## 5) Timing resolution target

Use the macro sample table (`|dt|` at 1/5/10 m/s) to verify timer precision.

Rule of thumb:

- You want timing jitter significantly below the smallest expected $|t_{BA} - t_{AB}|$ at low wind.
- If your low-wind delta is a few microseconds, sub-microsecond timestamp stability is strongly preferred.

## 6) What to do next

- Lock final geometry values (`POD_TILT_DEG`, roof height, pod radius).
- Re-run macro and copy the TOF diagnostic lines into firmware constants.
- Add runtime telemetry of raw $t_{AB}$, $t_{BA}$, inferred $c$, and solved $U_{axis}$ for field debugging.

## 7) Current validated snapshot (Mar 5, 2026)

From your latest successful macro run:

- Reflector dz: $62.00\ \text{mm}$
- Pod tilt: $32.30^\circ$
- Path length: $L = 146.70\ \text{mm} = 0.14670\ \text{m}$
- Beam projection: $p = |\sin(32.3^\circ)| = 0.5344$
- Assumed speed of sound at $20^\circ C$: $c = 343.42\ \text{m/s}$
- No-wind TOF: $t_0 \approx 427.17\ \mu s$
- Wind solve factor: $K = \frac{L}{2p} = 0.137269\ \text{m}$

Reference timing split from this geometry (at $20^\circ C$):

- $U=1\ \text{m/s}$: $t_{AB}=426.51\ \mu s$, $t_{BA}=427.84\ \mu s$, $|\Delta t|=1.33\ \mu s$
- $U=5\ \text{m/s}$: $t_{AB}=423.88\ \mu s$, $t_{BA}=430.52\ \mu s$, $|\Delta t|=6.65\ \mu s$
- $U=10\ \text{m/s}$: $t_{AB}=420.63\ \mu s$, $t_{BA}=433.93\ \mu s$, $|\Delta t|=13.30\ \mu s$

These are the current constants to carry into firmware until geometry changes.

## 8) Beam spread + reflector test diagnostics (in macro)

`MACRO.py` now includes a Monte-Carlo ray-trace diagnostic that runs with:

- Beam half-angle sweep (`TOF_BEAM_TRACE_HALF_ANGLES`, default `25` and `30` deg)
- Receiver capture-radius sweep (`TOF_BEAM_TRACE_RECEIVER_RADII_MM`, default `8/16/25` mm)
- Beam-energy weighting (`TOF_BEAM_WEIGHT_EXPONENTS`, default `n=6` and `n=10`)
- Reflector mode sweep (`TOF_REFLECTOR_TEST_MODES`: `flat`, `dish`, `cone`)

### Weighted hit metric

For each ray, weight is:

$$w = \cos(\theta)^n$$

Where $\theta$ is off-axis angle from the beam axis. Reported weighted hit fraction is:

$$\text{weighted hit} = \frac{\sum w_{\text{hit}}}{\sum w_{\text{all valid rays}}}$$

### Obstruction / interference indicators

The diagnostic reports:

- Bore clipping count (source-side angular clipping proxy)
- Standoff intersection count (ray-segment vs standoff-cylinder proxy)
- Pod-rim clip percent at receiver plane
- Roof footprint radius distribution (`p90/p99`)
- Roof-hit fraction falling inside PAR aperture/wall radii

### Reflector comparison output

For each reflector mode it reports:

- Geometric hit fraction by receiver radius
- Weighted hit fractions by receiver radius for each $n$
- Miss-radius distribution (`p50/p90/p99`)
- Center-ray path-length delta vs flat baseline

The macro also prints a simple recommendation:

- `Flat reflector acceptable` when best weighted gain is small
- `Consider weak focusing reflector` when weighted gain is meaningful

## 9) Latest beam-trace result and decision (Mar 5, 2026 @ 21:04)

Geometry and slope sanity checks passed:

- Axis slope from tilt: $-0.63217$
- Expected slope from geometry: $-0.63226$
- Center-ray alignment remains valid (flat check miss around $0.01\ \text{mm}$)

Key flat-reflector results from the latest run:

- Half-angle $25^\circ$:
   - Weighted hit ($n=10$):
      - $R=8\ \text{mm}$: $2.52\%$
      - $R=16\ \text{mm}$: $8.03\%$
      - $R=25\ \text{mm}$: $18.40\%$
- Half-angle $30^\circ$:
   - Weighted hit ($n=10$):
      - $R=8\ \text{mm}$: $2.57\%$
      - $R=16\ \text{mm}$: $8.71\%$
      - $R=25\ \text{mm}$: $17.77\%$

Comparison against optional weak-focusing test modes (`dish`, `cone`):

- Both test modes reduced weighted hit fraction versus flat for the same settings.
- Both introduced center-ray path change relative to flat baseline:
   - Dish: about $-2.507\ \text{mm}$
   - Cone: about $-2.398\ \text{mm}$

Current design recommendation:

- Keep the flat reflector for now.
- Do not change pod tilt, pod spacing, or reflector height.
- Treat the beam trace as a relative geometric robustness metric, while final SNR/reliability is still validated by RX analog chain and first-arrival detection logic.

Observed interference indicators from this run:

- Standoff interaction counts are non-zero (hundreds of rays in this Monte-Carlo setup), so maintain cautious comparator gating/windowing in firmware.
- Roof-hit fractions in PAR aperture/wall regions are non-trivial, so avoid adding new roof features near the current footprint until firmware timing is validated on bench.
