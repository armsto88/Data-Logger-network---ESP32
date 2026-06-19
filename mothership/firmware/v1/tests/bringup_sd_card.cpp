// Mothership V1 bringup: SD card init and write
// Validates the SPI SD card interface on the V1 PCB.
// GPIO13=CS, GPIO18=SCK, GPIO19=MISO, GPIO23=MOSI

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef PIN_SD_CS
#define PIN_SD_CS 13
#endif
#ifndef PIN_SD_SCK
#define PIN_SD_SCK 18
#endif
#ifndef PIN_SD_MISO
#define PIN_SD_MISO 19
#endif
#ifndef PIN_SD_MOSI
#define PIN_SD_MOSI 23
#endif
#ifndef SD_SPI_SPEED
#define SD_SPI_SPEED 400000  // 400 kHz — SD spec init speed, most conservative
#endif

static SPIClass gSDSPI(HSPI);

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 SD Card Bring-up ===");
  Serial.printf("SD CS=%d SCK=%d MISO=%d MOSI=%d\n",
                PIN_SD_CS, PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI);

  // Manual CS control for diagnostics
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);  // deselect

  // Read MISO idle state
  pinMode(PIN_SD_MISO, INPUT);
  Serial.printf("[DBG] MISO idle state: %d (expect 1/HIGH due to pull-up or card)\n", digitalRead(PIN_SD_MISO));

  // Assert CS and check MISO response
  digitalWrite(PIN_SD_CS, LOW);
  delay(1);
  Serial.printf("[DBG] MISO with CS asserted: %d (expect 0 if card responds)\n", digitalRead(PIN_SD_MISO));
  digitalWrite(PIN_SD_CS, HIGH);

  // Init SPI on the correct pins using HSPI
  gSDSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (!SD.begin(PIN_SD_CS, gSDSPI, SD_SPI_SPEED)) {
    Serial.println("[SD] SD.begin() FAILED — check wiring and card presence");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("[SD] SD.begin() OK");

  // Print card info
  Serial.printf("[SD] Card type: ");
  switch (SD.cardType()) {
    case CARD_MMC:    Serial.println("MMC"); break;
    case CARD_SD:     Serial.println("SD"); break;
    case CARD_SDHC:   Serial.println("SDHC"); break;
    case CARD_UNKNOWN: Serial.println("UNKNOWN"); break;
    default:          Serial.println("NONE"); break;
  }

  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  Serial.printf("[SD] Total: %llu bytes (%.1f MB)\n", totalBytes, totalBytes / 1048576.0);
  Serial.printf("[SD] Used:  %llu bytes (%.1f MB)\n", usedBytes, usedBytes / 1048576.0);

  // Write test
  const char* testPath = "/bringup_test.txt";
  Serial.printf("[SD] Writing test file: %s\n", testPath);

  File f = SD.open(testPath, FILE_WRITE);
  if (!f) {
    Serial.println("[SD] FAILED to open file for write");
    while (true) { delay(1000); }
  }

  unsigned long writeMs = millis();
  f.printf("Mothership V1 SD bring-up test\n");
  f.printf("Written at millis=%lu\n", millis());
  f.printf("Card size: %llu bytes\n", totalBytes);
  f.close();
  writeMs = millis() - writeMs;
  Serial.printf("[SD] Write OK (%lu ms)\n", writeMs);

  // Read-back test
  f = SD.open(testPath, FILE_READ);
  if (!f) {
    Serial.println("[SD] FAILED to open file for read");
    while (true) { delay(1000); }
  }

  Serial.println("[SD] Read-back:");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    Serial.printf("  %s\n", line.c_str());
  }
  f.close();

  // Clean up test file
  if (SD.remove(testPath)) {
    Serial.println("[SD] Test file removed");
  }

  Serial.println();
  Serial.println("=== SD Card bring-up PASSED ===");
}

void loop() {
  delay(5000);
  Serial.printf("t=%lu ms | SD card still mounted, type=%d\n", millis(), SD.cardType());
}