#pragma once
// V3 ultrasonic directional channel control — header-only
// DRV/REL switching with FORBIDDEN-state enforcement.
//
// V3 truth table:
//   DRV=LOW,  REL=LOW  → Idle / receive
//   DRV=HIGH, REL=LOW  → Transmit
//   DRV=LOW,  REL=HIGH → Damping (transducer short)
//   DRV=HIGH, REL=HIGH → FORBIDDEN while TX_PULSE is active
//
// REL_* is DAMP/SHUNT in V3 (net names unchanged, purpose clarified).

#include <Arduino.h>
#include "v3_ultrasonic_pins.h"

// RX mux direction table (V3 overview §18 — MUX_B is MSB, MUX_A is LSB):
//   N: MUX_B=0, MUX_A=0  (Y0)
//   E: MUX_B=0, MUX_A=1  (Y1)
//   S: MUX_B=1, MUX_A=0  (Y2)
//   W: MUX_B=1, MUX_A=1  (Y3)
static inline void setRxDirection(char dir) {
  switch (dir) {
    case 'N':
      digitalWrite(PIN_MUX_A, LOW);
      digitalWrite(PIN_MUX_B, LOW);
      break;
    case 'E':
      digitalWrite(PIN_MUX_A, HIGH);
      digitalWrite(PIN_MUX_B, LOW);
      break;
    case 'S':
      digitalWrite(PIN_MUX_A, LOW);
      digitalWrite(PIN_MUX_B, HIGH);
      break;
    case 'W':
      digitalWrite(PIN_MUX_A, HIGH);
      digitalWrite(PIN_MUX_B, HIGH);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// clearAllDirections — all DRV/REL LOW (idle/receive state)
// ---------------------------------------------------------------------------
static inline void clearAllDirections() {
  digitalWrite(PIN_DRV_N, LOW);
  digitalWrite(PIN_DRV_E, LOW);
  digitalWrite(PIN_DRV_S, LOW);
  digitalWrite(PIN_DRV_W, LOW);

  digitalWrite(PIN_REL_N, LOW);
  digitalWrite(PIN_REL_E, LOW);
  digitalWrite(PIN_REL_S, LOW);
  digitalWrite(PIN_REL_W, LOW);
}

// ---------------------------------------------------------------------------
// setTxTransmit — set one channel to transmit state
//   DRV_x=HIGH, REL_x=LOW, all others IDLE.
// Calls clearAllDirections() first to enforce "only one DRV HIGH".
// ---------------------------------------------------------------------------
static inline void setTxTransmit(char dir) {
  clearAllDirections();

  switch (dir) {
    case 'N':
      digitalWrite(PIN_DRV_N, HIGH);
      digitalWrite(PIN_REL_N, LOW);
      break;
    case 'E':
      digitalWrite(PIN_DRV_E, HIGH);
      digitalWrite(PIN_REL_E, LOW);
      break;
    case 'S':
      digitalWrite(PIN_DRV_S, HIGH);
      digitalWrite(PIN_REL_S, LOW);
      break;
    case 'W':
      digitalWrite(PIN_DRV_W, HIGH);
      digitalWrite(PIN_REL_W, LOW);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// setDamping — apply transducer shunt after TX burst
//   1. Verify DRV_x is LOW (forbidden if DRV_x HIGH + REL_x HIGH)
//   2. Wait nonOverlapUs (default 2 µs)
//   3. Set REL_x=HIGH
//   4. If dampingUs > 0: wait, then REL_x=LOW
//   5. If dampingUs == 0: leave REL_x HIGH (caller must clear later)
// Returns true on success, false if DRV_x was HIGH (forbidden).
// ---------------------------------------------------------------------------
static inline bool setDamping(char dir, uint32_t dampingUs, uint32_t nonOverlapUs = 2) {
  // Check DRV_x is LOW — FORBIDDEN to have DRV_x HIGH + REL_x HIGH
  uint8_t drvState = LOW;
  switch (dir) {
    case 'N': drvState = digitalRead(PIN_DRV_N); break;
    case 'E': drvState = digitalRead(PIN_DRV_E); break;
    case 'S': drvState = digitalRead(PIN_DRV_S); break;
    case 'W': drvState = digitalRead(PIN_DRV_W); break;
    default: return false;
  }
  if (drvState == HIGH) {
    return false;  // FORBIDDEN: would create DRV_x HIGH + REL_x HIGH
  }

  // Non-overlap delay (let TX_PULSE discharge path settle)
  if (nonOverlapUs > 0) {
    delayMicroseconds(nonOverlapUs);
  }

  // Apply damping (shunt transducer)
  switch (dir) {
    case 'N': digitalWrite(PIN_REL_N, HIGH); break;
    case 'E': digitalWrite(PIN_REL_E, HIGH); break;
    case 'S': digitalWrite(PIN_REL_S, HIGH); break;
    case 'W': digitalWrite(PIN_REL_W, HIGH); break;
    default: return false;
  }

  // Hold damping for specified interval, then clear
  if (dampingUs > 0) {
    delayMicroseconds(dampingUs);
    switch (dir) {
      case 'N': digitalWrite(PIN_REL_N, LOW); break;
      case 'E': digitalWrite(PIN_REL_E, LOW); break;
      case 'S': digitalWrite(PIN_REL_S, LOW); break;
      case 'W': digitalWrite(PIN_REL_W, LOW); break;
      default: break;
    }
  }
  // If dampingUs == 0, REL_x stays HIGH — caller must call clearDamping()

  return true;
}

// ---------------------------------------------------------------------------
// clearDamping — force REL_x LOW for a specific direction
// ---------------------------------------------------------------------------
static inline void clearDamping(char dir) {
  switch (dir) {
    case 'N': digitalWrite(PIN_REL_N, LOW); break;
    case 'E': digitalWrite(PIN_REL_E, LOW); break;
    case 'S': digitalWrite(PIN_REL_S, LOW); break;
    case 'W': digitalWrite(PIN_REL_W, LOW); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// isTxStateSafe — returns true if no channel has both DRV and REL HIGH
// (the FORBIDDEN state while TX_PULSE is active)
// ---------------------------------------------------------------------------
static inline bool isTxStateSafe() {
  if (digitalRead(PIN_DRV_N) == HIGH && digitalRead(PIN_REL_N) == HIGH) return false;
  if (digitalRead(PIN_DRV_E) == HIGH && digitalRead(PIN_REL_E) == HIGH) return false;
  if (digitalRead(PIN_DRV_S) == HIGH && digitalRead(PIN_REL_S) == HIGH) return false;
  if (digitalRead(PIN_DRV_W) == HIGH && digitalRead(PIN_REL_W) == HIGH) return false;
  return true;
}

// ---------------------------------------------------------------------------
// activeDrvCount — count DRV pins that are HIGH
// ---------------------------------------------------------------------------
static inline int activeDrvCount() {
  int count = 0;
  if (digitalRead(PIN_DRV_N) == HIGH) count++;
  if (digitalRead(PIN_DRV_E) == HIGH) count++;
  if (digitalRead(PIN_DRV_S) == HIGH) count++;
  if (digitalRead(PIN_DRV_W) == HIGH) count++;
  return count;
}

// ---------------------------------------------------------------------------
// isRelHigh — check if a specific REL (damp/shunt) pin is HIGH
// ---------------------------------------------------------------------------
static inline bool isRelHigh(char dir) {
  switch (dir) {
    case 'N': return digitalRead(PIN_REL_N) == HIGH;
    case 'E': return digitalRead(PIN_REL_E) == HIGH;
    case 'S': return digitalRead(PIN_REL_S) == HIGH;
    case 'W': return digitalRead(PIN_REL_W) == HIGH;
    default: return false;
  }
}