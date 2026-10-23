// Mothership V1 bringup: Power hold control
// Validates PWR_HOLD (GPIO26) can keep VSYS rail active.
// Pattern: ON 15 s, OFF 15 s, repeat. Probe VSYS rail while toggling.
//
// Q2 (VSYS switch) is held on by ANY of: Q4 (PWR_HOLD), Q3 (INT_RTC, i.e. a
// DS3231 alarm flag with its interrupt enabled), Q40 (SW10 service + USB) or
// D1 (config button, while pressed). The ESP32 cannot see INT_RTC, so a flag
// left set by an earlier RTC test silently keeps the board on. Before
// toggling, this sketch reports and clears A1F/A2F and disables A1IE/A2IE,
// so the OFF phase tests PWR_HOLD alone. SW10 must be in RUN.

#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_SDA
#define PIN_SDA 21
#endif
#ifndef PIN_SCL
#define PIN_SCL 22
#endif
#ifndef DS3231_ADDR
#define DS3231_ADDR 0x68
#endif

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif

#ifndef PWR_HOLD_ON_MS
#define PWR_HOLD_ON_MS 15000
#endif

#ifndef PWR_HOLD_OFF_MS
#define PWR_HOLD_OFF_MS 15000
#endif

static int rtcRead(uint8_t reg) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)DS3231_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

static bool rtcWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

// Release INT_RTC: clear both alarm flags, disable both alarm interrupts.
static void releaseRtcWake() {
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  const int ctrl = rtcRead(0x0E), stat = rtcRead(0x0F);
  if (ctrl < 0 || stat < 0) {
    Serial.println("[RTC] DS3231 not readable: INT_RTC state unknown, OFF phase may not cut power");
    return;
  }
  const bool asserting = (ctrl & 0x04) && (((stat & 0x01) && (ctrl & 0x01)) || ((stat & 0x02) && (ctrl & 0x02)));
  Serial.printf("[RTC] before: control=0x%02X (INTCN=%d A2IE=%d A1IE=%d) status=0x%02X (A2F=%d A1F=%d) -> INT %s\n",
                ctrl, (ctrl >> 2) & 1, (ctrl >> 1) & 1, ctrl & 1, stat, (stat >> 1) & 1, stat & 1,
                asserting ? "ASSERTED (was holding VSYS on)" : "idle");
  rtcWrite(0x0E, ctrl & ~0x03);
  rtcWrite(0x0F, stat & ~0x03);
  Serial.printf("[RTC] after:  control=0x%02X status=0x%02X -> INT released\n", rtcRead(0x0E), rtcRead(0x0F));
}

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
  Serial.println("SW10 must be in RUN and the config button released.");
  releaseRtcWake();
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