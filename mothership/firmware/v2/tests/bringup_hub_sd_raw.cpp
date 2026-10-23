// FieldHub v2 r6 bring-up: raw microSD probe, run when sd-card/sd-sweep fail.
//
// Bit-bangs SPI mode 0 at ~50 kHz on the SD pins: no SPI peripheral, no SD
// library, no FatFs. It separates "the card never answers" from a
// library/timing problem, and checks each ESP32 -> series resistor ->
// pull-up path on its own.
//
// r6 SD nets (netlist-verified):
//   GPIO13 -R24- SD_CS   (R175 10k pull-up) -> TF1.2
//   GPIO23 -R26- SD_MOSI (R6 10k pull-up)   -> TF1.3
//   GPIO18 -R27- SD_SCK  (no pull)          -> TF1.5
//   GPIO19 ----- SD_MISO (R4 10k pull-up)   <- TF1.7
//
// Checks:
//   SD_PATH_CS / SD_PATH_MOSI / SD_PATH_MISO: the pin reads HIGH with its
//     internal ~45k pull-down on, so the 10k pull-up is reachable through
//     the series resistor. This does not prove the TF1 solder joints.
//   SD_RAW_CMD0: after 80 wake-up clocks, CMD0 (GO_IDLE) returns R1 = 0x01.
//   SD_RAW_CMD8: CMD8 echoes 0x1AA (SD v2 card, voltage accepted).
//   SD_MISO_ACTIVITY: MISO went low at least once (the card drove DO).
//   SD_<path>_ACMD41: CMD55+ACMD41(HCS) reaches R1 = 0x00 within 2 s. This is
//     where the card starts its internal init and draws its inrush current;
//     CMD0/CMD8 alone do not exercise that.
//   SD_<path>_CMD58: OCR reports power-up complete (bit 31), and CCS.
// The CMD0..CMD58 sequence runs twice: bit-banged (BB, ~50 kHz), then on the
// VSPI peripheral at 400 kHz (HW). BB passing with HW failing points at the
// SPI peripheral path; both failing at ACMD41 points at card power.
//
// "s" holds static levels on CS/CLK/MOSI for DMM probing at the TF1 pin toes.
//
// Build: pio run -e mothership-v2-bringup-sd-raw -t upload
// Run:   scripts/hub_bringup.py run <BOARD> sd-raw

#include <Arduino.h>

#include <SPI.h>

#include "hub_bringup_report.h"

#ifndef SD_RAW_HALF_PERIOD_US
#define SD_RAW_HALF_PERIOD_US 10  // ~50 kHz, well under the 400 kHz init limit
#endif

static uint32_t gMisoLowBits = 0;
static SPIClass gSpi(VSPI);
static bool gUseHw = false;

static uint8_t xfer(uint8_t out) {
  if (gUseHw) {
    const uint8_t v = gSpi.transfer(out);
    gMisoLowBits += 8 - __builtin_popcount(v);
    return v;
  }
  uint8_t in = 0;
  for (int b = 7; b >= 0; b--) {
    digitalWrite(PIN_SD_MOSI, (out >> b) & 1);
    delayMicroseconds(SD_RAW_HALF_PERIOD_US);
    digitalWrite(PIN_SD_SCK, HIGH);
    const int bit = digitalRead(PIN_SD_MISO);
    if (!bit) gMisoLowBits++;
    in = (in << 1) | bit;
    delayMicroseconds(SD_RAW_HALF_PERIOD_US);
    digitalWrite(PIN_SD_SCK, LOW);
  }
  return in;
}

static uint8_t crc7(const uint8_t* d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    uint8_t v = d[i];
    for (int b = 0; b < 8; b++) {
      crc <<= 1;
      if ((v & 0x80) ^ (crc & 0x80)) crc ^= 0x09;
      v <<= 1;
    }
  }
  return crc & 0x7F;
}

// Send a 6-byte command frame, then clock up to 10 bytes looking for R1.
// The bytes read are written to resp as hex for the log.
static int command(uint8_t cmd, uint32_t arg, uint8_t crc, char* resp, size_t cap, uint8_t extra[4]) {
  (void)crc;  // the CRC7 is always computed; the parameter documents the expected value
  uint8_t frame[5] = {static_cast<uint8_t>(0x40 | cmd), static_cast<uint8_t>(arg >> 24),
                      static_cast<uint8_t>(arg >> 16), static_cast<uint8_t>(arg >> 8),
                      static_cast<uint8_t>(arg)};
  digitalWrite(PIN_SD_CS, LOW);
  xfer(0xFF);
  for (uint8_t b : frame) xfer(b);
  xfer((crc7(frame, 5) << 1) | 1);
  int r1 = -1;
  size_t n = 0;
  resp[0] = '\0';
  for (int i = 0; i < 10; i++) {
    const uint8_t v = xfer(0xFF);
    n += snprintf(resp + n, cap - n, "%02X ", v);
    if (r1 < 0 && v != 0xFF) {
      r1 = v;
      if (extra) {
        for (int k = 0; k < 4; k++) {
          extra[k] = xfer(0xFF);
          n += snprintf(resp + n, cap - n, "%02X ", extra[k]);
        }
      }
      break;
    }
  }
  digitalWrite(PIN_SD_CS, HIGH);
  xfer(0xFF);
  return r1;
}

static bool pathHigh(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(2);
  const bool high = digitalRead(pin) == HIGH;
  pinMode(pin, INPUT);
  return high;
}

// CMD0 -> CMD8 -> CMD55/ACMD41 loop -> CMD58 on the current path.
static void initSequence(const char* path) {
  char name[32], resp[64];
  for (int i = 0; i < 10; i++) xfer(0xFF);  // >= 74 clocks with CS high

  int r1 = -1;
  int attempt = 0;
  for (attempt = 1; attempt <= 5 && r1 != 0x01; attempt++) {
    r1 = command(0, 0, 0x95, resp, sizeof(resp), nullptr);
    Serial.printf("  %s CMD0 attempt %d: %s\n", path, attempt, resp);
  }
  snprintf(name, sizeof(name), "SD_%s_CMD0", path);
  HUB_CHECK(name, r1 == 0x01, "R1=%s after %d attempt(s)%s",
            r1 < 0 ? "none (all 0xFF)" : String(r1, HEX).c_str(), attempt - 1,
            r1 < 0 ? ": card never drove DO" : "");
  if (r1 != 0x01) return;

  uint8_t r7[4] = {};
  const int r1b = command(8, 0x1AA, 0x87, resp, sizeof(resp), r7);
  Serial.printf("  %s CMD8: %s\n", path, resp);
  const bool v2 = r1b == 0x01 && (r7[2] & 0x0F) == 0x01 && r7[3] == 0xAA;
  snprintf(name, sizeof(name), "SD_%s_CMD8", path);
  HUB_CHECK(name, v2, "R1=0x%02X R7=%02X %02X %02X %02X", r1b & 0xFF, r7[0], r7[1], r7[2], r7[3]);

  const uint32_t t0 = millis();
  int tries = 0, r41 = -1, r55 = -1;
  while (millis() - t0 < 2000) {
    tries++;
    r55 = command(55, 0, 0x65, resp, sizeof(resp), nullptr);
    r41 = command(41, v2 ? 0x40000000 : 0, 0x77, resp, sizeof(resp), nullptr);
    if (r41 == 0x00) break;
    delay(5);
  }
  const uint32_t readyMs = millis() - t0;
  snprintf(name, sizeof(name), "SD_%s_ACMD41", path);
  HUB_CHECK(name, r41 == 0x00, "R1=0x%02X after %d tries / %lu ms (last CMD55 R1=0x%02X)",
            r41 & 0xFF, tries, readyMs, r55 & 0xFF);
  if (r41 != 0x00) return;

  uint8_t ocr[4] = {};
  const int r58 = command(58, 0, 0xFD, resp, sizeof(resp), ocr);
  Serial.printf("  %s CMD58: %s\n", path, resp);
  snprintf(name, sizeof(name), "SD_%s_CMD58", path);
  HUB_CHECK(name, r58 == 0x00 && (ocr[0] & 0x80), "OCR=%02X%02X%02X%02X powered-up=%d CCS=%d (%s)",
            ocr[0], ocr[1], ocr[2], ocr[3], (ocr[0] >> 7) & 1, (ocr[0] >> 6) & 1,
            (ocr[0] & 0x40) ? "SDHC/SDXC" : "SDSC");
}

// Library-shaped init on the HW path: CMD0, optional CMD59 (CRC on), CMD8,
// CMD58, then CMD55/ACMD41 back-to-back for 1000 ms, as sd_diskio.cpp does.
// Only the CRC mode and the ACMD41 argument change between variants, so a
// variant that stays idle (R1 = 0x01) names what the card objects to.
static void runVariants() {
  struct Variant { const char* name; bool crcOn; uint32_t arg; };
  static const Variant kVariants[] = {
      {"SD_VAR_CRCOFF_ARG40000000", false, 0x40000000},
      {"SD_VAR_CRCOFF_ARG40100000", false, 0x40100000},
      {"SD_VAR_CRCON_ARG40000000", true, 0x40000000},
      {"SD_VAR_CRCON_ARG40100000", true, 0x40100000},  // what the SD library sends
  };
  char resp[64];
  for (const Variant& v : kVariants) {
    int r0 = -1;
    for (int i = 0; i < 5 && r0 != 0x01; i++) r0 = command(0, 0, 0x95, resp, sizeof(resp), nullptr);
    int r59 = -2;
    if (v.crcOn) r59 = command(59, 1, 0x83, resp, sizeof(resp), nullptr);
    uint8_t r7[4] = {}, ocr[4] = {};
    const int r8 = command(8, 0x1AA, 0x87, resp, sizeof(resp), r7);
    const int r58 = command(58, 0, 0xFD, resp, sizeof(resp), ocr);
    const uint32_t t0 = millis();
    int tries = 0, r41 = -1;
    while (millis() - t0 < 1000) {
      tries++;
      command(55, 0, 0x65, resp, sizeof(resp), nullptr);
      r41 = command(41, v.arg, 0, resp, sizeof(resp), nullptr);
      if (r41 == 0x00) break;
    }
    char detail[160];
    snprintf(detail, sizeof(detail),
             "CMD0=0x%02X CMD59=%s CMD8=0x%02X CMD58=0x%02X OCR=%02X%02X%02X%02X ACMD41=0x%02X after %d tries/%lu ms",
             r0 & 0xFF, v.crcOn ? String(r59 & 0xFF, HEX).c_str() : "-", r8 & 0xFF, r58 & 0xFF, ocr[0], ocr[1],
             ocr[2], ocr[3], r41 & 0xFF, tries, millis() - t0);
    // Spec-compliant SPI-mode ACMD41 (bits 29:0 zero) must work: that is the
    // hardware verdict. The 0x40100000 variants describe the card, not the
    // board: a failure there means the stock SD library cannot mount it.
    if (v.arg == 0x40000000) HUB_CHECK(v.name, r41 == 0x00, "%s", detail);
    else if (r41 != 0x00) HUB_WARN(v.name, "%s; card rejects the SD library's ACMD41 argument", detail);
    else HUB_CHECK(v.name, true, "%s", detail);
  }
}

static void runSuite() {
  hubResetCounts();
  gMisoLowBits = 0;

  HUB_CHECK("SD_PATH_CS", pathHigh(PIN_SD_CS), "GPIO13 -> R24 -> R175 pull-up");
  HUB_CHECK("SD_PATH_MOSI", pathHigh(PIN_SD_MOSI), "GPIO23 -> R26 -> R6 pull-up");

  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_SCK, LOW);
  pinMode(PIN_SD_SCK, OUTPUT);
  digitalWrite(PIN_SD_MOSI, HIGH);
  pinMode(PIN_SD_MOSI, OUTPUT);
  pinMode(PIN_SD_MISO, INPUT);
  delay(10);

  // A card left selected by a reset mid-transaction keeps driving DO until it
  // sees clocks with CS high, which would read as a missing R4. Release it.
  for (int i = 0; i < 2; i++) xfer(0xFF);
  HUB_CHECK("SD_PATH_MISO", pathHigh(PIN_SD_MISO), "GPIO19 -> R4 pull-up (after DO release clocks)");

  gUseHw = false;
  initSequence("BB");
  HUB_CHECK("SD_MISO_ACTIVITY", gMisoLowBits > 0, "%lu low bits sampled on MISO", gMisoLowBits);

  gUseHw = true;
  gSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, -1);  // CS stays a plain GPIO
  gSpi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  initSequence("HW");
  runVariants();
  gSpi.endTransaction();
  gSpi.end();
  gUseHw = false;

  pinMode(PIN_SD_CS, INPUT);
  pinMode(PIN_SD_SCK, INPUT);
  pinMode(PIN_SD_MOSI, INPUT);
  hubSummary();
}

static void staticLevels() {
  pinMode(PIN_SD_CS, OUTPUT);
  pinMode(PIN_SD_SCK, OUTPUT);
  pinMode(PIN_SD_MOSI, OUTPUT);
  for (int phase = 0; phase < 2; phase++) {
    const int a = phase == 0 ? LOW : HIGH;
    digitalWrite(PIN_SD_CS, a);
    digitalWrite(PIN_SD_SCK, !a);
    digitalWrite(PIN_SD_MOSI, a);
    Serial.printf("STATIC %d/2 for 20 s: TF1.2 CS=%s  TF1.5 CLK=%s  TF1.3 MOSI=%s (TF1.4 VDD should be 3.3 V)\n",
                  phase + 1, a ? "3.3V" : "0V", a ? "0V" : "3.3V", a ? "3.3V" : "0V");
    delay(20000);
  }
  pinMode(PIN_SD_CS, INPUT);
  pinMode(PIN_SD_SCK, INPUT);
  pinMode(PIN_SD_MOSI, INPUT);
  Serial.println("STATIC done");
}

void setup() {
  hubHoldPower();
  Serial.begin(115200);
  delay(300);
  hubBanner("raw microSD probe (bit-banged SPI)");
  hubWaitForStart();
  runSuite();
  Serial.println("Commands: r = re-run | s = static levels for DMM probing");
}

void loop() {
  char line[16];
  if (!hubReadLine(line, sizeof(line))) {
    delay(10);
    return;
  }
  if (line[0] == 'r') runSuite();
  else if (line[0] == 's') staticLevels();
  else if (line[0] != '\0') Serial.println("Commands: r = re-run | s = static levels for DMM probing");
}
