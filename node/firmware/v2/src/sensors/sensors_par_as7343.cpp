#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AS7341.h>

#include "sensors_par_as7343.h"

extern TwoWire WireRtc;
extern bool muxSelectChannel(uint8_t ch);

namespace {

constexpr uint8_t kMuxChAs734x = 1;
constexpr size_t kSpectralCount = 8;

const char* const kSpectralLabels[kSpectralCount] = {
  "SPECTRAL_415",
  "SPECTRAL_445",
  "SPECTRAL_480",
  "SPECTRAL_515",
  "SPECTRAL_555",
  "SPECTRAL_590",
  "SPECTRAL_630",
  "SPECTRAL_680",
};

float g_lastSpectral[kSpectralCount] = {
  NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN
};

Adafruit_AS7341 g_par;
bool g_ready = false;
uint32_t g_lastSampleMs = 0;
bool g_haveSample = false;

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

  if (!g_par.readAllChannels()) {
    g_haveSample = false;
    Serial.println(F("[PAR] AS734x read failed"));
    return;
  }

  // PAR proxy from visible bands. Replace with calibrated PAR conversion later.
  g_lastSpectral[0] = (float)g_par.getChannel(AS7341_CHANNEL_415nm_F1);
  g_lastSpectral[1] = (float)g_par.getChannel(AS7341_CHANNEL_445nm_F2);
  g_lastSpectral[2] = (float)g_par.getChannel(AS7341_CHANNEL_480nm_F3);
  g_lastSpectral[3] = (float)g_par.getChannel(AS7341_CHANNEL_515nm_F4);
  g_lastSpectral[4] = (float)g_par.getChannel(AS7341_CHANNEL_555nm_F5);
  g_lastSpectral[5] = (float)g_par.getChannel(AS7341_CHANNEL_590nm_F6);
  g_lastSpectral[6] = (float)g_par.getChannel(AS7341_CHANNEL_630nm_F7);
  g_lastSpectral[7] = (float)g_par.getChannel(AS7341_CHANNEL_680nm_F8);

  float parProxy = 0.0f;
  for (size_t i = 0; i < kSpectralCount; ++i) {
    parProxy += g_lastSpectral[i];
  }

  g_lastSampleMs = now;
  g_haveSample = true;

  Serial.printf("[PAR] F1=%.0f F4=%.0f F8=%.0f NIR=%.0f PAR_PROXY=%.1f\n",
                g_lastSpectral[0],
                g_lastSpectral[3],
                g_lastSpectral[7],
                (float)g_par.getChannel(AS7341_CHANNEL_NIR),
                parProxy);
}

} // namespace

namespace par_as7343_backend {

bool init() {
  if (g_ready) return true;

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

  if (!g_par.setGain(AS7341_GAIN_4X)) {
    Serial.println(F("[PAR] AS734x setGain failed"));
    return false;
  }

  if (!g_par.enableSpectralMeasurement(true)) {
    Serial.println(F("[PAR] AS734x enableSpectralMeasurement failed"));
    return false;
  }

  g_ready = true;
  g_haveSample = false;
  Serial.println(F("[PAR] AS734x ready on mux ch1"));
  return true;
}

size_t count() {
  return g_ready ? kSpectralCount : 0;
}

const char* label(size_t index) {
  if (index < kSpectralCount) return kSpectralLabels[index];
  return "UNKNOWN";
}

const char* type(size_t index) {
  if (index < kSpectralCount) return "PAR";
  return "UNKNOWN";
}

bool read(size_t index, float& outValue) {
  if (!g_ready || index >= kSpectralCount) return false;

  sampleIfNeeded();
  if (!g_haveSample) return false;

  outValue = g_lastSpectral[index];
  return true;
}

} // namespace par_as7343_backend
