#pragma once
// V3 ultrasonic edge capture ISR and helpers — header-only
// Captures first 3 + last edge timestamps for TOF measurement.
// V3: TOF is referenced to the first TX rising edge (txFirstEdgeUs),
//     not to the listen-start time.

#include <Arduino.h>
#include "v3_ultrasonic_pins.h"

// Capture tuning constants (all with #ifndef guards)
#ifndef BLANKING_US
#define BLANKING_US 320
#endif

#ifndef TOF_TIMEOUT_US
#define TOF_TIMEOUT_US 3000
#endif

#ifndef MIN_VALID_TOF_US
#define MIN_VALID_TOF_US 220
#endif

#ifndef POST_ENABLE_GUARD_US
#define POST_ENABLE_GUARD_US 20
#endif

// ---------------------------------------------------------------------------
// CaptureResult — all timestamps relative to TX first edge
// ---------------------------------------------------------------------------
struct CaptureResult {
  bool detected;           // at least one valid edge found
  uint32_t edgeCount;      // total edges captured
  int32_t tofUs;           // first valid edge relative to TX first edge
  int32_t firstEdgeUs;     // first edge relative to TX first edge
  int32_t secondEdgeUs;    // second edge relative to TX first edge
  int32_t thirdEdgeUs;     // third edge relative to TX first edge
  int32_t lastEdgeUs;      // last edge relative to TX first edge
};

// ---------------------------------------------------------------------------
// ISR shared state (volatile, matching V2 pattern)
//
// WARNING: The volatile globals below use `static` linkage, meaning each
// translation unit that includes this header gets its own copy.  This header
// MUST only be included from a single .cpp file in any given build; otherwise
// ISR writes and reader reads will refer to different variables and captures
// will silently fail.
// ---------------------------------------------------------------------------
namespace v3_capture {

volatile uint32_t g_edgeCount = 0;
volatile uint32_t g_firstEdgeUs = 0;
volatile uint32_t g_secondEdgeUs = 0;
volatile uint32_t g_thirdEdgeUs = 0;
volatile uint32_t g_lastEdgeUs = 0;
volatile bool g_captureArmed = false;

// ---------------------------------------------------------------------------
// ISR — RISING edge on PIN_TOF_EDGE
// Stores first 3 edge timestamps + last edge timestamp.
// Do NOT use Serial here.
// ---------------------------------------------------------------------------
static void IRAM_ATTR onTofEdge() {
  if (!g_captureArmed) {
    return;
  }
  const uint32_t nowUs = micros();
  const uint32_t newCount = g_edgeCount + 1;
  g_edgeCount = newCount;
  if (newCount == 1) {
    g_firstEdgeUs = nowUs;
  } else if (newCount == 2) {
    g_secondEdgeUs = nowUs;
  } else if (newCount == 3) {
    g_thirdEdgeUs = nowUs;
  }
  g_lastEdgeUs = nowUs;
}

// ---------------------------------------------------------------------------
// installTofEdgeIsr — attach RISING interrupt on PIN_TOF_EDGE
// ---------------------------------------------------------------------------
static inline void installTofEdgeIsr() {
  attachInterrupt(digitalPinToInterrupt(PIN_TOF_EDGE), onTofEdge, RISING);
}

// ---------------------------------------------------------------------------
// armCapture / disarmCapture — control ISR acceptance
// ---------------------------------------------------------------------------
static inline void armCapture() {
  g_captureArmed = true;
}

static inline void disarmCapture() {
  g_captureArmed = false;
}

// ---------------------------------------------------------------------------
// resetEdgeCapture — clear all edge state
// ---------------------------------------------------------------------------
static inline void resetEdgeCapture() {
  g_edgeCount = 0;
  g_firstEdgeUs = 0;
  g_secondEdgeUs = 0;
  g_thirdEdgeUs = 0;
  g_lastEdgeUs = 0;
}

// ---------------------------------------------------------------------------
// Helper: compute edge timestamp relative to TX first edge
// Returns -1 if edge timestamp is 0 (not captured)
// ---------------------------------------------------------------------------
static inline int32_t edgeRelUs(uint32_t edgeTs, uint32_t t0) {
  if (edgeTs == 0) {
    return -1;
  }
  return static_cast<int32_t>(edgeTs - t0);
}

// ---------------------------------------------------------------------------
// getCaptureResult — poll for edges, compute TOF relative to txFirstEdgeUs
//   txFirstEdgeUs: micros() timestamp of the first TX rising edge (V3 time origin)
//   timeoutUs: maximum time to wait for a valid edge
// Returns CaptureResult with all timestamps relative to txFirstEdgeUs.
// ---------------------------------------------------------------------------
static inline CaptureResult getCaptureResult(uint32_t txFirstEdgeUs, uint32_t timeoutUs) {
  CaptureResult result;
  result.detected = false;
  result.edgeCount = 0;
  result.tofUs = -1;
  result.firstEdgeUs = -1;
  result.secondEdgeUs = -1;
  result.thirdEdgeUs = -1;
  result.lastEdgeUs = -1;

  const uint32_t pollStartUs = micros();
  uint32_t handledCount = 0;

  while ((micros() - pollStartUs) < timeoutUs) {
    const uint32_t countNow = g_edgeCount;
    if (countNow > handledCount) {
      handledCount = countNow;
      // Compute TOF relative to TX first edge (V3: not listenStartUs)
      const uint32_t edgeUs = g_lastEdgeUs;
      const int32_t tofRel = static_cast<int32_t>(edgeUs - txFirstEdgeUs);

      if (tofRel >= static_cast<int32_t>(MIN_VALID_TOF_US)) {
        result.detected = true;
        result.tofUs = tofRel;
        break;
      }
    }
  }

  // Fill in all edge timestamps relative to TX first edge
  result.edgeCount = g_edgeCount;
  result.firstEdgeUs = edgeRelUs(g_firstEdgeUs, txFirstEdgeUs);
  result.secondEdgeUs = edgeRelUs(g_secondEdgeUs, txFirstEdgeUs);
  result.thirdEdgeUs = edgeRelUs(g_thirdEdgeUs, txFirstEdgeUs);
  result.lastEdgeUs = edgeRelUs(g_lastEdgeUs, txFirstEdgeUs);

  // If we didn't detect a valid edge but did capture something, report last TOF
  if (!result.detected && g_edgeCount > 0) {
    result.tofUs = static_cast<int32_t>(g_lastEdgeUs - txFirstEdgeUs);
  }

  return result;
}

} // namespace v3_capture