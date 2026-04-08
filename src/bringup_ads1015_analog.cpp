#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif

#ifndef ADS1015_ADDR
#define ADS1015_ADDR 0x48
#endif

#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 8
#endif

#ifndef TX_PWM_PIN
#define TX_PWM_PIN 25
#endif

#ifndef TX_PWM_ACTIVE_HIGH
#define TX_PWM_ACTIVE_HIGH 1
#endif

static const uint8_t kTxOnLevel = TX_PWM_ACTIVE_HIGH ? HIGH : LOW;

Adafruit_ADS1015 ads;

static float read_channel_avg_volts(uint8_t ch, int16_t &avgRaw) {
  long sum = 0;
  for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {
    sum += ads.readADC_SingleEnded(ch);
    delay(2);
  }

  avgRaw = (int16_t)(sum / SAMPLE_COUNT);
  return ads.computeVolts(avgRaw);
}

void setup() {
  Serial.begin(115200);
  delay(800);

  // Keep 22V rail enabled while validating ADC connector input selection.
  pinMode(TX_PWM_PIN, OUTPUT);
  digitalWrite(TX_PWM_PIN, kTxOnLevel);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  if (!ads.begin(ADS1015_ADDR, &Wire)) {
    Serial.println();
    Serial.println("ADS1015 not found. Check address, wiring, and power.");
    while (true) {
      delay(1000);
    }
  }

  // Gain = 1 gives approximately +/-4.096V full-scale.
  ads.setGain(GAIN_ONE);

  Serial.println();
  Serial.println("ESP32-WROOM ADS1015 analog bring-up");
  Serial.printf("22V EN pin=%d state=%s\n", TX_PWM_PIN, TX_PWM_ACTIVE_HIGH ? "ON(HIGH)" : "ON(LOW)");
  Serial.printf("I2C SDA=%d SCL=%d ADS1015=0x%02X\n", I2C_SDA_PIN, I2C_SCL_PIN, ADS1015_ADDR);
  Serial.printf("Gain=%d, averaging=%d samples/channel\n", (int)GAIN_ONE, SAMPLE_COUNT);
}

void loop() {
  int16_t raw0 = 0, raw1 = 0, raw2 = 0, raw3 = 0;
  float v0 = read_channel_avg_volts(0, raw0);
  float v1 = read_channel_avg_volts(1, raw1);
  float v2 = read_channel_avg_volts(2, raw2);
  float v3 = read_channel_avg_volts(3, raw3);

  Serial.printf("A0 raw=%6d V=%1.4f | A1 raw=%6d V=%1.4f | A2 raw=%6d V=%1.4f | A3 raw=%6d V=%1.4f\n",
                raw0, v0,
                raw1, v1,
                raw2, v2,
                raw3, v3);

  delay(1000);
}
