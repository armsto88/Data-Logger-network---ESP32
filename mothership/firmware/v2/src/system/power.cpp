#include "system/power.h"
#include "system/pins.h"

static bool gPwrHoldAsserted = false;

void powerInit() {
  // PWR_HOLD must be the first initialized output. Never drive it LOW during
  // startup: even a short low pulse can release the external power gate before
  // wake-source detection has completed.
  digitalWrite(PIN_PWR_HOLD, HIGH);
  pinMode(PIN_PWR_HOLD, OUTPUT);
  gPwrHoldAsserted = true;

  // Config latch sense (active LOW when latch is set)
  pinMode(PIN_CONFIG_WAKE, INPUT);

  // Config latch clear (pulse HIGH to clear)
  digitalWrite(PIN_CONFIG_CLEAR, LOW);
  pinMode(PIN_CONFIG_CLEAR, OUTPUT);

  // Status LED
  pinMode(PIN_CFG_LED, OUTPUT);
  digitalWrite(PIN_CFG_LED, LOW);

  // Battery ADC
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);  // match node attenuation for full 0-3.3V range
  analogSetAttenuation(ADC_11db);  // 0–3.6 V range

  // Modem power enable (start OFF)
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);

  // Modem power-good (input-only)
  pinMode(PIN_MODEM_PG, INPUT);

  // Modem PWRKEY (start LOW — NMOS gate)
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);

  // Modem STATUS
  pinMode(PIN_MODEM_STATUS, INPUT);

}

void assertPwrHold() {
  digitalWrite(PIN_PWR_HOLD, HIGH);
  gPwrHoldAsserted = true;
  Serial.println("[PWR] PWR_HOLD asserted — VSYS rail held on");
}

void releasePwrHold() {
  Serial.println("[PWR] PWR_HOLD releasing — board will power off");
  digitalWrite(PIN_CFG_LED, LOW);
  digitalWrite(PIN_PWR_HOLD, LOW);
  gPwrHoldAsserted = false;
  // Should not reach here — board powers off
}

bool readConfigWake() {
  return digitalRead(PIN_CONFIG_WAKE) == LOW;  // Active LOW
}

void clearConfigLatch() {
  digitalWrite(PIN_CONFIG_CLEAR, HIGH);
  delay(CONFIG_CLEAR_PULSE_MS);
  digitalWrite(PIN_CONFIG_CLEAR, LOW);
  delay(5);
  Serial.println("[PWR] Config latch cleared");
}

float readBatteryVoltage() {
  // Calibrated pin millivolts (eFuse Vref characterisation). The previous maths
  // scaled raw counts by an assumed 3.3 V full scale, but at ADC_11db the
  // ESP32's full scale is nearer 3.9 V, so it read ~14% low (3.18 V on a
  // 3.7 V cell) and sat under the 3.5 V upload / OTA battery guards.
  uint32_t sumMv = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sumMv += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  const float pinV = static_cast<float>(sumMv) / BAT_ADC_SAMPLES / 1000.0f;
  // V_bat = V_adc × (R1 + R2) / R2
  return pinV * (BAT_DIVIDER_R1 + BAT_DIVIDER_R2) / BAT_DIVIDER_R2;
}

void setLed(bool on) {
  digitalWrite(PIN_CFG_LED, on ? HIGH : LOW);
}

void toggleLed() {
  digitalWrite(PIN_CFG_LED, !digitalRead(PIN_CFG_LED));
}

bool isPwrHoldAsserted() {
  return gPwrHoldAsserted;
}

bool isConfigButtonPressed() {
  return readConfigWake();
}
