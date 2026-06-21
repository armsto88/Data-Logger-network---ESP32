// Mothership V1 bringup: Shared A7670G AT-command helper
// Header-only inline helpers used by the modem bring-up test sketches.
// Include this from a test sketch in the same directory:
//   #include "modem_at_helper.h"
//
// All pin/timing constants come from src/system/pins.h (via the shared
// -I $PROJECT_DIR/src build flag). They are #ifndef-guarded there, so this
// header does not redefine them. platformio.ini may override any of them
// with -D flags if needed.

#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "system/pins.h"

// ---------------------------------------------------------------------------
// UART setup
// ---------------------------------------------------------------------------

inline void modemInitUart() {
  // Serial2 on ESP32: begin(baud, config, RX_PIN, TX_PIN)
  // RX = PIN_MODEM_RX (16, ESP32 RX2 <- modem TXD)
  // TX = PIN_MODEM_TX (17, ESP32 TX2 -> modem RXD)
  Serial2.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  Serial2.setTimeout(2000);
  Serial2.clearWriteError();
  while (Serial2.available()) { Serial2.read(); }  // flush RX buffer
}

// ---------------------------------------------------------------------------
// Power rail control
// ---------------------------------------------------------------------------

// Forward declarations so ordering in this header does not matter.
inline bool modemSendAT(const char* cmd, String& response, uint32_t timeoutMs);
inline bool modemSendATRaw(const char* cmd, String& response, uint32_t timeoutMs);

inline bool modemRailOn() {
  // Set 4V_EN LOW before enabling output to minimise any glitch that could
  // briefly enable the TPS63020 and brown out VSYS. (The PWM soft-start that
  // follows ramps from 0 %, so a momentary LOW is harmless either way.)
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);

  pinMode(PIN_MODEM_PG, INPUT);  // input-only, external pull-up

  // PWM soft-start: ramp 4V_EN 0 -> 100% over 500 ms to limit inrush.
  ledcSetup(0, 1000, 10);        // channel 0, 1 kHz, 10-bit (0-1023)
  ledcAttachPin(PIN_4V_EN, 0);
  unsigned long ssStart = millis();
  while (millis() - ssStart < 500) {
    int duty = map((int)(millis() - ssStart), 0, 500, 0, 1023);
    ledcWrite(0, duty);
    delay(5);
  }
  // Switch to full DC HIGH.
  ledcDetachPin(PIN_4V_EN);
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, HIGH);

  // Wait for power-good (ESP_PG HIGH).
  unsigned long start = millis();
  while (millis() - start < MODEM_PG_TIMEOUT_MS) {
    if (digitalRead(PIN_MODEM_PG) == HIGH) {
      return true;
    }
    delay(10);
  }
  return false;  // power-good did not arrive in time
}

inline void modemRailOff() {
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);
}

// ---------------------------------------------------------------------------
// PWRKEY and STATUS
// ---------------------------------------------------------------------------

inline void modemPulsePwrkey() {
  // NMOS gate: HIGH = pulls PWRKEY low = "press", LOW = release.
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);   // release first
  delay(50);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);  // press
  delay(MODEM_PWRKEY_ON_MS);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);   // release
}

inline int modemReadStatus() {
  pinMode(PIN_MODEM_STATUS, INPUT);
  return digitalRead(PIN_MODEM_STATUS);
}

// Wait for the modem to boot: STATUS HIGH, or a successful AT handshake.
// Returns true if the modem appears to be booted.
inline bool modemWaitBoot(uint32_t timeoutMs) {
  unsigned long start = millis();

  // First, just wait for STATUS HIGH (give the modem time to pull it up).
  while (millis() - start < timeoutMs) {
    if (modemReadStatus() == HIGH) {
      // STATUS is high — modem is powered. Now confirm UART responds.
      break;
    }
    delay(100);
  }

  // Give UART a moment to settle, then try AT a few times.
  delay(500);
  unsigned long probeStart = millis();
  while (millis() - probeStart < (timeoutMs - (millis() - start))) {
    String resp;
    if (modemSendAT("AT", resp, 2000)) {
      return true;
    }
    delay(500);
  }
  return false;
}

// ---------------------------------------------------------------------------
// AT command send / receive
// ---------------------------------------------------------------------------

// Send cmd + "\r\n" on Serial2, collect response until "OK" or "ERROR" or timeout.
// Returns true if "OK" was seen in the response.
inline bool modemSendAT(const char* cmd, String& response, uint32_t timeoutMs) {
  while (Serial2.available()) { Serial2.read(); }  // flush
  Serial2.print(cmd);
  Serial2.print("\r\n");
  Serial2.flush();

  response = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      response += c;
      // Check for final result codes.
      if (response.indexOf("OK\r\n") >= 0) return true;
      if (response.indexOf("ERROR\r\n") >= 0) return false;
      if (response.indexOf("COMMAND NOT SUPPORT\r\n") >= 0) return false;
    }
    delay(5);
  }
  return false;  // timed out
}

// Same as modemSendAT but returns true if ANY response was received (for
// commands whose final line is not OK/ERROR, e.g. AT+CPOF -> POWER DOWN).
inline bool modemSendATRaw(const char* cmd, String& response, uint32_t timeoutMs) {
  while (Serial2.available()) { Serial2.read(); }  // flush
  Serial2.print(cmd);
  Serial2.print("\r\n");
  Serial2.flush();

  response = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      response += c;
    }
    delay(5);
  }
  return response.length() > 0;
}

// ---------------------------------------------------------------------------
// Graceful power-off
// ---------------------------------------------------------------------------

inline void modemGracefulOff() {
  // 1. Try AT+CPOF (graceful). Allow up to 5 s for "POWER DOWN".
  String resp;
  bool cpofOk = modemSendATRaw("AT+CPOF", resp, 5000);
  if (cpofOk) {
    Serial.println("[OFF] AT+CPOF responded:");
    Serial.print(resp);
  } else {
    Serial.println("[OFF] AT+CPOF no response — falling back to PWRKEY long-press");
  }

  // 2. Wait for STATUS LOW (up to 5 s).
  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (modemReadStatus() == LOW) break;
    delay(100);
  }

  // 3. If STATUS still HIGH, force PWRKEY long-press (>=2.5 s).
  if (modemReadStatus() == HIGH) {
    Serial.println("[OFF] STATUS still HIGH — PWRKEY long-press 2.5 s");
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    delay(2500);
    digitalWrite(PIN_MODEM_PWRKEY, LOW);
    delay(2000);
  }

  // 4. Disable the 4V rail.
  modemRailOff();
  Serial.println("[OFF] 4V_EN = LOW (rail off)");
}