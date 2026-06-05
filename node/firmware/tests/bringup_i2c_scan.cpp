#include <Arduino.h>
#include <Wire.h>

// Default ESP32 I2C pins unless overridden by build flags.
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 21
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 22
#endif

static void runScan() {
  uint8_t found = 0;
  Serial.println("I2C scan start");

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.printf("Found device at 0x%02X\n", addr);
      found++;
    } else if (error == 4) {
      Serial.printf("Unknown error at 0x%02X\n", addr);
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found");
  }
  Serial.println("I2C scan done");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("ESP32-WROOM I2C scanner bring-up");
  Serial.printf("Using SDA=%d SCL=%d\n", I2C_SDA_PIN, I2C_SCL_PIN);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

void loop() {
  runScan();
  delay(5000);
}
