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
  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sum += analogRead(PIN_BATTERY_ADC);
  }
  float avg = static_cast<float>(sum) / BAT_ADC_SAMPLES;
  // V_bat = V_adc × (R1 + R2) / R2
  // V_adc = avg × VREF / ADC_MAX
  // V_bat = avg × VREF × (R1 + R2) / (R2 × ADC_MAX)
  float vBat = avg * BAT_ADC_VREF * (BAT_DIVIDER_R1 + BAT_DIVIDER_R2) / (BAT_DIVIDER_R2 * BAT_ADC_MAX);
  return vBat;
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
