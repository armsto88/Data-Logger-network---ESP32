/*
 * Node V3 Low-Voltage Bring-up (Phase 2)
 *
 * Verify 3V3, VREF, filtered rails, and VREF settling time.
 * 22V boost is NEVER enabled in this sketch.
 * No TX bursts, no DRV/REL pins driven.
 *
 * Use with DMM to probe:
 *   - 3V3_COMP (comparator supply)
 *   - 3V3_RXAMP (RX amplifier supply)
 *   - 3V3_MUX (mux supply)
 *   - VREF (reference voltage)
 */

#include <Arduino.h>

#ifdef DISABLE_BROWNOUT
#include "soc/rtc_cntl_reg.h"
#endif

// V3 shared headers — single source of truth
#include "v3_ultrasonic_pins.h"
#include "v3_ultrasonic_safe.h"
#include "v3_ultrasonic_burst.h"
#include "v3_ultrasonic_direction.h"
#include "v3_ultrasonic_capture.h"

// ---------------------------------------------------------------------------
// Pin-state table — name, GPIO, mode (at init time)
// ---------------------------------------------------------------------------
struct PinInfo {
  const char* name;
  uint8_t gpio;
  uint8_t mode;     // OUTPUT or INPUT
};

static const PinInfo pinTable[] = {
  {"TOF_EDGE",      PIN_TOF_EDGE,      INPUT},
  {"RX_EN_N",       PIN_RX_EN_N,       OUTPUT},
  {"MUX_A",         PIN_MUX_A,         OUTPUT},
  {"MUX_B",         PIN_MUX_B,         OUTPUT},
  {"DRV_N",         PIN_DRV_N,         OUTPUT},
  {"DRV_E",         PIN_DRV_E,         OUTPUT},
  {"DRV_S",         PIN_DRV_S,         OUTPUT},
  {"DRV_W",         PIN_DRV_W,         OUTPUT},
  {"REL_N",         PIN_REL_N,         OUTPUT},
  {"REL_E",         PIN_REL_E,         OUTPUT},
  {"REL_S",         PIN_REL_S,         OUTPUT},
  {"REL_W",         PIN_REL_W,         OUTPUT},
  {"TX_BURST_PWM",  PIN_TX_BURST_PWM,  OUTPUT},
  {"TX_22V_EN_N",  PIN_TX_22V_EN_N,   OUTPUT},
  {"PWR_HOLD",      PIN_PWR_HOLD,      OUTPUT},
};

static const int PIN_COUNT = sizeof(pinTable) / sizeof(pinTable[0]);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool g_rxEnabled = false;   // tracks RX_EN_N state (true = enabled)

// ---------------------------------------------------------------------------
// Print all pin states
// ---------------------------------------------------------------------------
static void printPinStates() {
  Serial.println("---- Pin States ----");
  Serial.println("  NAME            GPIO  LEVEL  MODE");
  for (int i = 0; i < PIN_COUNT; i++) {
    int level = digitalRead(pinTable[i].gpio);
    const char* modeStr = (pinTable[i].mode == OUTPUT) ? "OUT" : "IN ";
    // Pad name to 16 chars
    char buf[20];
    snprintf(buf, sizeof(buf), "%-16s", pinTable[i].name);
    Serial.print("  ");
    Serial.print(buf);
    Serial.print(pinTable[i].gpio < 10 ? " " : "");
    Serial.print(pinTable[i].gpio);
    Serial.print("    ");
    Serial.print(level);
    Serial.print("    ");
    Serial.println(modeStr);
  }
  Serial.println("--------------------");
}

// ---------------------------------------------------------------------------
// VREF settle test
// Enable RX path, count TOF_EDGE edges in 10 consecutive 100ms windows,
// print results, disable RX path.
// ---------------------------------------------------------------------------
static void measureVrefSettle() {
  Serial.println("---- VREF Settle Measurement ----");
  Serial.println("Enabling RX path (RX_EN_N=LOW)...");
  enableRxPath();

  // Install ISR if not already done
  v3_capture::installTofEdgeIsr();

  const int WINDOWS = 10;
  const uint32_t WINDOW_MS = 100;
  uint32_t counts[WINDOWS];

  for (int i = 0; i < WINDOWS; i++) {
    v3_capture::resetEdgeCapture();
    v3_capture::armCapture();

    const uint32_t t0 = millis();
    while ((millis() - t0) < WINDOW_MS) {
      // busy-wait
    }

    v3_capture::disarmCapture();
    counts[i] = v3_capture::g_edgeCount;

    Serial.print("  window ");
    if (i < 9) Serial.print(" ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(counts[i]);
    Serial.println(" edges");
  }

  // Summary
  uint32_t total = 0;
  for (int i = 0; i < WINDOWS; i++) {
    total += counts[i];
  }
  float mean = static_cast<float>(total) / static_cast<float>(WINDOWS);

  Serial.print("TOTAL=");
  Serial.print(total);
  Serial.print(" MEAN=");
  Serial.print(mean, 1);
  Serial.print(" edges/window -- ");

  // Simple stability check: last 3 windows within 20% of each other
  if (WINDOWS >= 3) {
    uint32_t lateMin = counts[WINDOWS - 3];
    uint32_t lateMax = counts[WINDOWS - 3];
    for (int i = WINDOWS - 2; i < WINDOWS; i++) {
      if (counts[i] < lateMin) lateMin = counts[i];
      if (counts[i] > lateMax) lateMax = counts[i];
    }
    if (lateMax == 0 || (lateMax - lateMin) * 100 / lateMax < 20) {
      Serial.println("STABLE");
    } else {
      Serial.println("NOT STABLE (last 3 windows vary >20%)");
    }
  } else {
    Serial.println();
  }

  // Disable RX path
  disableRxPath();
  Serial.println("RX path disabled (RX_EN_N=HIGH).");
  Serial.println("---- VREF Settle Done ----");
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Low-Voltage Bring-up (Phase 2) ===");
  Serial.println("  r = Toggle RX_EN_N (probe 3V3_COMP, 3V3_RXAMP, 3V3_MUX, VREF)");
  Serial.println("  v = Measure VREF settle (edge count over 10x 100ms windows)");
  Serial.println("  p = Print all pin states");
  Serial.println("  m = Set mux direction (N/S/E/W)");
  Serial.println("  h = Hold PWR_HOLD");
  Serial.println("  l = Release PWR_HOLD");
  Serial.println("  ? = Menu");
  Serial.println();
  Serial.print("RX_EN_N=");
  Serial.print(g_rxEnabled ? "LOW (enabled)" : "HIGH (disabled)");
  Serial.println();
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(800);

  // Safe state: all outputs configured, boost OFF, RX disabled, PWR_HOLD HIGH
  initSafeState();

  // TOF_EDGE is input-only on GPIO34, configure ISR
  pinMode(PIN_TOF_EDGE, INPUT);
  v3_capture::installTofEdgeIsr();

  Serial.println();
  Serial.println("=== V3 Low-Voltage Bring-up (Phase 2) ===");
  Serial.println("Boost is OFF. No TX, no DRV/REL driven.");
  Serial.println("Use DMM to probe 3V3 rails and VREF.");
  Serial.println();

  // Print initial pin states
  printPinStates();

  printMenu();
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
  if (Serial.available() <= 0) {
    delay(20);
    return;
  }

  const char cmd = static_cast<char>(Serial.read());

  switch (cmd) {
    case 'r': {
      // Toggle RX_EN_N
      g_rxEnabled = !g_rxEnabled;
      if (g_rxEnabled) {
        enableRxPath();
        Serial.println("RX_EN_N: LOW (mux enabled) -- probe 3V3_COMP, 3V3_RXAMP, 3V3_MUX, VREF");
      } else {
        disableRxPath();
        Serial.println("RX_EN_N: HIGH (mux disabled, safe default)");
      }
      break;
    }

    case 'v': {
      // VREF settle measurement
      measureVrefSettle();
      break;
    }

    case 'p': {
      // Print all pin states
      printPinStates();
      break;
    }

    case 'm': {
      // Set mux direction — wait for next char
      Serial.println("Enter direction (N/S/E/W):");
      while (Serial.available() <= 0) {
        delay(20);
      }
      char dir = static_cast<char>(Serial.read());
      if (dir == 'n' || dir == 'N' || dir == 's' || dir == 'S' ||
          dir == 'e' || dir == 'E' || dir == 'w' || dir == 'W') {
        // Uppercase
        if (dir >= 'a' && dir <= 'z') dir = dir - ('a' - 'A');
        setRxDirection(dir);
        Serial.print("MUX direction: ");
        Serial.print(dir);
        Serial.println();
      } else {
        Serial.println("Invalid direction (use N/S/E/W)");
      }
      break;
    }

    case 'h': {
      // Hold PWR_HOLD
      holdPower();
      Serial.println("PWR_HOLD: HIGH (latched)");
      break;
    }

    case 'l': {
      // Release PWR_HOLD
      releasePower();
      Serial.println("PWR_HOLD: LOW (board may power off)");
      break;
    }

    case '?': {
      printMenu();
      break;
    }

    default:
      break;
  }
}