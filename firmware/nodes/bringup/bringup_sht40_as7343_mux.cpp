// bringup_sht40_as7343_mux.cpp
//
// Validates SHT40 (mux ch 0) and AS7343 (mux ch 1) on the PCA9548A I2C mux.
// Prints all channel readings to serial every 2 seconds.
// No logging, no queue, no power-cut -- pure diagnostic loop.
//
// Hardware:
//   SDA = 18, SCL = 19
//   Mux PCA9548A at MUX_ADDR (default 0x71)
//   SHT40 at 0x44 on mux ch 0
//   AS7343 at 0x39 on mux ch 1
//
// Build: pio run -e esp32wroom-sht40-as7343-mux -t upload
//
// AS7343 channel map (18-ch AUTOSMUX):
//   Cycle 1: FZ(450nm), FY(555nm), FXL(600nm), NIR(855nm), VIS1, FD1
//   Cycle 2: F2(425nm), F3(475nm), F4(515nm), F6(640nm), VIS2, FD2
//   Cycle 3: F1(405nm), F7(690nm), F8(745nm), F5(550nm), VIS3, FD3
//
// PAR proxy = sum of channels in 400-700nm: F1+F2+FZ+F3+F4+F5+FY+FXL+F6+F7
// NDVI      = (NIR855 - F7_690) / (NIR855 + F7_690)

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include <SparkFun_AS7343.h>

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
#define MUX_CH_AS7343  1

// ------------------------------------------------------------------ drivers
static Adafruit_SHT4x  g_sht4;
static SfeAS7343ArdI2C g_as7343;
static bool            g_sht40_ok  = false;
static bool            g_as7343_ok = false;

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

// ------------------------------------------------------------------ SHT40 init
static bool init_sht40() {
  if (!mux_select(MUX_CH_SHT40)) return false;
  delay(5);
  if (!g_sht4.begin(&Wire)) {
    Serial.println("[SHT40] not found on mux ch 0");
    return false;
  }
  g_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  g_sht4.setHeater(SHT4X_NO_HEATER);
  Serial.println("[SHT40] init OK");
  return true;
}

// ------------------------------------------------------------------ AS7343 init
static bool init_as7343() {
  if (!mux_select(MUX_CH_AS7343)) return false;
  delay(5);
  if (!g_as7343.begin(kAS7343Addr, Wire)) {
    Serial.println("[AS7343] not found on mux ch 1");
    return false;
  }
  if (!g_as7343.powerOn()) {
    Serial.println("[AS7343] powerOn failed");
    return false;
  }
  // AGAIN_16 = 16x gain. Good for indoor/diffuse outdoor.
  // Reduce to AGAIN_4 or AGAIN_2 if readings saturate (65535) in direct sun.
  if (!g_as7343.setAgain(AGAIN_16)) {
    Serial.println("[AS7343] setAgain failed");
    return false;
  }
  if (!g_as7343.setAutoSmux(AUTOSMUX_18_CHANNELS)) {
    Serial.println("[AS7343] setAutoSmux failed");
    return false;
  }
  if (!g_as7343.enableSpectralMeasurement()) {
    Serial.println("[AS7343] enableSpectralMeasurement failed");
    return false;
  }
  Serial.println("[AS7343] init OK  (GAIN=x16, AUTOSMUX=18ch)");
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

// ------------------------------------------------------------------ read AS7343
static void read_as7343() {
  if (!mux_select(MUX_CH_AS7343)) return;
  delay(2);
  if (!g_as7343.readSpectraDataFromSensor()) {
    Serial.println("[AS7343] read failed");
    return;
  }

  // Raw counts per channel
  uint16_t f1  = g_as7343.getChannelData(CH_PURPLE_F1_405NM);
  uint16_t f2  = g_as7343.getChannelData(CH_DARK_BLUE_F2_425NM);
  uint16_t fz  = g_as7343.getChannelData(CH_BLUE_FZ_450NM);
  uint16_t f3  = g_as7343.getChannelData(CH_LIGHT_BLUE_F3_475NM);
  uint16_t f4  = g_as7343.getChannelData(CH_BLUE_F4_515NM);
  uint16_t f5  = g_as7343.getChannelData(CH_GREEN_F5_550NM);
  uint16_t fy  = g_as7343.getChannelData(CH_GREEN_FY_555NM);
  uint16_t fxl = g_as7343.getChannelData(CH_ORANGE_FXL_600NM);
  uint16_t f6  = g_as7343.getChannelData(CH_BROWN_F6_640NM);
  uint16_t f7  = g_as7343.getChannelData(CH_RED_F7_690NM);
  uint16_t f8  = g_as7343.getChannelData(CH_DARK_RED_F8_745NM);
  uint16_t nir = g_as7343.getChannelData(CH_NIR_855NM);

  Serial.println("[AS7343] --- spectral channels (raw counts, GAIN=x16) ---");
  Serial.printf("  F1  405nm (purple)       : %6u\n", f1);
  Serial.printf("  F2  425nm (dark blue)    : %6u\n", f2);
  Serial.printf("  FZ  450nm (blue)         : %6u\n", fz);
  Serial.printf("  F3  475nm (light blue)   : %6u\n", f3);
  Serial.printf("  F4  515nm (blue-green)   : %6u\n", f4);
  Serial.printf("  F5  550nm (green narrow) : %6u\n", f5);
  Serial.printf("  FY  555nm (green wide)   : %6u\n", fy);
  Serial.printf("  FXL 600nm (orange)       : %6u\n", fxl);
  Serial.printf("  F6  640nm (red-orange)   : %6u\n", f6);
  Serial.printf("  F7  690nm (red)          : %6u\n", f7);
  Serial.printf("  F8  745nm (deep red)     : %6u\n", f8);
  Serial.printf("  NIR 855nm                : %6u\n", nir);

  // Saturation check
  bool sat = (f1==65535||f2==65535||fz==65535||f3==65535||f4==65535||
              f5==65535||fy==65535||fxl==65535||f6==65535||f7==65535);
  if (sat) Serial.println("  *** WARNING: channel(s) saturated -- reduce gain ***");

  // PAR proxy: equal-weight sum of the 10 channels in 400-700nm range.
  // To convert to umol/m2/s: PAR_umol = PAR_PROXY * K  (K from regression vs reference sensor).
  float par_proxy = (float)(f1 + f2 + fz + f3 + f4 + f5 + fy + fxl + f6 + f7);
  Serial.printf("  PAR_PROXY (sum 400-700nm counts) : %.0f\n", par_proxy);

  // NDVI = (NIR - red) / (NIR + red)  using F7 690nm as the red channel.
  // Expected: vegetation ~0.3-0.8; bare soil / bench lighting ~-0.5 to 0.1
  float denom = (float)nir + (float)f7;
  if (denom > 0.0f)
    Serial.printf("  NDVI (NIR-F7)/(NIR+F7) : %.4f\n", ((float)nir - (float)f7) / denom);
  else
    Serial.println("  NDVI : invalid (zero denominator)");

  Serial.println();
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SHT40 + AS7343 mux bringup ===");
  Serial.printf("  I2C SDA=%d SCL=%d   MUX=0x%02X\n",
                I2C_SDA_PIN, I2C_SCL_PIN, MUX_ADDR);
  Serial.printf("  SHT40  -> mux ch %d (I2C 0x44)\n", MUX_CH_SHT40);
  Serial.printf("  AS7343 -> mux ch %d (I2C 0x39)\n\n", MUX_CH_AS7343);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  mux_disable_all();
  delay(10);

  g_sht40_ok  = init_sht40();
  mux_disable_all();
  g_as7343_ok = init_as7343();
  mux_disable_all();

  Serial.printf("\nSHT40=%s   AS7343=%s\n\n",
                g_sht40_ok  ? "OK" : "FAIL",
                g_as7343_ok ? "OK" : "FAIL");
}

void loop() {
  Serial.println("---- sample ----");
  if (g_sht40_ok)  read_sht40();
  mux_disable_all();
  if (g_as7343_ok) read_as7343();
  mux_disable_all();
  delay(2000);
}
