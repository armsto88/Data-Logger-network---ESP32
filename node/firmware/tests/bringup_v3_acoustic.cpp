/*
 * Node V3 Acoustic Test — Phases 9 + 10
 *
 * Open vs blocked acoustic test, reciprocal direction test.
 * Full V3 measurement sequence with corrected timing.
 * All timing referenced to TX first edge.
 * Acoustic gate: configurable start/end relative to TX first edge.
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
static uint32_t g_dampingUs      = 50;
static uint32_t g_blankingUs     = BLANKING_US;
static uint32_t g_guardUs        = POST_ENABLE_GUARD_US;
static uint32_t g_prechargeMs    = BOOST_PRECHARGE_MS;

// Acoustic gate: only edges within [GATE_START_US, GATE_END_US] from TX first edge
// are counted as valid detections.
// Override at build time: -DGATE_START_US=200 -DGATE_END_US=400 (10 cm path, TOF≈294µs)
#ifndef GATE_START_US
#define GATE_START_US 350
#endif
#ifndef GATE_END_US
#define GATE_END_US 550
#endif
static int32_t  g_gateStartUs    = GATE_START_US;
static int32_t  g_gateEndUs      = GATE_END_US;

#ifndef INTER_SHOT_MS
#define INTER_SHOT_MS 120
#endif

#ifndef WARMUP_SHOTS
#define WARMUP_SHOTS 10
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
// V3 measurement state machine — single shot with acoustic gate
// Returns CaptureResult with TOF relative to TX first edge.
// Only edges within the acoustic gate window are considered valid.
// ---------------------------------------------------------------------------
static CaptureResult fireV3ShotGated(char txDir, char rxDir) {
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
  clearAllDirections();
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

  // Poll for edges within the acoustic gate window
  CaptureResult result;
  result.detected = false;
  result.edgeCount = 0;
  result.tofUs = -1;
  result.firstEdgeUs = -1;
  result.secondEdgeUs = -1;
  result.thirdEdgeUs = -1;
  result.lastEdgeUs = -1;

  const uint32_t gateStartAbs = txFirstEdge + static_cast<uint32_t>(g_gateStartUs);
  const uint32_t gateEndAbs   = txFirstEdge + static_cast<uint32_t>(g_gateEndUs);
  const uint32_t pollDeadline = txFirstEdge + static_cast<uint32_t>(g_gateEndUs) + 500;  // extra margin

  // Wait until gate window opens
  while (micros() < gateStartAbs) {
    // busy-wait
  }

  // Poll through gate window
  uint32_t handledCount = 0;
  while (micros() < pollDeadline) {
    const uint32_t countNow = v3_capture::g_edgeCount;
    if (countNow > handledCount) {
      handledCount = countNow;
      const uint32_t edgeUs = v3_capture::g_lastEdgeUs;
      const int32_t tofRel = static_cast<int32_t>(edgeUs - txFirstEdge);

      // Check if edge is within acoustic gate
      if (tofRel >= g_gateStartUs && tofRel <= g_gateEndUs) {
        result.detected = true;
        result.tofUs = tofRel;
        break;
      }
    }
  }

  // Fill in all edge timestamps relative to TX first edge
  result.edgeCount = v3_capture::g_edgeCount;
  result.firstEdgeUs = v3_capture::edgeRelUs(v3_capture::g_firstEdgeUs, txFirstEdge);
  result.secondEdgeUs = v3_capture::edgeRelUs(v3_capture::g_secondEdgeUs, txFirstEdge);
  result.thirdEdgeUs = v3_capture::edgeRelUs(v3_capture::g_thirdEdgeUs, txFirstEdge);
  result.lastEdgeUs = v3_capture::edgeRelUs(v3_capture::g_lastEdgeUs, txFirstEdge);

  // If no gated detection but edges exist, report last TOF anyway
  if (!result.detected && v3_capture::g_edgeCount > 0) {
    result.tofUs = static_cast<int32_t>(v3_capture::g_lastEdgeUs - txFirstEdge);
  }

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

// ---------------------------------------------------------------------------
// Score summary struct
// ---------------------------------------------------------------------------
struct ScoreSummary {
  int detectedCount;
  int totalShots;
  int32_t medianTof;
  int32_t minTof;
  int32_t maxTof;
  int32_t jitterIqr;
  int32_t medianEdges;
  float meanEdges;
};

static ScoreSummary computeSummary(int32_t* tofs, int detCount, int32_t* edgeCounts, int totalShots) {
  ScoreSummary s;
  s.detectedCount = detCount;
  s.totalShots = totalShots;
  s.medianTof = -1;
  s.minTof = -1;
  s.maxTof = -1;
  s.jitterIqr = -1;
  s.medianEdges = -1;
  s.meanEdges = 0.0f;

  if (detCount > 0) {
    sortInt32(tofs, detCount);
    s.minTof = tofs[0];
    s.maxTof = tofs[detCount - 1];
    s.medianTof = tofs[detCount / 2];

    if (detCount >= 4) {
      const int q1Idx = detCount / 4;
      const int q3Idx = (3 * detCount) / 4;
      s.jitterIqr = tofs[q3Idx] - tofs[q1Idx];
    } else {
      s.jitterIqr = s.maxTof - s.minTof;
    }
  }

  if (totalShots > 0) {
    int32_t totalEdges = 0;
    for (int i = 0; i < totalShots; i++) {
      totalEdges += edgeCounts[i];
    }
    sortInt32(edgeCounts, totalShots);
    s.medianEdges = edgeCounts[totalShots / 2];
    s.meanEdges = static_cast<float>(totalEdges) / static_cast<float>(totalShots);
  }

  return s;
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

// ===========================================================================
// Open-path round (menu 'o') — 50 shots, N→S
// ===========================================================================
static ScoreSummary runOpenPathRound() {
  Serial.println("==== OPEN-PATH ROUND START (N->S, 50 shots) ====");

  const int warmup = WARMUP_SHOTS;
  const int scored = 50;

  // Warmup
  for (int i = 0; i < warmup; i++) {
    fireV3ShotGated('N', 'S');
    delay(INTER_SHOT_MS);
  }

  int32_t tofs[50];
  int32_t edgeCounts[50];
  int detCount = 0;

  for (int i = 0; i < scored; i++) {
    CaptureResult cr = fireV3ShotGated('N', 'S');
    printShotResult(i + 1, cr);

    edgeCounts[i] = static_cast<int32_t>(cr.edgeCount);
    if (cr.detected && detCount < 50) {
      tofs[detCount++] = cr.tofUs;
    }

    delay(INTER_SHOT_MS);
  }

  ScoreSummary s = computeSummary(tofs, detCount, edgeCounts, scored);
  Serial.println("--- Open-path Summary ---");
  printScoreSummary(s, 'N', 'S');
  Serial.println("---- OPEN-PATH ROUND DONE ----");

  return s;
}

// ===========================================================================
// Blocked-path round (menu 'b') — 50 shots, user blocks path
// ===========================================================================
static ScoreSummary runBlockedPathRound() {
  Serial.println("==== BLOCKED-PATH ROUND START ====");
  Serial.println("BLOCK the acoustic path between N and S transducers now.");
  Serial.println("Press any key to start...");
  while (Serial.available() <= 0) { delay(20); }
  (void)Serial.read();

  const int warmup = WARMUP_SHOTS;
  const int scored = 50;

  // Warmup
  for (int i = 0; i < warmup; i++) {
    fireV3ShotGated('N', 'S');
    delay(INTER_SHOT_MS);
  }

  int32_t tofs[50];
  int32_t edgeCounts[50];
  int detCount = 0;

  for (int i = 0; i < scored; i++) {
    CaptureResult cr = fireV3ShotGated('N', 'S');
    printShotResult(i + 1, cr);

    edgeCounts[i] = static_cast<int32_t>(cr.edgeCount);
    if (cr.detected && detCount < 50) {
      tofs[detCount++] = cr.tofUs;
    }

    delay(INTER_SHOT_MS);
  }

  ScoreSummary s = computeSummary(tofs, detCount, edgeCounts, scored);
  Serial.println("--- Blocked-path Summary ---");
  printScoreSummary(s, 'N', 'S');
  Serial.println("---- BLOCKED-PATH ROUND DONE ----");

  return s;
}

// ===========================================================================
// Paired reciprocal (menu 'p') — N→S and S→N, 24 shots each
// ===========================================================================
static void doPairedReciprocal() {
  Serial.println("==== PAIRED RECIPROCAL START (N<->S) ====");

  const int warmup = WARMUP_SHOTS;
  const int scored = 24;

  // N→S
  Serial.println("-- Firing N->S (24 shots) --");
  for (int i = 0; i < warmup; i++) {
    fireV3ShotGated('N', 'S');
    delay(INTER_SHOT_MS);
  }

  int32_t tofsNS[24];
  int32_t edgeCountsNS[24];
  int detNS = 0;

  for (int i = 0; i < scored; i++) {
    CaptureResult cr = fireV3ShotGated('N', 'S');
    printShotResult(i + 1, cr);
    edgeCountsNS[i] = static_cast<int32_t>(cr.edgeCount);
    if (cr.detected && detNS < 24) {
      tofsNS[detNS++] = cr.tofUs;
    }
    delay(INTER_SHOT_MS);
  }

  ScoreSummary sNS = computeSummary(tofsNS, detNS, edgeCountsNS, scored);
  Serial.println("--- N->S Summary ---");
  printScoreSummary(sNS, 'N', 'S');

  delay(500);

  // S→N
  Serial.println("-- Firing S->N (24 shots) --");
  for (int i = 0; i < warmup; i++) {
    fireV3ShotGated('S', 'N');
    delay(INTER_SHOT_MS);
  }

  int32_t tofsSN[24];
  int32_t edgeCountsSN[24];
  int detSN = 0;

  for (int i = 0; i < scored; i++) {
    CaptureResult cr = fireV3ShotGated('S', 'N');
    printShotResult(i + 1, cr);
    edgeCountsSN[i] = static_cast<int32_t>(cr.edgeCount);
    if (cr.detected && detSN < 24) {
      tofsSN[detSN++] = cr.tofUs;
    }
    delay(INTER_SHOT_MS);
  }

  ScoreSummary sSN = computeSummary(tofsSN, detSN, edgeCountsSN, scored);
  Serial.println("--- S->N Summary ---");
  printScoreSummary(sSN, 'S', 'N');

  // Delta calculation
  Serial.println();
  Serial.println("==== RECIPROCAL DELTA ====");
  if (sNS.medianTof > 0 && sSN.medianTof > 0) {
    const int32_t delta = sSN.medianTof - sNS.medianTof;  // t_SN - t_NS
    Serial.print("DELTA_US=");
    Serial.print(delta);
    Serial.print(" (t_SN - t_NS = ");
    Serial.print(sSN.medianTof);
    Serial.print(" - ");
    Serial.print(sNS.medianTof);
    Serial.println(")");

    if (delta > 0) {
      Serial.println("  Sign: POSITIVE (S->N slower than N->S)");
    } else if (delta < 0) {
      Serial.println("  Sign: NEGATIVE (N->S slower than S->N)");
    } else {
      Serial.println("  Sign: ZERO (symmetric)");
    }

    Serial.print("  |DELTA|=");
    Serial.print(delta < 0 ? -delta : delta);
    Serial.println("us");
  } else {
    Serial.println("DELTA: NA (insufficient detections in one or both directions)");
  }

  Serial.println("---- PAIRED RECIPROCAL DONE ----");
}

// ===========================================================================
// All-axis reciprocal (menu 'a') — N↔S and E↔W
// ===========================================================================
static void doAllAxisReciprocal() {
  Serial.println("==== ALL-AXIS RECIPROCAL START ====");

  const int warmup = WARMUP_SHOTS;
  const int scored = 24;
  const char axes[][2] = {{'N', 'S'}, {'E', 'W'}};

  for (int ax = 0; ax < 2; ax++) {
    const char a = axes[ax][0];
    const char b = axes[ax][1];

    Serial.print("-- Axis ");
    Serial.print(a);
    Serial.print("<->");
    Serial.println(b);
    Serial.println(" --");

    // A→B
    Serial.print("Firing ");
    Serial.print(a);
    Serial.print("->");
    Serial.println(b);
    for (int i = 0; i < warmup; i++) {
      fireV3ShotGated(a, b);
      delay(INTER_SHOT_MS);
    }

    int32_t tofsAB[24];
    int32_t edgeCountsAB[24];
    int detAB = 0;

    for (int i = 0; i < scored; i++) {
      CaptureResult cr = fireV3ShotGated(a, b);
      printShotResult(i + 1, cr);
      edgeCountsAB[i] = static_cast<int32_t>(cr.edgeCount);
      if (cr.detected && detAB < 24) {
        tofsAB[detAB++] = cr.tofUs;
      }
      delay(INTER_SHOT_MS);
    }

    ScoreSummary sAB = computeSummary(tofsAB, detAB, edgeCountsAB, scored);
    printScoreSummary(sAB, a, b);

    delay(500);

    // B→A
    Serial.print("Firing ");
    Serial.print(b);
    Serial.print("->");
    Serial.println(a);
    for (int i = 0; i < warmup; i++) {
      fireV3ShotGated(b, a);
      delay(INTER_SHOT_MS);
    }

    int32_t tofsBA[24];
    int32_t edgeCountsBA[24];
    int detBA = 0;

    for (int i = 0; i < scored; i++) {
      CaptureResult cr = fireV3ShotGated(b, a);
      printShotResult(i + 1, cr);
      edgeCountsBA[i] = static_cast<int32_t>(cr.edgeCount);
      if (cr.detected && detBA < 24) {
        tofsBA[detBA++] = cr.tofUs;
      }
      delay(INTER_SHOT_MS);
    }

    ScoreSummary sBA = computeSummary(tofsBA, detBA, edgeCountsBA, scored);
    printScoreSummary(sBA, b, a);

    // Delta
    if (sAB.medianTof > 0 && sBA.medianTof > 0) {
      const int32_t delta = sBA.medianTof - sAB.medianTof;
      Serial.print("DELTA ");
      Serial.print(b);
      Serial.print(a);
      Serial.print("_US - ");
      Serial.print(a);
      Serial.print(b);
      Serial.print("_US = ");
      Serial.println(delta);
    } else {
      Serial.println("DELTA: NA (insufficient detections)");
    }

    Serial.println();
  }

  Serial.println("---- ALL-AXIS RECIPROCAL DONE ----");
}

// ===========================================================================
// Manual single shot (menu 'm') — user picks TX and RX direction
// ===========================================================================
static void doManualShot() {
  Serial.print("Enter TX direction (N/S/E/W): ");
  while (Serial.available() <= 0) { delay(20); }
  char txDir = static_cast<char>(Serial.read());
  // Normalize
  if (txDir >= 'a' && txDir <= 'z') txDir -= ('a' - 'A');

  if (txDir != 'N' && txDir != 'S' && txDir != 'E' && txDir != 'W') {
    Serial.println("Invalid TX direction.");
    return;
  }
  Serial.println(txDir);

  Serial.print("Enter RX direction (N/S/E/W): ");
  while (Serial.available() <= 0) { delay(20); }
  char rxDir = static_cast<char>(Serial.read());
  if (rxDir >= 'a' && rxDir <= 'z') rxDir -= ('a' - 'A');

  if (rxDir != 'N' && rxDir != 'S' && rxDir != 'E' && rxDir != 'W') {
    Serial.println("Invalid RX direction.");
    return;
  }
  Serial.println(rxDir);

  Serial.print("Firing: TX=");
  Serial.print(txDir);
  Serial.print(" RX=");
  Serial.println(rxDir);

  CaptureResult cr = fireV3ShotGated(txDir, rxDir);
  printShotResult(1, cr);

  if (!isTxStateSafe()) {
    Serial.println("WARNING: FORBIDDEN state detected after shot!");
  }
}

// ===========================================================================
// Open vs blocked comparison (menu 'o' then 'b')
// ===========================================================================
static ScoreSummary g_lastOpenPath;   // stored from last 'o' run
static bool g_hasOpenPathResult = false;

static void doOpenPath() {
  g_lastOpenPath = runOpenPathRound();
  g_hasOpenPathResult = true;
}

static void doBlockedPath() {
  ScoreSummary blocked = runBlockedPathRound();

  if (g_hasOpenPathResult) {
    Serial.println();
    Serial.println("==== OPEN vs BLOCKED COMPARISON ====");

    const float openDetPct = (100.0f * static_cast<float>(g_lastOpenPath.detectedCount)) / static_cast<float>(g_lastOpenPath.totalShots);
    const float blockDetPct = (100.0f * static_cast<float>(blocked.detectedCount)) / static_cast<float>(blocked.totalShots);

    Serial.print("  Open:    DET_PCT=");
    Serial.print(openDetPct, 1);
    Serial.print(" MED_TOF=");
    Serial.print(g_lastOpenPath.medianTof);
    Serial.print("us JITTER=");
    Serial.print(g_lastOpenPath.jitterIqr);
    Serial.println("us");

    Serial.print("  Blocked: DET_PCT=");
    Serial.print(blockDetPct, 1);
    Serial.print(" MED_TOF=");
    Serial.print(blocked.medianTof);
    Serial.print("us JITTER=");
    Serial.print(blocked.jitterIqr);
    Serial.println("us");

    Serial.print("  Detection rate delta: ");
    Serial.print(openDetPct - blockDetPct, 1);
    Serial.println("% (open - blocked)");

    if (g_lastOpenPath.medianTof > 0 && blocked.medianTof > 0) {
      const int32_t tofDelta = blocked.medianTof - g_lastOpenPath.medianTof;
      Serial.print("  Median TOF separation: ");
      Serial.print(tofDelta);
      Serial.println("us (blocked - open)");
    }

    if (g_lastOpenPath.jitterIqr > 0 && blocked.jitterIqr > 0) {
      const int32_t jitterDelta = blocked.jitterIqr - g_lastOpenPath.jitterIqr;
      Serial.print("  Jitter delta: ");
      Serial.print(jitterDelta);
      Serial.println("us (blocked - open)");
    }
  } else {
    Serial.println("No open-path result stored. Run 'o' first for comparison.");
  }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Acoustic Test (Phase 9-10) ===");
  Serial.print("  Burst: ");
  Serial.print(g_burstCycles);
  Serial.print(" cyc, Damping: ");
  Serial.print(g_dampingUs);
  Serial.print("us, Gate: ");
  Serial.print(g_gateStartUs);
  Serial.print("-");
  Serial.print(g_gateEndUs);
  Serial.println("us from TX edge");
  Serial.print("  Blanking: ");
  Serial.print(g_blankingUs);
  Serial.print("us, Guard: ");
  Serial.print(g_guardUs);
  Serial.print("us, Precharge: ");
  Serial.print(g_prechargeMs);
  Serial.println("ms");
  Serial.println("  o = Open-path round (50 shots, N->S)");
  Serial.println("  b = Blocked-path round (50 shots, prompt to block)");
  Serial.println("  p = Paired reciprocal (N<->S, 24 shots each direction)");
  Serial.println("  a = All-axis reciprocal (N<->S, E<->W)");
  Serial.println("  g = Set acoustic gate (start_us end_us from TX edge)");
  Serial.println("  d = Set damping duration");
  Serial.println("  c = Set burst cycles");
  Serial.println("  t = Timing parameter menu (blanking, guard, precharge)");
  Serial.println("  m = Manual single shot (TX=dir RX=dir)");
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
    case 'o':
    case 'O':
      doOpenPath();
      break;

    case 'b':
    case 'B':
      doBlockedPath();
      break;

    case 'p':
    case 'P':
      doPairedReciprocal();
      break;

    case 'a':
    case 'A':
      doAllAxisReciprocal();
      break;

    case 'g':
    case 'G': {
      Serial.print("Current gate: ");
      Serial.print(g_gateStartUs);
      Serial.print("-");
      Serial.print(g_gateEndUs);
      Serial.println("us from TX edge");
      Serial.print("Enter gate start_us (0-5000): ");
      while (Serial.available() <= 0) { delay(20); }
      String startStr = Serial.readStringUntil('\n');
      startStr.trim();
      int32_t startVal = startStr.toInt();
      if (startVal >= 0 && startVal <= 5000) {
        g_gateStartUs = startVal;
      }

      Serial.print("Enter gate end_us (0-10000): ");
      while (Serial.available() <= 0) { delay(20); }
      String endStr = Serial.readStringUntil('\n');
      endStr.trim();
      int32_t endVal = endStr.toInt();
      if (endVal >= 0 && endVal <= 10000) {
        g_gateEndUs = endVal;
      }

      if (g_gateEndUs <= g_gateStartUs) {
        Serial.println("ERROR: gate end must be > gate start. Resetting to defaults.");
        g_gateStartUs = 350;
        g_gateEndUs = 550;
      }

      Serial.print("Gate: ");
      Serial.print(g_gateStartUs);
      Serial.print("-");
      Serial.print(g_gateEndUs);
      Serial.println("us from TX edge");
      break;
    }

    case 'd':
    case 'D': {
      Serial.print("Current damping: ");
      Serial.print(g_dampingUs);
      Serial.println("us");
      Serial.print("Enter damping_us (0-500): ");
      while (Serial.available() <= 0) { delay(20); }
      String val = Serial.readStringUntil('\n');
      val.trim();
      uint32_t dampVal = val.toInt();
      if (dampVal <= 500) {
        g_dampingUs = dampVal;
        Serial.print("Damping set to ");
        Serial.print(g_dampingUs);
        Serial.println("us");
      } else {
        Serial.println("Invalid value. Must be 0-500.");
      }
      break;
    }

    case 'c':
    case 'C': {
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
      Serial.print("us, precharge: ");
      Serial.print(g_prechargeMs);
      Serial.println("ms");
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

      Serial.print("Enter precharge_ms (0-500): ");
      while (Serial.available() <= 0) { delay(20); }
      String preStr = Serial.readStringUntil('\n');
      preStr.trim();
      uint32_t preVal = preStr.toInt();
      if (preVal <= 500) {
        g_prechargeMs = preVal;
      }

      Serial.print("Blanking=");
      Serial.print(g_blankingUs);
      Serial.print("us Guard=");
      Serial.print(g_guardUs);
      Serial.print("us Precharge=");
      Serial.print(g_prechargeMs);
      Serial.println("ms");
      break;
    }

    case 'm':
    case 'M':
      doManualShot();
      break;

    case '?':
      printMenu();
      break;

    default:
      break;
  }
}