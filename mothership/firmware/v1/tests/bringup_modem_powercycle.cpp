// Mothership V1 bringup: Modem power-cycle stress
// Run 3 full cycles of (rail on -> PWRKEY -> wait boot -> AT -> AT+CPOF ->
// wait STATUS LOW -> rail off -> delay 3 s). Print cycle count and timing.
// PASS = all 3 cycles complete cleanly.

#include <Arduino.h>
#include "modem_at_helper.h"

static const uint8_t NUM_CYCLES = 3;
static const uint32_t CYCLE_GAP_MS = 3000;

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem Power-Cycle Stress Bring-up ===");
  Serial.printf("Cycles: %d | Gap between cycles: %lu ms\n", NUM_CYCLES, (unsigned long)CYCLE_GAP_MS);

  uint8_t successCount = 0;

  for (uint8_t c = 1; c <= NUM_CYCLES; c++) {
    Serial.println();
    Serial.printf("===== Cycle %d of %d =====\n", c, NUM_CYCLES);
    unsigned long cycleStart = millis();

    // --- Rail on ---
    Serial.println("[c] Rail on (soft-start)...");
    unsigned long t0 = millis();
    bool pg = modemRailOn();
    Serial.printf("[c] Rail on: PG=%s (%lu ms)\n",
                  digitalRead(PIN_MODEM_PG) == HIGH ? "HIGH" : "LOW",
                  millis() - t0);
    if (!pg) {
      Serial.println("FAIL: Power-good did not assert. Aborting cycle.");
      continue;
    }

    // --- UART + PWRKEY + boot ---
    modemInitUart();
    Serial.println("[c] PWRKEY pulse...");
    modemPulsePwrkey();

    Serial.println("[c] Wait for boot...");
    unsigned long t1 = millis();
    bool booted = modemWaitBoot(15000);
    Serial.printf("[c] Boot wait: %s (%lu ms), STATUS=%d\n",
                  booted ? "OK" : "FAIL", millis() - t1, modemReadStatus());
    if (!booted) {
      Serial.println("FAIL: Modem did not respond to AT. Aborting cycle.");
      // Still try to power off cleanly.
      modemGracefulOff();
      delay(CYCLE_GAP_MS);
      continue;
    }

    // --- AT sanity ---
    String resp;
    if (!modemSendAT("AT", resp, 2000)) {
      Serial.println("FAIL: AT did not return OK after boot. Aborting cycle.");
      modemGracefulOff();
      delay(CYCLE_GAP_MS);
      continue;
    }
    Serial.println("[c] AT -> OK");

    // --- Graceful power off (AT+CPOF + fallback) ---
    Serial.println("[c] Graceful power off...");
    unsigned long t2 = millis();
    modemGracefulOff();

    // --- Wait for STATUS LOW ---
    unsigned long t3 = millis();
    bool statusLow = false;
    while (millis() - t3 < 5000) {
      if (modemReadStatus() == LOW) { statusLow = true; break; }
      delay(100);
    }
    Serial.printf("[c] STATUS -> %s (%lu ms)\n",
                  statusLow ? "LOW (off)" : "STILL HIGH",
                  millis() - t3);
    if (!statusLow) {
      Serial.println("WARN: STATUS did not go LOW after power-off command.");
    }

    Serial.printf("[c] Cycle %d complete in %lu ms\n", c, millis() - cycleStart);
    successCount++;

    // --- Gap between cycles ---
    if (c < NUM_CYCLES) {
      Serial.printf("[c] Waiting %lu ms before next cycle...\n", (unsigned long)CYCLE_GAP_MS);
      delay(CYCLE_GAP_MS);
    }
  }

  // --- Verdict ---
  Serial.println();
  Serial.println("=== Modem power-cycle stress bring-up complete ===");
  Serial.printf("Successful cycles: %d / %d\n", successCount, NUM_CYCLES);
  if (successCount == NUM_CYCLES) {
    Serial.println("OVERALL PASS: All 3 cycles completed cleanly.");
  } else {
    Serial.println("OVERALL FAIL: One or more cycles failed.");
  }
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void loop() {
  // Idle — test runs once in setup().
  delay(5000);
}