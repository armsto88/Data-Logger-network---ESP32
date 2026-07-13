/*
 * Node V3 Directional TX + Damping Bring-up (Phase 5-6)
 *
 * One directional channel at a time, then damping state validation.
 * Follows the V3 measurement state machine from the design guide §6.4:
 *   SELECT_PATH → BOOST_PRECHARGE → TRANSMIT → DAMP → RX_PREPARE → CAPTURE → CLEANUP
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

// ---------------------------------------------------------------------------
// RTC warm flag — persists across brownout/power-on resets
// ---------------------------------------------------------------------------
RTC_DATA_ATTR static bool g_boostWarmFlag = false;

// ---------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------
static int g_burstCycles       = BURST_CYCLES;
static uint32_t g_dampingUs    = 20;   // damping duration (µs), 0 = leave REL HIGH
static uint32_t g_prechargeMs  = BOOST_PRECHARGE_MS;
static bool g_boostEnabled     = false;

// RX open time: approximate time after TX first edge when RX should be enabled.
// For ~20 cm path at 340 m/s, TOF ≈ 588 µs.  Open RX well before expected arrival.
#ifndef RX_OPEN_TIME_US
#define RX_OPEN_TIME_US 400
#endif

// Mux settle time after enabling RX path
#ifndef MUX_SETTLE_US
#define MUX_SETTLE_US 5
#endif

// ---------------------------------------------------------------------------
// Opposite direction lookup
// ---------------------------------------------------------------------------
static char oppositeDir(char dir) {
  switch (dir) {
    case 'N': return 'S';
    case 'S': return 'N';
    case 'E': return 'W';
    case 'W': return 'E';
    default: return dir;
  }
}

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
// V3 single-shot measurement sequence (§6.4 state machine)
// Returns CaptureResult with TOF referenced to firstEdgeUs.
// ---------------------------------------------------------------------------
static CaptureResult fireV3Shot(char dir) {
  // 1. SELECT_PATH: set RX mux direction (opposite of TX), disable RX, set TX transmit
  char rxDir = oppositeDir(dir);
  setRxDirection(rxDir);
  disableRxPath();
  setTxTransmit(dir);   // DRV_x=HIGH, REL_x=LOW, all others IDLE

  // 2. BOOST_PRECHARGE: enable boost, wait for precharge
  enableBoost();
  delay(g_prechargeMs);

  // 3. TRANSMIT: send burst, record firstEdgeUs as time origin
  v3_capture::resetEdgeCapture();
  BurstResult br = sendBurst40kHz(g_burstCycles);
  const uint32_t firstEdgeUs = br.firstEdgeUs;  // V3 time origin

  // 4. DAMP: set DRV_x LOW, then apply damping
  //    setTxTransmit already set DRV_x=HIGH, REL_x=LOW
  //    First clear DRV_x (set all idle), then apply damping
  clearAllDirections();  // DRV_x=LOW, REL_x=LOW for all
  if (!setDamping(dir, g_dampingUs)) {  // REL_x=HIGH for dampingUs, then LOW
    Serial.println("WARNING: setDamping() failed (DRV still HIGH), damping skipped");
  }

  // 5. RX_PREPARE: wait until configured receive-open time, enable RX, settle + guard
  //    Calculate how long since firstEdgeUs and wait if needed
  const uint32_t nowUs = micros();
  const uint32_t elapsedSinceTx = nowUs - firstEdgeUs;
  if (elapsedSinceTx < RX_OPEN_TIME_US) {
    delayMicroseconds(RX_OPEN_TIME_US - elapsedSinceTx);
  }
  enableRxPath();
  delayMicroseconds(MUX_SETTLE_US);
  delayMicroseconds(POST_ENABLE_GUARD_US);

  // 6. CAPTURE: arm capture, poll for edges, compute TOF relative to firstEdgeUs
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();
  CaptureResult cap = v3_capture::getCaptureResult(firstEdgeUs, TOF_TIMEOUT_US);
  v3_capture::disarmCapture();

  // If capture timed out with no detection, ensure safe state before cleanup
  if (!cap.detected) {
    failSafeCleanup();
  }

  // 7. CLEANUP: clear all directions, disable RX, disable boost
  clearAllDirections();
  disableRxPath();
  disableBoost();
  failSafeCleanup();  // idempotent safety net

  return cap;
}

// ---------------------------------------------------------------------------
// Print single-shot result
// ---------------------------------------------------------------------------
static void printShotResult(char dir, const BurstResult& br, const CaptureResult& cap) {
  Serial.print("SHOT TX=");
  Serial.print(dir);
  Serial.print(" RX=");
  Serial.print(oppositeDir(dir));
  Serial.print(" DET=");
  Serial.print(cap.detected ? 1 : 0);
  Serial.print(" TOF=");
  Serial.print(cap.tofUs);
  Serial.print("us EDGES=");
  Serial.print(cap.edgeCount);
  Serial.print(" E0=");
  Serial.print(cap.firstEdgeUs);
  Serial.print(" E1=");
  Serial.print(cap.secondEdgeUs);
  Serial.print(" E2=");
  Serial.print(cap.thirdEdgeUs);
  Serial.print(" ELAST=");
  Serial.print(cap.lastEdgeUs);
  Serial.print(" DAMP=");
  Serial.print(g_dampingUs);
  Serial.print("us CYC=");
  Serial.print(br.cycles);
  Serial.print(" BURST_DUR=");
  Serial.print(static_cast<int32_t>(br.lastEdgeUs - br.firstEdgeUs));
  Serial.println("us");
}

// ---------------------------------------------------------------------------
// Fire single shot N/E/S/W
// ---------------------------------------------------------------------------
static void fireSingleShot(char dir) {
  if (!g_boostEnabled) {
    Serial.println("** Boost not enabled — enable first (e) **");
    Serial.println("Firing anyway (may get weak or no detection)");
  }

  BurstResult br;
  // We need to capture the BurstResult from inside fireV3Shot,
  // but fireV3Shot returns CaptureResult. Re-run the sequence manually
  // so we can print both.

  // 1. SELECT_PATH
  char rxDir = oppositeDir(dir);
  setRxDirection(rxDir);
  disableRxPath();
  setTxTransmit(dir);

  // 2. BOOST_PRECHARGE
  enableBoost();
  delay(g_prechargeMs);

  // 3. TRANSMIT
  v3_capture::resetEdgeCapture();
  br = sendBurst40kHz(g_burstCycles);
  const uint32_t firstEdgeUs = br.firstEdgeUs;

  // 4. DAMP
  clearAllDirections();
  if (!setDamping(dir, g_dampingUs)) {
    Serial.println("WARNING: setDamping() failed (DRV still HIGH), damping skipped");
  }

  // 5. RX_PREPARE
  const uint32_t nowUs = micros();
  const uint32_t elapsedSinceTx = nowUs - firstEdgeUs;
  if (elapsedSinceTx < RX_OPEN_TIME_US) {
    delayMicroseconds(RX_OPEN_TIME_US - elapsedSinceTx);
  }
  enableRxPath();
  delayMicroseconds(MUX_SETTLE_US);
  delayMicroseconds(POST_ENABLE_GUARD_US);

  // 6. CAPTURE
  v3_capture::resetEdgeCapture();
  v3_capture::armCapture();
  CaptureResult cap = v3_capture::getCaptureResult(firstEdgeUs, TOF_TIMEOUT_US);
  v3_capture::disarmCapture();

  // If capture timed out with no detection, ensure safe state before cleanup
  if (!cap.detected) {
    failSafeCleanup();
  }

  // 7. CLEANUP
  clearAllDirections();
  disableRxPath();
  disableBoost();
  failSafeCleanup();  // idempotent safety net

  printShotResult(dir, br, cap);
}

// ---------------------------------------------------------------------------
// Damping sweep: fire 5 shots at each damping interval (0, 10, 20, 50 µs)
// ---------------------------------------------------------------------------
static void runDampingSweep(char dir) {
  const uint32_t dampingIntervals[] = {0, 10, 20, 50};
  const int numIntervals = 4;
  const int SHOTS_PER_INTERVAL = 5;
  const int INTER_SHOT_MS = 120;

  if (!g_boostEnabled) {
    Serial.println("** Boost not enabled — enable first (e) **");
  }

  Serial.print("DAMPING SWEEP TX=");
  Serial.print(dir);
  Serial.print(" RX=");
  Serial.print(oppositeDir(dir));
  Serial.print(" shots_per_interval=");
  Serial.println(SHOTS_PER_INTERVAL);

  for (int di = 0; di < numIntervals; ++di) {
    const uint32_t dampUs = dampingIntervals[di];
    int detectedCount = 0;
    int32_t tofs[SHOTS_PER_INTERVAL];
    uint32_t edgeCounts[SHOTS_PER_INTERVAL];

    for (int shot = 0; shot < SHOTS_PER_INTERVAL; ++shot) {
      // SELECT_PATH
      char rxDir = oppositeDir(dir);
      setRxDirection(rxDir);
      disableRxPath();
      setTxTransmit(dir);

      // BOOST_PRECHARGE
      enableBoost();
      delay(g_prechargeMs);

      // TRANSMIT
      v3_capture::resetEdgeCapture();
      BurstResult br = sendBurst40kHz(g_burstCycles);
      const uint32_t firstEdgeUs = br.firstEdgeUs;

      // DAMP with current interval
      clearAllDirections();
      if (!setDamping(dir, dampUs)) {
        Serial.println("WARNING: setDamping() failed (DRV still HIGH), damping skipped");
      }

      // RX_PREPARE
      const uint32_t nowUs = micros();
      const uint32_t elapsedSinceTx = nowUs - firstEdgeUs;
      if (elapsedSinceTx < RX_OPEN_TIME_US) {
        delayMicroseconds(RX_OPEN_TIME_US - elapsedSinceTx);
      }
      enableRxPath();
      delayMicroseconds(MUX_SETTLE_US);
      delayMicroseconds(POST_ENABLE_GUARD_US);

      // CAPTURE
      v3_capture::resetEdgeCapture();
      v3_capture::armCapture();
      CaptureResult cap = v3_capture::getCaptureResult(firstEdgeUs, TOF_TIMEOUT_US);
      v3_capture::disarmCapture();

      // CLEANUP
      clearAllDirections();
      disableRxPath();
      disableBoost();

      edgeCounts[shot] = cap.edgeCount;
      if (cap.detected) {
        tofs[detectedCount++] = cap.tofUs;
      }

      delay(INTER_SHOT_MS);
    }

    // Summary for this damping interval
    int32_t medianTof = -1;
    if (detectedCount > 0) {
      sortInt32(tofs, detectedCount);
      medianTof = tofs[detectedCount / 2];
    }

    // Median edge count
    uint32_t ecSorted[SHOTS_PER_INTERVAL];
    for (int i = 0; i < SHOTS_PER_INTERVAL; ++i) ecSorted[i] = edgeCounts[i];
    // Simple insertion sort for small array
    for (int i = 1; i < SHOTS_PER_INTERVAL; ++i) {
      uint32_t key = ecSorted[i];
      int j = i - 1;
      while (j >= 0 && ecSorted[j] > key) {
        ecSorted[j + 1] = ecSorted[j];
        j--;
      }
      ecSorted[j + 1] = key;
    }
    const uint32_t medianEdges = ecSorted[SHOTS_PER_INTERVAL / 2];

    Serial.print("DAMP TX=");
    Serial.print(dir);
    Serial.print(" DAMP_US=");
    Serial.print(dampUs);
    Serial.print(" DET=");
    Serial.print(detectedCount);
    Serial.print("/");
    Serial.print(SHOTS_PER_INTERVAL);
    Serial.print(" MED_TOF=");
    Serial.print(medianTof);
    Serial.print("us MED_EDGES=");
    Serial.println(medianEdges);
  }

  Serial.println("DAMPING SWEEP DONE");
}

// ---------------------------------------------------------------------------
// All channels sequential (N, E, S, W)
// ---------------------------------------------------------------------------
static void fireAllChannels() {
  const char dirs[] = {'N', 'E', 'S', 'W'};
  const int INTER_SHOT_MS = 200;

  if (!g_boostEnabled) {
    Serial.println("** Boost not enabled — enable first (e) **");
  }

  Serial.println("ALL CHANNELS SEQUENTIAL");

  for (int i = 0; i < 4; ++i) {
    fireSingleShot(dirs[i]);
    delay(INTER_SHOT_MS);
  }

  Serial.println("ALL CHANNELS DONE");
}

// ---------------------------------------------------------------------------
// Inactive-channel check: fire N, verify E/S/W DRV/REL are all LOW
// ---------------------------------------------------------------------------
static void runInactiveChannelCheck() {
  Serial.println("INACTIVE-CHANNEL CHECK: fire N, verify E/S/W quiet");

  if (!g_boostEnabled) {
    Serial.println("** Boost not enabled — enable first (e) **");
  }

  // Set up for North transmit
  setRxDirection('S');
  disableRxPath();
  setTxTransmit('N');

  // Verify North is in transmit state
  bool drvN = digitalRead(PIN_DRV_N) == HIGH;
  bool relN = digitalRead(PIN_REL_N) == LOW;
  Serial.print("N: DRV=");
  Serial.print(drvN ? "HIGH" : "LOW");
  Serial.print(" REL=");
  Serial.println(relN ? "HIGH" : "LOW");

  // Verify East, South, West are all idle
  bool drvE = digitalRead(PIN_DRV_E) == HIGH;
  bool relE = digitalRead(PIN_REL_E) == HIGH;
  bool drvS = digitalRead(PIN_DRV_S) == HIGH;
  bool relS = digitalRead(PIN_REL_S) == HIGH;
  bool drvW = digitalRead(PIN_DRV_W) == HIGH;
  bool relW = digitalRead(PIN_REL_W) == HIGH;

  Serial.print("E: DRV=");
  Serial.print(drvE ? "HIGH" : "LOW");
  Serial.print(" REL=");
  Serial.println(relE ? "HIGH" : "LOW");
  Serial.print("S: DRV=");
  Serial.print(drvS ? "HIGH" : "LOW");
  Serial.print(" REL=");
  Serial.println(relS ? "HIGH" : "LOW");
  Serial.print("W: DRV=");
  Serial.print(drvW ? "HIGH" : "LOW");
  Serial.print(" REL=");
  Serial.println(relW ? "HIGH" : "LOW");

  bool pass = !drvE && !relE && !drvS && !relS && !drvW && !relW;
  Serial.print("RESULT: ");
  Serial.println(pass ? "PASS (E/S/W all idle)" : "FAIL (inactive channel has active pin)");

  // Cleanup
  clearAllDirections();
  disableRxPath();
  disableBoost();
}

// ---------------------------------------------------------------------------
// FORBIDDEN state test: verify isTxStateSafe() blocks DRV=HIGH + REL=HIGH
// ---------------------------------------------------------------------------
static void runForbiddenStateTest() {
  Serial.println("FORBIDDEN STATE TEST");
  int passCount = 0;
  int failCount = 0;

  // Start from safe state
  clearAllDirections();

  // Test 1: Try to set DRV_N=HIGH and REL_N=HIGH simultaneously
  Serial.println("  Test 1: DRV_N=HIGH + REL_N=HIGH simultaneously");
  digitalWrite(PIN_DRV_N, HIGH);
  digitalWrite(PIN_REL_N, HIGH);

  bool safe = isTxStateSafe();
  Serial.print("    isTxStateSafe() = ");
  Serial.print(safe ? "true" : "false");
  Serial.print(" -- ");
  if (!safe) {
    Serial.println("PASS (correctly detected forbidden state)");
    passCount++;
  } else {
    Serial.println("FAIL (should have returned false)");
    failCount++;
  }

  // Test 2: Verify setDamping() refuses to activate while DRV is HIGH
  Serial.println("  Test 2: setDamping('N', 20) while DRV_N=HIGH");
  bool dampResult = setDamping('N', 20);
  Serial.print("    setDamping returned ");
  Serial.print(dampResult ? "true" : "false");
  Serial.print(" -- ");
  if (!dampResult) {
    Serial.println("PASS (correctly refused damping with DRV HIGH)");
    passCount++;
  } else {
    Serial.println("FAIL (should have returned false)");
    failCount++;
  }

  // Cleanup: return to safe state
  clearAllDirections();

  // Test 3: Verify safe state after cleanup
  Serial.println("  Test 3: isTxStateSafe() after clearAllDirections()");
  safe = isTxStateSafe();
  Serial.print("    isTxStateSafe() = ");
  Serial.print(safe ? "true" : "false");
  Serial.print(" -- ");
  if (safe) {
    Serial.println("PASS (safe after cleanup)");
    passCount++;
  } else {
    Serial.println("FAIL (should be safe after cleanup)");
    failCount++;
  }

  // Test 4: Verify setDamping works when DRV is LOW (normal damping)
  Serial.println("  Test 4: setDamping('N', 20) with DRV_N=LOW (normal)");
  dampResult = setDamping('N', 20);
  Serial.print("    setDamping returned ");
  Serial.print(dampResult ? "true" : "false");
  Serial.print(" -- ");
  if (dampResult) {
    Serial.println("PASS (damping accepted with DRV LOW)");
    passCount++;
  } else {
    Serial.println("FAIL (should have returned true)");
    failCount++;
  }

  // Cleanup
  clearAllDirections();

  // Summary
  Serial.print("FORBIDDEN STATE TEST: PASS=");
  Serial.print(passCount);
  Serial.print("/4 FAIL=");
  Serial.print(failCount);
  Serial.print("/4 -- ");
  Serial.println(failCount == 0 ? "ALL PASS" : "SOME FAILURES");
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Directional TX + Damping (Phase 5-6) ===");
  Serial.println("  n/s/w = Fire single shot N/S/W (DRV=HIGH, REL=LOW)");
  Serial.println("  E = Fire single shot East (uppercase E)");
  Serial.println("  d = Damping sweep (0/10/20/50 us, selected channel)");
  Serial.println("  a = All channels sequential (N,E,S,W)");
  Serial.println("  i = Inactive-channel check (fire N, verify E/S/W quiet)");
  Serial.println("  x = FORBIDDEN state test (verify DRV+REL block)");
  Serial.println("  D = Set damping duration (us)");
  Serial.println("  c = Set burst cycles");
  Serial.println("  P = Set precharge delay (ms)");
  Serial.println("  e = Enable boost");
  Serial.println("  b = Disable boost");
  Serial.println("  ? = Menu");
  Serial.println();
  Serial.print("Config: cycles=");
  Serial.print(g_burstCycles);
  Serial.print(" damp=");
  Serial.print(g_dampingUs);
  Serial.print("us precharge=");
  Serial.print(g_prechargeMs);
  Serial.print("ms boost=");
  Serial.println(g_boostEnabled ? "ON" : "OFF");
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

  // If boost was enabled before reboot, re-enable immediately
  if (g_boostWarmFlag) {
    Serial.println("BOOST WARM: re-enabling boost (cap pre-charged from previous attempt)");
    enableBoost();
    delay(getBoostPrechargeMs());
    delay(50);  // extra settling
    g_boostEnabled = true;
    Serial.println("BOOST WARM: boost ready");
  }

  // Install TOF edge ISR
  v3_capture::installTofEdgeIsr();

  Serial.println("V3 Directional TX + Damping bring-up ready");
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

  switch (raw) {
    // Directional single shots
    case 'n':
      fireSingleShot('N');
      break;
    case 'E':  // Uppercase E for East (lowercase 'e' = enable boost)
      fireSingleShot('E');
      break;
    case 's':
      fireSingleShot('S');
      break;
    case 'w':
      fireSingleShot('W');
      break;

    // Damping sweep (lowercase d)
    case 'd': {
      Serial.println("Enter direction for damping sweep (n/E/s/w):");
      while (Serial.available() <= 0) { delay(20); }
      char dir = static_cast<char>(Serial.read());
      if (dir == 'n' || dir == 'N' || dir == 'e' || dir == 'E' ||
          dir == 's' || dir == 'S' || dir == 'w' || dir == 'W') {
        // Normalise to uppercase
        if (dir >= 'a' && dir <= 'z') dir = dir - ('a' - 'A');
        runDampingSweep(dir);
      } else {
        Serial.println("Invalid direction");
      }
      break;
    }

    // All channels sequential
    case 'a':
      fireAllChannels();
      break;

    // Inactive-channel check
    case 'i':
      runInactiveChannelCheck();
      break;

    // FORBIDDEN state test
    case 'x':
      runForbiddenStateTest();
      break;

    // Set damping duration (uppercase D)
    case 'D': {
      Serial.print("Current damping: ");
      Serial.print(g_dampingUs);
      Serial.println(" us");
      Serial.println("Enter damping duration (us, 0=leave REL HIGH):");
      while (Serial.available() <= 0) { delay(20); }
      String line = Serial.readStringUntil('\n');
      line.trim();
      long val = line.toInt();
      if (val >= 0 && val <= 500) {
        g_dampingUs = static_cast<uint32_t>(val);
        Serial.print("Damping set to ");
        Serial.print(g_dampingUs);
        Serial.println(" us");
      } else {
        Serial.println("Invalid — must be 0-500");
      }
      break;
    }

    // Set burst cycles (lowercase c)
    case 'c': {
      Serial.print("Current cycles: ");
      Serial.println(g_burstCycles);
      Serial.println("Enter burst cycles (4-16):");
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

    // Set precharge delay (uppercase P)
    case 'P': {
      Serial.print("Current precharge: ");
      Serial.print(g_prechargeMs);
      Serial.println(" ms");
      Serial.println("Enter precharge delay (ms, 0-200):");
      while (Serial.available() <= 0) { delay(20); }
      String line = Serial.readStringUntil('\n');
      line.trim();
      long val = line.toInt();
      if (val >= 0 && val <= 200) {
        g_prechargeMs = static_cast<uint32_t>(val);
        setBoostPrechargeMs(g_prechargeMs);
        Serial.print("Precharge set to ");
        Serial.print(g_prechargeMs);
        Serial.println(" ms");
      } else {
        Serial.println("Invalid — must be 0-200");
      }
      break;
    }

    // Enable boost (lowercase e)
    case 'e':
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

    // Disable boost (lowercase b)
    case 'b':
      g_boostWarmFlag = false;
      g_boostEnabled = false;
      disableBoost();
      Serial.println("Boost: OFF (TX_22V_EN_N=HIGH) — safe default");
      break;

    case '?':
      printMenu();
      break;

    default:
      break;
  }
}