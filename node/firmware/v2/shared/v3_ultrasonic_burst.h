#pragma once
// V3 ultrasonic burst generation — header-only
// Bit-banged 40 kHz burst with timestamps referenced to the first TX rising edge.
// Production firmware should replace this with ESP32 RMT TX for deterministic timing.

#include <Arduino.h>
#include "v3_ultrasonic_pins.h"

// Default burst parameters
#ifndef BURST_CYCLES
#define BURST_CYCLES 12
#endif

#ifndef BURST_HALF_PERIOD_US
#define BURST_HALF_PERIOD_US 12   // ~41.7 kHz before software overhead
#endif

// ---------------------------------------------------------------------------
// BurstResult — timestamps for V3 time-origin accounting
//   firstEdgeUs: micros() at first TX rising edge (V3 time origin)
//   lastEdgeUs:  micros() at final TX falling edge
//   cycles:     actual cycles sent
// ---------------------------------------------------------------------------
struct BurstResult {
  uint32_t firstEdgeUs;    // micros() at first TX rising edge (V3 time origin)
  uint32_t lastEdgeUs;     // micros() at final TX falling edge
  int cycles;              // actual cycles sent
};

// ---------------------------------------------------------------------------
// sendBurst40kHz — bit-banged burst, returns BurstResult
// V3: firstEdgeUs is captured at the FIRST rising edge, not at function entry.
// This is the time origin for all TOF measurements.
// ---------------------------------------------------------------------------
static inline BurstResult sendBurst40kHz(int cycles) {
  const uint32_t halfPeriodUs = BURST_HALF_PERIOD_US;
  BurstResult result;
  result.cycles = cycles;
  result.firstEdgeUs = 0;
  result.lastEdgeUs = 0;

  for (int i = 0; i < cycles; i++) {
    digitalWrite(PIN_TX_BURST_PWM, HIGH);
    if (i == 0) {
      // Capture time origin at the very first TX rising edge
      result.firstEdgeUs = micros();
    }
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PIN_TX_BURST_PWM, LOW);
    delayMicroseconds(halfPeriodUs);
  }

  result.lastEdgeUs = micros();
  return result;
}

// ---------------------------------------------------------------------------
// sendBurstDefault — convenience wrapper using BURST_CYCLES default
// ---------------------------------------------------------------------------
static inline BurstResult sendBurstDefault() {
  return sendBurst40kHz(BURST_CYCLES);
}