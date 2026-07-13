/*
 * Node V3 Coupling Test — Phase 8
 *
 * Electrical coupling test. TX with RX transducer disconnected/isolated.
 * Full V3 measurement sequence (boost → TX → damping → RX open → capture).
 * TOF referenced to TX first edge.
 * Score N shots with detection rate, median TOF, jitter, edge count.
 * Compare: RX connected vs RX disconnected (user prompted).
 *
 * Enforces V3 truth table — no DRV+REL=HIGH allowed.
 *
 * Shared headers used:
 *   v3_ultrasonic_pins.h      — pin definitions
 *   v3_ultrasonic_safe.h      — safe state, boost, RX control
 *   v3_ultrasonic_burst.h     — burst generation with BurstResult
 *   v3_ultrasonic_direction.h — direction control with setTxTransmit(), setDamping()
 *   v3_ultrasonic_capture.h   — edge capture with CaptureResult
 */

#include <Arduino.h>

#ifdef DISABLE_BROWNOUT
#include "soc/rtc_cntl_reg.h"
#endif

#include <esp_sleep.h>

#include "v3_ultrasonic_pins.h"
#include "v3_ultrasonic_safe.h"
#include "v3_ultrasonic_burst.h"
#include "v3_ultrasonic_direction.h"
#include "v3_ultrasonic_capture.h"

RTC_DATA_ATTR static bool g_warmFlag = false;

// ---------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------
static int      g_burstCycles    = BURST_CYCLES;
static uint32_t g_dampingUs      = 50;       // µs damping after TX
static uint32_t g_blankingUs     = BLANKING_US;
static uint32_t g_guardUs        = POST_ENABLE_GUARD_US;
static uint32_t g_prechargeMs    = BOOST_PRECHARGE_MS;

#ifndef INTER_SHOT_MS
#define INTER_SHOT_MS 120
#endif

#ifndef WARMUP_SHOTS
#define WARMUP_SHOTS 10
#endif

#ifndef SCORE_SHOTS
#define SCORE_SHOTS 24
#endif

// ---------------------------------------------------------------------------
// Sort helper
// ---------------------------------------------------------------------------
static void sortInt32(int32_t* data, int count) {
  for (int i = 0; i < count - 1; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (data[j] < data[i]) {
        const int32_t tmp = data[i];
        data[i] = data[j];
        data[j] = tmp;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// V3 measurement state machine — single shot
// Returns CaptureResult with TOF relative to TX first edge.
// ---------------------------------------------------------------------------
static CaptureResult fireV3Shot(char txDir, char rxDir) {
  // 1. SELECT_PATH
  setRxDirection(rxDir);
  disableRxPath();
  setTxTransmit(txDir);

  // 2. BOOST_PRECHARGE
  enableBoost();
  delay(g_prechargeMs);

  // 3. TRANSMIT
  BurstResult burst = sendBurst40kHz(g_burstCycles);

  // 4. DAMP — clear TX first (setDamping checks DRV is LOW)
  clearAllDirections();  // DRV_x LOW before damping
  if (!setDamping(txDir, g_dampingUs)) {
    Serial.println("WARNING: setDamping() failed (DRV still HIGH), damping skipped");
  }

  // 5. RX_PREPARE — wait blanking from TX first edge, then enable RX
  const uint32_t txFirstEdge = burst.firstEdgeUs;
  const uint32_t burstDurationUs = burst.lastEdgeUs - burst.firstEdgeUs;
  const uint32_t totalBlankFromEdge = burstDurationUs + g_blankingUs;

  // Wait until blanking has elapsed from TX first edge
  while ((micros() - txFirstEdge) < totalBlankFromEdge) {
    // busy-wait
  }

  enableRxPath();
  delayMicroseconds(50);   // mux settle
  delayMicroseconds(g_guardUs);

  // 6. CAPTURE
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();

  CaptureResult result = v3_capture::getCaptureResult(txFirstEdge, TOF_TIMEOUT_US);

  v3_capture::disarmCapture();

  // If capture timed out with no detection, ensure safe state before cleanup
  if (!result.detected) {
    failSafeCleanup();
  }

  // 7. CLEANUP
  clearAllDirections();
  disableRxPath();
  disableBoost();
  failSafeCleanup();  // idempotent safety net

  return result;
}

// ---------------------------------------------------------------------------
// Print single-shot result
// ---------------------------------------------------------------------------
static void printShotResult(int shotNum, const CaptureResult& cr) {
  Serial.print("SHOT=");
  Serial.print(shotNum);
  Serial.print(" DET=");
  Serial.print(cr.detected ? 1 : 0);
  Serial.print(" TOF_US=");
  Serial.print(cr.tofUs);
  Serial.print(" EDGES=");
  Serial.print(cr.edgeCount);
  Serial.print(" E0=");
  Serial.print(cr.firstEdgeUs);
  Serial.print(" E1=");
  Serial.print(cr.secondEdgeUs);
  Serial.print(" E2=");
  Serial.print(cr.thirdEdgeUs);
  Serial.print(" ELAST=");
  Serial.println(cr.lastEdgeUs);
}

// ===========================================================================
// Single shot (menu 'f')
// ===========================================================================
static void doSingleShot() {
  Serial.println("Firing single shot: TX=N, RX=S");
  CaptureResult cr = fireV3Shot('N', 'S');
  printShotResult(1, cr);

  // Verify truth table safety
  if (!isTxStateSafe()) {
    Serial.println("WARNING: FORBIDDEN state detected after shot!");
  }
}

// ===========================================================================
// Score 24 shots (menu 's')
// ===========================================================================
struct ScoreSummary {
  int detectedCount;
  int totalShots;
  int32_t medianTof;
  int32_t minTof;
  int32_t maxTof;
  int32_t jitterIqr;     // IQR-based jitter
  int32_t medianEdges;
  float meanEdges;
};

static ScoreSummary scoreShots(char txDir, char rxDir, int warmup, int scored) {
  ScoreSummary summary;
  summary.detectedCount = 0;
  summary.totalShots = scored;
  summary.medianTof = -1;
  summary.minTof = -1;
  summary.maxTof = -1;
  summary.jitterIqr = -1;
  summary.medianEdges = -1;
  summary.meanEdges = 0.0f;

  // Warmup
  for (int i = 0; i < warmup; i++) {
    fireV3Shot(txDir, rxDir);
    delay(INTER_SHOT_MS);
  }

  int32_t tofs[64];
  int32_t edgeCounts[64];
  int32_t totalEdges = 0;

  for (int i = 0; i < scored; i++) {
    CaptureResult cr = fireV3Shot(txDir, rxDir);
    printShotResult(i + 1, cr);

    edgeCounts[i] = static_cast<int32_t>(cr.edgeCount);
    totalEdges += edgeCounts[i];

    if (cr.detected && summary.detectedCount < 64) {
      tofs[summary.detectedCount] = cr.tofUs;
      summary.detectedCount++;
    }

    delay(INTER_SHOT_MS);
  }

  // Compute statistics
  if (summary.detectedCount > 0) {
    sortInt32(tofs, summary.detectedCount);
    summary.minTof = tofs[0];
    summary.maxTof = tofs[summary.detectedCount - 1];
    summary.medianTof = tofs[summary.detectedCount / 2];

    // IQR-based jitter
    if (summary.detectedCount >= 4) {
      const int q1Idx = summary.detectedCount / 4;
      const int q3Idx = (3 * summary.detectedCount) / 4;
      summary.jitterIqr = tofs[q3Idx] - tofs[q1Idx];
    } else {
      summary.jitterIqr = summary.maxTof - summary.minTof;
    }
  }

  sortInt32(edgeCounts, scored);
  summary.medianEdges = edgeCounts[scored / 2];
  summary.meanEdges = static_cast<float>(totalEdges) / static_cast<float>(scored);

  return summary;
}

static void printScoreSummary(const ScoreSummary& s, char txDir, char rxDir) {
  const float detPct = (100.0f * static_cast<float>(s.detectedCount)) / static_cast<float>(s.totalShots);

  Serial.print("SCORE TX=");
  Serial.print(txDir);
  Serial.print(" RX=");
  Serial.print(rxDir);
  Serial.print(" DET=");
  Serial.print(s.detectedCount);
  Serial.print("/");
  Serial.print(s.totalShots);
  Serial.print(" DET_PCT=");
  Serial.print(detPct, 1);
  Serial.print(" MED_TOF_US=");
  Serial.print(s.medianTof);
  Serial.print(" JITTER_IQR_US=");
  Serial.print(s.jitterIqr);
  Serial.print(" MIN_US=");
  Serial.print(s.minTof);
  Serial.print(" MAX_US=");
  Serial.print(s.maxTof);
  Serial.print(" MEAN_EDGES=");
  Serial.print(s.meanEdges, 2);
  Serial.print(" MEDIAN_EDGES=");
  Serial.println(s.medianEdges);
}

static void doScoreShots() {
  Serial.println("Scoring 24 shots: TX=N, RX=S (10 warmup first)");
  ScoreSummary s = scoreShots('N', 'S', WARMUP_SHOTS, SCORE_SHOTS);
  Serial.println("--- Summary ---");
  printScoreSummary(s, 'N', 'S');
}

// ===========================================================================
// Coupling round (menu 'c') — compare RX connected vs disconnected
// ===========================================================================
static void doCouplingRound() {
  // Phase A: RX connected (baseline)
  Serial.println("==== COUPLING ROUND: RX CONNECTED ====");
  Serial.println("Ensure RX transducer is CONNECTED. Press any key to start...");
  while (Serial.available() <= 0) { delay(20); }
  (void)Serial.read();

  ScoreSummary connected = scoreShots('N', 'S', WARMUP_SHOTS, SCORE_SHOTS);
  Serial.println("--- Connected Summary ---");
  printScoreSummary(connected, 'N', 'S');

  // Phase B: RX disconnected
  Serial.println();
  Serial.println("==== COUPLING ROUND: RX DISCONNECTED ====");
  Serial.println("DISCONNECT the RX transducer cable now.");
  Serial.println("Press any key to start...");
  while (Serial.available() <= 0) { delay(20); }
  (void)Serial.read();

  ScoreSummary disconnected = scoreShots('N', 'S', WARMUP_SHOTS, SCORE_SHOTS);
  Serial.println("--- Disconnected Summary ---");
  printScoreSummary(disconnected, 'N', 'S');

  // Comparison
  Serial.println();
  Serial.println("==== COUPLING COMPARISON ====");
  Serial.print("  Connected:    DET=");
  Serial.print(connected.detectedCount);
  Serial.print("/");
  Serial.print(connected.totalShots);
  Serial.print(" MED_TOF=");
  Serial.print(connected.medianTof);
  Serial.print("us JITTER=");
  Serial.print(connected.jitterIqr);
  Serial.println("us");

  Serial.print("  Disconnected: DET=");
  Serial.print(disconnected.detectedCount);
  Serial.print("/");
  Serial.print(disconnected.totalShots);
  Serial.print(" MED_TOF=");
  Serial.print(disconnected.medianTof);
  Serial.print("us JITTER=");
  Serial.print(disconnected.jitterIqr);
  Serial.println("us");

  const int detDelta = connected.detectedCount - disconnected.detectedCount;
  Serial.print("  Detection delta: ");
  Serial.print(detDelta);
  Serial.println(" (connected - disconnected)");

  if (disconnected.detectedCount > 0 && connected.detectedCount > 0) {
    const int32_t tofDelta = disconnected.medianTof - connected.medianTof;
    Serial.print("  Median TOF delta: ");
    Serial.print(tofDelta);
    Serial.println("us (disconnected - connected)");
  }

  if (disconnected.detectedCount == 0) {
    Serial.println("  RESULT: PASS — no detection with RX disconnected (no electrical coupling)");
  } else {
    Serial.println("  RESULT: FAIL — detection with RX disconnected suggests electrical coupling");
  }
}

// ===========================================================================
// Damping sweep (menu 'd')
// ===========================================================================
static void doDampingSweep() {
  Serial.println("==== DAMPING SWEEP START ====");
  Serial.println("5 shots at each damping interval: TX=N, RX=S");

  const uint32_t dampingValues[] = {0, 10, 20, 50};
  const int numValues = sizeof(dampingValues) / sizeof(dampingValues[0]);
  const uint32_t savedDamping = g_dampingUs;

  for (int vi = 0; vi < numValues; vi++) {
    g_dampingUs = dampingValues[vi];

    int detectedCount = 0;
    int32_t tofs[5];
    int32_t totalEdges = 0;

    for (int shot = 0; shot < 5; shot++) {
      CaptureResult cr = fireV3Shot('N', 'S');
      totalEdges += static_cast<int32_t>(cr.edgeCount);
      if (cr.detected) {
        tofs[detectedCount++] = cr.tofUs;
      }
      delay(INTER_SHOT_MS);
    }

    int32_t medianTof = -1;
    if (detectedCount > 0) {
      sortInt32(tofs, detectedCount);
      medianTof = tofs[detectedCount / 2];
    }

    const float detPct = (100.0f * static_cast<float>(detectedCount)) / 5.0f;
    const float meanEdges = static_cast<float>(totalEdges) / 5.0f;

    Serial.print("DAMP_US=");
    Serial.print(g_dampingUs);
    Serial.print(" DET=");
    Serial.print(detectedCount);
    Serial.print("/5 DET_PCT=");
    Serial.print(detPct, 1);
    Serial.print(" MED_TOF=");
    Serial.print(medianTof);
    Serial.print("us MEAN_EDGES=");
    Serial.println(meanEdges, 2);
  }

  g_dampingUs = savedDamping;
  Serial.println("---- DAMPING SWEEP DONE ----");
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Coupling Test (Phase 8) ===");
  Serial.print("  Burst: ");
  Serial.print(g_burstCycles);
  Serial.print(" cyc, Damping: ");
  Serial.print(g_dampingUs);
  Serial.print("us, Blanking: ");
  Serial.print(g_blankingUs);
  Serial.print("us, Guard: ");
  Serial.print(g_guardUs);
  Serial.print("us, Precharge: ");
  Serial.print(g_prechargeMs);
  Serial.println("ms");
  Serial.println("  f = Fire single shot (TX=N, RX=S)");
  Serial.println("  s = Score 24 shots (TX=N, RX=S)");
  Serial.println("  c = Coupling round (RX disconnected prompt)");
  Serial.println("  d = Damping sweep during coupling test");
  Serial.println("  b = Change burst cycles");
  Serial.println("  t = Change blanking/guard timing");
  Serial.println("  e = Enable boost");
  Serial.println("  ? = Menu");
}

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(800);

  initSafeState();

  v3_capture::installTofEdgeIsr();

  if (g_warmFlag) {
    Serial.println("WARM: RTC flag set from previous boot");
  }
  g_warmFlag = true;

  printMenu();
}

// ===========================================================================
// loop()
// ===========================================================================
void loop() {
  if (Serial.available() <= 0) {
    delay(20);
    return;
  }

  const char cmd = static_cast<char>(Serial.read());

  switch (cmd) {
    case 'f':
    case 'F':
      doSingleShot();
      break;

    case 's':
    case 'S':
      doScoreShots();
      break;

    case 'c':
    case 'C':
      doCouplingRound();
      break;

    case 'd':
    case 'D':
      doDampingSweep();
      break;

    case 'b':
    case 'B': {
      Serial.print("Current burst cycles: ");
      Serial.println(g_burstCycles);
      Serial.print("Enter new burst cycles (1-64): ");
      while (Serial.available() <= 0) { delay(20); }
      String val = Serial.readStringUntil('\n');
      val.trim();
      int cycles = val.toInt();
      if (cycles >= 1 && cycles <= 64) {
        g_burstCycles = cycles;
        Serial.print("Burst cycles set to ");
        Serial.println(g_burstCycles);
      } else {
        Serial.println("Invalid value. Must be 1-64.");
      }
      break;
    }

    case 't':
    case 'T': {
      Serial.print("Current blanking: ");
      Serial.print(g_blankingUs);
      Serial.print("us, guard: ");
      Serial.print(g_guardUs);
      Serial.println("us");
      Serial.print("Enter blanking_us (0-5000): ");
      while (Serial.available() <= 0) { delay(20); }
      String blankStr = Serial.readStringUntil('\n');
      blankStr.trim();
      uint32_t blankVal = blankStr.toInt();
      if (blankVal <= 5000) {
        g_blankingUs = blankVal;
      }

      Serial.print("Enter guard_us (0-1000): ");
      while (Serial.available() <= 0) { delay(20); }
      String guardStr = Serial.readStringUntil('\n');
      guardStr.trim();
      uint32_t guardVal = guardStr.toInt();
      if (guardVal <= 1000) {
        g_guardUs = guardVal;
      }

      Serial.print("Blanking=");
      Serial.print(g_blankingUs);
      Serial.print("us Guard=");
      Serial.print(g_guardUs);
      Serial.println("us");
      break;
    }

    case 'e':
    case 'E': {
      Serial.println("Enabling boost (hard enable, may brownout)...");
      Serial.flush();
      delay(10);
      enableBoost();
      delay(g_prechargeMs);
      Serial.println("Boost ON. Press 'x' to disable.");
      break;
    }

    case 'x':
    case 'X': {
      disableBoost();
      Serial.println("Boost OFF (safe default)");
      break;
    }

    case '?':
      printMenu();
      break;

    default:
      break;
  }
}