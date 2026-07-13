/*
 * Node V3 Continuous TX Debug — for DMM-only debugging (no oscilloscope)
 *
 * Toggles GPIO25 at 40 kHz continuously so a DMM can read steady-state
 * averages at each test point in the TX signal chain.
 *
 * Menu (single-key, serial):
 *   t = Toggle continuous TX on/off
 *   e = Toggle boost ON/OFF
 *   d = Toggle DRV_N HIGH/LOW (enables N-directional P-MOS Q17)
 *   r = Toggle all DRV pins (cycle N/E/S/W)
 *   s = Print current state
 *   ? = Menu
 *
 * DMM probe guide (with continuous TX ON, boost ON):
 *   GPIO25    → ~1.65 V (bit-bang OK)
 *   PWM_5V    → ~2.5 V  (TC4427 working — if 0V: TC4427 dead/missing)
 *   HS_GATE   → ~16-18V (Q21 switching — if 22V: Q21 not switching)
 *   TX_PULSE  → ~11 V   (Q16 switching — if 0V: Q16/Q21 chain dead)
 *   TD_N_A    → ~11 V   (with DRV_N HIGH — if 0V: Q17/Q22 missing)
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
// State
// ---------------------------------------------------------------------------
static bool g_txRunning    = false;
static bool g_boostOn       = false;
static int  g_drvActive     = -1;  // -1 = none, 0=N, 1=E, 2=S, 3=W

// ---------------------------------------------------------------------------
// Continuous TX task — toggles GPIO25 at ~40 kHz
// Runs in a tight loop in loop() when g_txRunning is true.
// Uses the same bit-bang timing as sendBurst40kHz but never stops.
// ---------------------------------------------------------------------------
static inline void continuousTxStep() {
  digitalWrite(PIN_TX_BURST_PWM, HIGH);
  delayMicroseconds(BURST_HALF_PERIOD_US);
  digitalWrite(PIN_TX_BURST_PWM, LOW);
  delayMicroseconds(BURST_HALF_PERIOD_US);
}

// ---------------------------------------------------------------------------
// Print state
// ---------------------------------------------------------------------------
static void printState() {
  Serial.println();
  Serial.println("=== Current State ===");
  Serial.print("  TX running:    ");
  Serial.println(g_txRunning ? "YES (40 kHz continuous)" : "NO");
  Serial.print("  Boost:          ");
  Serial.println(g_boostOn ? "ON" : "OFF");
  Serial.print("  DRV active:     ");
  if (g_drvActive < 0) {
    Serial.println("none (all LOW)");
  } else {
    const char* dirs[] = {"N", "E", "S", "W"};
    Serial.print(dirs[g_drvActive]);
    Serial.print(" (DRV HIGH, REL LOW)");
  }
  Serial.println();
}

static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Continuous TX Debug (for DMM probing) ===");
  Serial.println("  t = Toggle continuous TX on/off");
  Serial.println("  e = Toggle boost ON/OFF");
  Serial.println("  d = Toggle DRV_N HIGH/LOW (N-directional P-MOS)");
  Serial.println("  r = Cycle DRV (none→N→E→S→W→none)");
  Serial.println("  s = Print current state");
  Serial.println("  ? = Menu");
  printState();
}

// ---------------------------------------------------------------------------
// Set DRV for a given direction (or clear all)
// ---------------------------------------------------------------------------
static void setDrv(int dirIndex) {
  clearAllDirections();
  g_drvActive = dirIndex;
  if (dirIndex >= 0) {
    const char dirs[] = {'N', 'E', 'S', 'W'};
    setTxTransmit(dirs[dirIndex]);
  }
}

// ===========================================================================
// setup
// ===========================================================================
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(800);

  initSafeState();
  // Note: no ISR needed for this test — we're not capturing edges

  Serial.println();
  Serial.println("=== V3 Continuous TX Debug ===");
  Serial.println("Use a DMM to probe test points while TX is running.");
  Serial.println("See comments at top of file for expected voltages.");
  printMenu();
}

// ===========================================================================
// loop
// ===========================================================================
void loop() {
  // Check for serial input
  if (Serial.available() > 0) {
    char c = Serial.read();
    // Consume trailing newline
    while (Serial.available() > 0 && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
      Serial.read();
    }

    switch (c) {
      case 't':
      case 'T':
        g_txRunning = !g_txRunning;
        if (!g_txRunning) {
          digitalWrite(PIN_TX_BURST_PWM, LOW);
        }
        Serial.print("TX: ");
        Serial.println(g_txRunning ? "ON (continuous 40 kHz)" : "OFF");
        break;

      case 'e':
      case 'E':
        g_boostOn = !g_boostOn;
        if (g_boostOn) {
          enableBoost();
        } else {
          disableBoost();
        }
        Serial.print("Boost: ");
        Serial.println(g_boostOn ? "ON" : "OFF");
        break;

      case 'd':
      case 'D':
        if (g_drvActive == 0) {
          setDrv(-1);
          Serial.println("DRV_N: LOW (cleared)");
        } else {
          setDrv(0);
          Serial.println("DRV_N: HIGH (N-directional P-MOS enabled)");
        }
        break;

      case 'r':
      case 'R':
        {
          int next = g_drvActive + 1;
          if (next > 3) next = -1;
          setDrv(next);
          Serial.print("DRV: ");
          if (next < 0) {
            Serial.println("none (all LOW)");
          } else {
            const char* dirs[] = {"N", "E", "S", "W"};
            Serial.print(dirs[next]);
            Serial.println(" HIGH");
          }
        }
        break;

      case 's':
      case 'S':
        printState();
        break;

      case '?':
        printMenu();
        break;

      default:
        break;
    }
  }

  // Run continuous TX if enabled
  if (g_txRunning) {
    continuousTxStep();
  } else {
    delay(10);  // idle yield
  }
}