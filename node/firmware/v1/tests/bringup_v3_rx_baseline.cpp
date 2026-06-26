/*
 * Node V3 RX Baseline — Phase 7
 *
 * RX chain validation with TX disabled.
 * Noise floor, AND-gate blanking, mux direction check.
 *
 * NO boost, NO TX at any point.
 * Do NOT drive any DRV/REL pins.
 *
 * Shared headers used:
 *   v3_ultrasonic_pins.h      — pin definitions
 *   v3_ultrasonic_safe.h      — safe state, boost, RX control
 *   v3_ultrasonic_burst.h     — burst generation (not used here, included for completeness)
 *   v3_ultrasonic_direction.h — direction control (mux only here)
 *   v3_ultrasonic_capture.h   — edge capture
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
static uint32_t g_noiseWindowMs  = 2500;   // default noise window in ms
static int      g_noiseRepeats   = 12;     // default repeat count

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
// Print pin states
// ---------------------------------------------------------------------------
static void printPinStates() {
  Serial.println("---- Pin States ----");
  Serial.print("  RX_EN_N=");    Serial.println(digitalRead(PIN_RX_EN_N));
  Serial.print("  MUX_A=");      Serial.println(digitalRead(PIN_MUX_A));
  Serial.print("  MUX_B=");      Serial.println(digitalRead(PIN_MUX_B));
  Serial.print("  DRV_N=");      Serial.println(digitalRead(PIN_DRV_N));
  Serial.print("  DRV_E=");      Serial.println(digitalRead(PIN_DRV_E));
  Serial.print("  DRV_S=");      Serial.println(digitalRead(PIN_DRV_S));
  Serial.print("  DRV_W=");      Serial.println(digitalRead(PIN_DRV_W));
  Serial.print("  REL_N=");      Serial.println(digitalRead(PIN_REL_N));
  Serial.print("  REL_E=");      Serial.println(digitalRead(PIN_REL_E));
  Serial.print("  REL_S=");      Serial.println(digitalRead(PIN_REL_S));
  Serial.print("  REL_W=");      Serial.println(digitalRead(PIN_REL_W));
  Serial.print("  TX_22V_EN_N="); Serial.println(digitalRead(PIN_TX_22V_EN_N));
  Serial.print("  TX_BURST_PWM="); Serial.println(digitalRead(PIN_TX_BURST_PWM));
  Serial.print("  PWR_HOLD=");    Serial.println(digitalRead(PIN_PWR_HOLD));
  Serial.println("----");
}

// ===========================================================================
// TEST 0 — RX-disabled edge count (expect 0, AND gate blocks)
// ===========================================================================
static void testRxDisabledEdgeCount() {
  Serial.println("==== RX-DISABLED EDGE COUNT START ====");
  Serial.println("V3: AND gate blocks TOF_EDGE when RX_EN_N=HIGH");
  Serial.println("Expect 0 edges in 1 second");

  // Ensure RX disabled, no boost, no TX
  disableRxPath();
  disableBoost();

  // Arm capture and count edges for 1 second
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();

  const uint32_t t0 = micros();
  while ((micros() - t0) < 1000000UL) {
    // busy-wait 1 second
  }
  v3_capture::disarmCapture();

  const uint32_t edgeCount = v3_capture::g_edgeCount;

  Serial.print("RX_DISABLED EDGES=");
  Serial.print(edgeCount);
  Serial.print(" / 1s -- ");
  if (edgeCount == 0) {
    Serial.println("PASS (AND gate blocks all edges)");
  } else {
    Serial.println("FAIL (edges leaked through AND gate)");
  }
  Serial.println("---- RX-DISABLED EDGE COUNT DONE ----");
}

// ===========================================================================
// TEST 1 — RX-enabled noise baseline per mux direction (N, S, E, W)
// ===========================================================================
static void testRxEnabledNoiseBaseline() {
  Serial.println("==== RX-ENABLED NOISE BASELINE START ====");
  Serial.println("Measuring noise floor per mux direction (2.5s each)");

  const char dirs[] = {'N', 'S', 'E', 'W'};
  const uint32_t windowUs = 2500000UL;  // 2.5 seconds

  for (int d = 0; d < 4; d++) {
    const char dir = dirs[d];

    // Set mux direction
    setRxDirection(dir);

    // Enable RX path
    enableRxPath();
    delayMicroseconds(POST_ENABLE_GUARD_US);

    // Count edges
    v3_capture::resetEdgeCapture();
    v3_capture::armCapture();

    const uint32_t t0 = micros();
    while ((micros() - t0) < windowUs) {
      // busy-wait
    }
    v3_capture::disarmCapture();

    const uint32_t edgeCount = v3_capture::g_edgeCount;
    const float edgesPerSec = static_cast<float>(edgeCount) / 2.5f;

    Serial.print("MUX_DIR=");
    Serial.print(dir);
    Serial.print(" EDGES=");
    Serial.print(edgeCount);
    Serial.print(" EDGES/SEC=");
    Serial.println(edgesPerSec, 1);

    // Disable RX between directions
    disableRxPath();
    delay(100);
  }

  Serial.println("---- RX-ENABLED NOISE BASELINE DONE ----");
}

// ===========================================================================
// TEST V — VREF settle timing
// ===========================================================================
static void testVrefSettle() {
  Serial.println("==== VREF SETTLE TIMING START ====");
  Serial.println("Enable RX, count edges in 10 consecutive 100ms windows");

  // Set mux to N (arbitrary)
  setRxDirection('N');
  enableRxPath();
  delayMicroseconds(POST_ENABLE_GUARD_US);

  for (int win = 0; win < 10; win++) {
    v3_capture::resetEdgeCapture();
    v3_capture::armCapture();

    const uint32_t t0 = micros();
    while ((micros() - t0) < 100000UL) {  // 100 ms window
      // busy-wait
    }
    v3_capture::disarmCapture();

    const uint32_t edgeCount = v3_capture::g_edgeCount;
    const float edgesPerSec = static_cast<float>(edgeCount) * 10.0f;  // scale 100ms -> 1s

    Serial.print("WINDOW=");
    Serial.print(win + 1);
    Serial.print("/10 EDGES=");
    Serial.print(edgeCount);
    Serial.print(" EDGES/SEC=");
    Serial.println(edgesPerSec, 1);
  }

  disableRxPath();
  Serial.println("---- VREF SETTLE TIMING DONE ----");
}

// ===========================================================================
// TEST N — Custom noise baseline (user-specified window/repeats)
// ===========================================================================
static void testCustomNoiseBaseline() {
  Serial.print("Enter window_ms (1-10000): ");
  while (Serial.available() <= 0) { delay(20); }
  String winStr = Serial.readStringUntil('\n');
  winStr.trim();
  uint32_t windowMs = winStr.toInt();
  if (windowMs < 1) windowMs = 1;
  if (windowMs > 10000) windowMs = 10000;

  Serial.print("Enter repeats (1-50): ");
  while (Serial.available() <= 0) { delay(20); }
  String repStr = Serial.readStringUntil('\n');
  repStr.trim();
  int repeats = repStr.toInt();
  if (repeats < 1) repeats = 1;
  if (repeats > 50) repeats = 50;

  Serial.print("WINDOW_MS=");
  Serial.print(windowMs);
  Serial.print(" REPEATS=");
  Serial.println(repeats);

  const uint32_t windowUs = windowMs * 1000UL;
  const char dirs[] = {'N', 'S', 'E', 'W'};

  for (int d = 0; d < 4; d++) {
    const char dir = dirs[d];
    Serial.print("---- MUX_DIR=");
    Serial.print(dir);
    Serial.println(" ----");

    int32_t counts[50];
    int32_t totalCount = 0;

    for (int i = 0; i < repeats; i++) {
      setRxDirection(dir);
      enableRxPath();
      delayMicroseconds(POST_ENABLE_GUARD_US);

      v3_capture::resetEdgeCapture();
      v3_capture::armCapture();

      const uint32_t t0 = micros();
      while ((micros() - t0) < windowUs) {
        // busy-wait
      }
      v3_capture::disarmCapture();

      const uint32_t edgeCount = v3_capture::g_edgeCount;
      counts[i] = static_cast<int32_t>(edgeCount);
      totalCount += counts[i];

      const float edgesPerSec = static_cast<float>(edgeCount) * 1000.0f / static_cast<float>(windowMs);

      Serial.print("  SAMPLE=");
      Serial.print(i + 1);
      Serial.print(" EDGES=");
      Serial.print(edgeCount);
      Serial.print(" EDGES/SEC=");
      Serial.println(edgesPerSec, 1);

      disableRxPath();
      delay(40);
    }

    sortInt32(counts, repeats);
    const int32_t medianCount = counts[repeats / 2];
    const float meanCount = static_cast<float>(totalCount) / static_cast<float>(repeats);
    const float meanPerSec = meanCount * 1000.0f / static_cast<float>(windowMs);

    Serial.print("DIR=");
    Serial.print(dir);
    Serial.print(" MEAN_EDGES=");
    Serial.print(meanCount, 2);
    Serial.print(" MEDIAN_EDGES=");
    Serial.print(medianCount);
    Serial.print(" MEAN_EDGES/SEC=");
    Serial.println(meanPerSec, 1);
  }

  Serial.println("---- CUSTOM NOISE BASELINE DONE ----");
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 RX Baseline (Phase 7) ===");
  Serial.print("  Noise window: ");
  Serial.print(g_noiseWindowMs);
  Serial.print("ms, repeats: ");
  Serial.println(g_noiseRepeats);
  Serial.println("  0 = RX-disabled edge count (expect 0)");
  Serial.println("  1 = RX-enabled noise baseline (all mux dirs)");
  Serial.println("  n = Noise baseline (custom window/repeats)");
  Serial.println("  v = VREF settle timing");
  Serial.println("  m = Set mux direction manually (N/S/E/W)");
  Serial.println("  r = Toggle RX_EN_N");
  Serial.println("  p = Print pin states");
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

  // Initialize safe state (all outputs configured, DRV/REL LOW, boost OFF, RX disabled)
  initSafeState();

  // Install ISR
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
    case '0':
      testRxDisabledEdgeCount();
      break;

    case '1':
      testRxEnabledNoiseBaseline();
      break;

    case 'n':
    case 'N':
      testCustomNoiseBaseline();
      break;

    case 'v':
    case 'V':
      testVrefSettle();
      break;

    case 'm':
    case 'M': {
      Serial.print("Enter mux direction (N/S/E/W): ");
      while (Serial.available() <= 0) { delay(20); }
      char dir = static_cast<char>(Serial.read());
      if (dir == 'n') dir = 'N';
      if (dir == 's') dir = 'S';
      if (dir == 'e') dir = 'E';
      if (dir == 'w') dir = 'W';
      if (dir == 'N' || dir == 'S' || dir == 'E' || dir == 'W') {
        setRxDirection(dir);
        Serial.print("MUX direction set to ");
        Serial.print(dir);
        Serial.print(" (MUX_A=");
        Serial.print(digitalRead(PIN_MUX_A));
        Serial.print(" MUX_B=");
        Serial.println(digitalRead(PIN_MUX_B));
      } else {
        Serial.println("Invalid direction. Use N/S/E/W.");
      }
      break;
    }

    case 'r':
    case 'R': {
      // Toggle RX_EN_N
      const uint8_t currentState = digitalRead(PIN_RX_EN_N);
      if (currentState == HIGH) {
        enableRxPath();
        Serial.println("RX_EN_N: LOW (mux enabled)");
      } else {
        disableRxPath();
        Serial.println("RX_EN_N: HIGH (mux disabled, safe default)");
      }
      break;
    }

    case 'p':
    case 'P':
      printPinStates();
      break;

    case '?':
      printMenu();
      break;

    default:
      break;
  }
}