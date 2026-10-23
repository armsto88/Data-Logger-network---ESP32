// FieldHub v2 r6 bring-up: root I2C bus, TCA9546A mux and sensor ports
// (Stage H steps 1-3, Stage G step 5 hot-plug).
//
// r6 topology (netlist-verified from the 2026-10-10 r6 EasyEDA source):
//   GPIO21 SDA1 / GPIO22 SCL1, F2 4.7k pull-ups to 3V3_SYS. Root devices:
//     0x48 ADS1015 (U30, behind PCA9306 U29 on the 5 V side, SDA2/SCL2)
//     0x68 DS3231M (U49)
//     0x71 TCA9546A (U33): A0 high via R160 0R, A1 low via R150 0R, A2 GND.
//          SJ4/SJ5 are alternative straps in parallel with those 0R links;
//          bridging either one shorts 3V3_SYS to GND. Both must stay open.
//   Mux channel -> port (33R series F4/F8/F6/F7, USBLC6 ESD D5/D8/D7/D9,
//   4.7k pull-ups to 3V3_SEN on each branch):
//     ch0 -> CN19   ch1 -> CN20   ch2 -> CN17   ch3 -> CN18
//   Port pinout: 1 = 3V3_SEN, 2 = GND, 3 = SDAx, 4 = SCLx.
//   3V3_SEN (U3 TPS2553) has EN tied to IN and FAULT unrouted: it is on
//   whenever 3V3_SYS is on and firmware can neither switch nor observe it.
//
// Root-bus devices also answer while a channel is selected (they are
// upstream of the mux), so per-channel results list only new addresses.
//
// Serial commands after the run:
//   r   re-run the suite        w   watch: rescan every 2 s, print changes
//                                   (hot-plug each harness; Enter stops)
//
// Build: pio run -e mothership-v2-bringup-i2c-mux -t upload
// Run:   scripts/hub_bringup.py run <BOARD> i2c-mux

#include <Arduino.h>
#include <Wire.h>

#include "hub_bringup_report.h"

#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif
#ifndef ADS1015_ADDR
#define ADS1015_ADDR 0x48
#endif
#ifndef DS3231_ADDR
#define DS3231_ADDR 0x68
#endif

static const char* kPortForChannel[4] = {"CN19", "CN20", "CN17", "CN18"};

struct ScanResult {
  uint8_t present[16];  // bitmap of 0x00..0x7F
  uint8_t count;
  uint8_t busErrors;    // endTransmission() codes other than 0 (ACK) / 2 (NACK)
  uint8_t lastError;
};

static bool isSet(const ScanResult& s, uint8_t a) { return s.present[a >> 3] & (1 << (a & 7)); }

static ScanResult scanBus() {
  ScanResult s = {};
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    const uint8_t err = Wire.endTransmission();
    if (err == 0) {
      s.present[a >> 3] |= 1 << (a & 7);
      s.count++;
    } else if (err != 2) {
      s.busErrors++;
      s.lastError = err;
    }
  }
  return s;
}

static void formatAddrs(const ScanResult& s, const ScanResult* exclude, char* out, size_t cap) {
  size_t n = 0;
  out[0] = '\0';
  for (uint8_t a = 0x08; a <= 0x77 && n + 6 < cap; a++) {
    if (isSet(s, a) && !(exclude && isSet(*exclude, a))) {
      n += snprintf(out + n, cap - n, "%s0x%02X", n ? " " : "", a);
    }
  }
  if (n == 0) snprintf(out, cap, "none");
}

static bool muxWrite(uint8_t mask) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(mask);
  return Wire.endTransmission() == 0;
}

static int muxRead() {
  if (Wire.requestFrom((uint8_t)MUX_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

static uint8_t crc8(const uint8_t* d, size_t n) {  // Sensirion: poly 0x31, init 0xFF
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

// Sensirion SHT4x command -> 6-byte reply with two CRC-protected words.
static bool sht4xCommand(uint8_t addr, uint8_t cmd, uint16_t waitMs, uint16_t* w0, uint16_t* w1) {
  Wire.beginTransmission(addr);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;
  delay(waitMs);
  if (Wire.requestFrom(addr, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  *w0 = (b[0] << 8) | b[1];
  *w1 = (b[3] << 8) | b[4];
  return true;
}

static void identifySht4x(uint8_t ch, uint8_t addr) {
  char name[24];
  snprintf(name, sizeof(name), "CH%u_SHT4X_0x%02X", ch, addr);
  uint16_t s0 = 0, s1 = 0, rawT = 0, rawRH = 0;
  const bool serialOk = sht4xCommand(addr, 0x89, 2, &s0, &s1);
  const bool measOk = serialOk && sht4xCommand(addr, 0xFD, 10, &rawT, &rawRH);
  if (!measOk) {
    HUB_CHECK(name, false, "%s: %s failed CRC/ack", kPortForChannel[ch],
              serialOk ? "measurement" : "serial-number read");
    return;
  }
  const float t = -45.0f + 175.0f * rawT / 65535.0f;
  const float rh = -6.0f + 125.0f * rawRH / 65535.0f;
  HUB_CHECK(name, t > -20.0f && t < 60.0f && rh > -1.0f && rh < 101.0f,
            "%s: serial %04X%04X, %.2f C, %.1f %%RH", kPortForChannel[ch], s0, s1, t, rh);
}

static void checkRtc() {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(0x0F);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)DS3231_ADDR, (uint8_t)4) != 4) {
    HUB_CHECK("RTC_REGISTERS", false, "could not read status/temperature registers");
    return;
  }
  const uint8_t status = Wire.read();
  Wire.read();  // 0x10 aging offset
  const int8_t tMsb = static_cast<int8_t>(Wire.read());
  const uint8_t tLsb = Wire.read();
  const float tC = tMsb + (tLsb >> 6) * 0.25f;
  HUB_CHECK("RTC_REGISTERS", tC > -10.0f && tC < 70.0f, "status=0x%02X die temp %.2f C", status, tC);
  if (status & 0x80) {
    HUB_WARN("RTC_OSF", "oscillator-stop flag set: time invalid until set (expected on a new board)");
  }
}

static ScanResult gRoot;
static ScanResult gChannel[4];

static void scanChannels(bool report) {
  for (uint8_t ch = 0; ch < 4; ch++) {
    muxWrite(1 << ch);
    gChannel[ch] = scanBus();
    if (!report) continue;

    char name[24], addrs[96];
    formatAddrs(gChannel[ch], &gRoot, addrs, sizeof(addrs));
    snprintf(name, sizeof(name), "CH%u_BUS_OK", ch);
    HUB_CHECK(name, gChannel[ch].busErrors == 0, "%s: %u bus errors (last code %u), downstream: %s",
              kPortForChannel[ch], gChannel[ch].busErrors, gChannel[ch].lastError, addrs);

    bool any = false;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      if (!isSet(gChannel[ch], a) || isSet(gRoot, a)) continue;
      any = true;
      if (a == 0x44 || a == 0x45) {
        identifySht4x(ch, a);
      } else if (a == 0x39) {
        snprintf(name, sizeof(name), "CH%u_AS734X_0x39", ch);
        HUB_WARN(name, "%s: acked (AS7341/AS7343 family); not register-checked", kPortForChannel[ch]);
      }
    }
    if (!any) {
      snprintf(name, sizeof(name), "CH%u_EMPTY", ch);
      HUB_WARN(name, "%s: nothing connected, port not exercised", kPortForChannel[ch]);
    }
  }
  muxWrite(0x00);
}

static void runSuite() {
  hubResetCounts();

  // Before the I2C peripheral owns the pins: lines must idle high, and must
  // stay high against the ~45k internal pull-down (proves F2 is fitted).
  Wire.end();
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(2);
  const int sdaIdle = digitalRead(PIN_SDA), sclIdle = digitalRead(PIN_SCL);
  HUB_CHECK("I2C_IDLE_HIGH", sdaIdle && sclIdle, "SDA1=%d SCL1=%d (0 = held low)", sdaIdle, sclIdle);
  pinMode(PIN_SDA, INPUT_PULLDOWN);
  pinMode(PIN_SCL, INPUT_PULLDOWN);
  delay(2);
  const int sdaPd = digitalRead(PIN_SDA), sclPd = digitalRead(PIN_SCL);
  HUB_CHECK("I2C_PULLUPS", sdaPd && sclPd, "against internal pull-down: SDA1=%d SCL1=%d (F2 4.7k)", sdaPd, sclPd);
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(50);

  // Mux power-on state is all channels off; force it in case of a warm reset.
  muxWrite(0x00);
  gRoot = scanBus();
  char addrs[96];
  formatAddrs(gRoot, nullptr, addrs, sizeof(addrs));
  HUB_CHECK("ROOT_BUS_OK", gRoot.busErrors == 0, "%u bus errors (last code %u); found: %s",
            gRoot.busErrors, gRoot.lastError, addrs);
  HUB_CHECK("ROOT_DS3231_0x68", isSet(gRoot, DS3231_ADDR), "RTC U49");
  HUB_CHECK("ROOT_TCA9546A_0x71", isSet(gRoot, MUX_ADDR), "mux U33%s",
            isSet(gRoot, 0x70) || isSet(gRoot, 0x72) || isSet(gRoot, 0x73)
                ? "; a 0x70/0x72/0x73 ack suggests a wrong R150/R160/SJ4/SJ5 strap" : "");
  HUB_CHECK("ROOT_ADS1015_0x48", isSet(gRoot, ADS1015_ADDR), "ADC U30 through PCA9306 U29 (needs 5V_SYS)");
  const bool exact = gRoot.count == 3 && isSet(gRoot, DS3231_ADDR) && isSet(gRoot, MUX_ADDR) &&
                     isSet(gRoot, ADS1015_ADDR);
  HUB_CHECK("ROOT_EXACT_SET", exact, "expected exactly 0x48 0x68 0x71, got %s", addrs);

  if (isSet(gRoot, DS3231_ADDR)) checkRtc();

  if (!isSet(gRoot, MUX_ADDR)) {
    HUB_CHECK("MUX_REGISTER", false, "mux not present; channel tests skipped");
    hubSummary();
    return;
  }

  bool regOk = true;
  char regDetail[64] = "";
  size_t n = 0;
  for (uint8_t ch = 0; ch < 4; ch++) {
    muxWrite(1 << ch);
    const int rb = muxRead();
    n += snprintf(regDetail + n, sizeof(regDetail) - n, "%sch%u=0x%02X", ch ? " " : "", ch, rb & 0xFF);
    regOk &= rb == (1 << ch);
  }
  muxWrite(0x00);
  const int off = muxRead();
  regOk &= off == 0x00;
  HUB_CHECK("MUX_REGISTER", regOk, "readback %s off=0x%02X", regDetail, off & 0xFF);

  scanChannels(true);

  const ScanResult after = scanBus();
  bool same = after.count == gRoot.count && after.busErrors == 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) same &= isSet(after, a) == isSet(gRoot, a);
  HUB_CHECK("MUX_DESELECT", same, "root bus identical after channel 0x00 (no channel stuck on)");

  // Rise-time margin with the harnesses fitted: same answer at 400 kHz.
  Wire.setClock(400000);
  bool fastOk = true;
  const ScanResult root400 = scanBus();
  fastOk &= root400.busErrors == 0 && root400.count == gRoot.count;
  for (uint8_t ch = 0; ch < 4; ch++) {
    muxWrite(1 << ch);
    const ScanResult s = scanBus();
    fastOk &= s.busErrors == 0 && s.count == gChannel[ch].count;
  }
  muxWrite(0x00);
  Wire.setClock(100000);
  HUB_CHECK("I2C_400KHZ", fastOk, "root and all four channels give the same device sets at 400 kHz");

  hubSummary();
}

static void watchLoop() {
  Serial.println("Watch mode: plug/unplug harnesses on CN17-CN20. Enter stops.");
  ScanResult prev[4];
  memcpy(prev, gChannel, sizeof(prev));
  while (!Serial.available()) {
    scanChannels(false);
    for (uint8_t ch = 0; ch < 4; ch++) {
      if (memcmp(prev[ch].present, gChannel[ch].present, 16) != 0 ||
          gChannel[ch].busErrors != prev[ch].busErrors) {
        char addrs[96];
        formatAddrs(gChannel[ch], &gRoot, addrs, sizeof(addrs));
        Serial.printf("  t=%lus ch%u %s: %s (bus errors %u)\n", millis() / 1000, ch,
                      kPortForChannel[ch], addrs, gChannel[ch].busErrors);
      }
    }
    memcpy(prev, gChannel, sizeof(prev));
    const ScanResult root = scanBus();
    if (memcmp(root.present, gRoot.present, 16) != 0 || root.busErrors) {
      char addrs[96];
      formatAddrs(root, nullptr, addrs, sizeof(addrs));
      Serial.printf("  t=%lus ROOT changed: %s (bus errors %u)\n", millis() / 1000, addrs, root.busErrors);
    }
    delay(2000);
  }
  while (Serial.available()) Serial.read();
  Serial.println("Watch mode stopped.");
}

void setup() {
  hubHoldPower();
  Serial.begin(115200);
  delay(300);
  hubBanner("I2C root bus, TCA9546A mux, sensor ports");
  Serial.printf("SDA=%d SCL=%d mux=0x%02X; ch0..3 = CN19 CN20 CN17 CN18\n", PIN_SDA, PIN_SCL, MUX_ADDR);
  hubWaitForStart();
  runSuite();
  Serial.println("Commands: r = re-run | w = hot-plug watch");
}

void loop() {
  char line[16];
  if (!hubReadLine(line, sizeof(line))) {
    delay(10);
    return;
  }
  if (line[0] == 'r') runSuite();
  else if (line[0] == 'w') watchLoop();
  else if (line[0] != '\0') Serial.println("Commands: r = re-run | w = hot-plug watch");
}
