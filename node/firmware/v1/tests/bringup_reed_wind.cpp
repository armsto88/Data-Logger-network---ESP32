/*
 * Reed Wind Anemometer Bring-up — WH-SP-WS01 Cup Anemometer
 *
 * Tests the WH-SP-WS01 reed-switch cup anemometer on the Node V2
 * AUX WIND input (J52).
 *
 * HARDWARE SETUP
 * ───────────────
 * Connector:  J52 / AUX WIND
 *   Pin 1 = GND
 *   Pin 2 = REED_SIG
 *   Pin 3 = 3V3_SYS
 *
 * Anemometer: WH-SP-WS01 — cup anemometer with reed switch.
 *   Two wires: signal (to REED_SIG) and ground (to GND).
 *   One pulse per revolution (falling edge = reed closes).
 *
 * Signal path: REED_SIG → solder jumper → GPIO4 (shared with RX_EN_N)
 *
 *   IMPORTANT: The solder jumper on the Node V2 PCB must be CLOSED
 *   to connect REED_SIG to GPIO4. If the jumper is open, no signal
 *   will reach the MCU.
 *
 * Pull-up: The existing RX_EN_N pull-up on GPIO4 doubles as the
 *   reed pull-up. GPIO4 is pulled HIGH by default and pulled LOW
 *   when the reed switch closes.
 *
 * Polarity: Falling edge = reed switch closes (magnet passes).d
 *   One falling edge per revolution.
 *
 * Safety constraints (reed mode):
 *   - TX_22V_EN_N (GPIO5) must be held HIGH → boost OFF
 *   - TX_BURST_PWM (GPIO25) must be held LOW → no burst
 *
 * WH-SP-WS01 CALIBRATION
 * ──────────────────────
 *   V = 0.085 × f + 0.1   (m/s, f in Hz)
 *   V_kmh = V_ms × 3.6    (km/h)
 *
 *   Starting threshold: ~0.5–0.8 m/s
 *   Max measurable:     ~30 m/s
 *
 * DEBOUNCE
 * ────────
 *   Reed switches bounce for 1–5 ms. A 5 ms debounce window in the
 *   ISR rejects spurious edges. At 30 m/s the pulse rate is ~350 Hz
 *   (2.9 ms period), so 5 ms debounce is safe for speeds up to
 *   ~25 m/s. Above that, some pulses may be suppressed.
 *
 * Menu:
 *   ? / h = Print menu
 *   c     = Continuous mode (1 Hz updates, any key to stop)
 *   s     = Single 5-second sample
 *   t <n> = Timed sample for <n> seconds (1–60)
 *   r     = Reset edge counter
 *   p     = Print pin config and calibration info
 *   b     = Read battery voltage (ADC on GPIO35)
 *   d     = GPIO diagnostic (read GPIO4 state continuously)
 *   g     = Manually inject an edge (for testing ISR)
 *   q     = Quit (release PWR_HOLD, deep sleep)
 */

#include <Arduino.h>
#include <esp_sleep.h>

// ── Pin definitions (all #ifndef guarded for build-flag override) ──────────

#ifndef REED_SIG_PIN
#define REED_SIG_PIN 4       // GPIO4, shared with RX_EN_N
#endif

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif

#ifndef TX_22V_EN_N_PIN
#define TX_22V_EN_N_PIN 5    // Active-LOW enable for 22V boost; HIGH = OFF
#endif

#ifndef TX_BURST_PWM_PIN
#define TX_BURST_PWM_PIN 25  // PWM burst gate; LOW = no burst
#endif

#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 35
#endif

#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF 3.3f
#endif

#ifndef BAT_DIVIDER_SCALE
#define BAT_DIVIDER_SCALE 3.62f
#endif

#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES 32
#endif

// ── Calibration constants ─────────────────────────────────────────────────

#ifndef REED_WIND_FACTOR
#define REED_WIND_FACTOR 0.085f   // m/s per Hz
#endif

#ifndef REED_WIND_OFFSET
#define REED_WIND_OFFSET 0.1f     // m/s offset
#endif

#ifndef REED_DEBOUNCE_MS
#define REED_DEBOUNCE_MS 5        // ms debounce window
#endif

// ── Global state ───────────────────────────────────────────────────────────

static volatile uint32_t g_edgeCount = 0;
static volatile unsigned long g_lastEdgeMs = 0;

// ── ISR: reed switch falling edge ─────────────────────────────────────────

static void IRAM_ATTR onReedFalling() {
  unsigned long now = millis();
  if (now - g_lastEdgeMs >= REED_DEBOUNCE_MS) {
    g_lastEdgeMs = now;
    g_edgeCount++;
  }
}

// ── Helpers ────────────────────────────────────────────────────────────────

static float calcWindSpeed(float freqHz) {
  return REED_WIND_FACTOR * freqHz + REED_WIND_OFFSET;
}

static float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sum += analogRead(BAT_ADC_PIN);
  }
  float avg = (float)sum / BAT_ADC_SAMPLES;
  float vAdc = avg / 4095.0f * BAT_ADC_VREF;
  return vAdc * BAT_DIVIDER_SCALE;
}

static void printMenu() {
  Serial.println();
  Serial.println("=== WH-SP-WS01 Reed Anemometer Bring-up ===");
  Serial.printf("Pins: REED_SIG=%d  PWR_HOLD=%d  TX_22V_EN_N=%d"
                 "  TX_BURST_PWM=%d  BAT_ADC=%d\n",
                 REED_SIG_PIN, PWR_HOLD_PIN, TX_22V_EN_N_PIN,
                 TX_BURST_PWM_PIN, BAT_ADC_PIN);
  Serial.printf("Cal: V = %.3f × f + %.1f  (m/s, f in Hz)\n",
                 REED_WIND_FACTOR, REED_WIND_OFFSET);
  Serial.printf("Debounce: %d ms\n", REED_DEBOUNCE_MS);
  Serial.println();
  Serial.println("  ? / h  Print this menu");
  Serial.println("  c      Continuous mode (1 Hz, any key to stop)");
  Serial.println("  s      Single 5-second sample");
  Serial.println("  t <n>  Timed sample for <n> seconds (1-60)");
  Serial.println("  r      Reset edge counter");
  Serial.println("  p      Print pin config and calibration info");
  Serial.println("  b      Read battery voltage");
  Serial.println("  d      GPIO diagnostic (read GPIO4 state continuously)");
  Serial.println("  g      Manually inject an edge (for testing ISR)");
  Serial.println("  q      Quit (release PWR_HOLD, deep sleep)");
  Serial.println();
}

static void printPinConfig() {
  Serial.println("--- Pin configuration ---");
  Serial.printf("  REED_SIG      = GPIO%d\n", REED_SIG_PIN);
  Serial.printf("  PWR_HOLD      = GPIO%d\n", PWR_HOLD_PIN);
  Serial.printf("  TX_22V_EN_N   = GPIO%d (HIGH = boost OFF)\n", TX_22V_EN_N_PIN);
  Serial.printf("  TX_BURST_PWM  = GPIO%d (LOW = no burst)\n", TX_BURST_PWM_PIN);
  Serial.printf("  BAT_ADC       = GPIO%d\n", BAT_ADC_PIN);
  Serial.println();
  Serial.println("--- Calibration ---");
  Serial.printf("  V = %.3f × f + %.1f  m/s\n", REED_WIND_FACTOR, REED_WIND_OFFSET);
  Serial.printf("  V_kmh = V_ms × 3.6\n");
  Serial.printf("  Debounce: %d ms\n", REED_DEBOUNCE_MS);
  Serial.println();
  Serial.println("--- Hardware notes ---");
  Serial.println("  J52 AUX WIND: Pin1=GND  Pin2=REED_SIG  Pin3=3V3_SYS");
  Serial.println("  Solder jumper MUST be closed to connect REED_SIG to GPIO4");
  Serial.println("  RX_EN_N pull-up on GPIO4 doubles as reed pull-up");
  Serial.println("  Falling edge = reed closes (magnet passes)");
  Serial.println();
}

// ── Sample helpers ─────────────────────────────────────────────────────────

static uint32_t resetAndStartCounting() {
  noInterrupts();
  g_edgeCount = 0;
  g_lastEdgeMs = 0;
  interrupts();
  return millis();
}

static void reportSample(uint32_t edges, unsigned long durationMs) {
  float durationS = durationMs / 1000.0f;
  float freqHz = (durationS > 0.0f) ? (edges / durationS) : 0.0f;
  float windMs = calcWindSpeed(freqHz);
  float windKmh = windMs * 3.6f;

  Serial.printf("  Edges: %lu   Duration: %.1f s   Freq: %.2f Hz"
                 "   Wind: %.2f m/s (%.1f km/h)\n",
                 (unsigned long)edges, durationS, freqHz, windMs, windKmh);
}

static void continuousMode() {
  Serial.println("Continuous mode — printing every second. Press any key to stop.");
  Serial.println("  Time(s)  Edges  Freq(Hz)  Wind(m/s)  Wind(km/h)");
  Serial.println("  ------   -----  --------  ---------  ---------");

  unsigned long startTime = millis();
  uint32_t lastEdges = 0;

  while (true) {
    delay(1000);

    noInterrupts();
    uint32_t currentEdges = g_edgeCount;
    interrupts();

    uint32_t deltaEdges = currentEdges - lastEdges;
    lastEdges = currentEdges;

    float elapsed = (millis() - startTime) / 1000.0f;
    float freqHz = deltaEdges;  // edges in 1 s = Hz
    float windMs = calcWindSpeed(freqHz);
    float windKmh = windMs * 3.6f;

    Serial.printf("  %6.1f   %5lu   %7.2f   %8.2f    %8.1f\n",
                   elapsed, (unsigned long)deltaEdges, freqHz, windMs, windKmh);

    if (Serial.available()) {
      Serial.readString();  // discard input
      Serial.println("Continuous mode stopped.");
      break;
    }
  }
}

static void timedSample(unsigned long seconds) {
  Serial.printf("Counting edges for %lu second(s)...\n", seconds);
  uint32_t startMs = resetAndStartCounting();

  // Wait for the sample duration
  unsigned long sampleMs = seconds * 1000;
  while (millis() - startMs < sampleMs) {
    delay(100);
  }

  unsigned long elapsedMs = millis() - startMs;

  noInterrupts();
  uint32_t edges = g_edgeCount;
  interrupts();

  reportSample(edges, elapsedMs);
}

// ── GPIO diagnostic ────────────────────────────────────────────────────────

static void gpioDiagnostic() {
  Serial.println("GPIO Diagnostic — reading GPIO4 state every 100ms.");
  Serial.println("  Watch for state changes when you spin the anemometer.");
  Serial.println("  If state never changes, the signal may not be reaching GPIO4.");
  Serial.println("  Try shorting J52 Pin2 (REED_SIG) to J52 Pin1 (GND).");
  Serial.println("  Press any key to stop.");
  Serial.printf("  Initial GPIO4 state: %d (expect 1=HIGH)\n", digitalRead(REED_SIG_PIN));
  Serial.println();
  Serial.println("  Time(ms)   GPIO4   Edges");
  Serial.println("  --------   -----   ------");

  uint32_t lastEdges = g_edgeCount;

  while (true) {
    int pinState = digitalRead(REED_SIG_PIN);

    noInterrupts();
    uint32_t currentEdges = g_edgeCount;
    interrupts();

    uint32_t deltaEdges = currentEdges - lastEdges;
    lastEdges = currentEdges;

    Serial.printf("  %8lu     %d     %lu\n", millis(), pinState, (unsigned long)deltaEdges);

    if (Serial.available()) {
      Serial.readString();
      Serial.println("Diagnostic stopped.");
      break;
    }
    delay(100);
  }
}

// ── Setup ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  // Latch power
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);

  // Safety: hold boost OFF and burst OFF for reed mode
  pinMode(TX_22V_EN_N_PIN, OUTPUT);
  digitalWrite(TX_22V_EN_N_PIN, HIGH);   // HIGH = boost OFF (active-low enable)

  pinMode(TX_BURST_PWM_PIN, OUTPUT);
  digitalWrite(TX_BURST_PWM_PIN, LOW);    // LOW = no burst

  // Configure reed input with interrupt
  // Use INPUT_PULLUP to ensure a defined HIGH state even if the external
  // pull-up is weak or disconnected. The 15k external pull-up on RX_EN_N
  // provides the primary pull-up; the internal ~45k pull-up adds redundancy.
  // The WH-SP-WS01 reed switch pulls LOW when closed.
  pinMode(REED_SIG_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_SIG_PIN), onReedFalling, FALLING);

  Serial.println();
  Serial.println("=== WH-SP-WS01 Reed Anemometer Bring-up ===");
  Serial.println();
  Serial.println("NOTE: Solder jumper on J52 AUX WIND must be CLOSED");
  Serial.println("      to connect REED_SIG to GPIO4.");
  Serial.println("      If no edges are detected, check the jumper!");
  Serial.println();

  printMenu();
}

// ── Loop ───────────────────────────────────────────────────────────────────

void loop() {
  if (!Serial.available()) {
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char cmd = line.charAt(0);

  switch (cmd) {
    case '?':
    case 'h':
      printMenu();
      break;

    case 'c':
      continuousMode();
      break;

    case 's':
      timedSample(5);
      break;

    case 't': {
      // Parse optional seconds argument
      int spaceIdx = line.indexOf(' ');
      unsigned long secs = 5;
      if (spaceIdx > 0) {
        secs = constrain(line.substring(spaceIdx + 1).toInt(), 1, 60);
      }
      timedSample(secs);
      break;
    }

    case 'r':
      noInterrupts();
      g_edgeCount = 0;
      g_lastEdgeMs = 0;
      interrupts();
      Serial.println("Edge counter reset.");
      break;

    case 'p':
      printPinConfig();
      break;

    case 'b': {
      float vBat = readBatteryVoltage();
      Serial.printf("Battery: %.2f V\n", vBat);
      break;
    }

    case 'd':
      gpioDiagnostic();
      break;

    case 'g': {
      Serial.println("Injecting test edge via ISR...");
      onReedFalling();
      noInterrupts();
      uint32_t edges = g_edgeCount;
      interrupts();
      Serial.printf("Edge count after inject: %lu\n", (unsigned long)edges);
      break;
    }

    case 'q':
      Serial.println("Releasing PWR_HOLD and entering deep sleep...");
      Serial.flush();
      delay(100);
      digitalWrite(PWR_HOLD_PIN, LOW);
      esp_deep_sleep_start();
      break;

    default:
      Serial.printf("Unknown command '%c'. Press ? for menu.\n", cmd);
      break;
  }
}