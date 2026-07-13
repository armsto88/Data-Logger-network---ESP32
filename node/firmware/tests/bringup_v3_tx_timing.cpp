/*
 * Node V3 TX Timing Diagnostic — auto-run (no menu)
 *
 * Fires TX bursts and captures ALL edges in a 5 ms window (no gate filtering).
 * Prints every edge timestamp relative to TX first edge.
 * This reveals the true acoustic arrival time, ringdown, and any reverberation.
 *
 * Runs automatically on boot:
 *   1. RX enabled, boost ON, mux=N, fire 5 TX bursts (N), capture 5ms each
 *   2. Print all edge timestamps for each shot
 *   3. Summary: edge count, first/last edge, distribution
 */

#include <Arduino.h>

#ifdef DISABLE_BROWNOUT
#include "soc/rtc_cntl_reg.h"
#endif

#include "v3_ultrasonic_pins.h"
#include "v3_ultrasonic_safe.h"
#include "v3_ultrasonic_burst.h"
#include "v3_ultrasonic_direction.h"
#include "v3_ultrasonic_capture.h"

// ---------------------------------------------------------------------------
// Capture all edges in a window — returns edge count, fills timestamps
// ---------------------------------------------------------------------------
static uint32_t captureAllEdges(uint32_t txFirstEdgeUs, uint32_t windowUs) {
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();

  const uint32_t t0 = micros();
  while ((micros() - t0) < windowUs) {
    // busy-wait
  }
  v3_capture::disarmCapture();

  return v3_capture::g_edgeCount;
}

// ---------------------------------------------------------------------------
// Print all captured edges
// ---------------------------------------------------------------------------
static void printEdges(uint32_t txFirstEdgeUs) {
  const uint32_t count = v3_capture::g_edgeCount;
  Serial.print("  Total edges: ");
  Serial.println(count);

  if (count > 0) {
    int32_t first = static_cast<int32_t>(v3_capture::g_firstEdgeUs) - static_cast<int32_t>(txFirstEdgeUs);
    int32_t second = static_cast<int32_t>(v3_capture::g_secondEdgeUs) - static_cast<int32_t>(txFirstEdgeUs);
    int32_t third = static_cast<int32_t>(v3_capture::g_thirdEdgeUs) - static_cast<int32_t>(txFirstEdgeUs);
    int32_t last = static_cast<int32_t>(v3_capture::g_lastEdgeUs) - static_cast<int32_t>(txFirstEdgeUs);

    Serial.print("  E0(first)=  ");
    Serial.print(first);
    Serial.println("us");
    Serial.print("  E1(second)= ");
    Serial.print(second);
    Serial.println("us");
    Serial.print("  E2(third)=  ");
    Serial.print(third);
    Serial.println("us");
    Serial.print("  E(last)=    ");
    Serial.print(last);
    Serial.println("us");
  }
}

static void section(const char* title) {
  Serial.println();
  Serial.println("========================================");
  Serial.print("  ");
  Serial.println(title);
  Serial.println("========================================");
}

// ===========================================================================
// setup — runs all tests automatically
// ===========================================================================
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(800);

  initSafeState();
  v3_capture::installTofEdgeIsr();

  Serial.println();
  Serial.println("=== V3 TX Timing Diagnostic (auto-run) ===");
  Serial.println("Fires TX bursts, captures ALL edges in 5ms window (no gate).");
  Serial.println("Reveals true acoustic arrival, ringdown, reverberation.");
  Serial.println();

  const uint32_t CAPTURE_WINDOW_US = 5000UL;  // 5 ms
  const int NUM_SHOTS = 5;

  // Configure: RX=S, boost ON, TX=N
  setRxDirection('S');
  enableRxPath();
  delay(100);

  // ------------------------------------------------------------------
  // PART A: 5 shots with boost ON, 4-cycle burst, TX=N, RX=S
  // ------------------------------------------------------------------
  section("PART A: 5 shots, boost ON, 4-cycle burst, TX=N RX=S");

  for (int shot = 0; shot < NUM_SHOTS; shot++) {
    // Precharge boost
    enableBoost();
    delay(50);

    // TX=N
    setTxTransmit('N');
    delayMicroseconds(10);

    BurstResult burst = sendBurst40kHz(4);

    // Damping
    clearAllDirections();
    setDamping('N', 50);

    // Capture all edges in 5ms window
    uint32_t count = captureAllEdges(burst.firstEdgeUs, CAPTURE_WINDOW_US);

    Serial.print("SHOT ");
    Serial.print(shot + 1);
    Serial.print(": burst_dur=");
    Serial.print(burst.lastEdgeUs - burst.firstEdgeUs);
    Serial.print("us  ");
    printEdges(burst.firstEdgeUs);

    // Cleanup
    clearAllDirections();
    delay(200);  // inter-shot delay
  }

  // ------------------------------------------------------------------
  // PART B: 5 shots with boost ON, 12-cycle burst, TX=N, RX=S
  // ------------------------------------------------------------------
  section("PART B: 5 shots, boost ON, 12-cycle burst, TX=N RX=S");

  for (int shot = 0; shot < NUM_SHOTS; shot++) {
    enableBoost();
    delay(50);

    setTxTransmit('N');
    delayMicroseconds(10);

    BurstResult burst = sendBurst40kHz(12);

    clearAllDirections();
    setDamping('N', 50);

    uint32_t count = captureAllEdges(burst.firstEdgeUs, CAPTURE_WINDOW_US);

    Serial.print("SHOT ");
    Serial.print(shot + 1);
    Serial.print(": burst_dur=");
    Serial.print(burst.lastEdgeUs - burst.firstEdgeUs);
    Serial.print("us  ");
    printEdges(burst.firstEdgeUs);

    clearAllDirections();
    delay(200);
  }

  // ------------------------------------------------------------------
  // PART C: 5 shots with boost OFF (TX path only, no 22V), TX=N, RX=S
  // ------------------------------------------------------------------
  section("PART C: 5 shots, boost OFF, 4-cycle burst, TX=N RX=S");

  for (int shot = 0; shot < NUM_SHOTS; shot++) {
    disableBoost();
    delay(50);

    setTxTransmit('N');
    delayMicroseconds(10);

    BurstResult burst = sendBurst40kHz(4);

    clearAllDirections();
    setDamping('N', 50);

    uint32_t count = captureAllEdges(burst.firstEdgeUs, CAPTURE_WINDOW_US);

    Serial.print("SHOT ");
    Serial.print(shot + 1);
    Serial.print(": burst_dur=");
    Serial.print(burst.lastEdgeUs - burst.firstEdgeUs);
    Serial.print("us  ");
    printEdges(burst.firstEdgeUs);

    clearAllDirections();
    delay(200);
  }

  // ------------------------------------------------------------------
  // SUMMARY
  // ------------------------------------------------------------------
  section("DIAGNOSTIC COMPLETE");
  Serial.println("Interpretation:");
  Serial.println("  If edges appear at ~294us (10cm TOF), acoustic path works");
  Serial.println("  If edges only at >1000us, signal is ringdown/reverberation");
  Serial.println("  If Part C (boost OFF) shows 0 edges, TX needs 22V to produce signal");
  Serial.println("  Compare Part A (4-cycle) vs Part B (12-cycle) for signal strength");
  Serial.println();
  Serial.println("Done. Reset to re-run.");

  digitalWrite(PIN_PWR_HOLD, HIGH);
  while (true) {
    delay(1000);
  }
}

void loop() {
  // nothing
}