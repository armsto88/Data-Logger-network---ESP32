// FieldHub v2 r6 bring-up: reed / cup-anemometer input (Stage H step 6).
//
// r6 topology (netlist-verified):
//   reed connector (BOM ref "edaimagecopyoccupy", B2B-PH-SM4): 1/3/4 = GND,
//   2 = REED_SIG. D15 ESDS311 to GND at the connector, then R8 1k into the
//   GPIO36 node, which has R9 10k to 3V3_SYS and C2 100 nF to GND.
//   Open contact -> ~3.3 V (HIGH); closed -> 3.3 * 1k/11k = 0.3 V (LOW).
//   GPIO36 is input-only with no internal pull, so R9 is the only pull-up.
//
// ESP32 erratum: GPIO36/39 can see a short low glitch when ADC1/ADC2 or the
// Wi-Fi RF front end powers up. The battery ADC (GPIO34) is on ADC1, so the
// immunity checks below look for false falling edges with the contact open
// while ADC1 and Wi-Fi are exercised. A count there is false wind.
//
// Calibration: WH-SP-WS01, one closure per revolution, 0.6667 m/s per Hz.
//
// Serial commands after the run:
//   r       full suite (automatic checks, then operator steps)
//   a       automatic checks only (no operator)
//   n <N>   exact count: make exactly N closures by hand, then send Enter
//   c       continuous 1 s counts (Enter stops)
//
// Build: pio run -e mothership-v2-bringup-reed -t upload
// Run:   scripts/hub_bringup.py run <BOARD> reed

#include <Arduino.h>
#include <WiFi.h>

#include "hub_bringup_report.h"

#ifndef PIN_REED
#define PIN_REED 36
#endif
#ifndef REED_DEBOUNCE_US
#define REED_DEBOUNCE_US 5000
#endif
#ifndef REED_WIND_FACTOR
#define REED_WIND_FACTOR 0.6667f
#endif
#ifndef REED_OPERATOR_TIMEOUT_MS
#define REED_OPERATOR_TIMEOUT_MS 30000
#endif
#ifndef REED_SPIN_WINDOW_MS
#define REED_SPIN_WINDOW_MS 10000
#endif

static volatile uint32_t gRawEdges = 0;
static volatile uint32_t gCounted = 0;
static volatile uint32_t gLastRawUs = 0;
static volatile uint32_t gLastCountedUs = 0;
static volatile uint32_t gMinRawGapUs = UINT32_MAX;

static void IRAM_ATTR onReedFalling() {
  const uint32_t now = micros();
  if (gRawEdges) {
    const uint32_t gap = now - gLastRawUs;
    if (gap < gMinRawGapUs) gMinRawGapUs = gap;
  }
  gLastRawUs = now;
  gRawEdges++;
  if (gCounted == 0 || now - gLastCountedUs >= REED_DEBOUNCE_US) {
    gLastCountedUs = now;
    gCounted++;
  }
}

static void resetCounts() {
  noInterrupts();
  gRawEdges = gCounted = 0;
  gMinRawGapUs = UINT32_MAX;
  interrupts();
}

static bool levelStable(int level, uint32_t forMs) {
  const uint32_t t0 = millis();
  while (millis() - t0 < forMs) {
    if (digitalRead(PIN_REED) != level) return false;
    delay(1);
  }
  return true;
}

static bool runAutomatic() {
  const bool idleHigh = levelStable(HIGH, 500);
  HUB_CHECK("REED_IDLE_HIGH", idleHigh, "GPIO36=%d over 500 ms (R9 pull-up; contact must be open)",
            digitalRead(PIN_REED));
  if (!idleHigh) {
    HUB_WARN("REED_IMMUNITY_SKIPPED", "input not idle-high; open the contact and re-run");
    return false;
  }

  resetCounts();
  uint32_t sink = 0;
  for (int i = 0; i < 2000; i++) {
    sink += analogRead(PIN_BATTERY_ADC);
    sink += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  HUB_CHECK("REED_ADC1_IMMUNITY", gRawEdges == 0,
            "%lu false edges during 4000 ADC1 reads on GPIO34 (checksum %lu)", gRawEdges, sink & 0xFF);

  resetCounts();
  for (int i = 0; i < 5; i++) {
    WiFi.mode(WIFI_STA);
    delay(300);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
  HUB_CHECK("REED_WIFI_IMMUNITY", gRawEdges == 0, "%lu false edges over 5 Wi-Fi on/off cycles", gRawEdges);
  return true;
}

static void runOperator() {
  const bool closed = hubPromptUntil("short reed pin 2 to pin 1 (GND), or turn the cups until the contact closes",
                                     REED_OPERATOR_TIMEOUT_MS, [] { return levelStable(LOW, 20); });
  HUB_CHECK("REED_CLOSE_DETECT", closed, "GPIO36 read LOW for 20 ms: %s", closed ? "yes" : "timed out");
  if (closed) {
    const bool released = hubPromptUntil("release the contact", REED_OPERATOR_TIMEOUT_MS,
                                         [] { return levelStable(HIGH, 20); });
    HUB_CHECK("REED_RELEASE_DETECT", released, "GPIO36 back HIGH for 20 ms: %s",
              released ? "yes" : "timed out");
  }

  Serial.printf("ACTION: spin the cups now (%u s window)\n", REED_SPIN_WINDOW_MS / 1000);
  delay(1500);
  resetCounts();
  delay(REED_SPIN_WINDOW_MS);
  const uint32_t counted = gCounted, raw = gRawEdges, minGap = gMinRawGapUs;
  const float hz = counted / (REED_SPIN_WINDOW_MS / 1000.0f);
  HUB_CHECK("REED_SPIN", counted >= 5,
            "%lu counted (%lu raw edges, %lu bounce), %.2f Hz = %.2f m/s, min raw gap %lu us",
            counted, raw, raw - counted, hz, hz * REED_WIND_FACTOR, minGap == UINT32_MAX ? 0 : minGap);
}

static void runExactCount(long expected) {
  hubResetCounts();
  resetCounts();
  Serial.printf("ACTION: make exactly %ld closures, then send Enter\n", expected);
  while (!Serial.available()) delay(10);
  while (Serial.available()) Serial.read();
  char name[32];
  snprintf(name, sizeof(name), "REED_COUNT_%ld", expected);
  HUB_CHECK(name, (long)gCounted == expected, "counted %lu of %ld (%lu raw edges)", gCounted, expected,
            gRawEdges);
  hubSummary();
}

static void continuousLoop() {
  Serial.println("Counts per second (Enter stops)");
  while (!Serial.available()) {
    resetCounts();
    delay(1000);
    Serial.printf("  t=%lus %lu Hz (%lu raw) = %.2f m/s, level %d\n", millis() / 1000, gCounted, gRawEdges,
                  gCounted * REED_WIND_FACTOR, digitalRead(PIN_REED));
  }
  while (Serial.available()) Serial.read();
}

static void printHelp() {
  Serial.println("Commands: r = full suite | a = automatic only | n <N> = exact count | c = continuous");
}

void setup() {
  hubHoldPower();
  Serial.begin(115200);
  delay(300);
  hubBanner("reed / anemometer input");
  pinMode(PIN_REED, INPUT);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  attachInterrupt(digitalPinToInterrupt(PIN_REED), onReedFalling, FALLING);
  Serial.printf("REED=GPIO%d (R8 1k, R9 10k pull-up, C2 100nF), debounce %u us\n", PIN_REED,
                REED_DEBOUNCE_US);
  hubWaitForStart();
  hubResetCounts();
  if (runAutomatic()) runOperator();
  hubSummary();
  printHelp();
}

void loop() {
  char line[32];
  if (!hubReadLine(line, sizeof(line))) {
    delay(10);
    return;
  }
  if (line[0] == 'r' || line[0] == 'a') {
    hubResetCounts();
    if (runAutomatic() && line[0] == 'r') runOperator();
    hubSummary();
  } else if (line[0] == 'n') {
    const long n = atol(line + 1);
    if (n > 0) runExactCount(n);
    else Serial.println("n <N>: N > 0");
  } else if (line[0] == 'c') {
    continuousLoop();
  } else if (line[0] != '\0') {
    printHelp();
  }
}
