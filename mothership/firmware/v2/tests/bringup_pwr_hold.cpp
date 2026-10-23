// Mothership V1 bringup: Power hold control
// Validates PWR_HOLD (GPIO26) can keep VSYS rail active.
// Pattern: ON 15 s, OFF 15 s, repeat. Probe VSYS rail while toggling.

#include <Arduino.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif

#ifndef PWR_HOLD_ON_MS
#define PWR_HOLD_ON_MS 15000
#endif

#ifndef PWR_HOLD_OFF_MS
#define PWR_HOLD_OFF_MS 15000
#endif

void setup() {
  // CRITICAL: assert PWR_HOLD immediately so the board stays powered
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 PWR_HOLD Bring-up ===");
  Serial.printf("PWR_HOLD pin = GPIO%d (active HIGH)\n", PIN_PWR_HOLD);
  Serial.printf("Pattern: ON %d ms, OFF %d ms\n", PWR_HOLD_ON_MS, PWR_HOLD_OFF_MS);
  Serial.println("Probe VSYS and 3V3_SYS rails while states toggle.");
  Serial.println("Board should stay powered during ON phase and cut power during OFF phase.");
}

void loop() {
  digitalWrite(PIN_PWR_HOLD, HIGH);
  Serial.printf("t=%lu ms | PWR_HOLD=ON  (board should stay powered)\n", millis());
  delay(PWR_HOLD_ON_MS);

  Serial.printf("t=%lu ms | PWR_HOLD=OFF (board will lose power in ~seconds)\n", millis());
  Serial.println(">>> If you can read this, the OFF phase did not cut power. Check P-FET gate.");
  digitalWrite(PIN_PWR_HOLD, LOW);
  delay(PWR_HOLD_OFF_MS);
}