// Node V2 Main Systems Bring-up — combined menu-driven sketch
// Tests all V2 main systems except ultrasonic in a single flash.
// Follows patterns from firmware/nodes/bringup/ sketches.

#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_SHTC3.h>
#include <Adafruit_ADS1X15.h>
#include <RTClib.h>

// ── Pin definitions (all #ifndef guarded for build-flag override) ──────────

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

#ifndef ADS1015_ADDR
#define ADS1015_ADDR 0x48
#endif

#ifndef RTC_ADDR
#define RTC_ADDR 0x68
#endif

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif

#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 35
#endif

#ifndef BAT_ADC_VREF
#define BAT_ADC_VREF 3.3f
#endif

#ifndef BAT_DIVIDER_SCALE
#define BAT_DIVIDER_SCALE 3.62f
#endif

#ifndef BAT_ADC_SAMPLES
#define BAT_ADC_SAMPLES 32
#endif

#ifndef TX_BURST_PWM_PIN
#define TX_BURST_PWM_PIN 25
#endif

#ifndef TX_22V_EN_N_PIN
#define TX_22V_EN_N_PIN 5
#endif

#ifndef RX_EN_N_PIN
#define RX_EN_N_PIN 4
#endif

#ifndef TOF_EDGE_PIN
#define TOF_EDGE_PIN 34
#endif

#ifndef ALARM_INTERVAL_S
#define ALARM_INTERVAL_S 10
#endif

#ifndef ALARM_HOLD_MS
#define ALARM_HOLD_MS 8000
#endif

// ── Global objects ─────────────────────────────────────────────────────────

static Adafruit_ADS1015 ads;
static RTC_DS3231 rtc;
static bool adsInitialised = false;
static bool rtcInitialised = false;

// ── Helpers ────────────────────────────────────────────────────────────────

static void printMenu() {
  Serial.println();
  Serial.println("=== Node V2 Main Systems Bring-up ===");
  Serial.printf("Pins: SDA=%d SCL=%d PWR_HOLD=%d BAT_ADC=%d"
                 " TX_BURST_PWM=%d TX_22V_EN_N=%d RX_EN_N=%d TOF_EDGE=%d\n",
                 I2C_SDA_PIN, I2C_SCL_PIN, PWR_HOLD_PIN, BAT_ADC_PIN,
                 TX_BURST_PWM_PIN, TX_22V_EN_N_PIN, RX_EN_N_PIN, TOF_EDGE_PIN);
  Serial.printf("I2C: MUX=0x%02X SHTC3=0x%02X ADS1015=0x%02X RTC=0x%02X\n",
                 MUX_ADDR, SHTC3_ADDR, ADS1015_ADDR, RTC_ADDR);
  Serial.println();
  Serial.println("  1 = I2C bus scan");
  Serial.println("  2 = Mux channel sweep + SHTC3 read");
  Serial.println("  3 = Battery voltage (IO35 ADC)");
  Serial.println("  4 = PWR_HOLD latch toggle (15s ON / 15s OFF)");
  Serial.println("  5 = TX_BURST_PWM gate toggle (1s ON / 2s OFF)");
  Serial.println("  6 = TX_22V_EN_N boost enable toggle (5s ON / 5s OFF)");
  Serial.println("  7 = RX_EN_N + AND-gate verification");
  Serial.println("  8 = ADS1015 analog read (4 channels)");
  Serial.println("  9 = DS3231 RTC alarm (10s interval, 8s hold)");
  Serial.println("  r = Repeat this menu");
  Serial.println("  h = Hold PWR_HOLD HIGH (keep board alive)");
  Serial.println("  l = Release PWR_HOLD (board may power off)");
  Serial.println();
}

// ── Test 1: I2C bus scan ───────────────────────────────────────────────────

static void testI2cScan() {
  Serial.println("--- I2C bus scan start ---");
  uint8_t found = 0;

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("Found device at 0x%02X\n", addr);
      found++;
    } else if (err == 4) {
      Serial.printf("Unknown error at 0x%02X\n", addr);
    }
  }

  if (found == 0) {
    Serial.println("No I2C devices found");
  }
  Serial.printf("Scan done — %u device(s) found\n", found);
  Serial.printf("Expected: 0x%02X(MUX) 0x%02X(SHTC3) 0x%02X(RTC) 0x%02X(ADS)\n",
                 MUX_ADDR, SHTC3_ADDR, RTC_ADDR, ADS1015_ADDR);
  Serial.println("--- I2C bus scan end ---");
}

// ── Test 2: Mux channel sweep + SHTC3 ─────────────────────────────────────

static bool muxSelectChannel(uint8_t channel) {
  if (channel > 7) return false;
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)(1U << channel));
  return Wire.endTransmission() == 0;
}

static void muxDisableAll() {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
}

static bool probeShtc3(float &tempC, float &rh) {
  SHTC3 sensor;
  SHTC3_Status_TypeDef status = sensor.begin(Wire);
  if (status != SHTC3_Status_Nominal) return false;

  status = sensor.setMode(SHTC3_CMD_CSE_TF_NPM);
  if (status != SHTC3_Status_Nominal) return false;

  status = sensor.update();
  if (status != SHTC3_Status_Nominal) return false;
  if (!sensor.passTcrc || !sensor.passRHcrc) return false;

  tempC = sensor.toDegC();
  rh = sensor.toPercent();
  return true;
}

static void testMuxShtc3() {
  Serial.println("--- Mux channel sweep + SHTC3 start ---");

  for (uint8_t ch = 0; ch < 8; ch++) {
    Serial.printf("CH%u: ", ch);
    if (!muxSelectChannel(ch)) {
      Serial.println("mux select failed");
      continue;
    }
    delay(2);

    float tC = 0.0f, rh = 0.0f;
    if (probeShtc3(tC, rh)) {
      Serial.printf("SHTC3 @0x%02X OK | T=%.2f C RH=%.2f %%\n", SHTC3_ADDR, tC, rh);
    } else {
      Serial.printf("no valid SHTC3 response @0x%02X\n", SHTC3_ADDR);
    }
  }

  muxDisableAll();
  Serial.println("--- Mux channel sweep + SHTC3 end ---");
}

// ── Test 3: Battery voltage ────────────────────────────────────────────────

static uint16_t readAdcAvg() {
  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; ++i) {
    sum += static_cast<uint16_t>(analogRead(BAT_ADC_PIN));
    delay(2);
  }
  return static_cast<uint16_t>(sum / BAT_ADC_SAMPLES);
}

static void testBattery() {
  Serial.println("--- Battery voltage start ---");
  Serial.printf("[CFG] ADC_PIN=%d SAMPLES=%d VREF=%.4f DIV_SCALE=%.4f\n",
                 BAT_ADC_PIN, BAT_ADC_SAMPLES, BAT_ADC_VREF, BAT_DIVIDER_SCALE);
  Serial.println("[NOTE] V2 uses 47k/47k divider — calibrate BAT_DIVIDER_SCALE against DMM.");

  uint16_t raw = readAdcAvg();
  float pinV = (static_cast<float>(raw) / 4095.0f) * BAT_ADC_VREF;
  float batV = pinV * BAT_DIVIDER_SCALE;

  Serial.printf("BAT raw=%4u pin_V=%.4f batt_V=%.4f\n", raw, pinV, batV);
  Serial.println("--- Battery voltage end ---");
}

// ── Test 4: PWR_HOLD latch toggle ──────────────────────────────────────────

static void testPwrHold() {
  Serial.println("--- PWR_HOLD latch toggle start ---");
  Serial.println("Drive PWR_HOLD HIGH for 15s, then LOW for 15s.");
  Serial.println("Probe VSYS with DMM.");

  digitalWrite(PWR_HOLD_PIN, HIGH);
  Serial.printf("t=%lu ms | PWR_HOLD=HIGH (ON)\n", millis());
  delay(15000);

  digitalWrite(PWR_HOLD_PIN, LOW);
  Serial.printf("t=%lu ms | PWR_HOLD=LOW (OFF)\n", millis());
  Serial.println("[WARN] Board may power off during the 15s OFF window!");
  delay(15000);

  // Re-latch to keep board alive after test
  digitalWrite(PWR_HOLD_PIN, HIGH);
  Serial.printf("t=%lu ms | PWR_HOLD=HIGH (re-latched)\n", millis());
  Serial.println("--- PWR_HOLD latch toggle end ---");
}

// ── Test 5: TX_BURST_PWM gate toggle ───────────────────────────────────────

static void testTxBurstPwm() {
  Serial.println("--- TX_BURST_PWM gate toggle start ---");
  Serial.println("Drive TX_BURST_PWM HIGH for 1s, LOW for 2s.");
  Serial.println("Probe 22V rail. Note: this only tests the burst PWM path, not the 22V enable.");

  digitalWrite(TX_BURST_PWM_PIN, HIGH);
  Serial.printf("t=%lu ms | TX_BURST_PWM=HIGH\n", millis());
  delay(1000);

  digitalWrite(TX_BURST_PWM_PIN, LOW);
  Serial.printf("t=%lu ms | TX_BURST_PWM=LOW\n", millis());
  delay(2000);

  Serial.println("--- TX_BURST_PWM gate toggle end ---");
}

// ── Test 6: TX_22V_EN_N boost enable toggle ───────────────────────────────

static void testTx22vEnable() {
  Serial.println("--- TX_22V_EN_N boost enable toggle start ---");
  Serial.println("Drive TX_22V_EN_N LOW (boost ON) for 5s, then HIGH (boost OFF) for 5s.");
  Serial.println("Probe 22V_SYS and EN_22 test points.");
  Serial.println("Note: LOW on TX_22V_EN_N = HIGH on EN_22 = boost enabled (inverter).");

  digitalWrite(TX_22V_EN_N_PIN, LOW);
  Serial.printf("t=%lu ms | TX_22V_EN_N=LOW (boost ON, EN_22=HIGH)\n", millis());
  delay(5000);

  digitalWrite(TX_22V_EN_N_PIN, HIGH);
  Serial.printf("t=%lu ms | TX_22V_EN_N=HIGH (boost OFF, EN_22=LOW)\n", millis());
  delay(5000);

  Serial.println("--- TX_22V_EN_N boost enable toggle end ---");
}

// ── Test 7: RX_EN_N + AND-gate verification ───────────────────────────────

static void testRxEnableAndGate() {
  Serial.println("--- RX_EN_N + AND-gate verification start ---");
  bool pass = true;

  // Step 1: RX_EN_N HIGH → mux disabled, RX_WINDOW_EN expected LOW
  digitalWrite(RX_EN_N_PIN, HIGH);
  delay(1);
  int edgeHigh = digitalRead(TOF_EDGE_PIN);
  Serial.printf("RX_EN_N=HIGH → TOF_EDGE=%d (expect LOW, RX_WINDOW_EN=LOW)\n", edgeHigh);
  if (edgeHigh != LOW) {
    Serial.println("[FAIL] TOF_EDGE should be LOW when RX_EN_N is HIGH");
    pass = false;
  } else {
    Serial.println("[PASS] TOF_EDGE is LOW when RX_EN_N is HIGH");
  }

  // Step 2: RX_EN_N LOW → mux enabled, RX_WINDOW_EN expected HIGH
  digitalWrite(RX_EN_N_PIN, LOW);
  delay(1);
  int edgeLow = digitalRead(TOF_EDGE_PIN);
  Serial.printf("RX_EN_N=LOW  → TOF_EDGE=%d (RX_WINDOW_EN=HIGH, TOF_EDGE reflects COMP_RAW)\n", edgeLow);
  Serial.println("[INFO] TOF_EDGE should reflect COMP_RAW AND RX_WINDOW_EN.");
  Serial.println("       With no ultrasonic burst, COMP_RAW may be LOW or toggling.");

  // Step 3: Restore safe default
  digitalWrite(RX_EN_N_PIN, HIGH);
  delay(1);
  int edgeRestore = digitalRead(TOF_EDGE_PIN);
  Serial.printf("RX_EN_N=HIGH (restored) → TOF_EDGE=%d\n", edgeRestore);

  Serial.printf("--- RX_EN_N + AND-gate verification %s ---\n", pass ? "PASSED" : "FAILED");
}

// ── Test 8: ADS1015 analog read ────────────────────────────────────────────

static void testAds1015() {
  Serial.println("--- ADS1015 analog read start ---");

  if (!adsInitialised) {
    if (!ads.begin(ADS1015_ADDR, &Wire)) {
      Serial.printf("ADS1015 not found at 0x%02X. Check wiring.\n", ADS1015_ADDR);
      Serial.println("--- ADS1015 analog read end (FAIL) ---");
      return;
    }
    ads.setGain(GAIN_ONE);
    adsInitialised = true;
  }

  // Hold TX_BURST_PWM HIGH to keep 22V rail alive if sensors need it
  digitalWrite(TX_BURST_PWM_PIN, HIGH);
  Serial.println("[INFO] TX_BURST_PWM driven HIGH for ADS1015 read (22V rail enabled).");

  for (uint8_t ch = 0; ch < 4; ch++) {
    int16_t raw = ads.readADC_SingleEnded(ch);
    float volts = ads.computeVolts(raw);
    Serial.printf("A%u raw=%6d V=%.4f\n", ch, raw, volts);
  }

  digitalWrite(TX_BURST_PWM_PIN, LOW);
  Serial.println("[INFO] TX_BURST_PWM driven LOW (22V rail disabled).");
  Serial.println("--- ADS1015 analog read end ---");
}

// ── Test 9: DS3231 RTC alarm ───────────────────────────────────────────────

static uint8_t toBcd(uint8_t v) {
  return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

static bool rtcWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool rtcReadReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(static_cast<uint8_t>(RTC_ADDR), static_cast<uint8_t>(1));
  if (!Wire.available()) return false;
  value = Wire.read();
  return true;
}

static bool writeAlarm1Exact(const DateTime &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x07);  // Alarm1 seconds register
  Wire.write(toBcd(static_cast<uint8_t>(t.second())) & 0x7F);
  Wire.write(toBcd(static_cast<uint8_t>(t.minute())) & 0x7F);
  Wire.write(toBcd(static_cast<uint8_t>(t.hour())) & 0x3F);
  Wire.write(toBcd(static_cast<uint8_t>(t.day())) & 0x3F);
  return Wire.endTransmission() == 0;
}

static bool clearAlarmFlags() {
  uint8_t status = 0;
  if (!rtcReadReg(0x0F, status)) return false;
  status &= static_cast<uint8_t>(~0x03);  // clear A1F and A2F
  return rtcWriteReg(0x0F, status);
}

static bool enableAlarmInterrupt() {
  uint8_t ctrl = 0;
  if (!rtcReadReg(0x0E, ctrl)) return false;
  ctrl |= 0x04;  // INTCN=1
  ctrl |= 0x01;  // A1IE=1
  return rtcWriteReg(0x0E, ctrl);
}

static bool alarmFlagSet() {
  uint8_t status = 0;
  if (!rtcReadReg(0x0F, status)) return false;
  return (status & 0x01) != 0;
}

static void printTime(const char *prefix, const DateTime &t) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  Serial.print(prefix);
  Serial.println(buf);
}

static bool armNextAlarm(DateTime fromNow) {
  DateTime next = fromNow + TimeSpan(0, 0, 0, ALARM_INTERVAL_S);
  if (!writeAlarm1Exact(next)) return false;
  printTime("[A1] armed for ", next);
  return true;
}

static void testRtcAlarm() {
  Serial.println("--- DS3231 RTC alarm test start ---");
  Serial.println("[NOTE] On V2, RTC INT/SQW drives an NFET gate controlling the main power gate.");
  Serial.println("       It is NOT connected to a GPIO. This test only verifies I2C alarm setting/clearing.");

  if (!rtcInitialised) {
    if (!rtc.begin(&Wire)) {
      Serial.printf("DS3231 not found at 0x%02X\n", RTC_ADDR);
      Serial.println("--- DS3231 RTC alarm test end (FAIL) ---");
      return;
    }
    rtcInitialised = true;
  }

  // Set RTC to build time
  DateTime buildTime(F(__DATE__), F(__TIME__));
  rtc.adjust(buildTime);
  printTime("[RTC] set to build time: ", buildTime);

  // Clear and arm
  if (!clearAlarmFlags()) {
    Serial.println("[RTC] failed to clear alarm flags");
  }
  if (!enableAlarmInterrupt()) {
    Serial.println("[RTC] failed to enable INTCN/A1IE");
  }

  DateTime now = rtc.now();
  printTime("[RTC] now: ", now);
  if (!armNextAlarm(now)) {
    Serial.println("[RTC] failed to arm Alarm1");
    Serial.println("--- DS3231 RTC alarm test end (FAIL) ---");
    return;
  }

  Serial.printf("[CFG] ALARM_INTERVAL=%ds ALARM_HOLD=%dms\n", ALARM_INTERVAL_S, ALARM_HOLD_MS);
  Serial.println("[INFO] Polling A1F flag. Wait for alarm to fire...");

  // Poll for alarm (with timeout)
  unsigned long startMs = millis();
  bool fired = false;
  while (millis() - startMs < (unsigned long)(ALARM_INTERVAL_S + 5) * 1000UL) {
    if (alarmFlagSet()) {
      DateTime firedAt = rtc.now();
      printTime("[A1] fired at ", firedAt);
      fired = true;

      Serial.printf("[A1] holding active for %d ms before clear\n", ALARM_HOLD_MS);
      delay(ALARM_HOLD_MS);

      if (clearAlarmFlags()) {
        Serial.println("[A1] flag cleared");
      } else {
        Serial.println("[A1] clear failed");
      }

      DateTime afterClear = rtc.now();
      if (!armNextAlarm(afterClear)) {
        Serial.println("[A1] re-arm failed");
      }
      break;
    }
    delay(25);
  }

  if (!fired) {
    Serial.println("[A1] alarm did NOT fire within timeout — check RTC wiring");
  }

  Serial.println("--- DS3231 RTC alarm test end ---");
}

// ── setup / loop ───────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(800);

  // Latch power immediately
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);

  // Safe defaults for V2-specific pins
  pinMode(TX_22V_EN_N_PIN, OUTPUT);
  digitalWrite(TX_22V_EN_N_PIN, HIGH);  // boost OFF (active-low)

  pinMode(RX_EN_N_PIN, OUTPUT);
  digitalWrite(RX_EN_N_PIN, HIGH);  // mux disabled (active-low)

  pinMode(TX_BURST_PWM_PIN, OUTPUT);
  digitalWrite(TX_BURST_PWM_PIN, LOW);

  pinMode(TOF_EDGE_PIN, INPUT);  // read-only

  // ADC setup
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  // I2C init
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("=== Node V2 Main Systems Bring-up ===");
  Serial.println("Power latched. All V2 pins configured to safe defaults.");
  printMenu();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    // Consume any trailing newline/carriage-return
    while (Serial.available()) {
      char n = Serial.read();
      if (n != '\r' && n != '\n') {
        // Push back? No — just discard; single-char menu is sufficient.
      }
    }

    switch (c) {
      case '1': testI2cScan();       break;
      case '2': testMuxShtc3();      break;
      case '3': testBattery();       break;
      case '4': testPwrHold();       break;
      case '5': testTxBurstPwm();    break;
      case '6': testTx22vEnable();   break;
      case '7': testRxEnableAndGate(); break;
      case '8': testAds1015();       break;
      case '9': testRtcAlarm();      break;
      case 'r':
      case 'R': printMenu();         break;
      case 'h':
      case 'H':
        digitalWrite(PWR_HOLD_PIN, HIGH);
        Serial.println("PWR_HOLD driven HIGH — board stays alive.");
        break;
      case 'l':
      case 'L':
        Serial.println("[WARN] Releasing PWR_HOLD. Board may power off!");
        digitalWrite(PWR_HOLD_PIN, LOW);
        break;
      default:
        break;
    }

    // Re-print menu after each test (unless it was 'r' which already prints it)
    if (c != 'r' && c != 'R' && c != 'h' && c != 'H' && c != 'l' && c != 'L') {
      printMenu();
    }
  }
}