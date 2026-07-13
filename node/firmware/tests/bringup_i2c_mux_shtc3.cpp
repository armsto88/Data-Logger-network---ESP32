#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_SHTC3.h>

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif

#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif

#ifndef SHTC3_ADDR
#define SHTC3_ADDR 0x70
#endif

static bool mux_select_channel(uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)(1U << channel));
  return Wire.endTransmission() == 0;
}

static void mux_disable_all() {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
}

static bool probe_shtc3_with_library(float &tempC, float &rh, SHTC3_Status_TypeDef &statusOut) {
  SHTC3 sensor;

  statusOut = sensor.begin(Wire);
  if (statusOut != SHTC3_Status_Nominal) return false;

  // Use a clock-stretching mode for maximum compatibility across targets.
  statusOut = sensor.setMode(SHTC3_CMD_CSE_TF_NPM);
  if (statusOut != SHTC3_Status_Nominal) return false;

  statusOut = sensor.update();
  if (statusOut != SHTC3_Status_Nominal) return false;
  if (!sensor.passTcrc || !sensor.passRHcrc) return false;

  tempC = sensor.toDegC();
  rh = sensor.toPercent();
  return true;
}

static void scan_mux_channels_once() {
  Serial.println("--- MUX channel sweep start ---");

  for (uint8_t ch = 0; ch < 8; ch++) {
    Serial.printf("CH%u: ", ch);

    if (!mux_select_channel(ch)) {
      Serial.println("mux select failed");
      continue;
    }
    delay(2);

    float tC = 0.0f;
    float rh = 0.0f;
    SHTC3_Status_TypeDef st = SHTC3_Status_Error;
    if (probe_shtc3_with_library(tC, rh, st)) {
      Serial.printf("SHTC3 @0x%02X OK | T=%.2f C RH=%.2f %%\n", SHTC3_ADDR, tC, rh);
    } else {
      Serial.printf("no valid SHTC3 response @0x%02X | status=%d\n", SHTC3_ADDR, (int)st);
    }
  }

  mux_disable_all();
  Serial.println("--- MUX channel sweep end ---");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(800);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("ESP32-WROOM I2C mux SHTC3 test");
  Serial.printf("I2C SDA=%d SCL=%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.printf("MUX=0x%02X SHTC3=0x%02X\n", MUX_ADDR, SHTC3_ADDR);
  Serial.println("Move the same SHTC3 sensor between channels and re-check results.");
}

void loop() {
  scan_mux_channels_once();
  delay(3000);
}
