#include <Arduino.h>

#ifndef PIN_TOF_EDGE
#define PIN_TOF_EDGE 34
#endif
#ifndef PIN_RX_EN
#define PIN_RX_EN 4
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
#ifndef PIN_TX_PWM
#define PIN_TX_PWM 25
#endif
#ifndef RX_EN_ACTIVE_HIGH
#define RX_EN_ACTIVE_HIGH 1
#endif
#ifndef TX_PWM_ACTIVE_HIGH
#define TX_PWM_ACTIVE_HIGH 1
#endif
#ifndef BLANKING_US
#define BLANKING_US 250
#endif
#ifndef TOF_TIMEOUT_US
#define TOF_TIMEOUT_US 3000
#endif
#ifndef MIN_VALID_TOF_US
#define MIN_VALID_TOF_US 180
#endif
#ifndef BURST_CYCLES
#define BURST_CYCLES 12
#endif
#ifndef INTER_SHOT_MS
#define INTER_SHOT_MS 500
#endif
#ifndef SCORE_SHOTS
#define SCORE_SHOTS 24
#endif
#ifndef WARMUP_SHOTS
#define WARMUP_SHOTS 10
#endif
#ifndef VERBOSE_SHOTS
#define VERBOSE_SHOTS 0
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
#ifndef POST_ENABLE_GUARD_US
#define POST_ENABLE_GUARD_US 0
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

#if PIN_TX_PWM == PIN_DRV_N || PIN_TX_PWM == PIN_DRV_E || PIN_TX_PWM == PIN_DRV_S || PIN_TX_PWM == PIN_DRV_W
#error "PIN_TX_PWM conflicts with one of DRV_* pins. Set PIN_TX_PWM to the dedicated TX enable GPIO."
#endif

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

constexpr uint8_t kTxOnLevel = TX_PWM_ACTIVE_HIGH ? HIGH : LOW;
constexpr uint8_t kTxOffLevel = TX_PWM_ACTIVE_HIGH ? LOW : HIGH;
constexpr uint8_t kRxOnLevel = RX_EN_ACTIVE_HIGH ? HIGH : LOW;
constexpr uint8_t kRxOffLevel = RX_EN_ACTIVE_HIGH ? LOW : HIGH;

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

void enableRxPath() {
  digitalWrite(PIN_RX_EN, kRxOnLevel);
}

void disableRxPath() {
  digitalWrite(PIN_RX_EN, kRxOffLevel);
}

void sendBurst40kHz(int cycles) {
  const uint32_t halfPeriodUs = 12;
  for (int i = 0; i < cycles; ++i) {
    digitalWrite(PIN_TX_PWM, kTxOnLevel);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(PIN_TX_PWM, kTxOffLevel);
    delayMicroseconds(halfPeriodUs);
  }
}

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

bool runSingleTest(char txDir, char rxDir, bool relState, bool drvState,
                   int32_t* outTofUs = nullptr, int32_t* outEdgeAfterArmUs = nullptr) {
  setRxDirection(rxDir);
  disableRxPath();
  setTxCombo(txDir, relState, drvState);

  g_captureArmed = false;
  delay(2);

  resetEdgeCapture();

  sendBurst40kHz(BURST_CYCLES);
  const uint32_t txDoneUs = micros();

  delayMicroseconds(g_blankingUs);

  // Start listening only after blanking to suppress early TX feedthrough/ring-down.
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
  return detected;
}

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

void runRxDisabledBaseline(uint32_t windowUs, int repeats) {
  if (repeats > NOISE_REPEATS) {
    repeats = NOISE_REPEATS;
  }

  Serial.print("==== RX-DISABLED BASELINE START WINDOW_US=");
  Serial.print(windowUs);
  Serial.print(" REPEATS=");
  Serial.print(repeats);
  Serial.println(" ====");

  int32_t counts[NOISE_REPEATS];
  int32_t totalCount = 0;

  for (int i = 0; i < repeats; ++i) {
    clearTxSelects();
    disableRxPath();
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

void runCouplingRound(const char* label) {
  Serial.print("==== COUPLING ROUND START LABEL=");
  Serial.print(label);
  Serial.println(" ====");
  Serial.println("Reminder: disconnect RX transducer cable for electrical-coupling check.");

  runNoiseBaseline(NOISE_WINDOW_US, NOISE_REPEATS);
  scoreCouplingCombo('N', 'S', false, false);
  scoreCouplingCombo('N', 'S', true, false);
  scoreCouplingCombo('N', 'S', false, true);
  scoreCouplingCombo('N', 'S', true, true);

  Serial.print("---- COUPLING ROUND DONE LABEL=");
  Serial.print(label);
  Serial.println(" ----");
}

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
    digitalWrite(PIN_TX_PWM, kTxOffLevel);
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
          case 1:
            digitalWrite(PIN_RX_EN, kRxOffLevel);
            delayMicroseconds(20);
            digitalWrite(PIN_RX_EN, kRxOnLevel);
            break;
          case 2:
            setRxDirection('N');
            delayMicroseconds(20);
            setRxDirection('S');
            break;
          case 3:
            setTxCombo('N', true, true);
            delayMicroseconds(20);
            clearTxSelects();
            break;
          case 4:
            sendBurst40kHz(BURST_CYCLES);
            break;
          case 5:
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
  runAggressorScenario("RX_EN_TOGGLE", 1);
  runAggressorScenario("MUX_SWITCH", 2);
  runAggressorScenario("DRVREL_SWITCH", 3);
  runAggressorScenario("TX_PWM_ONLY", 4);
  runAggressorScenario("TX_PWM_WITH_ROUTE", 5);
  Serial.println("---- AGGRESSOR MATRIX DONE ----");
}

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

char toUpperAscii(char c) {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

void printCommandHelp() {
  Serial.println("Commands: O=open-path, B=blocked-path, C=coupling, N=noise baseline, D=RX-disabled baseline, P=paired axis, A=aggressor matrix, S=blanking/guard sweep, R=manual, H=help");
}

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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Ultrasonic First Test (route-finder) ===");

  pinMode(PIN_TOF_EDGE, INPUT);
  pinMode(PIN_RX_EN, OUTPUT);
  pinMode(PIN_MUX_A, OUTPUT);
  pinMode(PIN_MUX_B, OUTPUT);

  pinMode(PIN_DRV_N, OUTPUT);
  pinMode(PIN_DRV_E, OUTPUT);
  pinMode(PIN_DRV_S, OUTPUT);
  pinMode(PIN_DRV_W, OUTPUT);

  pinMode(PIN_REL_N, OUTPUT);
  pinMode(PIN_REL_E, OUTPUT);
  pinMode(PIN_REL_S, OUTPUT);
  pinMode(PIN_REL_W, OUTPUT);

  pinMode(PIN_TX_PWM, OUTPUT);

  clearTxSelects();
  disableRxPath();
  digitalWrite(PIN_TX_PWM, kTxOffLevel);

  attachInterrupt(digitalPinToInterrupt(PIN_TOF_EDGE), onTofEdge, RISING);

  Serial.printf("TOF_EDGE=%d RX_EN=%d MUX_A=%d MUX_B=%d TX_PWM=%d\n", PIN_TOF_EDGE, PIN_RX_EN, PIN_MUX_A, PIN_MUX_B, PIN_TX_PWM);
  Serial.printf("DRV[N,E,S,W]=[%d,%d,%d,%d] REL[N,E,S,W]=[%d,%d,%d,%d]\n", PIN_DRV_N, PIN_DRV_E, PIN_DRV_S, PIN_DRV_W, PIN_REL_N, PIN_REL_E, PIN_REL_S, PIN_REL_W);
  Serial.printf("BURST_CYCLES=%d BLANKING_US=%d TIMEOUT_US=%d\n", BURST_CYCLES, BLANKING_US, TOF_TIMEOUT_US);
  Serial.printf("MIN_VALID_TOF_US=%d WARMUP_SHOTS=%d SCORE_SHOTS=%d\n", MIN_VALID_TOF_US, WARMUP_SHOTS, SCORE_SHOTS);
  Serial.printf("NOISE_WINDOW_US=%d NOISE_REPEATS=%d COUPLING_SHOTS=%d\n", NOISE_WINDOW_US, NOISE_REPEATS, COUPLING_SHOTS);
  Serial.printf("POST_ENABLE_GUARD_US=%d AGG_WINDOW_US=%d AGG_REPEATS=%d SWEEP_SHOTS=%d\n",
                POST_ENABLE_GUARD_US, AGG_WINDOW_US, AGG_REPEATS, SWEEP_SHOTS);
  printCommandHelp();
  Serial.println("Type N for RX-noise baseline, D for RX-disabled baseline, C for TX-coupling (RX transducer unplugged).");
  Serial.println("Use A to rank digital aggressors and S to sweep blanking/guard/min-tof.");
  Serial.println("Then run O/B/P for acoustic discrimination checks.");
}

void loop() {
  if (Serial.available() <= 0) {
    delay(20);
    return;
  }

  char cmd = static_cast<char>(Serial.read());
  cmd = toUpperAscii(cmd);

  if (cmd == 'O') {
    runRouteFinderRound("OPEN");
  } else if (cmd == 'B') {
    runRouteFinderRound("BLOCKED");
  } else if (cmd == 'C') {
    runCouplingRound("COUPLING");
  } else if (cmd == 'N') {
    runNoiseBaseline(NOISE_WINDOW_US, NOISE_REPEATS);
  } else if (cmd == 'D') {
    runRxDisabledBaseline(NOISE_WINDOW_US, NOISE_REPEATS);
  } else if (cmd == 'P') {
    runPairedAxisRound("PAIR_NS", 'N', 'S');
  } else if (cmd == 'A') {
    runAggressorRound();
  } else if (cmd == 'S') {
    runSweepRound();
  } else if (cmd == 'R') {
    runRouteFinderRound("MANUAL");
  } else if (cmd == 'H') {
    printCommandHelp();
  }
}
