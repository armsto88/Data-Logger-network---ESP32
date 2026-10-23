// FieldHub v2 r6 bring-up: ADS1015 analogue inputs (Stage H step 4).
//
// r6 topology (netlist-verified):
//   ADS1015 U30 at 0x48 on SDA2/SCL2, the 5 V side of PCA9306 U29; the 3.3 V
//   side joins SDA1/SCL1 through R15/R16 33R. VDD = 5V_SYS through R28 10R.
//   Each input: port -> USBLC6 (D10/D11) -> 1k (R29-R32) -> AINx, with 10 nF
//   and a 1 MOhm pull-down at the pin, so an open input reads ~0 V.
//     AIN0 = CN15.4   AIN1 = CN15.3   AIN2 = CN16.3   AIN3 = CN16.4
//   Port pins 1/2 are 5V_SEN/GND. 5V_SEN (U4 TPS2553) has EN tied to IN and
//   FAULT unrouted: always on with 5V_SYS, not switchable or observable.
//
// Register-level driver on purpose: the config readback proves PGA/mode
// bits land, which a library hides. PGA is +/-6.144 V (3 mV/LSB), as the r6
// review requires for 0-5 V probes. That range does not permit inputs above
// VDD + 0.3 V.
//
// Serial commands after the run:
//   r              re-run the suite
//   c              stream all four channels every 500 ms (Enter stops)
//   p <ch> <mV>    reference point: apply a known voltage to AIN<ch>, measure
//                  it with a DMM at the port, and enter that reading
//
// Build: pio run -e mothership-v2-bringup-ads1015 -t upload
// Run:   scripts/hub_bringup.py run <BOARD> ads1015

#include <Arduino.h>
#include <Wire.h>

#include "hub_bringup_report.h"

#ifndef ADS1015_ADDR
#define ADS1015_ADDR 0x48
#endif
#ifndef ADS_NOISE_SAMPLES
#define ADS_NOISE_SAMPLES 64
#endif
#ifndef ADS_MAX_PP_LSB
#define ADS_MAX_PP_LSB 4  // 12 mV p-p; the 5 V boost sits next to the ADC
#endif

static constexpr float kMvPerLsb = 3.0f;  // 6.144 V / 2048
static const char* kPort[4] = {"CN15.4", "CN15.3", "CN16.3", "CN16.4"};

static constexpr uint16_t kCfgPga6144 = 0x0000;
static constexpr uint16_t kCfgPga4096 = 0x0200;
static constexpr uint16_t kCfgSingle = 0x0100;
static constexpr uint16_t kCfgDr1600 = 0x0080;
static constexpr uint16_t kCfgCompOff = 0x0003;

static bool writeReg(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(ADS1015_ADDR);
  Wire.write(reg);
  Wire.write(v >> 8);
  Wire.write(v & 0xFF);
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t reg, uint16_t* v) {
  Wire.beginTransmission(ADS1015_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADS1015_ADDR, (uint8_t)2) != 2) return false;
  *v = (Wire.read() << 8) | Wire.read();
  return true;
}

static uint16_t configFor(uint8_t ch, uint16_t pga) {
  return ((0x4 | ch) << 12) | pga | kCfgSingle | kCfgDr1600 | kCfgCompOff;
}

// One single-shot conversion, result in LSB (12-bit signed). False on I2C
// error or if OS never reports completion.
static bool convert(uint8_t ch, int16_t* code) {
  if (!writeReg(0x01, 0x8000 | configFor(ch, kCfgPga6144))) return false;
  const uint32_t t0 = micros();
  uint16_t cfg = 0;
  do {
    if (!readReg(0x01, &cfg)) return false;
    if (cfg & 0x8000) break;
  } while (micros() - t0 < 5000);
  if (!(cfg & 0x8000)) return false;
  uint16_t raw = 0;
  if (!readReg(0x00, &raw)) return false;
  *code = static_cast<int16_t>(raw) >> 4;
  return true;
}

struct ChannelStats {
  bool ok;
  float meanMv;
  int16_t minCode, maxCode;
};

static ChannelStats sample(uint8_t ch, int n) {
  ChannelStats s = {true, 0, 32767, -32768};
  long sum = 0;
  for (int i = 0; i < n; i++) {
    int16_t c = 0;
    if (!convert(ch, &c)) {
      s.ok = false;
      return s;
    }
    sum += c;
    s.minCode = min(s.minCode, c);
    s.maxCode = max(s.maxCode, c);
  }
  s.meanMv = static_cast<float>(sum) / n * kMvPerLsb;
  return s;
}

static void runSuite() {
  hubResetCounts();
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(50);

  Wire.beginTransmission(ADS1015_ADDR);
  const uint8_t ack = Wire.endTransmission();
  HUB_CHECK("ADS_ACK_0x48", ack == 0, "endTransmission=%u (needs 5V_SYS for U29/U30)", ack);
  if (ack != 0) {
    hubSummary();
    return;
  }

  uint16_t cfg = 0;
  if (readReg(0x01, &cfg)) {
    Serial.printf("  config at start = 0x%04X (power-on default 0x8583)\n", cfg);
  }

  // Two distinct non-starting patterns must read back exactly (OS reads 1 = idle).
  bool rwOk = true;
  uint16_t rb1 = 0, rb2 = 0;
  const uint16_t p1 = configFor(1, kCfgPga4096), p2 = configFor(2, kCfgPga6144);
  rwOk &= writeReg(0x01, p1) && readReg(0x01, &rb1) && rb1 == (0x8000 | p1);
  rwOk &= writeReg(0x01, p2) && readReg(0x01, &rb2) && rb2 == (0x8000 | p2);
  HUB_CHECK("ADS_CONFIG_RW", rwOk, "wrote 0x%04X/0x%04X, read 0x%04X/0x%04X", p1, p2, rb1, rb2);

  for (uint8_t ch = 0; ch < 4; ch++) {
    char name[24];
    const ChannelStats s = sample(ch, ADS_NOISE_SAMPLES);
    snprintf(name, sizeof(name), "ADS_A%u_CONVERT", ch);
    // Inputs cannot exceed ~5.3 V (VDD + 0.3), i.e. ~1766 LSB; a reading at
    // either rail means a broken input path, not a sensor value.
    const bool railed = s.maxCode >= 2047 || s.minCode <= -2048;
    HUB_CHECK(name, s.ok && !railed, "%s: %s, mean %.0f mV (%d..%d LSB)", kPort[ch],
              !s.ok ? "conversion timeout/I2C error" : railed ? "RAILED" : "ok", s.meanMv, s.minCode,
              s.maxCode);
    if (!s.ok) continue;

    const int pp = s.maxCode - s.minCode;
    snprintf(name, sizeof(name), "ADS_A%u_NOISE", ch);
    HUB_CHECK(name, pp <= ADS_MAX_PP_LSB, "%s: %d LSB p-p over %d samples (%.0f mV, limit %d LSB)",
              kPort[ch], pp, ADS_NOISE_SAMPLES, pp * kMvPerLsb, ADS_MAX_PP_LSB);

    if (fabsf(s.meanMv) > 15.0f) {
      snprintf(name, sizeof(name), "ADS_A%u_OPEN", ch);
      HUB_WARN(name, "%s reads %.0f mV, not ~0 V: source connected, or check the 1M pull-down",
               kPort[ch], s.meanMv);
    }
  }
  hubSummary();
}

static void pointCheck(uint8_t ch, long expectMv) {
  if (expectMv > 5300) {
    Serial.println("Refusing: above VDD + 0.3 V (5V_SYS ~5.0 V) exceeds the ADS1015 input limit.");
    return;
  }
  hubResetCounts();
  const ChannelStats s = sample(ch, ADS_NOISE_SAMPLES);
  const float tol = fabsf(expectMv) * 0.01f + 2 * kMvPerLsb;  // 1 % + 2 LSB
  char name[32];
  snprintf(name, sizeof(name), "ADS_A%u_POINT_%ldmV", ch, expectMv);
  HUB_CHECK(name, s.ok && fabsf(s.meanMv - expectMv) <= tol,
            "%s: read %.1f mV vs DMM %ld mV (err %+.1f mV, tol %.1f mV)", kPort[ch], s.meanMv,
            expectMv, s.meanMv - expectMv, tol);
  hubSummary();
}

static void streamLoop() {
  Serial.println("Streaming A0..A3 in mV (Enter stops)");
  while (!Serial.available()) {
    float mv[4];
    for (uint8_t ch = 0; ch < 4; ch++) {
      const ChannelStats s = sample(ch, 8);
      mv[ch] = s.ok ? s.meanMv : NAN;
    }
    Serial.printf("  t=%lus A0 %6.0f  A1 %6.0f  A2 %6.0f  A3 %6.0f mV\n", millis() / 1000, mv[0],
                  mv[1], mv[2], mv[3]);
    delay(500);
  }
  while (Serial.available()) Serial.read();
}

static void printHelp() {
  Serial.println("Commands: r = re-run | c = stream | p <ch> <mV> = reference point check");
}

void setup() {
  hubHoldPower();
  Serial.begin(115200);
  delay(300);
  hubBanner("ADS1015 analogue inputs");
  Serial.println("AIN0=CN15.4 AIN1=CN15.3 AIN2=CN16.3 AIN3=CN16.4, PGA +/-6.144 V, 3 mV/LSB");
  hubWaitForStart();
  runSuite();
  printHelp();
}

void loop() {
  char line[32];
  if (!hubReadLine(line, sizeof(line))) {
    delay(10);
    return;
  }
  if (line[0] == 'r') {
    runSuite();
  } else if (line[0] == 'c') {
    streamLoop();
  } else if (line[0] == 'p') {
    int ch = -1;
    long mv = 0;
    if (sscanf(line + 1, "%d %ld", &ch, &mv) == 2 && ch >= 0 && ch <= 3) pointCheck(ch, mv);
    else Serial.println("p <ch 0-3> <mV>");
  } else if (line[0] != '\0') {
    printHelp();
  }
}
