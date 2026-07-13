// Standalone AS7341 spectral-metadata bring-up test.
//
// PURPOSE
//   Isolate whether this node's AS7341 reads back the FIVE extended spectral
//   fields — Clear, NIR, applied gain, integration time, saturation flag — as
//   finite numbers. These are the fields (sensor IDs 1109-1113) arriving as
//   null in the backend while the 8 visible bands are numeric.
//
// WHY THIS IS DECISIVE
//   It reuses the PRODUCTION backend (src/sensors/sensors_par_as7343.cpp)
//   unchanged. The extended fields are cached in the SAME acquisition as the
//   visible bands (see sampleIfNeeded()), so:
//     - PASS here  => the sensor + backend produce the values fine, and the
//                     field nulls are stale/old-firmware data, not a live fault.
//     - FAIL here  => we've reproduced the fault on the bench and can see the
//                     exact backend log ([PAR] / STATUS2) that explains it.
//   Because it is the real backend, a green result folds into main.cpp with no
//   code translation.
//
// FLASH
//   pio run -e esp32wroom-spectral-metadata-test -t upload -t monitor
// RESTORE PRODUCTION FIRMWARE AFTER TESTING
//   pio run -e esp32wroom -t upload
//
// Point the AS7341 at reasonably even light (not pointed at a hard specular
// highlight). Auto-gain walks the ladder; saturated=1 means valid-but-clipped.

#include <Arduino.h>
#include <Wire.h>

#include "sensors/sensors_par_as7343.h"

#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 18
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 19
#endif
#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif
#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif
#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

// ---------------------------------------------------------------------------
// Symbols the AS7341 backend expects from the main firmware.
// The backend samples over I2C0 (WireRtc) behind a PCA9548A mux, selecting the
// AS7341's channel itself (kMuxChAs734x = 1). We provide the same two symbols
// main.cpp does so the production backend links unmodified.
// ---------------------------------------------------------------------------
TwoWire WireRtc(0);

bool muxSelectChannel(uint8_t ch) {
  if (ch > 7) return false;
  WireRtc.beginTransmission(MUX_ADDR);
  WireRtc.write((uint8_t)(1u << ch));
  return (WireRtc.endTransmission() == 0);
}

// Keep VSYS latched so the gated-power node doesn't cut itself mid-test when
// running on battery (harmless when powered via USB). Asserted first thing.
static void latchPowerHold() {
#if defined(PWR_HOLD_PIN) && (PWR_HOLD_PIN >= 0)
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);
#endif
}

static bool     g_initOk = false;
static uint32_t g_cycle  = 0;
static uint32_t g_pass   = 0;

void setup() {
  latchPowerHold();
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println(F("================================================="));
  Serial.println(F(" AS7341 spectral-metadata bring-up test"));
  Serial.println(F("================================================="));
  Serial.printf ("[FW] test build=%s %s\n", __DATE__, __TIME__);
  Serial.println(F("Proving Clear/NIR/gain/integration/saturation read finite."));

  WireRtc.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  WireRtc.setClock(100000);

  g_initOk = par_as7343_backend::init();
  if (!g_initOk) {
    Serial.println(F("[RESULT] FAIL — AS7341 init failed (sensor absent, mux, or wiring)."));
    Serial.println(F("         On such a node the 8 VISIBLE bands are null too."));
    Serial.println(F("         Fix detection/wiring before chasing the extended fields."));
  } else {
    Serial.println(F("[OK] AS7341 initialised on mux ch1. Sampling every 3 s..."));
  }
}

void loop() {
  delay(3000);
  g_cycle++;
  Serial.printf("\n----- cycle %lu -----\n", (unsigned long)g_cycle);

  if (!g_initOk) {
    // Re-attempt init in case the sensor was powered/plugged after boot.
    g_initOk = par_as7343_backend::init();
    if (!g_initOk) { Serial.println(F("[SPEC] AS7341 still not ready.")); return; }
    Serial.println(F("[OK] AS7341 came ready."));
  }

  // read() drives sampleIfNeeded() — the one acquisition that also caches the
  // extended fields. Read all 8 visible bands first (as the snapshot builder
  // does), then pull the metadata from that same exposure.
  float bands[8];
  bool visibleOk = true;
  for (size_t i = 0; i < 8; ++i) {
    if (!par_as7343_backend::read(i, bands[i]) || !isfinite(bands[i])) visibleOk = false;
  }

  if (!par_as7343_backend::metadataAvailable()) {
    Serial.println(F("[SPEC] metadataAvailable()=false — no valid exposure cached this cycle."));
    Serial.println(F("[VERDICT] FAIL: acquisition did not complete (see [PAR] line above)."));
    return;
  }

  par_as7343_backend::SpectralMetadata md = par_as7343_backend::getMetadata();
  const bool finite = isfinite(md.clear) && isfinite(md.nir) &&
                      isfinite(md.gain)  && isfinite(md.integrationMs);

  Serial.printf("[SPEC] visible : 415=%.0f 445=%.0f 480=%.0f 515=%.0f "
                "555=%.0f 590=%.0f 630=%.0f 680=%.0f\n",
                bands[0], bands[1], bands[2], bands[3],
                bands[4], bands[5], bands[6], bands[7]);
  Serial.printf("[SPEC] EXTENDED: clear=%.1f nir=%.1f gain=%.1fx integration_ms=%.2f "
                "saturated=%u valid=%u\n",
                md.clear, md.nir, md.gain, md.integrationMs,
                (unsigned)md.saturated, (unsigned)md.valid);

  if (md.valid && finite) {
    g_pass++;
    Serial.printf("[VERDICT] PASS: 5 extended fields finite (%lu/%lu cycles).\n",
                  (unsigned long)g_pass, (unsigned long)g_cycle);
    if (md.saturated)
      Serial.println(F("          NOTE: saturated=1 — values valid but clipped; even out the light."));
  } else {
    Serial.println(F("[VERDICT] FAIL: metadata invalid/non-finite — reproduces the backend null."));
  }
  if (!visibleOk)
    Serial.println(F("          WARN: a visible band was non-finite this cycle."));
}
