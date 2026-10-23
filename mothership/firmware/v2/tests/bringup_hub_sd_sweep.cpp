// FieldHub v2 r6 bring-up: microSD SPI speed sweep (Stage F, "SPI proof").
//
// Separates a wiring fault from a signal-integrity margin problem by mounting
// and verifying the card at 400 kHz, 4, 10, 20 and 40 MHz in turn. Production
// storage/sd_logger.cpp mounts at 40 MHz on VSPI, so a 40 MHz failure here
// means the shipped firmware will not get an SD archive on this board/card.
//
// r6 wiring (netlist-verified, same as r5):
//   GPIO13 -R24 22R- SD_CS   -> TF1.2      GPIO18 -R27 22R- SD_SCK -> TF1.5
//   GPIO23 -R26 22R- SD_MOSI -> TF1.3      GPIO19 --------- SD_MISO <- TF1.7
//   10k pull-ups: MISO (R4), MOSI (R6), DAT1 (R5), DAT2 (R7). TF1.CD unrouted.
//
// Per speed: 3 independent mounts, then SWEEP_BYTES of a speed-seeded
// pseudo-random pattern written, read back and compared, then deleted.
//
// Answering the start prompt with "n" runs the no-card check instead of the
// sweep (scripts/hub_bringup.py test "sd-nocard").
//
// Serial commands after the run:
//   r         re-run the sweep
//   n         no-card check: remove the card first; mount must fail fast
//   p <kHz>   probe loop: continuous mount + directory read at <kHz> for
//             scope captures of CLK/CMD/CS/DAT0 and TF1.4 droop (Enter stops)
//
// Build: pio run -e mothership-v2-bringup-sd-sweep -t upload
// Run:   scripts/hub_bringup.py run <BOARD> sd-sweep

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "hub_bringup_report.h"

#ifndef SWEEP_BYTES
#define SWEEP_BYTES (128UL * 1024UL)
#endif
#ifndef SWEEP_MOUNTS
#define SWEEP_MOUNTS 3
#endif
#ifndef NO_CARD_MAX_MS
#define NO_CARD_MAX_MS 3000  // a missing card must not stall a sync wake
#endif

static SPIClass gSDSPI(VSPI);  // same host as production sd_logger.cpp
static const uint32_t kSpeeds[] = {400000, 4000000, 10000000, 20000000, 40000000};
static const char* kTestPath = "/hub_sd_sweep.bin";
static uint8_t gBuf[512];

static uint32_t xorshift32(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

static void fillBlock(uint32_t& seed) {
  for (size_t i = 0; i < sizeof(gBuf); i += 4) {
    const uint32_t v = xorshift32(seed);
    memcpy(gBuf + i, &v, 4);
  }
}

static void sdRelease() {
  SD.end();
  gSDSPI.end();
}

static bool sdMount(uint32_t hz, uint32_t* elapsedMs) {
  sdRelease();
  gSDSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  const uint32_t t0 = millis();
  const bool ok = SD.begin(PIN_SD_CS, gSDSPI, hz) && SD.cardType() != CARD_NONE;
  if (elapsedMs) *elapsedMs = millis() - t0;
  return ok;
}

static const char* cardTypeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    case CARD_NONE: return "NONE";
    default:        return "UNKNOWN";
  }
}

// Write SWEEP_BYTES, read back, compare. Returns mismatched byte count, or -1
// on an open/short-write/short-read error.
static long writeVerify(uint32_t seed0, float* wrKBs, float* rdKBs) {
  SD.remove(kTestPath);
  File f = SD.open(kTestPath, FILE_WRITE);
  if (!f) return -1;
  uint32_t seed = seed0;
  uint32_t t0 = millis();
  for (uint32_t done = 0; done < SWEEP_BYTES; done += sizeof(gBuf)) {
    fillBlock(seed);
    if (f.write(gBuf, sizeof(gBuf)) != sizeof(gBuf)) { f.close(); return -1; }
  }
  f.close();  // close flushes; count it in the write time
  const uint32_t wrMs = millis() - t0;

  f = SD.open(kTestPath, FILE_READ);
  if (!f) return -1;
  if (f.size() != SWEEP_BYTES) { f.close(); return -1; }
  seed = seed0;
  long mismatches = 0;
  uint8_t rd[sizeof(gBuf)];
  t0 = millis();
  for (uint32_t done = 0; done < SWEEP_BYTES; done += sizeof(rd)) {
    if (f.read(rd, sizeof(rd)) != (int)sizeof(rd)) { f.close(); return -1; }
    fillBlock(seed);
    for (size_t i = 0; i < sizeof(rd); i++) mismatches += rd[i] != gBuf[i];
  }
  const uint32_t rdMs = millis() - t0;
  f.close();
  SD.remove(kTestPath);

  *wrKBs = SWEEP_BYTES / 1024.0f / (wrMs ? wrMs / 1000.0f : 0.001f);
  *rdKBs = SWEEP_BYTES / 1024.0f / (rdMs ? rdMs / 1000.0f : 0.001f);
  return mismatches;
}

static void runSweep() {
  hubResetCounts();

  // MISO idles high through R4 with CS deasserted, card or no card.
  sdRelease();
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_SD_MISO, INPUT);
  delay(2);
  const int misoIdle = digitalRead(PIN_SD_MISO);
  HUB_CHECK("SD_MISO_IDLE_HIGH", misoIdle == HIGH,
            "SD_MISO=%d with CS high (R4 10k pull-up)", misoIdle);

  uint32_t mountMs = 0;
  if (!sdMount(400000, &mountMs)) {
    HUB_CHECK("SD_CARD_PRESENT", false,
              "no response at 400 kHz after %lu ms: card missing, or wiring fault", mountMs);
    sdRelease();
    hubSummary();
    return;
  }
  const uint8_t type = SD.cardType();
  HUB_CHECK("SD_CARD_PRESENT", true, "%s, %llu MB card, %llu MB FAT total, %llu MB used",
            cardTypeName(type), SD.cardSize() / 1048576ULL, SD.totalBytes() / 1048576ULL,
            SD.usedBytes() / 1048576ULL);

  uint32_t highestPass = 0;
  bool contiguous = true;
  for (uint32_t hz : kSpeeds) {
    char name[24];
    snprintf(name, sizeof(name), "SD_%luK", (unsigned long)(hz / 1000));

    int mounts = 0;
    uint32_t worstMountMs = 0;
    for (int i = 0; i < SWEEP_MOUNTS; i++) {
      uint32_t ms = 0;
      if (sdMount(hz, &ms)) mounts++;
      worstMountMs = max(worstMountMs, ms);
    }
    if (mounts != SWEEP_MOUNTS) {
      HUB_CHECK(name, false, "mounted %d/%d (worst %lu ms)%s", mounts, SWEEP_MOUNTS,
                worstMountMs, hz == 40000000 ? "; production initSD() uses 40 MHz" : "");
      contiguous = false;
      continue;
    }

    float wr = 0, rd = 0;
    const long bad = writeVerify(hz ^ 0xA5C3F00Du, &wr, &rd);
    const bool ok = bad == 0;
    if (bad < 0) {
      HUB_CHECK(name, false, "mounted %d/%d but file I/O error on %lu-byte write/read",
                mounts, SWEEP_MOUNTS, (unsigned long)SWEEP_BYTES);
    } else {
      HUB_CHECK(name, ok, "mount %d/%d (worst %lu ms), %lu KiB wr %.0f KiB/s rd %.0f KiB/s, %ld bad bytes",
                mounts, SWEEP_MOUNTS, worstMountMs, (unsigned long)(SWEEP_BYTES / 1024), wr, rd, bad);
    }
    if (ok && contiguous) highestPass = hz;
    if (!ok) contiguous = false;
  }

  if (highestPass == 40000000) {
    HUB_WARN("SD_MARGIN", "all speeds pass; scope 40 MHz edges before calling it margin");
  } else {
    HUB_WARN("SD_MARGIN", "highest contiguous pass %lu kHz; production mounts at 40000 kHz",
             (unsigned long)(highestPass / 1000));
  }
  sdRelease();
  hubSummary();
}

static void runNoCardCheck() {
  hubResetCounts();
  uint32_t worst = 0;
  int mounted = 0;
  for (uint32_t hz : kSpeeds) {
    uint32_t ms = 0;
    if (sdMount(hz, &ms)) mounted++;
    worst = max(worst, ms);
  }
  sdRelease();
  HUB_CHECK("SD_NO_CARD_DETECTED", mounted == 0,
            "%d/%u speeds mounted with the card supposedly removed", mounted,
            (unsigned)(sizeof(kSpeeds) / sizeof(kSpeeds[0])));
  HUB_CHECK("SD_NO_CARD_FAST_FAIL", worst <= NO_CARD_MAX_MS,
            "worst failed mount took %lu ms (limit %u ms)", worst, NO_CARD_MAX_MS);
  hubSummary();
}

static void runProbeLoop(uint32_t khz) {
  Serial.printf("Probe loop at %lu kHz: trigger on SD_CS falling at TF1.2. Enter stops.\n",
                (unsigned long)khz);
  uint32_t ok = 0, fail = 0, lastPrint = millis();
  while (true) {
    if (Serial.available()) {
      while (Serial.available()) Serial.read();
      break;
    }
    if (sdMount(khz * 1000UL, nullptr)) {
      File root = SD.open("/");
      if (root) {
        File e = root.openNextFile();
        if (e) e.close();
        root.close();
      }
      ok++;
    } else {
      fail++;
    }
    if (millis() - lastPrint >= 1000) {
      Serial.printf("  %lu kHz: %lu ok / %lu fail\n", (unsigned long)khz, ok, fail);
      lastPrint = millis();
    }
  }
  sdRelease();
  Serial.printf("Probe loop stopped: %lu ok / %lu fail\n", ok, fail);
}

static void printHelp() {
  Serial.println("Commands: r = re-run sweep | n = no-card check | p <kHz> = probe loop | ? = help");
}

void setup() {
  hubHoldPower();
  Serial.begin(115200);
  delay(300);
  hubBanner("microSD speed sweep");
  Serial.printf("CS=%d SCK=%d MISO=%d MOSI=%d, VSPI, %lu KiB per speed, %d mounts per speed\n",
                PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI,
                (unsigned long)(SWEEP_BYTES / 1024), SWEEP_MOUNTS);
  char start[8];
  hubWaitForStart(start, sizeof(start));
  if (start[0] == 'n') runNoCardCheck();  // "n" + Enter at the start prompt
  else runSweep();
  printHelp();
}

void loop() {
  char line[32];
  if (!hubReadLine(line, sizeof(line))) {
    delay(10);
    return;
  }
  if (line[0] == 'r') runSweep();
  else if (line[0] == 'n') runNoCardCheck();
  else if (line[0] == 'p') {
    const long khz = atol(line + 1);
    if (khz >= 100 && khz <= 40000) runProbeLoop(khz);
    else Serial.println("p <kHz>: 100..40000");
  } else if (line[0] != '\0') printHelp();
}
