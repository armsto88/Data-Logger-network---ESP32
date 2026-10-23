// FieldHub v2 (r6) bring-up: shared header for the hub bring-up sketches.
// Header-only, included from a test sketch in the same directory:
//   #include "hub_bringup_report.h"
//
// Output contract (same as node/firmware/tests/bringup_full_selftest.cpp, so
// scripts/hub_bringup.py and scripts/node_test_harness.sh read it the same way):
//   RESULT|<NAME>|PASS|<details>
//   RESULT|<NAME>|FAIL|<details>
//   RESULT|<NAME>|WARN|<details>          informational, never blocks
//   RESULT|SUMMARY|<pass>/<pass+fail>|OVERALL:PASS      (or OVERALL:FAIL)
//
// Every sketch waits for a newline before running its suite, so a host that
// opens the port after the post-flash reset still captures the whole run.

#pragma once

#include <Arduino.h>
#include "system/pins.h"

#ifndef HUB_BRINGUP_START_WAIT_MS
#define HUB_BRINGUP_START_WAIT_MS 15000
#endif

static uint16_t gHubPass = 0, gHubFail = 0, gHubWarn = 0;

// Hold the main rail before anything else. Preloading the output latch HIGH
// before pinMode avoids a LOW glitch on GPIO26 that could drop VSYS.
inline void hubHoldPower() {
  digitalWrite(PIN_PWR_HOLD, HIGH);
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);
}

inline void hubResult(const char* name, const char* verdict, const char* fmt, ...) {
  char detail[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  Serial.printf("RESULT|%s|%s|%s\n", name, verdict, detail);
}

#define HUB_CHECK(name, cond, ...)                                  \
  do {                                                              \
    if (cond) { ++gHubPass; hubResult(name, "PASS", __VA_ARGS__); } \
    else      { ++gHubFail; hubResult(name, "FAIL", __VA_ARGS__); } \
  } while (0)

#define HUB_WARN(name, ...) \
  do { ++gHubWarn; hubResult(name, "WARN", __VA_ARGS__); } while (0)

inline void hubResetCounts() { gHubPass = gHubFail = gHubWarn = 0; }

inline bool hubSummary() {
  const bool ok = gHubFail == 0 && gHubPass > 0;
  Serial.printf("RESULT|SUMMARY|%u/%u|OVERALL:%s\n",
                gHubPass, gHubPass + gHubFail, ok ? "PASS" : "FAIL");
  return ok;
}

inline void hubBanner(const char* title) {
  Serial.println();
  Serial.printf("=== FieldHub v2 r6 bring-up: %s ===\n", title);
  Serial.printf("build %s %s\n", __DATE__, __TIME__);
}

// Block until a newline arrives or the timeout expires. Returns true if the
// host started the run, false on timeout (the suite still runs). Text sent
// before the newline lands in cmd, so a sketch can offer start-up variants.
inline bool hubWaitForStart(char* cmd = nullptr, size_t cap = 0) {
  Serial.printf("Send Enter to start (auto-start in %u s)...\n",
                HUB_BRINGUP_START_WAIT_MS / 1000);
  size_t len = 0;
  if (cmd && cap) cmd[0] = '\0';
  const uint32_t t0 = millis();
  while (millis() - t0 < HUB_BRINGUP_START_WAIT_MS) {
    while (Serial.available()) {
      const char c = static_cast<char>(Serial.read());
      if (c == '\n') return true;
      if (c != '\r' && cmd && len + 1 < cap) {
        cmd[len++] = c;
        cmd[len] = '\0';
      }
    }
    delay(10);
  }
  return false;
}

// Read one '\n'-terminated command line without blocking. Returns true when a
// complete line is in buf (CR stripped, NUL-terminated).
inline bool hubReadLine(char* buf, size_t cap) {
  static size_t len = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      len = 0;
      return true;
    }
    if (len + 1 < cap) buf[len++] = c;
  }
  return false;
}

// Wait up to timeoutMs for cond() to become true, printing a prompt once.
// Used for operator-in-the-loop steps (short a pin, insert a card, ...).
template <typename F>
inline bool hubPromptUntil(const char* prompt, uint32_t timeoutMs, F cond) {
  Serial.printf("ACTION: %s (%u s)\n", prompt, timeoutMs / 1000);
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (cond()) return true;
    delay(5);
  }
  return false;
}
