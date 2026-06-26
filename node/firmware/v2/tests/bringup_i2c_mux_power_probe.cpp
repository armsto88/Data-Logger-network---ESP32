#include <Arduino.h>
#include <Wire.h>

#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif

#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif

#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif

#ifndef MUX_CHANNEL_DWELL_MS
#define MUX_CHANNEL_DWELL_MS 15000
#endif

#ifndef MUX_ALL_OFF_DWELL_MS
#define MUX_ALL_OFF_DWELL_MS 5000
#endif

#ifndef MUX_FIXED_CHANNEL
#define MUX_FIXED_CHANNEL -1
#endif

#ifndef TX_PWM_PIN
#define TX_PWM_PIN 25
#endif

#ifndef TX_PWM_ACTIVE_HIGH
#define TX_PWM_ACTIVE_HIGH 1
#endif

static const uint8_t kTxOnLevel = TX_PWM_ACTIVE_HIGH ? HIGH : LOW;

static bool mux_write_mask(uint8_t mask) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(mask);
  return Wire.endTransmission() == 0;
}

static bool mux_select_channel(uint8_t channel) {
  if (channel > 7) return false;
  return mux_write_mask((uint8_t)(1U << channel));
}

static void mux_all_off() {
  mux_write_mask(0x00);
}

void setup() {
  Serial.begin(115200);
  delay(800);

  // Hold the 22V stage enabled while probing mux channel power.
  pinMode(TX_PWM_PIN, OUTPUT);
  digitalWrite(TX_PWM_PIN, kTxOnLevel);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("ESP32-WROOM I2C mux power probe");
  Serial.printf("I2C SDA=%d SCL=%d MUX=0x%02X\n", I2C_SDA_PIN, I2C_SCL_PIN, MUX_ADDR);
  Serial.printf("22V EN pin=%d state=%s\n", TX_PWM_PIN, TX_PWM_ACTIVE_HIGH ? "ON(HIGH)" : "ON(LOW)");
  Serial.printf("Channel dwell=%d ms, all-off dwell=%d ms\n", MUX_CHANNEL_DWELL_MS, MUX_ALL_OFF_DWELL_MS);
  if (MUX_FIXED_CHANNEL >= 0 && MUX_FIXED_CHANNEL <= 7) {
    Serial.printf("Fixed-channel mode: CH%d\n", MUX_FIXED_CHANNEL);
  } else {
    Serial.println("Sweep mode: CH0..CH7");
  }
}


void loop() {
  if (MUX_FIXED_CHANNEL >= 0 && MUX_FIXED_CHANNEL <= 7) {
    bool ok = mux_select_channel((uint8_t)MUX_FIXED_CHANNEL);
    Serial.printf("CH%d %s | probe power now\n", MUX_FIXED_CHANNEL, ok ? "ON" : "select failed");
    delay(MUX_CHANNEL_DWELL_MS);

    mux_all_off();
    Serial.println("ALL OFF | probe baseline");
    delay(MUX_ALL_OFF_DWELL_MS);
    return;
  }

  for (uint8_t ch = 0; ch < 8; ch++) {
    bool ok = mux_select_channel(ch);
    Serial.printf("CH%u %s | probe power now\n", ch, ok ? "ON" : "select failed");
    delay(MUX_CHANNEL_DWELL_MS);

    mux_all_off();
    Serial.println("ALL OFF | probe baseline");
    delay(MUX_ALL_OFF_DWELL_MS);
  }
}
