#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AS7341.h>

#include "sensors_par_as7343.h"

extern TwoWire WireRtc;
extern bool muxSelectChannel(uint8_t ch);

namespace {

constexpr uint8_t kMuxChAs734x = 1;

// Channel table exposed to the sensor registry. The first 8 are the raw visible
// spectral bands (415–680 nm). The remainder are additional AS7341 outputs the
// backend needs to normalise and interpret the bands:
//   CLEAR / NIR  — extra photodiodes (raw counts)
//   GAIN         — gain multiplier actually applied this read (auto-exposure)
//   ATIME_MS     — integration time this read, in ms
//   SAT          — saturation/validity flag (0 = ok, 1 = saturated/invalid)
// All are keyed by their label in sensors.cpp::resolveSensorId().
enum ChannelIndex {
  CH_415 = 0, CH_445, CH_480, CH_515, CH_555, CH_590, CH_630, CH_680,
  CH_CLEAR, CH_NIR, CH_GAIN, CH_ATIME_MS, CH_SAT,
  kChannelCount
};

// Only the 8 visible bands are registry channels. Everything from CH_CLEAR on
// is per-read metadata, packed separately by the snapshot builder.
constexpr size_t kBandCount = CH_CLEAR;  // = 8

const char* const kChannelLabels[kChannelCount] = {
  "SPECTRAL_415",
  "SPECTRAL_445",
  "SPECTRAL_480",
  "SPECTRAL_515",
  "SPECTRAL_555",
  "SPECTRAL_590",
  "SPECTRAL_630",
  "SPECTRAL_680",
  "SPECTRAL_CLEAR",
  "SPECTRAL_NIR",
  "SPECTRAL_GAIN",
  "SPECTRAL_ATIME_MS",
  "SPECTRAL_SAT",
};

float g_lastChannel[kChannelCount];

Adafruit_AS7341 g_par;
bool g_ready = false;
uint32_t g_lastSampleMs = 0;
bool g_haveSample = false;

// AS7341 STATUS2 (0xA3) saturation bits. ASAT_ANALOG (bit3) or ASAT_DIGITAL
// (bit4) mean at least one channel clipped; AVALID (bit6) means the spectral
// data is valid. Not exposed by the Adafruit library, so read directly.
constexpr uint8_t AS7341_STATUS2_REG    = 0xA3;
constexpr uint8_t AS7341_STATUS2_ASAT   = 0x18;  // ASAT_ANALOG | ASAT_DIGITAL
constexpr uint8_t AS7341_STATUS2_AVALID = 0x40;

// Auto-exposure gain ladder. We start near the middle and walk up (dark) or
// down (bright/saturated) so both deep shade and direct sun stay on-scale. The
// applied gain is reported so the backend can normalise raw counts to
// gain/integration-independent "basic counts".
struct GainStep { as7341_gain_t gain; float mult; };
const GainStep kGainLadder[] = {
  {AS7341_GAIN_0_5X,   0.5f},
  {AS7341_GAIN_1X,     1.0f},
  {AS7341_GAIN_2X,     2.0f},
  {AS7341_GAIN_4X,     4.0f},
  {AS7341_GAIN_8X,     8.0f},
  {AS7341_GAIN_16X,   16.0f},
  {AS7341_GAIN_32X,   32.0f},
  {AS7341_GAIN_64X,   64.0f},
  {AS7341_GAIN_128X, 128.0f},
  {AS7341_GAIN_256X, 256.0f},
  {AS7341_GAIN_512X, 512.0f},
};
constexpr size_t kGainLadderLen = sizeof(kGainLadder) / sizeof(kGainLadder[0]);
constexpr size_t kGainStart     = 3;  // AS7341_GAIN_4X — legacy default
constexpr size_t kMaxAutoGainTries = 6;
size_t g_gainIdx = kGainStart;

bool readStatus2(uint8_t& out) {
  WireRtc.beginTransmission(AS7341_I2CADDR_DEFAULT);
  WireRtc.write(AS7341_STATUS2_REG);
  if (WireRtc.endTransmission(false) != 0) return false;
  if (WireRtc.requestFrom((int)AS7341_I2CADDR_DEFAULT, 1) != 1) return false;
  if (!WireRtc.available()) return false;
  out = WireRtc.read();
  return true;
}

// Full-scale ADC count for the current ATIME/ASTEP. Saturation is reached at
// min(65535, (ATIME+1)*(ASTEP+1)); the auto-gain thresholds are fractions of it.
uint32_t fullScaleCounts() {
  uint32_t fs = (uint32_t)(g_par.getATIME() + 1) * (uint32_t)(g_par.getASTEP() + 1);
  return fs > 65535UL ? 65535UL : fs;
}

// Largest of the visible bands + Clear — the channels we drive exposure from.
float maxExposureCount() {
  float m = 0.0f;
  static const as7341_color_channel_t kExp[] = {
    AS7341_CHANNEL_415nm_F1, AS7341_CHANNEL_445nm_F2, AS7341_CHANNEL_480nm_F3,
    AS7341_CHANNEL_515nm_F4, AS7341_CHANNEL_555nm_F5, AS7341_CHANNEL_590nm_F6,
    AS7341_CHANNEL_630nm_F7, AS7341_CHANNEL_680nm_F8, AS7341_CHANNEL_CLEAR,
  };
  for (as7341_color_channel_t c : kExp) {
    float v = (float)g_par.getChannel(c);
    if (v > m) m = v;
  }
  return m;
}

void sampleIfNeeded() {
  uint32_t now = millis();
  if (g_haveSample && (now - g_lastSampleMs) < 300) {
    return;
  }

  if (!muxSelectChannel(kMuxChAs734x)) {
    g_haveSample = false;
    Serial.println(F("[PAR] AS734x mux select failed"));
    return;
  }
  delay(2);

  // Ping the AS7341 before attempting a full read. The Adafruit_AS7341
  // readAllChannels() path has internal delays and register accesses; if the
  // sensor is hung or absent, this ping returns quickly instead of burning
  // the full WireRtc timeout on every register access.
  WireRtc.beginTransmission(AS7341_I2CADDR_DEFAULT);
  if (WireRtc.endTransmission() != 0) {
    g_haveSample = false;
    Serial.println(F("[PAR] AS734x not responding — skipping spectral this cycle"));
    return;
  }

  // Auto-exposure loop: read, then step gain up (too dark) or down (saturated)
  // and re-read until the strongest channel sits mid-scale or we run out of
  // ladder/tries. Each re-read costs one integration (~50 ms), bounded by
  // kMaxAutoGainTries.
  uint8_t status2 = 0;
  bool statusValid = false;
  bool saturated = false;
  const uint32_t fs = fullScaleCounts();
  const float satHigh = 0.90f * (float)fs;  // step down above this
  const float satLow  = 0.04f * (float)fs;  // step up below this

  bool haveRead = false;
  for (size_t attempt = 0; attempt < kMaxAutoGainTries; ++attempt) {
    if (!g_par.readAllChannels()) {
      g_haveSample = false;
      Serial.println(F("[PAR] AS734x read failed"));
      return;
    }
    haveRead = true;

    status2 = 0;
    statusValid = readStatus2(status2);
    saturated = statusValid && ((status2 & AS7341_STATUS2_ASAT) != 0);

    const float maxCount = maxExposureCount();
    const bool tooBright = saturated || maxCount >= satHigh;
    const bool tooDark   = maxCount < satLow;

    // Never change gain after the final acquisition: the metadata must report
    // the gain that produced the accepted counts. Reserve another attempt for
    // every gain change so the new setting is actually sampled.
    const bool canRetry = (attempt + 1U) < kMaxAutoGainTries;
    if (canRetry && tooBright && g_gainIdx > 0) {
      g_gainIdx--;
      if (!g_par.setGain(kGainLadder[g_gainIdx].gain)) {
        g_haveSample = false;
        Serial.println(F("[PAR] AS734x auto-gain step-down failed"));
        return;
      }
      continue;
    }
    if (canRetry && tooDark && g_gainIdx < kGainLadderLen - 1) {
      g_gainIdx++;
      if (!g_par.setGain(kGainLadder[g_gainIdx].gain)) {
        g_haveSample = false;
        Serial.println(F("[PAR] AS734x auto-gain step-up failed"));
        return;
      }
      continue;
    }
    break;  // on-scale, or clamped at ladder end — accept this read
  }
  if (!haveRead) {
    g_haveSample = false;
    return;
  }

  // Raw visible bands.
  g_lastChannel[CH_415] = (float)g_par.getChannel(AS7341_CHANNEL_415nm_F1);
  g_lastChannel[CH_445] = (float)g_par.getChannel(AS7341_CHANNEL_445nm_F2);
  g_lastChannel[CH_480] = (float)g_par.getChannel(AS7341_CHANNEL_480nm_F3);
  g_lastChannel[CH_515] = (float)g_par.getChannel(AS7341_CHANNEL_515nm_F4);
  g_lastChannel[CH_555] = (float)g_par.getChannel(AS7341_CHANNEL_555nm_F5);
  g_lastChannel[CH_590] = (float)g_par.getChannel(AS7341_CHANNEL_590nm_F6);
  g_lastChannel[CH_630] = (float)g_par.getChannel(AS7341_CHANNEL_630nm_F7);
  g_lastChannel[CH_680] = (float)g_par.getChannel(AS7341_CHANNEL_680nm_F8);

  // Extra photodiodes + exposure metadata for backend normalisation.
  g_lastChannel[CH_CLEAR]    = (float)g_par.getChannel(AS7341_CHANNEL_CLEAR);
  g_lastChannel[CH_NIR]      = (float)g_par.getChannel(AS7341_CHANNEL_NIR);
  g_lastChannel[CH_GAIN]     = kGainLadder[g_gainIdx].mult;
  g_lastChannel[CH_ATIME_MS] = (float)g_par.getTINT();  // integration time (ms)

  // Saturation flag: derived from the ACCEPTED (cached) channel counts, not a
  // post-read STATUS2. readAllChannels() leaves the AS7341 free-running (its
  // final enableSpectralMeasurement(true) is never cleared), so reading STATUS2
  // after it returns races a fresh, in-flight integration: AVALID reads 0 and
  // the ASAT bits are stale. That pinned this flag at 1 on every exposure even
  // in dim light (~7% of full scale). Validity is already guaranteed by the
  // successful readAllChannels() (delayForData waited on data-ready); genuine
  // clipping is fully determined by whether the strongest channel sits at ADC
  // full scale for the applied ATIME/ASTEP. STATUS2/ASAT is still read above as
  // an auto-gain hint, but no longer gates the reported saturation.
  const float clipThreshold = 0.99f * (float)fs;
  const bool  clipped = maxExposureCount() >= clipThreshold;
  g_lastChannel[CH_SAT] = clipped ? 1.0f : 0.0f;

  float parProxy = 0.0f;
  for (size_t i = CH_415; i <= CH_680; ++i) parProxy += g_lastChannel[i];

  // Timestamp the completed exposure, not the start of a potentially long
  // auto-gain sequence. getMetadata() never starts a second acquisition:
  // Clear/NIR/gain/time must qualify these exact visible-band counts.
  g_lastSampleMs = millis();
  g_haveSample = true;

  Serial.printf("[PAR] F1=%.0f F4=%.0f F8=%.0f CLR=%.0f NIR=%.0f "
                "GAIN=%.1fx TINT=%.0fms SAT=%.0f PAR_PROXY=%.1f\n",
                g_lastChannel[CH_415], g_lastChannel[CH_515], g_lastChannel[CH_680],
                g_lastChannel[CH_CLEAR], g_lastChannel[CH_NIR],
                g_lastChannel[CH_GAIN], g_lastChannel[CH_ATIME_MS],
                g_lastChannel[CH_SAT], parProxy);
}

} // namespace

namespace par_as7343_backend {

bool init() {
  if (g_ready) return true;

  for (size_t i = 0; i < kChannelCount; ++i) g_lastChannel[i] = NAN;

  if (!muxSelectChannel(kMuxChAs734x)) {
    Serial.println(F("[PAR] AS734x mux ch1 not selectable"));
    return false;
  }
  delay(5);

  if (!g_par.begin(AS7341_I2CADDR_DEFAULT, &WireRtc)) {
    Serial.println(F("[PAR] AS734x begin failed on WireRtc"));
    return false;
  }

  g_par.powerEnable(true);
  if (!g_par.setATIME(29)) {
    Serial.println(F("[PAR] AS734x setATIME failed"));
    return false;
  }

  if (!g_par.setASTEP(599)) {
    Serial.println(F("[PAR] AS734x setASTEP failed"));
    return false;
  }

  g_gainIdx = kGainStart;
  if (!g_par.setGain(kGainLadder[g_gainIdx].gain)) {
    Serial.println(F("[PAR] AS734x setGain failed"));
    return false;
  }

  if (!g_par.enableSpectralMeasurement(true)) {
    Serial.println(F("[PAR] AS734x enableSpectralMeasurement failed"));
    return false;
  }

  g_ready = true;
  g_haveSample = false;
  Serial.println(F("[PAR] AS734x ready on mux ch1 (auto-gain enabled)"));
  return true;
}

size_t count() {
  // Registry channels = the 8 visible bands only. Clear/NIR/gain/integration/
  // saturation describe this one measurement and are packed as metadata, not
  // registered as separate sensors.
  return g_ready ? kBandCount : 0;
}

const char* label(size_t index) {
  if (index < kBandCount) return kChannelLabels[index];
  return "UNKNOWN";
}

const char* type(size_t index) {
  if (index < kBandCount) return "PAR";
  return "UNKNOWN";
}

bool read(size_t index, float& outValue) {
  if (!g_ready || index >= kBandCount) return false;

  sampleIfNeeded();
  if (!g_haveSample) return false;

  outValue = g_lastChannel[index];
  return true;
}

bool metadataAvailable() {
  return g_ready && g_haveSample;
}

SpectralMetadata getMetadata() {
  SpectralMetadata m{};
  m.valid = false;
  if (!g_ready || !g_haveSample) return m;

  m.clear         = g_lastChannel[CH_CLEAR];
  m.nir           = g_lastChannel[CH_NIR];
  m.gain          = g_lastChannel[CH_GAIN];
  m.integrationMs = g_lastChannel[CH_ATIME_MS];
  m.saturated     = (g_lastChannel[CH_SAT] != 0.0f) ? 1 : 0;
  m.valid         = true;
  return m;
}

} // namespace par_as7343_backend
