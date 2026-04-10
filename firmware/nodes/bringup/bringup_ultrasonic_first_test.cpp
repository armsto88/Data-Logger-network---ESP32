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

#if PIN_TX_PWM == PIN_DRV_N || PIN_TX_PWM == PIN_DRV_E || PIN_TX_PWM == PIN_DRV_S || PIN_TX_PWM == PIN_DRV_W
#error "PIN_TX_PWM conflicts with one of DRV_* pins. Set PIN_TX_PWM to the dedicated TX enable GPIO."
#endif

namespace {

volatile bool g_tofSeen = false;
volatile uint32_t g_tofEdgeUs = 0;

constexpr uint8_t kTxOnLevel = TX_PWM_ACTIVE_HIGH ? HIGH : LOW;
constexpr uint8_t kTxOffLevel = TX_PWM_ACTIVE_HIGH ? LOW : HIGH;
constexpr uint8_t kRxOnLevel = RX_EN_ACTIVE_HIGH ? HIGH : LOW;
constexpr uint8_t kRxOffLevel = RX_EN_ACTIVE_HIGH ? LOW : HIGH;

void IRAM_ATTR onTofEdge() {
  if (!g_tofSeen) {
    g_tofSeen = true;
    g_tofEdgeUs = micros();
  }
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

void printResult(char txDir, char rxDir, bool relState, bool drvState, bool detected, int32_t tofUs) {
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
  Serial.print(" BLANK_US=");
  Serial.print(BLANKING_US);
  Serial.print(" TIMEOUT_US=");
  Serial.println(TOF_TIMEOUT_US);
}

bool runSingleTest(char txDir, char rxDir, bool relState, bool drvState, int32_t* outTofUs = nullptr) {
  setRxDirection(rxDir);
  disableRxPath();
  setTxCombo(txDir, relState, drvState);

  delay(2);

  g_tofSeen = false;
  g_tofEdgeUs = 0;

  sendBurst40kHz(BURST_CYCLES);
  const uint32_t txDoneUs = micros();

  delayMicroseconds(BLANKING_US);

  // Start listening only after blanking to suppress early TX feedthrough/ring-down.
  enableRxPath();
  delayMicroseconds(20);

  g_tofSeen = false;
  g_tofEdgeUs = 0;
  const uint32_t listenStartUs = micros();

  bool detected = false;
  int32_t tofUs = -1;

  while ((micros() - listenStartUs) < TOF_TIMEOUT_US) {
    if (g_tofSeen) {
      const uint32_t edgeUs = g_tofEdgeUs;
      g_tofSeen = false;
      tofUs = static_cast<int32_t>(edgeUs - txDoneUs);

      if (tofUs >= MIN_VALID_TOF_US) {
        detected = true;
        break;
      }
    }
  }

#if VERBOSE_SHOTS
  printResult(txDir, rxDir, relState, drvState, detected, tofUs);
#endif

  if (outTofUs != nullptr) {
    *outTofUs = tofUs;
  }

  clearTxSelects();
  disableRxPath();
  return detected;
}

void runListenOnly(uint32_t windowUs) {
  clearTxSelects();
  enableRxPath();
  g_tofSeen = false;
  g_tofEdgeUs = 0;

  const uint32_t t0 = micros();
  while ((micros() - t0) < windowUs) {
    if (g_tofSeen) {
      break;
    }
  }

  Serial.print("MODE=listen_only WINDOW_US=");
  Serial.print(windowUs);
  Serial.print(" EVENT=");
  Serial.print(g_tofSeen ? 1 : 0);
  Serial.print(" EDGE_US=");
  Serial.println(g_tofSeen ? static_cast<int32_t>(g_tofEdgeUs - t0) : -1);
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
    runSingleTest(txDir, rxDir, relState, drvState, &dummyTof);
    delay(INTER_SHOT_MS);
  }

  int detectedCount = 0;
  int32_t tofs[SCORE_SHOTS];

  for (int i = 0; i < SCORE_SHOTS; ++i) {
    int32_t tofUs = -1;
    const bool detected = runSingleTest(txDir, rxDir, relState, drvState, &tofUs);
    if (detected && detectedCount < SCORE_SHOTS) {
      tofs[detectedCount++] = tofUs;
    }
    delay(INTER_SHOT_MS);
  }

  int32_t medianTof = -1;
  int32_t minTof = -1;
  int32_t maxTof = -1;
  int32_t jitter = -1;

  if (detectedCount > 0) {
    sortInt32(tofs, detectedCount);
    minTof = tofs[0];
    maxTof = tofs[detectedCount - 1];
    medianTof = tofs[detectedCount / 2];
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
  Serial.print(" JITTER_US=");
  Serial.print(jitter);
  Serial.print(" MIN_US=");
  Serial.print(minTof);
  Serial.print(" MAX_US=");
  Serial.println(maxTof);
}

char toUpperAscii(char c) {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

void printCommandHelp() {
  Serial.println("Commands: O=open-path round, B=blocked-path round, R=generic round, H=help");
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
  printCommandHelp();
  Serial.println("Type O then Enter for OPEN path. Place blocker, then type B for BLOCKED path.");
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
  } else if (cmd == 'R') {
    runRouteFinderRound("MANUAL");
  } else if (cmd == 'H') {
    printCommandHelp();
  }
}
