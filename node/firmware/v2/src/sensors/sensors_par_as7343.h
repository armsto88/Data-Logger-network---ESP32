#pragma once

#include <Arduino.h>

namespace par_as7343_backend {

// The AS7341 is ONE physical sensor. It exposes 8 visible spectral bands as
// registry "channels" (415–680 nm), plus per-read housekeeping (Clear, NIR, the
// auto-exposure gain/integration time, and a saturation flag). The housekeeping
// is NOT a set of independent sensors — it describes the single spectral
// measurement — so it rides the snapshot as metadata rather than claiming
// registry slots. See getMetadata().
struct SpectralMetadata {
  float   clear;           // wideband "Clear" photodiode, raw counts
  float   nir;             // ~910 nm NIR photodiode, raw counts
  float   gain;            // applied gain multiplier this read (0.5..512)
  float   integrationMs;   // integration time this read (ms)
  uint8_t saturated;       // 1 = saturated/invalid, 0 = valid
  bool    valid;           // false when no successful sample is available
};

bool init();

// Registry-facing channel count: the 8 visible bands only. Metadata is packed
// separately by the snapshot builder via getMetadata().
size_t count();
const char* label(size_t index);
const char* type(size_t index);
bool read(size_t index, float& outValue);

// True once init() has succeeded — the snapshot builder uses this to decide
// whether to append the metadata readings.
bool metadataAvailable();

// Returns the housekeeping values for the most recent spectral sample. Triggers
// a sample if needed (same cache as read()). `valid` is false if no sample could
// be taken this cycle.
SpectralMetadata getMetadata();

} // namespace par_as7343_backend
