#pragma once
// V3 ultrasonic safe-state control — header-only
// Safe idle, boost control, RX path, power hold, fail-safe cleanup.
// Every sketch calls initSafeState() in setup() and failSafeCleanup() on error/timeout.

#include <Arduino.h>
#include "v3_ultrasonic_pins.h"

// VREF settling: 500 ms after power-up before first acoustic measurement
// (V3 added distributed VREF decoupling totalling several µF)
#ifndef VREF_SETTLE_MS
#define VREF_SETTLE_MS 500
#endif

// Boost precharge: time for 22 V rail to stabilise after hard enable
#ifndef BOOST_PRECHARGE_MS
#define BOOST_PRECHARGE_MS 50
#endif

// ---------------------------------------------------------------------------
// Configurable precharge (runtime adjustable for bring-up)
// ---------------------------------------------------------------------------
static uint32_t s_boostPrechargeMs = BOOST_PRECHARGE_MS;

static inline uint32_t getBoostPrechargeMs() {
  return s_boostPrechargeMs;
}

static inline void setBoostPrechargeMs(uint32_t ms) {
  s_boostPrechargeMs = ms;
}

// ---------------------------------------------------------------------------
// Boost enable / disable (active-LOW via U49 inverter)
//   TX_22V_EN_N = HIGH → U49 output LOW → EN_22 LOW  → boost OFF
//   TX_22V_EN_N = LOW  → U49 output HIGH → EN_22 HIGH → boost ON
// ---------------------------------------------------------------------------
static inline void enableBoost() {
  digitalWrite(PIN_TX_22V_EN_N, LOW);   // LOW = boost ON
}

static inline void disableBoost() {
  digitalWrite(PIN_TX_22V_EN_N, HIGH);  // HIGH = boost OFF (safe default)
}

// ---------------------------------------------------------------------------
// Soft-start boost — gradual 100 µs pulses then hard enable + precharge
// Charges the 22 V output capacitor gently to avoid VSYS collapse.
// ---------------------------------------------------------------------------
static inline void softStartBoost() {
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
  delay(s_boostPrechargeMs);
  Serial.println("  boost ready");
}

// ---------------------------------------------------------------------------
// RX path enable / disable (active-LOW)
//   RX_EN_N = LOW  → mux enabled  (RX_WINDOW_EN = HIGH via inverter)
//   RX_EN_N = HIGH → mux disabled (safe default, TOF_EDGE blocked)
// ---------------------------------------------------------------------------
static inline void enableRxPath() {
  digitalWrite(PIN_RX_EN_N, LOW);   // LOW = mux enabled
}

static inline void disableRxPath() {
  digitalWrite(PIN_RX_EN_N, HIGH);  // HIGH = mux disabled (safe default)
}

// ---------------------------------------------------------------------------
// Power hold (active-HIGH keeps board powered)
// ---------------------------------------------------------------------------
static inline void holdPower() {
  digitalWrite(PIN_PWR_HOLD, HIGH);
}

static inline void releasePower() {
  digitalWrite(PIN_PWR_HOLD, LOW);
}

// ---------------------------------------------------------------------------
// initSafeState — call in setup()
// 1. Configure all outputs
// 2. Set all DRV/REL LOW (idle/receive)
// 3. TX_BURST_PWM LOW
// 4. TX_22V_EN_N HIGH (boost OFF)
// 5. RX_EN_N HIGH (mux disabled)
// 6. PWR_HOLD HIGH
// 7. MUX_A/B LOW
// 8. Wait VREF settle
// ---------------------------------------------------------------------------
static inline void initSafeState() {
  // Configure outputs
  pinMode(PIN_DRV_N, OUTPUT);
  pinMode(PIN_DRV_E, OUTPUT);
  pinMode(PIN_DRV_S, OUTPUT);
  pinMode(PIN_DRV_W, OUTPUT);
  pinMode(PIN_REL_N, OUTPUT);
  pinMode(PIN_REL_E, OUTPUT);
  pinMode(PIN_REL_S, OUTPUT);
  pinMode(PIN_REL_W, OUTPUT);
  pinMode(PIN_TX_BURST_PWM, OUTPUT);
  pinMode(PIN_TX_22V_EN_N, OUTPUT);
  pinMode(PIN_RX_EN_N, OUTPUT);
  pinMode(PIN_PWR_HOLD, OUTPUT);
  pinMode(PIN_MUX_A, OUTPUT);
  pinMode(PIN_MUX_B, OUTPUT);

  // All DRV/REL LOW (idle/receive per V3 truth table)
  digitalWrite(PIN_DRV_N, LOW);
  digitalWrite(PIN_DRV_E, LOW);
  digitalWrite(PIN_DRV_S, LOW);
  digitalWrite(PIN_DRV_W, LOW);
  digitalWrite(PIN_REL_N, LOW);
  digitalWrite(PIN_REL_E, LOW);
  digitalWrite(PIN_REL_S, LOW);
  digitalWrite(PIN_REL_W, LOW);

  // TX carrier LOW
  digitalWrite(PIN_TX_BURST_PWM, LOW);

  // Boost OFF (active-LOW → HIGH = off)
  digitalWrite(PIN_TX_22V_EN_N, HIGH);

  // RX mux disabled (active-LOW → HIGH = disabled)
  digitalWrite(PIN_RX_EN_N, HIGH);

  // Hold power on
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // MUX default
  digitalWrite(PIN_MUX_A, LOW);
  digitalWrite(PIN_MUX_B, LOW);

  // Wait for VREF to settle (distributed decoupling is several µF)
  delay(VREF_SETTLE_MS);
}

// ---------------------------------------------------------------------------
// failSafeCleanup — emergency cleanup on error/timeout
// 1. TX_BURST_PWM LOW
// 2. All DRV LOW
// 3. All REL LOW
// 4. RX_EN_N HIGH (mux disabled)
// 5. TX_22V_EN_N HIGH (boost OFF)
// ---------------------------------------------------------------------------
static inline void failSafeCleanup() {
  digitalWrite(PIN_TX_BURST_PWM, LOW);

  digitalWrite(PIN_DRV_N, LOW);
  digitalWrite(PIN_DRV_E, LOW);
  digitalWrite(PIN_DRV_S, LOW);
  digitalWrite(PIN_DRV_W, LOW);

  digitalWrite(PIN_REL_N, LOW);
  digitalWrite(PIN_REL_E, LOW);
  digitalWrite(PIN_REL_S, LOW);
  digitalWrite(PIN_REL_W, LOW);

  digitalWrite(PIN_RX_EN_N, HIGH);      // mux disabled
  digitalWrite(PIN_TX_22V_EN_N, HIGH);  // boost OFF
}