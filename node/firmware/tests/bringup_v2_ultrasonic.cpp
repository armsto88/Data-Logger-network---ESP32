/*
 * Node V2 Ultrasonic Anemometer Bring-up
 *
 * V2 hardware changes from V1:
 *   - RX enable polarity inverted: RX_EN_N active-LOW on GPIO4
 *   - Split TX control: TX_BURST_PWM (GPIO25) + TX_22V_EN_N (GPIO5, active-LOW)
 *   - AND gate on TOF_EDGE: TOF_EDGE = COMP_RAW AND RX_WINDOW_EN
 *     where RX_WINDOW_EN = NOT RX_EN_N
 *   - Boost precharge delay between enabling boost and sending burst
 *
 * Based on V1: firmware/nodes/bringup/bringup_ultrasonic_first_test.cpp
 */

#include <Arduino.h>

#ifdef DISABLE_BROWNOUT
#include "soc/rtc_cntl_reg.h"  // brownout disable
#endif

#include <esp_sleep.h>

// RTC memory persists across brownout/power-on resets
RTC_DATA_ATTR static bool g_boostWarmFlag = false;

// ---------------------------------------------------------------------------
// Pin definitions (all with #ifndef guards, V2 defaults)
// ---------------------------------------------------------------------------
#ifndef PIN_TOF_EDGE
#define PIN_TOF_EDGE 34
#endif
#ifndef PIN_RX_EN_N
#define PIN_RX_EN_N 4
#endif
#ifndef PIN_MUX_A
#define PIN_MUX_A 16
#endif
#ifndef PIN_MUX_B
#define PIN_MUX_B 17
#endif
#ifndef PIN_DRV_N
#define PIN_DRV_N 26
#endif
#ifndef PIN_DRV_E
#define PIN_DRV_E 27
#endif
#ifndef PIN_DRV_S
#define PIN_DRV_S 14
#endif
#ifndef PIN_DRV_W
#define PIN_DRV_W 13
#endif
#ifndef PIN_REL_N
#define PIN_REL_N 33
#endif
#ifndef PIN_REL_E
#define PIN_REL_E 32
#endif
#ifndef PIN_REL_S
#define PIN_REL_S 21
#endif
#ifndef PIN_REL_W
#define PIN_REL_W 22
#endif
#ifndef PIN_TX_BURST_PWM
#define PIN_TX_BURST_PWM 25
#endif
#ifndef PIN_TX_22V_EN_N
#define PIN_TX_22V_EN_N 5
#endif
#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 23
#endif

// ---------------------------------------------------------------------------
// Tuning constants (all with #ifndef guards)
// ---------------------------------------------------------------------------
#ifndef BLANKING_US
#define BLANKING_US 320
#endif
#ifndef TOF_TIMEOUT_US
#define TOF_TIMEOUT_US 3000
#endif
#ifndef MIN_VALID_TOF_US
#define MIN_VALID_TOF_US 220
#endif
#ifndef BURST_CYCLES
#define BURST_CYCLES 12
#endif
#ifndef INTER_SHOT_MS
#define INTER_SHOT_MS 120
#endif
#ifndef WARMUP_SHOTS
#define WARMUP_SHOTS 10
#endif
#ifndef SCORE_SHOTS
#define SCORE_SHOTS 24
#endif
#ifndef BOOST_PRECHARGE_MS
#define BOOST_PRECHARGE_MS 50
#endif
#ifndef POST_ENABLE_GUARD_US
#define POST_ENABLE_GUARD_US 20
#endif
#ifndef NOISE_WINDOW_US
#define NOISE_WINDOW_US 2500
#endif
#ifndef NOISE_REPEATS
#define NOISE_REPEATS 12
#endif
#ifndef COUPLING_SHOTS
#define COUPLING_SHOTS 24
#endif
#ifndef AGG_WINDOW_US
#define AGG_WINDOW_US 2500
#endif
#ifndef AGG_REPEATS
#define AGG_REPEATS 10
#endif
#ifndef SWEEP_SHOTS
#define SWEEP_SHOTS 10
#endif
#ifndef DEBUG_FIRST3_EDGES
#define DEBUG_FIRST3_EDGES 1
#endif
#ifndef VERBOSE_SHOTS
#define VERBOSE_SHOTS 0
#endif

// ---------------------------------------------------------------------------
// ISR shared state
// ---------------------------------------------------------------------------
namespace {

volatile uint32_t g_edgeCount = 0;
volatile uint32_t g_firstEdgeUs = 0;
volatile uint32_t g_lastEdgeUs = 0;
volatile uint32_t g_secondEdgeUs = 0;
volatile uint32_t g_thirdEdgeUs = 0;
volatile bool g_captureArmed = false;

uint32_t g_blankingUs = BLANKING_US;
uint32_t g_postEnableGuardUs = POST_ENABLE_GUARD_US;
uint32_t g_minValidTofUs = MIN_VALID_TOF_US;
uint32_t g_boostPrechargeMs = BOOST_PRECHARGE_MS;

bool g_manualBoostOn = false;
bool g_manualRxOn = false;

// ---------------------------------------------------------------------------
// ISR
// ---------------------------------------------------------------------------
void IRAM_ATTR onTofEdge() {
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
// Edge-capture helpers
// ---------------------------------------------------------------------------
void resetEdgeCapture() {
  g_edgeCount = 0;
  g_firstEdgeUs = 0;
  g_secondEdgeUs = 0;
  g_thirdEdgeUs = 0;
  g_lastEdgeUs = 0;
}

int32_t edgeRelUs(uint32_t edgeTs, uint32_t t0) {
  if (edgeTs == 0) {
    return -1;
  }
  return static_cast<int32_t>(edgeTs - t0);
}

// ---------------------------------------------------------------------------
// RX enable (V2 polarity — active-LOW)
// ---------------------------------------------------------------------------
static void enableRxPath() {
  digitalWrite(PIN_RX_EN_N, LOW);   // LOW = mux enabled (V2 active-low)
}

static void disableRxPath() {
  digitalWrite(PIN_RX_EN_N, HIGH);  // HIGH = mux disabled (safe default)
}

// ---------------------------------------------------------------------------
// Boost enable (V2 — active-LOW)
// ---------------------------------------------------------------------------
static void enableBoost() {
  digitalWrite(PIN_TX_22V_EN_N, LOW);  // LOW = boost ON (inverter -> EN_22 HIGH)
}

static void disableBoost() {
  digitalWrite(PIN_TX_22V_EN_N, HIGH);  // HIGH = boost OFF (safe default)
}

static void softStartBoost() {
  // Gradually charge the 22V output capacitor with microsecond enable pulses.
  // 100us ON is short enough that the 470uF VSYS cap sustains the ESP32
  // even when the MT3608 draws peak inrush at higher output voltages.
  // 100ms OFF gives the battery maximum recovery time between pulses.
  // Total soft-start time: ~5s before the final hard enable.
  const int PULSE_ON_US  = 100;
  const int PULSE_OFF_MS = 100;
  const int PULSES = 50;
  Serial.print("Soft-start: ");
  Serial.print(PULSES);
  Serial.print(" pulses, ");
  Serial.print(PULSE_ON_US);
  Serial.print("us ON / ");
  Serial.print(PULSE_OFF_MS);
  Serial.println("ms OFF");
  for (int i = 0; i < PULSES; i++) {
    digitalWrite(PIN_TX_22V_EN_N, LOW);   // boost ON
    delayMicroseconds(PULSE_ON_US);
    digitalWrite(PIN_TX_22V_EN_N, HIGH);  // boost OFF
    delay(PULSE_OFF_MS);
    if (i % 10 == 9) {
      Serial.print("  pulse ");
      Serial.print(i + 1);
      Serial.println(" done");
    }
  }
  // Final hard enable + precharge
  Serial.println("  final enable + precharge");
  digitalWrite(PIN_TX_22V_EN_N, LOW);
  delay(g_boostPrechargeMs);
  Serial.println("  boost ready");
}

// ---------------------------------------------------------------------------
// Burst generation (V2: TX_BURST_PWM only, no boost control here)
// ---------------------------------------------------------------------------
static void sendBurst40kHz(int cycles) {
  const uint32_t halfPeriodUs = 12;  // ~40 kHz
  for (int i = 0; i < cycles; i++) {
    digitalWrite(PIN_TX_BURST_PWM, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PIN_TX_BURST_PWM, LOW);
    delayMicroseconds(halfPeriodUs);
  }
}

// ---------------------------------------------------------------------------
// Direction switching (same logic as V1)
// ---------------------------------------------------------------------------
void setRxDirection(char dir) {
  switch (dir) {
    case 'N':
      digitalWrite(PIN_MUX_A, LOW);
      digitalWrite(PIN_MUX_B, LOW);
      break;
    case 'S':
      digitalWrite(PIN_MUX_A, HIGH);
      digitalWrite(PIN_MUX_B, LOW);
      break;
    case 'W':
      digitalWrite(PIN_MUX_A, LOW);
      digitalWrite(PIN_MUX_B, HIGH);
      break;
    case 'E':
      digitalWrite(PIN_MUX_A, HIGH);
      digitalWrite(PIN_MUX_B, HIGH);
      break;
    default:
      break;
  }
}

void clearTxSelects() {
  digitalWrite(PIN_REL_N, LOW);
  digitalWrite(PIN_REL_E, LOW);
  digitalWrite(PIN_REL_S, LOW);
  digitalWrite(PIN_REL_W, LOW);

  digitalWrite(PIN_DRV_N, LOW);
  digitalWrite(PIN_DRV_E, LOW);
  digitalWrite(PIN_DRV_S, LOW);
  digitalWrite(PIN_DRV_W, LOW);
}

void setTxCombo(char dir, bool relState, bool drvState) {
  clearTxSelects();

  switch (dir) {
    case 'N':
      digitalWrite(PIN_REL_N, relState ? HIGH : LOW);
      digitalWrite(PIN_DRV_N, drvState ? HIGH : LOW);
      break;
    case 'S':
      digitalWrite(PIN_REL_S, relState ? HIGH : LOW);
      digitalWrite(PIN_DRV_S, drvState ? HIGH : LOW);
      break;
    case 'E':
      digitalWrite(PIN_REL_E, relState ? HIGH : LOW);
      digitalWrite(PIN_DRV_E, drvState ? HIGH : LOW);
      break;
    case 'W':
      digitalWrite(PIN_REL_W, relState ? HIGH : LOW);
      digitalWrite(PIN_DRV_W, drvState ? HIGH : LOW);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Sort helper
// ---------------------------------------------------------------------------
void sortInt32(int32_t* data, int count) {
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
// Print single-shot result
// ---------------------------------------------------------------------------
void printResult(char txDir, char rxDir, bool relState, bool drvState,
                 bool detected, int32_t tofUs, int32_t edgeAfterArmUs,
                 int32_t edge0Us, int32_t edge1Us, int32_t edge2Us) {
  Serial.print("MODE=tx_rx_single TX=");
  Serial.print(txDir);
  Serial.print(" RX=");
  Serial.print(rxDir);
  Serial.print(" REL=");
  Serial.print(relState ? 1 : 0);
  Serial.print(" DRV=");
  Serial.print(drvState ? 1 : 0);
  Serial.print(" DETECT=");
  Serial.print(detected ? 1 : 0);
  Serial.print(" TOF_US=");
  Serial.print(tofUs);
  Serial.print(" EDGE_AFTER_ARM_US=");
  Serial.print(edgeAfterArmUs);
#if DEBUG_FIRST3_EDGES
  Serial.print(" E0_US=");
  Serial.print(edge0Us);
  Serial.print(" E1_US=");
  Serial.print(edge1Us);
  Serial.print(" E2_US=");
  Serial.print(edge2Us);
#endif
  Serial.print(" BLANK_US=");
  Serial.print(g_blankingUs);
  Serial.print(" TIMEOUT_US=");
  Serial.println(TOF_TIMEOUT_US);
}

// ---------------------------------------------------------------------------
// runSingleTest — V2 blanking sequence with boost and precharge
// ---------------------------------------------------------------------------
bool runSingleTest(char txDir, char rxDir, bool relState, bool drvState,
                   int32_t* outTofUs = nullptr, int32_t* outEdgeAfterArmUs = nullptr) {
  // 1. Set RX direction via mux
  setRxDirection(rxDir);

  // 2. Disable RX path (RX_EN_N = HIGH)
  disableRxPath();

  // 3. Set TX combo (one direction, REL+DRV)
  setTxCombo(txDir, relState, drvState);

  g_captureArmed = false;

  // 4. Delay 2 ms
  delay(2);

  // 5. Enable boost (TX_22V_EN_N = LOW)
  enableBoost();

  // 6. Delay BOOST_PRECHARGE_MS (V2 new step)
  delay(g_boostPrechargeMs);

  // 7. Reset edge capture
  resetEdgeCapture();

  // 8. Send burst on TX_BURST_PWM
  sendBurst40kHz(BURST_CYCLES);
  const uint32_t txDoneUs = micros();

  // 9. Delay BLANKING_US (firmware blanking)
  delayMicroseconds(g_blankingUs);

  // 10. Enable RX path (RX_EN_N = LOW) — also sets RX_WINDOW_EN=HIGH via inverter, unblocking AND gate
  enableRxPath();

  // 11. Delay 20 us
  delayMicroseconds(20);

  // 12. Reset edge capture again
  resetEdgeCapture();

  // 13. Optional POST_ENABLE_GUARD_US delay
  if (g_postEnableGuardUs > 0) {
    delayMicroseconds(g_postEnableGuardUs);
  }

  // 14. Arm capture
  g_captureArmed = true;
  const uint32_t listenStartUs = micros();

  // 15. Poll loop: wait for g_edgeCount change, check tofUs >= MIN_VALID_TOF_US, break on valid or timeout
  bool detected = false;
  int32_t tofUs = -1;
  int32_t edgeAfterArmUs = -1;
  uint32_t handledCount = 0;

  while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
    const uint32_t countNow = g_edgeCount;
    if (countNow > handledCount) {
      const uint32_t edgeUs = g_lastEdgeUs;
      handledCount = countNow;
      tofUs = static_cast<int32_t>(edgeUs - txDoneUs);
      edgeAfterArmUs = static_cast<int32_t>(edgeUs - listenStartUs);

      if (tofUs >= static_cast<int32_t>(g_minValidTofUs)) {
        detected = true;
        break;
      }
    }
  }

  // 16. Disarm, clear TX selects, disable RX, disable boost
  g_captureArmed = false;

#if VERBOSE_SHOTS
  printResult(txDir, rxDir, relState, drvState, detected, tofUs, edgeAfterArmUs,
              edgeRelUs(g_firstEdgeUs, listenStartUs),
              edgeRelUs(g_secondEdgeUs, listenStartUs),
              edgeRelUs(g_thirdEdgeUs, listenStartUs));
#endif

  if (outTofUs != nullptr) {
    *outTofUs = tofUs;
  }
  if (outEdgeAfterArmUs != nullptr) {
    *outEdgeAfterArmUs = edgeAfterArmUs;
  }

  clearTxSelects();
  disableRxPath();
  disableBoost();
  return detected;
}

// ---------------------------------------------------------------------------
// Listen-only helper (no TX, no boost)
// ---------------------------------------------------------------------------
void runListenOnly(uint32_t windowUs) {
  clearTxSelects();
  enableRxPath();
  g_captureArmed = false;
  resetEdgeCapture();
  g_captureArmed = true;

  const uint32_t t0 = micros();
  while ((micros() - t0) < windowUs) {
  }

  g_captureArmed = false;

  const uint32_t edgeCount = g_edgeCount;
  const int32_t firstEdgeUs = (edgeCount > 0) ? static_cast<int32_t>(g_firstEdgeUs - t0) : -1;

  Serial.print("MODE=listen_only WINDOW_US=");
  Serial.print(windowUs);
  Serial.print(" EDGE_COUNT=");
  Serial.print(edgeCount);
  Serial.print(" FIRST_EDGE_US=");
  Serial.println(firstEdgeUs);

  disableRxPath();
}

// ---------------------------------------------------------------------------
// scoreCombo — WARMUP_SHOTS discard, then SCORE_SHOTS scored
// ---------------------------------------------------------------------------
void scoreCombo(char txDir, char rxDir, bool relState, bool drvState) {
  for (int i = 0; i < WARMUP_SHOTS; ++i) {
    int32_t dummyTof = -1;
    runSingleTest(txDir, rxDir, relState, drvState, &dummyTof, nullptr);
    delay(INTER_SHOT_MS);
  }

  int detectedCount = 0;
  int32_t tofs[SCORE_SHOTS];
  int32_t edgeAfterArmVals[SCORE_SHOTS];

  for (int i = 0; i < SCORE_SHOTS; ++i) {
    int32_t tofUs = -1;
    int32_t edgeAfterArmUs = -1;
    const bool detected = runSingleTest(txDir, rxDir, relState, drvState, &tofUs, &edgeAfterArmUs);
    if (detected && detectedCount < SCORE_SHOTS) {
      tofs[detectedCount++] = tofUs;
      edgeAfterArmVals[detectedCount - 1] = edgeAfterArmUs;
    }
    delay(INTER_SHOT_MS);
  }

  int32_t medianTof = -1;
  int32_t medianEdgeAfterArmUs = -1;
  int32_t minTof = -1;
  int32_t maxTof = -1;
  int32_t jitter = -1;

  if (detectedCount > 0) {
    sortInt32(tofs, detectedCount);
    sortInt32(edgeAfterArmVals, detectedCount);
    minTof = tofs[0];
    maxTof = tofs[detectedCount - 1];
    medianTof = tofs[detectedCount / 2];
    medianEdgeAfterArmUs = edgeAfterArmVals[detectedCount / 2];
    jitter = maxTof - minTof;
  }

  const float detPct = (100.0f * static_cast<float>(detectedCount)) / static_cast<float>(SCORE_SHOTS);

  Serial.print("SCORE TX=");
  Serial.print(txDir);
  Serial.print(" RX=");
  Serial.print(rxDir);
  Serial.print(" REL=");
  Serial.print(relState ? 1 : 0);
  Serial.print(" DRV=");
  Serial.print(drvState ? 1 : 0);
  Serial.print(" DET=");
  Serial.print(detectedCount);
  Serial.print("/");
  Serial.print(SCORE_SHOTS);
  Serial.print(" DET_PCT=");
  Serial.print(detPct, 1);
  Serial.print(" MED_TOF_US=");
  Serial.print(medianTof);
  Serial.print(" MED_EDGE_AFTER_ARM_US=");
  Serial.print(medianEdgeAfterArmUs);
  Serial.print(" JITTER_US=");
  Serial.print(jitter);
  Serial.print(" MIN_US=");
  Serial.print(minTof);
  Serial.print(" MAX_US=");
  Serial.println(maxTof);
}

// ---------------------------------------------------------------------------
// scoreCouplingCombo — also tracks edge count per shot
// ---------------------------------------------------------------------------
void scoreCouplingCombo(char txDir, char rxDir, bool relState, bool drvState) {
  int detectedCount = 0;
  int32_t tofs[COUPLING_SHOTS];
  int32_t firstEdgeUsArr[COUPLING_SHOTS];
  int32_t edgeCountArr[COUPLING_SHOTS];
  int validTofCount = 0;
  int validFirstCount = 0;
  int32_t totalEdges = 0;

  for (int i = 0; i < COUPLING_SHOTS; ++i) {
    setRxDirection(rxDir);
    disableRxPath();
    setTxCombo(txDir, relState, drvState);
    g_captureArmed = false;
    delay(2);

    enableBoost();
    delay(g_boostPrechargeMs);

    resetEdgeCapture();
    sendBurst40kHz(BURST_CYCLES);
    const uint32_t txDoneUs = micros();

    delayMicroseconds(g_blankingUs);
    enableRxPath();
    delayMicroseconds(20);

    resetEdgeCapture();
    if (g_postEnableGuardUs > 0) {
      delayMicroseconds(g_postEnableGuardUs);
    }
    g_captureArmed = true;
    const uint32_t listenStartUs = micros();

    uint32_t handledCount = 0;
    bool detected = false;
    int32_t tofUs = -1;

    while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
      const uint32_t countNow = g_edgeCount;
      if (countNow > handledCount) {
        const uint32_t edgeUs = g_lastEdgeUs;
        handledCount = countNow;
        tofUs = static_cast<int32_t>(edgeUs - txDoneUs);
        if (tofUs >= static_cast<int32_t>(g_minValidTofUs)) {
          detected = true;
          break;
        }
      }
    }

    g_captureArmed = false;

    const uint32_t edgeCount = g_edgeCount;
    const int32_t firstEdgeUs = (edgeCount > 0) ? static_cast<int32_t>(g_firstEdgeUs - listenStartUs) : -1;

    if (detected) {
      detectedCount++;
      if (validTofCount < COUPLING_SHOTS) {
        tofs[validTofCount++] = tofUs;
      }
    }
    if (firstEdgeUs >= 0 && validFirstCount < COUPLING_SHOTS) {
      firstEdgeUsArr[validFirstCount++] = firstEdgeUs;
    }

    edgeCountArr[i] = static_cast<int32_t>(edgeCount);
    totalEdges += edgeCountArr[i];

    clearTxSelects();
    disableRxPath();
    disableBoost();
    delay(INTER_SHOT_MS);
  }

  int32_t medianTof = -1;
  int32_t minTof = -1;
  int32_t maxTof = -1;
  int32_t jitter = -1;
  if (validTofCount > 0) {
    sortInt32(tofs, validTofCount);
    minTof = tofs[0];
    maxTof = tofs[validTofCount - 1];
    medianTof = tofs[validTofCount / 2];
    jitter = maxTof - minTof;
  }

  int32_t medianFirstUs = -1;
  if (validFirstCount > 0) {
    sortInt32(firstEdgeUsArr, validFirstCount);
    medianFirstUs = firstEdgeUsArr[validFirstCount / 2];
  }

  sortInt32(edgeCountArr, COUPLING_SHOTS);
  const int32_t medianEdges = edgeCountArr[COUPLING_SHOTS / 2];
  const float meanEdges = static_cast<float>(totalEdges) / static_cast<float>(COUPLING_SHOTS);
  const float detPct = (100.0f * static_cast<float>(detectedCount)) / static_cast<float>(COUPLING_SHOTS);

  Serial.print("COUPLING TX=");
  Serial.print(txDir);
  Serial.print(" RX=");
  Serial.print(rxDir);
  Serial.print(" REL=");
  Serial.print(relState ? 1 : 0);
  Serial.print(" DRV=");
  Serial.print(drvState ? 1 : 0);
  Serial.print(" DET=");
  Serial.print(detectedCount);
  Serial.print("/");
  Serial.print(COUPLING_SHOTS);
  Serial.print(" DET_PCT=");
  Serial.print(detPct, 1);
  Serial.print(" MED_TOF_US=");
  Serial.print(medianTof);
  Serial.print(" JITTER_US=");
  Serial.print(jitter);
  Serial.print(" MIN_US=");
  Serial.print(minTof);
  Serial.print(" MAX_US=");
  Serial.print(maxTof);
  Serial.print(" MEAN_EDGES=");
  Serial.print(meanEdges, 2);
  Serial.print(" MEDIAN_EDGES=");
  Serial.print(medianEdges);
  Serial.print(" FIRST_EDGE_MED_US=");
  Serial.println(medianFirstUs);
}

// ---------------------------------------------------------------------------
// collectMedianTof — helper for paired-axis round
// ---------------------------------------------------------------------------
int32_t collectMedianTof(char txDir, char rxDir, bool relState, bool drvState, int* outDetections) {
  for (int i = 0; i < WARMUP_SHOTS; ++i) {
    int32_t dummyTof = -1;
    runSingleTest(txDir, rxDir, relState, drvState, &dummyTof, nullptr);
    delay(INTER_SHOT_MS);
  }

  int32_t tofs[SCORE_SHOTS];
  int detectedCount = 0;

  for (int i = 0; i < SCORE_SHOTS; ++i) {
    int32_t tofUs = -1;
    const bool detected = runSingleTest(txDir, rxDir, relState, drvState, &tofUs, nullptr);
    if (detected && detectedCount < SCORE_SHOTS) {
      tofs[detectedCount++] = tofUs;
    }
    delay(INTER_SHOT_MS);
  }

  if (outDetections != nullptr) {
    *outDetections = detectedCount;
  }

  if (detectedCount <= 0) {
    return -1;
  }

  sortInt32(tofs, detectedCount);
  return tofs[detectedCount / 2];
}

// ===========================================================================
// TEST 1 — AND-gate blanking verification (V2 new)
// ===========================================================================
void testAndGateBlanking() {
  Serial.println("==== AND-GATE BLANKING VERIFICATION START ====");
  Serial.println("V2: TOF_EDGE = COMP_RAW AND RX_WINDOW_EN");
  Serial.println("    RX_WINDOW_EN = NOT RX_EN_N");
  Serial.println("    When RX_EN_N=HIGH (mux disabled), RX_WINDOW_EN=LOW, TOF_EDGE forced LOW");

  int passCount = 0;
  int failCount = 0;

  for (int trial = 0; trial < 5; ++trial) {
    // --- Phase A: RX disabled, count edges for 1000 ms ---
    clearTxSelects();
    disableRxPath();  // RX_EN_N = HIGH -> RX_WINDOW_EN = LOW -> AND gate blocks
    disableBoost();
    g_captureArmed = false;
    resetEdgeCapture();
    g_captureArmed = true;

    const uint32_t t0a = micros();
    while ((micros() - t0a) < 1000000UL) {
    }
    g_captureArmed = false;
    const uint32_t disabledCount = g_edgeCount;

    // --- Phase B: RX enabled, count edges for 1000 ms ---
    enableRxPath();  // RX_EN_N = LOW -> RX_WINDOW_EN = HIGH -> AND gate passes
    g_captureArmed = false;
    resetEdgeCapture();
    g_captureArmed = true;

    const uint32_t t0b = micros();
    while ((micros() - t0b) < 1000000UL) {
    }
    g_captureArmed = false;
    const uint32_t enabledCount = g_edgeCount;

    disableRxPath();

    Serial.print("TRIAL=");
    Serial.print(trial + 1);
    Serial.print(" RX_DISABLED_EDGES=");
    Serial.print(disabledCount);
    Serial.print(" RX_ENABLED_EDGES=");
    Serial.println(enabledCount);

    if (disabledCount == 0) {
      passCount++;
    } else {
      failCount++;
    }
  }

  Serial.print("RESULT: PASS=");
  Serial.print(passCount);
  Serial.print("/5 FAIL=");
  Serial.print(failCount);
  Serial.print("/5 -- ");
  if (failCount == 0) {
    Serial.println("PASS (AND gate blocks all edges when RX disabled)");
  } else {
    Serial.println("FAIL (edges leaked through AND gate with RX disabled)");
  }
  Serial.println("---- AND-GATE BLANKING VERIFICATION DONE ----");
}

// ===========================================================================
// TEST 2 — Boost precharge timing sweep (V2 new)
// ===========================================================================
void testBoostPrechargeSweep() {
  Serial.println("==== BOOST PRECHARGE TIMING SWEEP START ====");
  Serial.println("Sweep precharge delay to find minimum for reliable detection");
  Serial.println("TX=N RX=S REL=1 DRV=1, 10 shots per precharge value");

  const uint32_t prechargeValues[] = {0, 5, 10, 20, 30, 50, 75, 100};
  const int numValues = sizeof(prechargeValues) / sizeof(prechargeValues[0]);
  const int shotsPerValue = 10;

  const uint32_t savedPrecharge = g_boostPrechargeMs;

  for (int vi = 0; vi < numValues; ++vi) {
    const uint32_t prechargeMs = prechargeValues[vi];
    int detectedCount = 0;
    int32_t tofs[10];

    for (int shot = 0; shot < shotsPerValue; ++shot) {
      // Manual burst sequence with custom precharge
      setRxDirection('S');
      disableRxPath();
      setTxCombo('N', true, true);
      g_captureArmed = false;
      delay(2);

      enableBoost();
      delay(prechargeMs);

      resetEdgeCapture();
      sendBurst40kHz(BURST_CYCLES);
      const uint32_t txDoneUs = micros();

      delayMicroseconds(g_blankingUs);
      enableRxPath();
      delayMicroseconds(20);

      resetEdgeCapture();
      if (g_postEnableGuardUs > 0) {
        delayMicroseconds(g_postEnableGuardUs);
      }
      g_captureArmed = true;
      const uint32_t listenStartUs = micros();

      bool detected = false;
      int32_t tofUs = -1;
      uint32_t handledCount = 0;

      while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
        const uint32_t countNow = g_edgeCount;
        if (countNow > handledCount) {
          const uint32_t edgeUs = g_lastEdgeUs;
          handledCount = countNow;
          tofUs = static_cast<int32_t>(edgeUs - txDoneUs);
          if (tofUs >= static_cast<int32_t>(g_minValidTofUs)) {
            detected = true;
            break;
          }
        }
      }

      g_captureArmed = false;

      if (detected) {
        tofs[detectedCount++] = tofUs;
      }

      clearTxSelects();
      disableRxPath();
      disableBoost();
      delay(INTER_SHOT_MS);
    }

    int32_t medianTof = -1;
    if (detectedCount > 0) {
      sortInt32(tofs, detectedCount);
      medianTof = tofs[detectedCount / 2];
    }

    Serial.print("precharge=");
    Serial.print(prechargeMs);
    Serial.print("ms detected=");
    Serial.print(detectedCount);
    Serial.print("/");
    Serial.print(shotsPerValue);
    Serial.print(" median_tof=");
    Serial.print(medianTof);
    Serial.println("us");
  }

  g_boostPrechargeMs = savedPrecharge;
  Serial.println("---- BOOST PRECHARGE TIMING SWEEP DONE ----");
}

// ===========================================================================
// TEST 3 — Split-TX independence test (V2 new)
// ===========================================================================
void testSplitTxIndependence() {
  Serial.println("==== SPLIT-TX INDEPENDENCE TEST START ====");
  Serial.println("Verify TX_BURST_PWM and TX_22V_EN_N are independent");

  // Check if boost is already warm (enabled via 'b' key before this test)
  if (!g_boostWarmFlag) {
    Serial.println("ERROR: Boost not warm. Press 'b' first to enable boost,");
    Serial.println("       accept reboot, then run test 3.");
    Serial.println("---- SPLIT-TX INDEPENDENCE TEST DONE ----");
    return;
  }

  // Phase A: Boost ON, no burst — count edges while boost is actively switching.
  // MT3608 switching noise WILL create edges — this is expected.
  Serial.println("-- Phase A: Boost ON, no burst (switching noise check) --");
  clearTxSelects();
  disableRxPath();

  g_captureArmed = false;
  resetEdgeCapture();
  enableRxPath();
  delayMicroseconds(20);
  g_captureArmed = true;

  const uint32_t t0a = micros();
  while ((micros() - t0a) < 500000UL) {
  }
  g_captureArmed = false;
  const uint32_t phaseAEdges = g_edgeCount;

  disableRxPath();

  Serial.print("PHASE_A EDGES=");
  Serial.print(phaseAEdges);
  Serial.println(" (switching noise, expected >0) -- INFO");

  // Phase A2: Disable boost, wait for switching to stop, then count edges.
  // This measures the ambient noise floor of the RX chain with no active
  // switching. Expect ~500-1000 edges/sec (same as Test 1 RX-enabled baseline).
  // The purpose is to confirm that disabling boost doesn't create ADDITIONAL
  // noise beyond the ambient baseline.
  Serial.println("-- Phase A2: Boost OFF, no burst (ambient noise floor) --");
  disableBoost();
  delay(200);  // let MT3608 switching fully stop

  g_captureArmed = false;
  resetEdgeCapture();
  enableRxPath();
  delayMicroseconds(20);
  g_captureArmed = true;

  const uint32_t t0a2 = micros();
  while ((micros() - t0a2) < 500000UL) {
  }
  g_captureArmed = false;
  const uint32_t phaseA2Edges = g_edgeCount;

  disableRxPath();

  Serial.print("PHASE_A2 EDGES=");
  Serial.print(phaseA2Edges);
  Serial.print(" (ambient noise floor, expect ~500-1000) -- ");
  if (phaseA2Edges < phaseAEdges) {
    Serial.println("OK (less than Phase A switching noise)");
  } else {
    Serial.println("UNUSUAL (equal or more than Phase A — check for interference)");
  }

  delay(200);

  // Phase B: Burst ON, boost OFF.
  // The 22V cap (100uF) has no significant discharge path when driver relays
  // are off (leakage ~1uA, time constant ~2200s). We must actively drain it
  // by firing many bursts through the transducer.
  Serial.println("-- Phase B: Burst ON, boost OFF (draining 22V cap) --");
  disableBoost();
  Serial.println("  Draining 22V cap with 100 bursts (no boost recharge)...");

  // Fire 100 bursts with TX direction selected to drain the 22V cap.
  // Each burst draws ~50mA for ~300us from the cap.
  // 100 bursts × 50mA × 300us ≈ 1500uC, cap has ~2200uC total.
  // After 100 bursts, cap should be at ~7V or less.
  setRxDirection('S');
  disableRxPath();
  for (int drain = 0; drain < 100; ++drain) {
    setTxCombo('N', true, true);
    sendBurst40kHz(BURST_CYCLES);
    clearTxSelects();
    delay(10);  // short delay between drain bursts
  }
  Serial.println("  Cap drained, waiting 2s for residual to settle...");
  delay(2000);

  // Now fire 10 test bursts with boost OFF and 22V cap drained.
  Serial.println("  Starting test bursts (22V cap should be near 0V)...");
  int phaseBDetected = 0;

  for (int shot = 0; shot < 10; ++shot) {
    setRxDirection('S');
    disableRxPath();
    setTxCombo('N', true, true);
    g_captureArmed = false;
    delay(2);

    // Boost stays OFF

    resetEdgeCapture();
    sendBurst40kHz(BURST_CYCLES);
    const uint32_t txDoneUs = micros();

    delayMicroseconds(g_blankingUs);
    enableRxPath();
    delayMicroseconds(20);

    resetEdgeCapture();
    if (g_postEnableGuardUs > 0) {
      delayMicroseconds(g_postEnableGuardUs);
    }
    g_captureArmed = true;
    const uint32_t listenStartUs = micros();

    bool detected = false;
    int32_t tofUs = -1;
    uint32_t handledCount = 0;

    while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
      const uint32_t countNow = g_edgeCount;
      if (countNow > handledCount) {
        const uint32_t edgeUs = g_lastEdgeUs;
        handledCount = countNow;
        tofUs = static_cast<int32_t>(edgeUs - txDoneUs);
        if (tofUs >= static_cast<int32_t>(g_minValidTofUs)) {
          detected = true;
          break;
        }
      }
    }

    g_captureArmed = false;

    if (detected) {
      phaseBDetected++;
    }

    clearTxSelects();
    disableRxPath();
    delay(INTER_SHOT_MS);
  }

  Serial.print("PHASE_B DETECTED=");
  Serial.print(phaseBDetected);
  Serial.print("/10 EXPECTED=0 -- ");
  Serial.println(phaseBDetected == 0 ? "PASS" : "FAIL");

  delay(200);

  // Phase C: Both ON (normal). Fire 10 bursts with boost + burst.
  // Should be >0% detection.
  // The 22V cap is now drained from Phase B, so re-enabling boost
  // will cause full inrush. Accept brownout — if it happens, press
  // 'b' then '3' again. The Phase C result from the successful run
  // is the one that matters.
  Serial.println("-- Phase C: Boost ON + Burst ON (normal) --");
  Serial.println("Re-enabling boost (22V cap drained, may brownout)...");
  Serial.println("  If brownout: press 'b' then '3' — Phase C will run with warm boost");
  Serial.flush();
  delay(10);
  enableBoost();
  delay(g_boostPrechargeMs);
  delay(50);   // extra settling
  Serial.println("Boost re-enabled");

  int phaseCDetected = 0;

  for (int shot = 0; shot < 10; ++shot) {
    setRxDirection('S');
    disableRxPath();
    setTxCombo('N', true, true);
    g_captureArmed = false;
    delay(2);

    // Boost already ON — no per-shot enable/disable cycle

    resetEdgeCapture();
    sendBurst40kHz(BURST_CYCLES);
    const uint32_t txDoneUs = micros();

    delayMicroseconds(g_blankingUs);
    enableRxPath();
    delayMicroseconds(20);

    resetEdgeCapture();
    if (g_postEnableGuardUs > 0) {
      delayMicroseconds(g_postEnableGuardUs);
    }
    g_captureArmed = true;
    const uint32_t listenStartUs = micros();

    bool detected = false;
    int32_t tofUs = -1;
    uint32_t handledCount = 0;

    while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
      const uint32_t countNow = g_edgeCount;
      if (countNow > handledCount) {
        const uint32_t edgeUs = g_lastEdgeUs;
        handledCount = countNow;
        tofUs = static_cast<int32_t>(edgeUs - txDoneUs);
        if (tofUs >= static_cast<int32_t>(g_minValidTofUs)) {
          detected = true;
          break;
        }
      }
    }

    g_captureArmed = false;

    if (detected) {
      phaseCDetected++;
    }

    clearTxSelects();
    disableRxPath();
    // Boost stays ON — no disableBoost() here
    delay(INTER_SHOT_MS);
  }

  disableBoost();  // disable boost once after all shots complete
  g_boostWarmFlag = false;

  Serial.print("PHASE_C DETECTED=");
  Serial.print(phaseCDetected);
  Serial.print("/10 EXPECTED>0 -- ");
  Serial.println(phaseCDetected > 0 ? "PASS" : "FAIL");

  Serial.println("---- SPLIT-TX INDEPENDENCE TEST DONE ----");
}

// ===========================================================================
// TEST 4 — Open-path route finder
// ===========================================================================
void runRouteFinderRound(const char* label) {
  Serial.print("==== ROUTE-FINDER ROUND START LABEL=");
  Serial.print(label);
  Serial.println(" ====");

  // Quiet check first to detect comparator chatter without transmit activity.
  runListenOnly(2000);
  delay(200);

  // Route-finder scoring for first path: TX North -> RX South.
  scoreCombo('N', 'S', false, false);
  scoreCombo('N', 'S', true, false);
  scoreCombo('N', 'S', false, true);
  scoreCombo('N', 'S', true, true);

  Serial.print("---- ROUTE-FINDER ROUND DONE LABEL=");
  Serial.print(label);
  Serial.println(" ----");
}

// ===========================================================================
// TEST 5 — Blocked-path route finder (same as test 4, user blocks path)
// ===========================================================================
void runBlockedPathRound() {
  Serial.println("==== BLOCKED-PATH ROUTE FINDER START ====");
  Serial.println("Block the acoustic path between transducers before proceeding.");
  Serial.println("Press any key to start...");
  while (Serial.available() <= 0) {
    delay(20);
  }
  (void)Serial.read();

  runRouteFinderRound("BLOCKED");
}

// ===========================================================================
// TEST 6 — Coupling round (RX unplugged)
// ===========================================================================
void runCouplingRound(const char* label) {
  Serial.print("==== COUPLING ROUND START LABEL=");
  Serial.print(label);
  Serial.println(" ====");
  Serial.println("Reminder: disconnect RX transducer cable for electrical-coupling check.");

  // Noise baseline first
  clearTxSelects();
  enableRxPath();
  g_captureArmed = false;
  resetEdgeCapture();
  g_captureArmed = true;
  const uint32_t noiseT0 = micros();
  while ((micros() - noiseT0) < NOISE_WINDOW_US) {
  }
  g_captureArmed = false;
  const uint32_t noiseEdges = g_edgeCount;
  disableRxPath();

  Serial.print("NOISE_BASELINE EDGES=");
  Serial.print(noiseEdges);
  Serial.print(" WINDOW_US=");
  Serial.println(NOISE_WINDOW_US);

  scoreCouplingCombo('N', 'S', false, false);
  scoreCouplingCombo('N', 'S', true, false);
  scoreCouplingCombo('N', 'S', false, true);
  scoreCouplingCombo('N', 'S', true, true);

  Serial.print("---- COUPLING ROUND DONE LABEL=");
  Serial.print(label);
  Serial.println(" ----");
}

// ===========================================================================
// TEST 7 — RX-enabled noise baseline
// ===========================================================================
void runNoiseBaseline(uint32_t windowUs, int repeats) {
  if (repeats > NOISE_REPEATS) {
    repeats = NOISE_REPEATS;
  }

  Serial.print("==== NOISE BASELINE START WINDOW_US=");
  Serial.print(windowUs);
  Serial.print(" REPEATS=");
  Serial.print(repeats);
  Serial.println(" ====");

  int32_t firstEdges[NOISE_REPEATS];
  int32_t counts[NOISE_REPEATS];
  int validFirstCount = 0;
  int32_t totalCount = 0;

  for (int i = 0; i < repeats; ++i) {
    clearTxSelects();
    enableRxPath();
    g_captureArmed = false;
    resetEdgeCapture();
    g_captureArmed = true;

    const uint32_t t0 = micros();
    while ((micros() - t0) < windowUs) {
    }

    g_captureArmed = false;

    const uint32_t edgeCount = g_edgeCount;
    const int32_t firstEdgeUs = (edgeCount > 0) ? static_cast<int32_t>(g_firstEdgeUs - t0) : -1;
    disableRxPath();

    counts[i] = static_cast<int32_t>(edgeCount);
    totalCount += counts[i];
    if (firstEdgeUs >= 0) {
      firstEdges[validFirstCount++] = firstEdgeUs;
    }

    Serial.print("NOISE_SAMPLE IDX=");
    Serial.print(i + 1);
    Serial.print(" EDGE_COUNT=");
    Serial.print(edgeCount);
    Serial.print(" FIRST_EDGE_US=");
    Serial.print(firstEdgeUs);
#if DEBUG_FIRST3_EDGES
    Serial.print(" E0_US=");
    Serial.print(edgeRelUs(g_firstEdgeUs, t0));
    Serial.print(" E1_US=");
    Serial.print(edgeRelUs(g_secondEdgeUs, t0));
    Serial.print(" E2_US=");
    Serial.print(edgeRelUs(g_thirdEdgeUs, t0));
#endif
    Serial.println();
    delay(40);
  }

  sortInt32(counts, repeats);
  const int32_t medianCount = counts[repeats / 2];
  const float meanCount = static_cast<float>(totalCount) / static_cast<float>(repeats);

  int32_t medianFirstUs = -1;
  if (validFirstCount > 0) {
    sortInt32(firstEdges, validFirstCount);
    medianFirstUs = firstEdges[validFirstCount / 2];
  }

  Serial.print("NOISE_SUMMARY REPEATS=");
  Serial.print(repeats);
  Serial.print(" MEAN_EDGE_COUNT=");
  Serial.print(meanCount, 2);
  Serial.print(" MEDIAN_EDGE_COUNT=");
  Serial.print(medianCount);
  Serial.print(" FIRST_EDGE_MED_US=");
  Serial.println(medianFirstUs);
  Serial.println("---- NOISE BASELINE DONE ----");
}

// ===========================================================================
// TEST 8 — RX-disabled noise baseline
// ===========================================================================
void runRxDisabledBaseline(uint32_t windowUs, int repeats) {
  if (repeats > NOISE_REPEATS) {
    repeats = NOISE_REPEATS;
  }

  Serial.print("==== RX-DISABLED BASELINE START WINDOW_US=");
  Serial.print(windowUs);
  Serial.print(" REPEATS=");
  Serial.print(repeats);
  Serial.println(" ====");
  Serial.println("V2: With AND gate, RX-disabled should show 0 edges (TOF_EDGE forced LOW)");

  int32_t counts[NOISE_REPEATS];
  int32_t totalCount = 0;

  for (int i = 0; i < repeats; ++i) {
    clearTxSelects();
    disableRxPath();
    disableBoost();
    g_captureArmed = false;
    resetEdgeCapture();
    g_captureArmed = true;

    const uint32_t t0 = micros();
    while ((micros() - t0) < windowUs) {
    }

    g_captureArmed = false;
    const uint32_t edgeCount = g_edgeCount;
    counts[i] = static_cast<int32_t>(edgeCount);
    totalCount += counts[i];

    Serial.print("RXOFF_SAMPLE IDX=");
    Serial.print(i + 1);
    Serial.print(" EDGE_COUNT=");
    Serial.print(edgeCount);
#if DEBUG_FIRST3_EDGES
    Serial.print(" E0_US=");
    Serial.print(edgeRelUs(g_firstEdgeUs, t0));
    Serial.print(" E1_US=");
    Serial.print(edgeRelUs(g_secondEdgeUs, t0));
    Serial.print(" E2_US=");
    Serial.print(edgeRelUs(g_thirdEdgeUs, t0));
#endif
    Serial.println();

    delay(40);
  }

  sortInt32(counts, repeats);
  const int32_t medianCount = counts[repeats / 2];
  const float meanCount = static_cast<float>(totalCount) / static_cast<float>(repeats);

  Serial.print("RXOFF_SUMMARY REPEATS=");
  Serial.print(repeats);
  Serial.print(" MEAN_EDGE_COUNT=");
  Serial.print(meanCount, 2);
  Serial.print(" MEDIAN_EDGE_COUNT=");
  Serial.println(medianCount);
  Serial.println("---- RX-DISABLED BASELINE DONE ----");
}

// ===========================================================================
// TEST 9 — Paired-axis round (N<->S reciprocal)
// ===========================================================================
void runPairedAxisRound(const char* label, char a, char b) {
  Serial.print("==== PAIRED AXIS ROUND START LABEL=");
  Serial.print(label);
  Serial.print(" AXIS=");
  Serial.print(a);
  Serial.print("<->");
  Serial.print(b);
  Serial.println(" ====");

  const bool relOpts[2] = {false, true};
  const bool drvOpts[2] = {false, true};

  for (int relIdx = 0; relIdx < 2; ++relIdx) {
    for (int drvIdx = 0; drvIdx < 2; ++drvIdx) {
      const bool relState = relOpts[relIdx];
      const bool drvState = drvOpts[drvIdx];

      int detAB = 0;
      int detBA = 0;
      const int32_t medAB = collectMedianTof(a, b, relState, drvState, &detAB);
      const int32_t medBA = collectMedianTof(b, a, relState, drvState, &detBA);

      Serial.print("PAIR AXIS=");
      Serial.print(a);
      Serial.print("<->");
      Serial.print(b);
      Serial.print(" REL=");
      Serial.print(relState ? 1 : 0);
      Serial.print(" DRV=");
      Serial.print(drvState ? 1 : 0);
      Serial.print(" DET_");
      Serial.print(a);
      Serial.print(b);
      Serial.print("=");
      Serial.print(detAB);
      Serial.print("/");
      Serial.print(SCORE_SHOTS);
      Serial.print(" MED_");
      Serial.print(a);
      Serial.print(b);
      Serial.print("_US=");
      Serial.print(medAB);
      Serial.print(" DET_");
      Serial.print(b);
      Serial.print(a);
      Serial.print("=");
      Serial.print(detBA);
      Serial.print("/");
      Serial.print(SCORE_SHOTS);
      Serial.print(" MED_");
      Serial.print(b);
      Serial.print(a);
      Serial.print("_US=");
      Serial.print(medBA);

      int32_t deltaUs = -999999;
      if (medAB > 0 && medBA > 0) {
        deltaUs = medAB - medBA;
      }

      Serial.print(" DELTA_US=");
      if (deltaUs == -999999) {
        Serial.print("NA");
      } else {
        Serial.print(deltaUs);
      }
      Serial.println();
      delay(150);
    }
  }

  Serial.print("---- PAIRED AXIS ROUND DONE LABEL=");
  Serial.print(label);
  Serial.println(" ----");
}

// ===========================================================================
// TEST A — Aggressor matrix (6 scenarios)
// ===========================================================================
void runAggressorScenario(const char* label, int scenarioId) {
  Serial.print("==== AGGRESSOR START LABEL=");
  Serial.print(label);
  Serial.print(" WINDOW_US=");
  Serial.print(AGG_WINDOW_US);
  Serial.print(" REPEATS=");
  Serial.print(AGG_REPEATS);
  Serial.println(" ====");

  int32_t counts[AGG_REPEATS];
  int32_t firstEdges[AGG_REPEATS];
  int firstValidCount = 0;
  int32_t totalCount = 0;

  for (int i = 0; i < AGG_REPEATS; ++i) {
    clearTxSelects();
    setRxDirection('S');
    disableRxPath();
    disableBoost();
    digitalWrite(PIN_TX_BURST_PWM, LOW);
    g_captureArmed = false;
    resetEdgeCapture();

    const uint32_t t0 = micros();
    enableRxPath();
    g_captureArmed = true;

    bool fired = false;
    while ((micros() - t0) < AGG_WINDOW_US) {
      const uint32_t dt = micros() - t0;
      if (!fired && dt >= 120) {
        fired = true;
        switch (scenarioId) {
          case 0:  // BASE_RX_ONLY — just listen, no stimulus
            break;
          case 1:  // RX_EN_N_TOGGLE — toggle RX enable at 120 us
            disableRxPath();
            delayMicroseconds(20);
            enableRxPath();
            break;
          case 2:  // MUX_SWITCH — toggle mux channel at 120 us
            setRxDirection('N');
            delayMicroseconds(20);
            setRxDirection('S');
            break;
          case 3:  // DRVREL_SWITCH — toggle DRV/REL at 120 us
            setTxCombo('N', true, true);
            delayMicroseconds(20);
            clearTxSelects();
            break;
          case 4:  // TX_BURST_ONLY — fire burst at 120 us (no route selected)
            sendBurst40kHz(BURST_CYCLES);
            break;
          case 5:  // TX_BURST_WITH_ROUTE — select route + fire burst at 120 us
            setTxCombo('N', true, true);
            sendBurst40kHz(BURST_CYCLES);
            break;
          default:
            break;
        }
      }
    }

    g_captureArmed = false;
    const uint32_t edgeCount = g_edgeCount;
    const int32_t firstEdgeUs = (edgeCount > 0) ? static_cast<int32_t>(g_firstEdgeUs - t0) : -1;

    counts[i] = static_cast<int32_t>(edgeCount);
    totalCount += counts[i];
    firstEdges[i] = firstEdgeUs;
    if (firstEdgeUs >= 0) {
      firstValidCount++;
    }

    clearTxSelects();
    disableRxPath();
    disableBoost();

    Serial.print("AGG_SAMPLE LABEL=");
    Serial.print(label);
    Serial.print(" IDX=");
    Serial.print(i + 1);
    Serial.print(" EDGE_COUNT=");
    Serial.print(edgeCount);
    Serial.print(" FIRST_EDGE_US=");
    Serial.println(firstEdgeUs);
    delay(30);
  }

  sortInt32(counts, AGG_REPEATS);
  const float meanCount = static_cast<float>(totalCount) / static_cast<float>(AGG_REPEATS);
  const int32_t medianCount = counts[AGG_REPEATS / 2];

  int32_t compactFirst[AGG_REPEATS];
  int compactIdx = 0;
  for (int i = 0; i < AGG_REPEATS; ++i) {
    if (firstEdges[i] >= 0) {
      compactFirst[compactIdx++] = firstEdges[i];
    }
  }

  int32_t medianFirstUs = -1;
  if (compactIdx > 0) {
    sortInt32(compactFirst, compactIdx);
    medianFirstUs = compactFirst[compactIdx / 2];
  }

  Serial.print("AGG_SUMMARY LABEL=");
  Serial.print(label);
  Serial.print(" MEAN_EDGE_COUNT=");
  Serial.print(meanCount, 2);
  Serial.print(" MEDIAN_EDGE_COUNT=");
  Serial.print(medianCount);
  Serial.print(" FIRST_EDGE_MED_US=");
  Serial.print(medianFirstUs);
  Serial.print(" FIRST_EDGE_VALID=");
  Serial.print(firstValidCount);
  Serial.print("/");
  Serial.println(AGG_REPEATS);
}

void runAggressorRound() {
  Serial.println("==== AGGRESSOR MATRIX START ====");
  runAggressorScenario("BASE_RX_ONLY", 0);
  runAggressorScenario("RX_EN_N_TOGGLE", 1);
  runAggressorScenario("MUX_SWITCH", 2);
  runAggressorScenario("DRVREL_SWITCH", 3);
  runAggressorScenario("TX_BURST_ONLY", 4);
  runAggressorScenario("TX_BURST_WITH_ROUTE", 5);
  Serial.println("---- AGGRESSOR MATRIX DONE ----");
}

// ===========================================================================
// TEST S — Blanking/guard/min-TOF sweep
// ===========================================================================
void runSweepRound() {
  const uint32_t blankingList[] = {320, 500, 800, 1100};
  const uint32_t guardList[] = {0, 80, 160, 240};
  const uint32_t minTofList[] = {220, 350, 500};

  const uint32_t oldBlanking = g_blankingUs;
  const uint32_t oldGuard = g_postEnableGuardUs;
  const uint32_t oldMinTof = g_minValidTofUs;

  Serial.println("==== SWEEP START (TX=N RX=S REL=1 DRV=1) ====");
  Serial.println("Columns: BLANK_US GUARD_US MIN_TOF_US DET/SWEEP_SHOTS MED_TOF_US");

  for (size_t mi = 0; mi < (sizeof(minTofList) / sizeof(minTofList[0])); ++mi) {
    for (size_t bi = 0; bi < (sizeof(blankingList) / sizeof(blankingList[0])); ++bi) {
      for (size_t gi = 0; gi < (sizeof(guardList) / sizeof(guardList[0])); ++gi) {
        g_minValidTofUs = minTofList[mi];
        g_blankingUs = blankingList[bi];
        g_postEnableGuardUs = guardList[gi];

        int detectedCount = 0;
        int32_t tofs[SWEEP_SHOTS];

        for (int i = 0; i < SWEEP_SHOTS; ++i) {
          int32_t tofUs = -1;
          const bool detected = runSingleTest('N', 'S', true, true, &tofUs, nullptr);
          if (detected && detectedCount < SWEEP_SHOTS) {
            tofs[detectedCount++] = tofUs;
          }
          delay(INTER_SHOT_MS);
        }

        int32_t medianTof = -1;
        if (detectedCount > 0) {
          sortInt32(tofs, detectedCount);
          medianTof = tofs[detectedCount / 2];
        }

        Serial.print("SWEEP BLANK_US=");
        Serial.print(g_blankingUs);
        Serial.print(" GUARD_US=");
        Serial.print(g_postEnableGuardUs);
        Serial.print(" MIN_TOF_US=");
        Serial.print(g_minValidTofUs);
        Serial.print(" DET=");
        Serial.print(detectedCount);
        Serial.print("/");
        Serial.print(SWEEP_SHOTS);
        Serial.print(" MED_TOF_US=");
        Serial.println(medianTof);
      }
    }
  }

  g_blankingUs = oldBlanking;
  g_postEnableGuardUs = oldGuard;
  g_minValidTofUs = oldMinTof;
  Serial.println("---- SWEEP DONE ----");
}

// ===========================================================================
// TEST R — Manual route finder
// ===========================================================================
void runManualRouteFinder() {
  Serial.println("==== MANUAL ROUTE FINDER ====");
  Serial.println("Enter: TX_DIR RX_DIR REL DRV  (e.g. N S 1 1)");
  Serial.println("Directions: N S E W");
  Serial.println("Type 'q' to quit");

  while (true) {
    // Flush input
    while (Serial.available() > 0) {
      (void)Serial.read();
    }

    Serial.print("> ");
    while (Serial.available() <= 0) {
      delay(20);
    }

    // Read line
    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.length() == 1 && (line[0] == 'q' || line[0] == 'Q')) {
      Serial.println("Exiting manual route finder.");
      break;
    }

    // Parse: TX_DIR RX_DIR REL DRV
    if (line.length() < 7) {
      Serial.println("Format: TX_DIR RX_DIR REL DRV  (e.g. N S 1 1)");
      continue;
    }

    char txDir = line.charAt(0);
    char rxDir = line.charAt(2);
    int relVal = line.charAt(4) - '0';
    int drvVal = line.charAt(6) - '0';

    bool relState = (relVal != 0);
    bool drvState = (drvVal != 0);

    Serial.print("Firing: TX=");
    Serial.print(txDir);
    Serial.print(" RX=");
    Serial.print(rxDir);
    Serial.print(" REL=");
    Serial.print(relState ? 1 : 0);
    Serial.print(" DRV=");
    Serial.println(drvState ? 1 : 0);

    int32_t tofUs = -1;
    int32_t edgeAfterArmUs = -1;
    const bool detected = runSingleTest(txDir, rxDir, relState, drvState, &tofUs, &edgeAfterArmUs);

    Serial.print("RESULT DETECT=");
    Serial.print(detected ? 1 : 0);
    Serial.print(" TOF_US=");
    Serial.print(tofUs);
    Serial.print(" EDGE_AFTER_ARM_US=");
    Serial.print(edgeAfterArmUs);
    Serial.print(" BLANK_US=");
    Serial.print(g_blankingUs);
    Serial.print(" PRECHARGE_MS=");
    Serial.print(g_boostPrechargeMs);
    Serial.print(" GUARD_US=");
    Serial.print(g_postEnableGuardUs);
    Serial.print(" MIN_TOF_US=");
    Serial.println(g_minValidTofUs);
  }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
void printMenu() {
  Serial.println();
  Serial.println("=== Node V2 Ultrasonic Bring-up ===");
  Serial.println("Pins: TOF_EDGE=34 RX_EN_N=4 MUX_A=16 MUX_B=17");
  Serial.println("      DRV_N=26 DRV_E=27 DRV_S=14 DRV_W=13");
  Serial.println("      REL_N=33 REL_E=32 REL_S=21 REL_W=22");
  Serial.println("      TX_BURST_PWM=25 TX_22V_EN_N=5 PWR_HOLD=23");
  Serial.print("Tune: BLANKING=");
  Serial.print(g_blankingUs);
  Serial.print("us TIMEOUT=");
  Serial.print(TOF_TIMEOUT_US);
  Serial.print("us MIN_TOF=");
  Serial.print(g_minValidTofUs);
  Serial.print("us BURST=");
  Serial.print(BURST_CYCLES);
  Serial.println("cyc");
  Serial.print("      PRECHARGE=");
  Serial.print(g_boostPrechargeMs);
  Serial.print("ms INTER_SHOT=");
  Serial.print(INTER_SHOT_MS);
  Serial.print("ms WARMUP=");
  Serial.print(WARMUP_SHOTS);
  Serial.print(" SCORE=");
  Serial.println(SCORE_SHOTS);
  Serial.println();
  Serial.println("  1 = AND-gate blanking verification (V2 new)");
  Serial.println("  2 = Boost precharge timing sweep (V2 new)");
  Serial.println("  3 = Split-TX independence test (V2 new)");
  Serial.println("  4 = Open-path route finder");
  Serial.println("  5 = Blocked-path route finder");
  Serial.println("  6 = Coupling round (RX unplugged)");
  Serial.println("  7 = RX-enabled noise baseline");
  Serial.println("  8 = RX-disabled noise baseline");
  Serial.println("  9 = Paired-axis round (N<->S reciprocal)");
  Serial.println("  A = Aggressor matrix (6 scenarios)");
  Serial.println("  S = Blanking/guard/min-TOF sweep");
  Serial.println("  R = Manual route finder");
  Serial.println("  b = Toggle boost ON/OFF (manual probe)");
  Serial.println("  r = Toggle RX_EN_N (manual probe)");
  Serial.println("  h = Hold PWR_HOLD HIGH");
  Serial.println("  l = Release PWR_HOLD");
}

char toUpperAscii(char c) {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

}  // namespace

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout detector
#endif
  Serial.begin(115200);
  delay(800);

  // Drive PWR_HOLD HIGH (latch power)
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Configure all pins
  pinMode(PIN_TOF_EDGE, INPUT);

  pinMode(PIN_RX_EN_N, OUTPUT);
  digitalWrite(PIN_RX_EN_N, HIGH);  // safe default = mux disabled

  pinMode(PIN_MUX_A, OUTPUT);
  digitalWrite(PIN_MUX_A, LOW);
  pinMode(PIN_MUX_B, OUTPUT);
  digitalWrite(PIN_MUX_B, LOW);

  pinMode(PIN_DRV_N, OUTPUT);
  digitalWrite(PIN_DRV_N, LOW);
  pinMode(PIN_DRV_E, OUTPUT);
  digitalWrite(PIN_DRV_E, LOW);
  pinMode(PIN_DRV_S, OUTPUT);
  digitalWrite(PIN_DRV_S, LOW);
  pinMode(PIN_DRV_W, OUTPUT);
  digitalWrite(PIN_DRV_W, LOW);

  pinMode(PIN_REL_N, OUTPUT);
  digitalWrite(PIN_REL_N, LOW);
  pinMode(PIN_REL_E, OUTPUT);
  digitalWrite(PIN_REL_E, LOW);
  pinMode(PIN_REL_S, OUTPUT);
  digitalWrite(PIN_REL_S, LOW);
  pinMode(PIN_REL_W, OUTPUT);
  digitalWrite(PIN_REL_W, LOW);

  pinMode(PIN_TX_BURST_PWM, OUTPUT);
  digitalWrite(PIN_TX_BURST_PWM, LOW);

  pinMode(PIN_TX_22V_EN_N, OUTPUT);
  digitalWrite(PIN_TX_22V_EN_N, HIGH);  // safe default = boost OFF

  // If boost was enabled before reboot, re-enable immediately.
  // The 22V output cap is partially charged from the brief period before
  // the GPIO reset, so the second inrush is much smaller.
  if (g_boostWarmFlag) {
    Serial.println("BOOST WARM: re-enabling boost (cap pre-charged from previous attempt)");
    enableBoost();
    delay(g_boostPrechargeMs);
    delay(50);  // extra settling
    Serial.println("BOOST WARM: boost ready — press '3' to run split-TX test");
  }

  // Attach interrupt on PIN_TOF_EDGE RISING
  attachInterrupt(digitalPinToInterrupt(PIN_TOF_EDGE), onTofEdge, RISING);

  // Print menu
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

  const char raw = static_cast<char>(Serial.read());

  // Handle case-sensitive commands first (lowercase b, r, h, l)
  switch (raw) {
    case 'b':
      // Toggle boost ON/OFF (manual probe)
      g_manualBoostOn = !g_manualBoostOn;
      if (g_manualBoostOn) {
        g_boostWarmFlag = true;
        Serial.println("BOOST: hard enable (may brownout and reboot)...");
        Serial.println("  After reboot, boost will auto-resume from RTC flag");
        Serial.flush();
        delay(10);
        enableBoost();  // GPIO5 LOW = boost ON — may cause brownout
        // If we get here without brownout, boost is on
        delay(g_boostPrechargeMs);
        Serial.println("BOOST: ON (no brownout)");
      } else {
        g_boostWarmFlag = false;
        disableBoost();
        Serial.println("BOOST: OFF (TX_22V_EN_N=HIGH) -- safe default");
      }
      return;
    case 'r':
      // Toggle RX_EN_N (manual probe)
      g_manualRxOn = !g_manualRxOn;
      if (g_manualRxOn) {
        enableRxPath();
        Serial.println("RX_EN_N: LOW (mux enabled) -- probe RX_WINDOW_EN and TOF_EDGE with scope");
      } else {
        disableRxPath();
        Serial.println("RX_EN_N: HIGH (mux disabled, safe default)");
      }
      return;
    case 'h':
      // Hold PWR_HOLD HIGH
      digitalWrite(PIN_PWR_HOLD, HIGH);
      Serial.println("PWR_HOLD: HIGH (latched)");
      return;
    case 'l':
      // Release PWR_HOLD
      digitalWrite(PIN_PWR_HOLD, LOW);
      Serial.println("PWR_HOLD: LOW (board may power off)");
      return;
    default:
      break;
  }

  // Uppercase and digit commands
  const char cmd = toUpperAscii(raw);

  switch (cmd) {
    case '1':
      testAndGateBlanking();
      break;
    case '2':
      testBoostPrechargeSweep();
      break;
    case '3':
      testSplitTxIndependence();
      break;
    case '4':
      runRouteFinderRound("OPEN");
      break;
    case '5':
      runBlockedPathRound();
      break;
    case '6':
      runCouplingRound("COUPLING");
      break;
    case '7':
      runNoiseBaseline(NOISE_WINDOW_US, NOISE_REPEATS);
      break;
    case '8':
      runRxDisabledBaseline(NOISE_WINDOW_US, NOISE_REPEATS);
      break;
    case '9':
      runPairedAxisRound("PAIR_NS", 'N', 'S');
      break;
    case 'A':
      runAggressorRound();
      break;
    case 'S':
      runSweepRound();
      break;
    case 'R':
      runManualRouteFinder();
      break;
    default:
      break;
  }

  // Re-print menu after test completes
  if (cmd >= '1' && cmd <= '9') {
    printMenu();
  } else if (cmd == 'A' || cmd == 'S' || cmd == 'R') {
    printMenu();
  }
}