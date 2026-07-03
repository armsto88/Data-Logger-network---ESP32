#include <Arduino.h>

#include "sensors_reed_wind.h"

// ---------------------------------------------------------------------------
// Configuration (overridable via build flags)
// ---------------------------------------------------------------------------

#ifndef REED_WIND_PIN
#define REED_WIND_PIN 4            // GPIO4 = RX_EN_N net (NOT the RTC; the DS3231
                                   // alarm powers VSYS via a FET gate on its own
                                   // line). Shared only with the ultrasonic RX.
#endif

#ifndef REED_WIND_PROBE_MS
#define REED_WIND_PROBE_MS 2000    // presence probe — cheap when calm / no sensor;
                                   // 2 edges in 2 s ≈ 0.67 m/s ≈ anemometer floor
#endif

#ifndef REED_WIND_WINDOW_MS
#define REED_WIND_WINDOW_MS 10000  // 10 s averaging window once rotation detected
#endif

#ifndef REED_WIND_MIN_EDGES
#define REED_WIND_MIN_EDGES 2      // < this in the probe = calm (also rejects a
                                   // stray single electrical glitch)
#endif

#ifndef REED_WIND_FACTOR
#define REED_WIND_FACTOR 0.6667f   // m/s per Hz (WH-SP-WS01: 1 Hz = 2.4 km/h)
#endif

#ifndef REED_WIND_OFFSET
#define REED_WIND_OFFSET 0.0f      // m/s offset (linear, no offset)
#endif

#ifndef REED_WIND_DEBOUNCE_MS
#define REED_WIND_DEBOUNCE_MS 5    // reed bounce rejection window
#endif

namespace {

volatile uint32_t g_edgeCount = 0;
volatile unsigned long g_lastEdgeMs = 0;
bool g_initialized = false;

void IRAM_ATTR onReedFalling() {
  unsigned long now = millis();
  if (now - g_lastEdgeMs >= REED_WIND_DEBOUNCE_MS) {
    g_lastEdgeMs = now;
    g_edgeCount++;
  }
}

void resetCount() {
  noInterrupts();
  g_edgeCount = 0;
  g_lastEdgeMs = 0;
  interrupts();
}

uint32_t readCount() {
  noInterrupts();
  const uint32_t c = g_edgeCount;
  interrupts();
  return c;
}

}  // namespace

namespace reed_wind_backend {

bool init() {
  if (g_initialized) return true;
  // The reed pulls the line LOW once per revolution. INPUT_PULLUP backs up the
  // external pull-up (and matches the DS3231's open-drain INT that shares the
  // pin). Count falling edges. On a node with no anemometer the line simply
  // stays high, so no edges are counted.
  pinMode(REED_WIND_PIN, INPUT_PULLUP);
  resetCount();
  attachInterrupt(digitalPinToInterrupt(REED_WIND_PIN), onReedFalling, FALLING);
  g_initialized = true;
  Serial.printf("[WIND] Reed anemometer backend ready (pin=%d, probe=%dms, "
                "window=%dms, V=%.3f*f+%.1f)\n",
                REED_WIND_PIN, REED_WIND_PROBE_MS, REED_WIND_WINDOW_MS,
                REED_WIND_FACTOR, REED_WIND_OFFSET);
  return true;
}

size_t count() { return g_initialized ? 1 : 0; }

const char* label(size_t index) { return (index == 0) ? "WIND_SPEED" : "UNKNOWN"; }
const char* type(size_t index)  { return (index == 0) ? "WIND" : "UNKNOWN"; }

bool read(size_t index, float& outValue) {
  if (index != 0 || !g_initialized) return false;

  resetCount();
  const uint32_t startMs = millis();

  // Probe: cheap first look. delay() is chunked so the RTOS/idle watchdog is fed.
  while (millis() - startMs < (uint32_t)REED_WIND_PROBE_MS) delay(50);

  if (readCount() < (uint32_t)REED_WIND_MIN_EDGES) {
    // Calm, no anemometer wired, or a stray single glitch.
    outValue = 0.0f;
    return true;
  }

  // Rotation detected — extend to the full window for a stable frequency.
  while (millis() - startMs < (uint32_t)REED_WIND_WINDOW_MS) delay(50);

  const uint32_t edges = readCount();
  const float elapsedS = (millis() - startMs) / 1000.0f;
  const float freqHz = (elapsedS > 0.0f) ? (edges / elapsedS) : 0.0f;
  outValue = REED_WIND_FACTOR * freqHz + REED_WIND_OFFSET;

  Serial.printf("[WIND] reed: %lu edges / %.2fs = %.2f Hz -> %.2f m/s\n",
                (unsigned long)edges, elapsedS, freqHz, outValue);
  return true;
}

}  // namespace reed_wind_backend
