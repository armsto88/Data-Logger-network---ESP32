/*
 * Node V3 RX vs Boost Diagnostic — auto-run (no menu)
 *
 * Tests whether the RX analog chain still produces edges when the 22V boost
 * is enabled.  The V3 noise baseline shows ~1000 edges/sec with RX enabled
 * and boost OFF.  If boost ON suppresses the comparator, that explains why
 * the acoustic test gets EDGES=0 (capture happens while boost is ON).
 *
 * Sequence (all auto-run on boot):
 *   1. RX disabled, boost OFF — expect 0 edges (AND gate blocks)
 *   2. RX enabled, boost OFF, mux=N — measure noise edges (baseline)
 *   3. RX enabled, boost OFF, mux=S — measure noise edges
 *   4. RX enabled, boost ON,  mux=N — measure noise edges (boost ON!)
 *   5. RX enabled, boost ON,  mux=S — measure noise edges (boost ON!)
 *   6. Fire one TX burst (N) with boost ON, RX enabled mux=S, capture edges
 *
 * Each noise window is 2 seconds.  Results print automatically.
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
// Helper: count edges for a fixed duration
// ---------------------------------------------------------------------------
static uint32_t countEdgesFor(uint32_t durationUs) {
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();
  const uint32_t t0 = micros();
  while ((micros() - t0) < durationUs) {
    // busy-wait
  }
  v3_capture::disarmCapture();
  return v3_capture::g_edgeCount;
}

// ---------------------------------------------------------------------------
// Helper: print a section header
// ---------------------------------------------------------------------------
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
  Serial.println("=== V3 RX vs Boost Diagnostic (auto-run) ===");
  Serial.println("Tests whether boost ON suppresses the RX chain.");
  Serial.println();

  const uint32_t WINDOW_US = 2000000UL;  // 2 seconds per test

  // ------------------------------------------------------------------
  // TEST 1: RX disabled, boost OFF — expect 0 edges
  // ------------------------------------------------------------------
  section("TEST 1: RX disabled, boost OFF (expect 0)");
  disableRxPath();
  disableBoost();
  delay(100);
  {
    uint32_t edges = countEdgesFor(WINDOW_US);
    Serial.print("EDGES=");
    Serial.print(edges);
    Serial.print(" / 2s  EDGES/SEC=");
    Serial.println(static_cast<float>(edges) / 2.0f, 1);
  }

  // ------------------------------------------------------------------
  // TEST 2: RX enabled, boost OFF, mux=N
  // ------------------------------------------------------------------
  section("TEST 2: RX enabled, boost OFF, mux=N");
  setRxDirection('N');
  enableRxPath();
  delay(100);
  {
    uint32_t edges = countEdgesFor(WINDOW_US);
    Serial.print("EDGES=");
    Serial.print(edges);
    Serial.print(" / 2s  EDGES/SEC=");
    Serial.println(static_cast<float>(edges) / 2.0f, 1);
  }

  // ------------------------------------------------------------------
  // TEST 3: RX enabled, boost OFF, mux=S
  // ------------------------------------------------------------------
  section("TEST 3: RX enabled, boost OFF, mux=S");
  setRxDirection('S');
  delay(100);
  {
    uint32_t edges = countEdgesFor(WINDOW_US);
    Serial.print("EDGES=");
    Serial.print(edges);
    Serial.print(" / 2s  EDGES/SEC=");
    Serial.println(static_cast<float>(edges) / 2.0f, 1);
  }

  // ------------------------------------------------------------------
  // TEST 4: RX enabled, boost ON, mux=N  *** KEY TEST ***
  // ------------------------------------------------------------------
  section("TEST 4: RX enabled, boost ON, mux=N *** KEY ***");
  setRxDirection('N');
  enableBoost();
  delay(100);  // let boost settle
  {
    uint32_t edges = countEdgesFor(WINDOW_US);
    Serial.print("EDGES=");
    Serial.print(edges);
    Serial.print(" / 2s  EDGES/SEC=");
    Serial.println(static_cast<float>(edges) / 2.0f, 1);
  }

  // ------------------------------------------------------------------
  // TEST 5: RX enabled, boost ON, mux=S  *** KEY TEST ***
  // ------------------------------------------------------------------
  section("TEST 5: RX enabled, boost ON, mux=S *** KEY ***");
  setRxDirection('S');
  delay(100);
  {
    uint32_t edges = countEdgesFor(WINDOW_US);
    Serial.print("EDGES=");
    Serial.print(edges);
    Serial.print(" / 2s  EDGES/SEC=");
    Serial.println(static_cast<float>(edges) / 2.0f, 1);
  }

  // ------------------------------------------------------------------
  // TEST 6: Fire one TX burst (N), RX enabled mux=S, boost ON, capture
  // ------------------------------------------------------------------
  section("TEST 6: TX burst N, RX=S, boost ON (full sequence, 1 shot)");
  setRxDirection('S');
  enableRxPath();
  delay(100);

  // Precharge boost
  enableBoost();
  delay(50);

  // TX=N
  setTxTransmit('N');
  delayMicroseconds(10);

  BurstResult burst = sendBurst40kHz(4);
  Serial.print("  Burst: firstEdge=");
  Serial.print(burst.firstEdgeUs);
  Serial.print(" lastEdge=");
  Serial.print(burst.lastEdgeUs);
  Serial.print(" dur=");
  Serial.print(burst.lastEdgeUs - burst.firstEdgeUs);
  Serial.println("us");

  // Damping
  clearAllDirections();
  setDamping('N', 50);

  // Capture for 2 ms (longer than the acoustic gate — just see if ANY edges)
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();
  const uint32_t t0 = micros();
  while ((micros() - t0) < 2000UL) {
    // busy-wait 2 ms
  }
  v3_capture::disarmCapture();

  Serial.print("  EDGES in 2ms after TX=");
  Serial.print(v3_capture::g_edgeCount);
  if (v3_capture::g_edgeCount > 0) {
    Serial.print(" firstEdgeRel=");
    Serial.print(static_cast<int32_t>(v3_capture::g_firstEdgeUs) - static_cast<int32_t>(burst.firstEdgeUs));
    Serial.print("us lastEdgeRel=");
    Serial.print(static_cast<int32_t>(v3_capture::g_lastEdgeUs) - static_cast<int32_t>(burst.firstEdgeUs));
    Serial.println("us");
  } else {
    Serial.println(" (none)");
  }

  // Cleanup
  clearAllDirections();
  disableRxPath();
  disableBoost();
  failSafeCleanup();

  // ------------------------------------------------------------------
  // SUMMARY
  // ------------------------------------------------------------------
  section("DIAGNOSTIC COMPLETE");
  Serial.println("Interpretation guide:");
  Serial.println("  Test 2/3 (boost OFF): ~1000 edges/sec = RX chain OK");
  Serial.println("  Test 4/5 (boost ON):  if <<100 edges/sec, boost suppresses RX");
  Serial.println("  Test 6: if EDGES=0, TX burst produces no detectable signal");
  Serial.println();
  Serial.println("Done. Reset to re-run.");

  // Hold power and stop
  digitalWrite(PIN_PWR_HOLD, HIGH);
  while (true) {
    delay(1000);
  }
}

void loop() {
  // nothing — all work in setup()
}