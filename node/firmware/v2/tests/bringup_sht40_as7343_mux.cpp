// bringup_sht40_as7343_mux.cpp
//
// Validates SHT40 (mux ch 0) and AS7341/AS734x-family spectral sensor
// (mux ch 1) on the PCA9548A I2C mux.
// Prints all channel readings to serial every 2 seconds.
// No logging, no queue, no power-cut -- pure diagnostic loop.
//
// Hardware:
//   SDA = 18, SCL = 19
//   Mux PCA9548A at MUX_ADDR (default 0x71)
//   SHT40 at 0x44 on mux ch 0
//   AS7341/AS734x sensor at 0x39 on mux ch 1
//
// Build: pio run -e esp32wroom-sht40-as7343-mux -t upload

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_AS7341.h>

// ------------------------------------------------------------------ pins / addresses
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 18
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 19
#endif
#ifndef MUX_ADDR
#define MUX_ADDR 0x71
#endif
#define MUX_CH_SHT40   0
#define MUX_CH_AS734X  1

// ------------------------------------------------------------------ drivers
static Adafruit_SHT4x  g_sht4;
static Adafruit_AS7341 g_as7341;
static bool            g_sht40_ok  = false;
static bool            g_as734x_ok = false;

// ------------------------------------------------------------------ mux helpers
static bool mux_select(uint8_t ch) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write((uint8_t)(1U << ch));
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    Serial.printf("[MUX] select ch%u failed (err=%u)\n", ch, err);
    return false;
  }
  return true;
}

static void mux_disable_all() {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(0x00);
  Wire.endTransmission();
}

static void scan_selected_bus(const char* label) {
  bool found = false;
  Serial.printf("[%s] I2C scan:", label);
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf(" 0x%02X", addr);
      found = true;
    }
  }
  if (!found) {
    Serial.print(" none");
  }
  Serial.println();
}

static bool read_i2c_u8(uint8_t addr, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t count = Wire.requestFrom((int)addr, 1);
  if (count != 1 || !Wire.available()) {
    return false;
  }

  value = Wire.read();
  return true;
}

static void print_as734x_device_id() {
  uint8_t deviceId = 0;
  if (read_i2c_u8(AS7341_I2CADDR_DEFAULT, AS7341_WHOAMI, deviceId)) {
    Serial.printf("[AS734x] WHOAMI reg 0x%02X = 0x%02X\n", AS7341_WHOAMI, deviceId);
  } else {
    Serial.printf("[AS734x] WHOAMI read failed at reg 0x%02X\n", AS7341_WHOAMI);
  }
}

// ------------------------------------------------------------------ SHT40 init
static bool init_sht40() {
  if (!mux_select(MUX_CH_SHT40)) return false;
  delay(5);
  scan_selected_bus("MUX ch0");
  if (!g_sht4.begin(&Wire)) {
    Serial.println("[SHT40] not found on mux ch 0");
    return false;
  }
  g_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  g_sht4.setHeater(SHT4X_NO_HEATER);
  Serial.println("[SHT40] init OK");
  return true;
}

// ------------------------------------------------------------------ AS7341/AS734x init
static bool init_as734x() {
  if (!mux_select(MUX_CH_AS734X)) return false;
  delay(5);
  scan_selected_bus("MUX ch1");
  print_as734x_device_id();

  if (!g_as7341.begin(AS7341_I2CADDR_DEFAULT, &Wire)) {
    Serial.println("[AS734x] begin failed on mux ch 1");
    return false;
  }

  g_as7341.powerEnable(true);
  if (!g_as7341.setATIME(29)) {
    Serial.println("[AS734x] setATIME failed");
    return false;
  }
  if (!g_as7341.setASTEP(599)) {
    Serial.println("[AS734x] setASTEP failed");
    return false;
  }
  if (!g_as7341.setGain(AS7341_GAIN_4X)) {
    Serial.println("[AS734x] setGain failed");
    return false;
  }
  if (!g_as7341.enableSpectralMeasurement(true)) {
    Serial.println("[AS734x] enableSpectralMeasurement failed");
    return false;
  }

  Serial.println("[AS734x] init OK  (GAIN=4X, ATIME=29, ASTEP=599)");
  return true;
}

// ------------------------------------------------------------------ read SHT40
static void read_sht40() {
  if (!mux_select(MUX_CH_SHT40)) return;
  delay(2);
  sensors_event_t humidity, temp;
  if (!g_sht4.getEvent(&humidity, &temp)) {
    Serial.println("[SHT40] read failed");
    return;
  }
  Serial.printf("[SHT40] AIR_TEMP = %.2f C    AIR_RH = %.2f %%\n",
                temp.temperature, humidity.relative_humidity);
}

// ------------------------------------------------------------------ read AS7341/AS734x
static void read_as734x() {
  if (!mux_select(MUX_CH_AS734X)) return;
  delay(2);
  if (!g_as7341.readAllChannels()) {
    Serial.println("[AS734x] read failed");
    return;
  }

  uint16_t f1    = g_as7341.getChannel(AS7341_CHANNEL_415nm_F1);
  uint16_t f2    = g_as7341.getChannel(AS7341_CHANNEL_445nm_F2);
  uint16_t f3    = g_as7341.getChannel(AS7341_CHANNEL_480nm_F3);
  uint16_t f4    = g_as7341.getChannel(AS7341_CHANNEL_515nm_F4);
  uint16_t clear0= g_as7341.getChannel(AS7341_CHANNEL_CLEAR_0);
  uint16_t nir0  = g_as7341.getChannel(AS7341_CHANNEL_NIR_0);
  uint16_t f5    = g_as7341.getChannel(AS7341_CHANNEL_555nm_F5);
  uint16_t f6    = g_as7341.getChannel(AS7341_CHANNEL_590nm_F6);
  uint16_t f7    = g_as7341.getChannel(AS7341_CHANNEL_630nm_F7);
  uint16_t f8    = g_as7341.getChannel(AS7341_CHANNEL_680nm_F8);
  uint16_t clear = g_as7341.getChannel(AS7341_CHANNEL_CLEAR);
  uint16_t nir   = g_as7341.getChannel(AS7341_CHANNEL_NIR);

  Serial.println("[AS734x] --- spectral channels (raw counts, GAIN=4X) ---");
  Serial.printf("  F1   415nm            : %6u\n", f1);
  Serial.printf("  F2   445nm            : %6u\n", f2);
  Serial.printf("  F3   480nm            : %6u\n", f3);
  Serial.printf("  F4   515nm            : %6u\n", f4);
  Serial.printf("  CLEAR0                : %6u\n", clear0);
  Serial.printf("  NIR0                  : %6u\n", nir0);
  Serial.printf("  F5   555nm            : %6u\n", f5);
  Serial.printf("  F6   590nm            : %6u\n", f6);
  Serial.printf("  F7   630nm            : %6u\n", f7);
  Serial.printf("  F8   680nm            : %6u\n", f8);
  Serial.printf("  CLEAR                 : %6u\n", clear);
  Serial.printf("  NIR                   : %6u\n", nir);

  float par_proxy = (float)(f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8);
  Serial.printf("  PAR_PROXY (sum F1-F8) : %.0f\n", par_proxy);

  float denom = (float)nir + (float)f8;
  if (denom > 0.0f)
    Serial.printf("  NDVI_PROXY (NIR-F8)/(NIR+F8) : %.4f\n", ((float)nir - (float)f8) / denom);
  else
    Serial.println("  NDVI_PROXY : invalid (zero denominator)");

  Serial.println();
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SHT40 + AS734x mux bringup ===");
  Serial.printf("  I2C SDA=%d SCL=%d   MUX=0x%02X\n",
                I2C_SDA_PIN, I2C_SCL_PIN, MUX_ADDR);
  Serial.printf("  SHT40  -> mux ch %d (I2C 0x44)\n", MUX_CH_SHT40);
  Serial.printf("  AS734x -> mux ch %d (I2C 0x39)\n\n", MUX_CH_AS734X);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  mux_disable_all();
  delay(10);

  g_sht40_ok  = init_sht40();
  mux_disable_all();
  g_as734x_ok = init_as734x();
  mux_disable_all();

  Serial.printf("\nSHT40=%s   AS734x=%s\n\n",
                g_sht40_ok  ? "OK" : "FAIL",
                g_as734x_ok ? "OK" : "FAIL");
}

void loop() {
  Serial.println("---- sample ----");
  if (g_sht40_ok)  read_sht40();
  mux_disable_all();
  if (!g_as734x_ok) {
    Serial.println("[AS734x] retrying init...");
    g_as734x_ok = init_as734x();
    mux_disable_all();
  }
  if (g_as734x_ok) {
    read_as734x();
  } else {
    Serial.println("[AS734x] unavailable on mux ch 1");
  }
  mux_disable_all();
  delay(2000);
}
