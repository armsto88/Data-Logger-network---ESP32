#include <Arduino.h>

static unsigned long counter = 0;

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("ESP32-WROOM serial counter bring-up");
}

void loop() {
  Serial.print("count=");
  Serial.println(counter++);
  delay(1000);
}
