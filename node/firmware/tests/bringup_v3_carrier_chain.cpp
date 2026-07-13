/*
 * Node V3 Carrier Chain Bring-up (Phase 4)
 *
 * Verify Q16 gate, TX_PULSE waveform on oscilloscope.
 * Bit-banged burst on GPIO25 only — no DRV/REL driven.
 * This sketch tests the carrier chain in isolation.
 *
 * Shared headers used:
 *   v3_ultrasonic_pins.h      — pin definitions
 *   v3_ultrasonic_safe.h      — safe state, boost, RX control
 *   v3_ultrasonic_burst.h     — burst generation with BurstResult
 *   v3_ultrasonic_direction.h — direction control (not driven here)
 *   v3_ultrasonic_capture.h   — edge capture (not used here)
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

// ---------------------------------------------------------------------------
// RTC warm flag — persists across brownout/power-on resets
// ---------------------------------------------------------------------------
RTC_DATA_ATTR static bool g_boostWarmFlag = false;

// ---------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------
static int g_burstCycles     = BURST_CYCLES;          // 4–16
// Half-period is a compile-time constant (BURST_HALF_PERIOD_US).
// Override at build time with -DBURST_HALF_PERIOD_US=<val>.
static bool g_boostEnabled  = false;

// ---------------------------------------------------------------------------
// Sort helper (for median in repeat mode)
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
// Print BurstResult timing
// ---------------------------------------------------------------------------
static void printBurstTiming(const BurstResult& br) {
  const uint32_t durationUs = br.lastEdgeUs - br.firstEdgeUs;
  Serial.print("  firstEdgeUs=");
  Serial.print(br.firstEdgeUs);
  Serial.print(" lastEdgeUs=");
  Serial.print(br.lastEdgeUs);
  Serial.print(" duration=");
  Serial.print(durationUs);
  Serial.print("us cycles=");
  Serial.println(br.cycles);
}

// ---------------------------------------------------------------------------
// Fire single burst — carrier chain only, no directional load
// ---------------------------------------------------------------------------
static BurstResult fireCarrierBurst() {
  if (!g_boostEnabled) {
    Serial.println("  ** Boost not enabled — enable boost first (e) **");
    Serial.println("  Firing anyway for scope observation of TX_PULSE without 22V");
  }

  // Ensure no DRV/REL pins are driven (carrier-chain-only test)
  clearAllDirections();

  // Send burst on GPIO25 only
  BurstResult br = sendBurst40kHz(g_burstCycles);

  return br;
}

// ---------------------------------------------------------------------------
// Fire repeated bursts (10 shots, 120 ms interval)
// ---------------------------------------------------------------------------
static void fireRepeatBurst() {
  const int REPEAT_COUNT   = 10;
  const int INTER_SHOT_MS  = 120;

  if (!g_boostEnabled) {
    Serial.println("  ** Boost not enabled — enable boost first (e) **");
    Serial.println("  Firing anyway for scope observation of TX_PULSE without 22V");
  }

  Serial.print("Repeat: ");
  Serial.print(REPEAT_COUNT);
  Serial.print(" shots, ");
  Serial.print(INTER_SHOT_MS);
  Serial.println("ms interval");

  int32_t durations[REPEAT_COUNT];
  int validCount = 0;

  for (int i = 0; i < REPEAT_COUNT; ++i) {
    clearAllDirections();
    BurstResult br = sendBurst40kHz(g_burstCycles);

    const uint32_t durationUs = br.lastEdgeUs - br.firstEdgeUs;
    if (validCount < REPEAT_COUNT) {
      durations[validCount++] = static_cast<int32_t>(durationUs);
    }

    Serial.print("  [");
    if (i < 9) Serial.print(' ');
    Serial.print(i + 1);
    Serial.print("] ");
    printBurstTiming(br);

    delay(INTER_SHOT_MS);
  }

  // Summary
  if (validCount > 0) {
    sortInt32(durations, validCount);
    const int32_t medianDur = durations[validCount / 2];
    const int32_t minDur    = durations[0];
    const int32_t maxDur    = durations[validCount - 1];
    const int32_t jitter    = maxDur - minDur;

    Serial.print("SUMMARY shots=");
    Serial.print(validCount);
    Serial.print(" median_dur=");
    Serial.print(medianDur);
    Serial.print("us min=");
    Serial.print(minDur);
    Serial.print("us max=");
    Serial.print(maxDur);
    Serial.print("us jitter=");
    Serial.print(jitter);
    Serial.println("us");
  }
}

// ---------------------------------------------------------------------------
// Print timing of last burst (re-fire and display)
// ---------------------------------------------------------------------------
static void printTiming() {
  clearAllDirections();
  BurstResult br = sendBurst40kHz(g_burstCycles);
  Serial.print("TIMING ");
  printBurstTiming(br);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Carrier Chain (Phase 4) ===");
  Serial.println("  f = Fire single burst (no directional load)");
  Serial.println("  r = Repeat burst (10 shots, 120ms interval)");
  Serial.println("  c = Change burst cycles (4-16)");
  Serial.println("  e = Enable boost");
  Serial.println("  d = Disable boost");
  Serial.println("  t = Print timing (first/last edge, duration)");
  Serial.println("  ? = Menu");
  Serial.println();
  Serial.print("Config: cycles=");
  Serial.print(g_burstCycles);
  Serial.print(" halfPeriod=");
  Serial.print(BURST_HALF_PERIOD_US);
  Serial.print("us (compile-time) boost=");
  Serial.println(g_boostEnabled ? "ON" : "OFF");
  Serial.println("NOTE: No DRV/REL pins are ever driven in this sketch");
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

  // Initialise all pins to safe state (includes VREF settle wait)
  initSafeState();

  // Half-period is a compile-time constant (BURST_HALF_PERIOD_US).
  // Override at build time with -DBURST_HALF_PERIOD_US=<val>.

  // If boost was enabled before reboot, re-enable immediately
  if (g_boostWarmFlag) {
    Serial.println("BOOST WARM: re-enabling boost (cap pre-charged from previous attempt)");
    enableBoost();
    delay(getBoostPrechargeMs());
    delay(50);  // extra settling
    g_boostEnabled = true;
    Serial.println("BOOST WARM: boost ready");
  }

  // Install TOF edge ISR (not used in this sketch, but consistent with V3 init)
  v3_capture::installTofEdgeIsr();

  Serial.println("V3 Carrier Chain bring-up ready");
  Serial.println("Probe: GPIO25, PWM_5V, HS_GATE, TX_PULSE, 22V_SYS");
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
    case 'f': {
      // Fire single burst
      BurstResult br = fireCarrierBurst();
      Serial.print("SINGLE ");
      printBurstTiming(br);
      break;
    }

    case 'r': {
      // Repeat burst
      fireRepeatBurst();
      break;
    }

    case 'c': {
      // Change burst cycles
      Serial.print("Current cycles: ");
      Serial.println(g_burstCycles);
      Serial.println("Enter cycles (4-16):");
      while (Serial.available() <= 0) { delay(20); }
      String line = Serial.readStringUntil('\n');
      line.trim();
      int val = line.toInt();
      if (val >= 4 && val <= 16) {
        g_burstCycles = val;
        Serial.print("Burst cycles set to ");
        Serial.println(g_burstCycles);
      } else {
        Serial.println("Invalid — must be 4-16");
      }
      break;
    }

    case 'e': {
      // Enable boost
      if (g_boostEnabled) {
        Serial.println("Boost already enabled");
      } else {
        Serial.println("Enabling boost (may brownout and reboot)...");
        Serial.println("After reboot, boost will auto-resume from RTC flag");
        Serial.flush();
        delay(10);
        g_boostWarmFlag = true;
        enableBoost();
        delay(getBoostPrechargeMs());
        g_boostEnabled = true;
        Serial.println("Boost: ON (no brownout)");
      }
      break;
    }

    case 'd': {
      // Disable boost
      g_boostWarmFlag = false;
      g_boostEnabled = false;
      disableBoost();
      Serial.println("Boost: OFF (TX_22V_EN_N=HIGH) — safe default");
      break;
    }

    case 't': {
      // Print timing
      printTiming();
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