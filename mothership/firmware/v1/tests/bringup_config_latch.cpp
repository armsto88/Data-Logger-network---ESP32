// Mothership V1 bringup: Config latch read and clear
// Validates SN74LVC2G74DCUR config latch on GPIO32 (sense) and GPIO25 (clear).
// The config latch holds a button press until firmware explicitly clears it.

#include <Arduino.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef PIN_CONFIG_WAKE
#define PIN_CONFIG_WAKE 32
#endif
#ifndef PIN_CONFIG_CLEAR
#define PIN_CONFIG_CLEAR 25
#endif
#ifndef PIN_CFG_LED
#define PIN_CFG_LED 27
#endif
#ifndef CONFIG_CLEAR_PULSE_MS
#define CONFIG_CLEAR_PULSE_MS 20
#endif

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Config latch sense pin (active LOW when latch is set)
  pinMode(PIN_CONFIG_WAKE, INPUT);

  // Config latch clear pin (pulse HIGH to clear)
  pinMode(PIN_CONFIG_CLEAR, OUTPUT);
  digitalWrite(PIN_CONFIG_CLEAR, LOW);

  // Status LED
  pinMode(PIN_CFG_LED, OUTPUT);
  digitalWrite(PIN_CFG_LED, LOW);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Config Latch Bring-up ===");
  Serial.printf("CONFIG_WAKE  = GPIO%d (INPUT, active LOW)\n", PIN_CONFIG_WAKE);
  Serial.printf("CONFIG_CLEAR = GPIO%d (OUTPUT, pulse HIGH %dms)\n", PIN_CONFIG_CLEAR, CONFIG_CLEAR_PULSE_MS);
  Serial.printf("CFG_LED      = GPIO%d\n", PIN_CFG_LED);

  // Read initial latch state
  int wakeState = digitalRead(PIN_CONFIG_WAKE);
  Serial.printf("Initial CONFIG_WAKE = %s\n", wakeState == LOW ? "LOW (CONFIG WAKE DETECTED)" : "HIGH (no config wake)");

  if (wakeState == LOW) {
    Serial.println(">>> CONFIG WAKE DETECTED — clearing latch now");
    digitalWrite(PIN_CONFIG_CLEAR, HIGH);
    delay(CONFIG_CLEAR_PULSE_MS);
    digitalWrite(PIN_CONFIG_CLEAR, LOW);
    delay(5);

    int afterClear = digitalRead(PIN_CONFIG_WAKE);
    Serial.printf("After clear: CONFIG_WAKE = %s\n", afterClear == LOW ? "LOW (clear FAILED)" : "HIGH (clear OK)");
  } else {
    Serial.println("No config wake pending. Press config button to set latch, then reset to test.");
  }

  Serial.println();
  Serial.println("Toggling CFG_LED to show life...");
}

void loop() {
  // Blink LED to show board is alive
  digitalWrite(PIN_CFG_LED, HIGH);
  delay(500);
  digitalWrite(PIN_CFG_LED, LOW);
  delay(500);

  // Continuously poll latch state
  int wakeState = digitalRead(PIN_CONFIG_WAKE);
  if (wakeState == LOW) {
    Serial.printf("t=%lu ms | CONFIG_WAKE=LOW — latch is SET\n", millis());
    Serial.println("Clearing latch...");
    digitalWrite(PIN_CONFIG_CLEAR, HIGH);
    delay(CONFIG_CLEAR_PULSE_MS);
    digitalWrite(PIN_CONFIG_CLEAR, LOW);
    delay(5);

    int afterClear = digitalRead(PIN_CONFIG_WAKE);
    Serial.printf("After clear: CONFIG_WAKE = %s\n", afterClear == LOW ? "LOW (FAILED)" : "HIGH (OK)");
  }
}