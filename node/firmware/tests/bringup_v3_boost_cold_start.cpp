/*
 * Node V3 Boost Cold-Start Bring-up (Phase 3)
 *
 * 22V cold-start validation using U49 inverter logic.
 * TX_22V_EN_N = LOW  → U49 output HIGH → EN_22 HIGH → boost ON
 * TX_22V_EN_N = HIGH → U49 output LOW  → EN_22 LOW  → boost OFF
 *
 * !!! WARNING !!!
 * This sketch is for U49-equipped boards ONLY.
 * Do NOT use on boards where U49 is bypassed and GPIO5 drives EN_22 directly
 * with inverted polarity. On those boards, the GPIO logic is reversed and
 * this sketch would enable boost when it should be off, risking hardware
 * damage. Use bringup_22v_boost_only.cpp for U49-bypassed boards instead.
 * !!! WARNING !!!
 */

#include <Arduino.h>

#ifdef DISABLE_BROWNOUT
#include "soc/rtc_cntl_reg.h"
#endif

#include <esp_sleep.h>

// V3 shared headers — single source of truth
#include "v3_ultrasonic_pins.h"
#include "v3_ultrasonic_safe.h"
#include "v3_ultrasonic_burst.h"
#include "v3_ultrasonic_direction.h"
#include "v3_ultrasonic_capture.h"

// RTC memory persists across brownout/power-on resets
RTC_DATA_ATTR static bool g_boostWarmFlag = false;

// ---------------------------------------------------------------------------
// Battery voltage — GPIO35 ADC with V2 scale factor
// ---------------------------------------------------------------------------
#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN 35
#endif

#ifndef BAT_SCALE_FACTOR
#define BAT_SCALE_FACTOR 3.62   // V2 resistor divider scale
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool g_boostOn = false;
static uint32_t g_prechargeMs = BOOST_PRECHARGE_MS;

// ---------------------------------------------------------------------------
// Read battery voltage
// ---------------------------------------------------------------------------
static float readBatteryVoltage() {
  // ESP32 ADC is 12-bit, 0-3.3V range (approximate)
  const int raw = analogRead(BAT_ADC_PIN);
  const float voltage = static_cast<float>(raw) / 4095.0f * 3.3f * BAT_SCALE_FACTOR;
  return voltage;
}

// ---------------------------------------------------------------------------
// Print reset reason
// ---------------------------------------------------------------------------
static void printResetReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON:  Serial.println("POWERON"); break;
    case ESP_RST_EXT:      Serial.println("EXTERNAL"); break;
    case ESP_RST_SW:       Serial.println("SOFTWARE"); break;
    case ESP_RST_PANIC:    Serial.println("PANIC"); break;
    case ESP_RST_INT_WDT:  Serial.println("INT_WDT"); break;
    case ESP_RST_TASK_WDT: Serial.println("TASK_WDT"); break;
    case ESP_RST_WDT:      Serial.println("WDT"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("DEEPSLEEP"); break;
    case ESP_RST_BROWNOUT: Serial.println("BROWNOUT ***"); break;
    case ESP_RST_SDIO:     Serial.println("SDIO"); break;
    default:               Serial.println("UNKNOWN"); break;
  }
}

// ---------------------------------------------------------------------------
// Print all pin states
// ---------------------------------------------------------------------------
static void printPinStates() {
  Serial.println("---- Pin States ----");
  Serial.println("  NAME            GPIO  LEVEL  MODE");

  const struct { const char* name; uint8_t gpio; } pins[] = {
    {"TOF_EDGE",      PIN_TOF_EDGE},
    {"RX_EN_N",       PIN_RX_EN_N},
    {"MUX_A",         PIN_MUX_A},
    {"MUX_B",         PIN_MUX_B},
    {"DRV_N",         PIN_DRV_N},
    {"DRV_E",         PIN_DRV_E},
    {"DRV_S",         PIN_DRV_S},
    {"DRV_W",         PIN_DRV_W},
    {"REL_N",         PIN_REL_N},
    {"REL_E",         PIN_REL_E},
    {"REL_S",         PIN_REL_S},
    {"REL_W",         PIN_REL_W},
    {"TX_BURST_PWM",  PIN_TX_BURST_PWM},
    {"TX_22V_EN_N",  PIN_TX_22V_EN_N},
    {"PWR_HOLD",      PIN_PWR_HOLD},
  };

  const int count = sizeof(pins) / sizeof(pins[0]);
  for (int i = 0; i < count; i++) {
    int level = digitalRead(pins[i].gpio);
    char buf[20];
    snprintf(buf, sizeof(buf), "%-16s", pins[i].name);
    Serial.print("  ");
    Serial.print(buf);
    Serial.print(pins[i].gpio < 10 ? " " : "");
    Serial.print(pins[i].gpio);
    Serial.print("    ");
    Serial.print(level);
    Serial.print("    ");
    Serial.println(level == HIGH ? "HIGH" : "LOW");
  }
  Serial.println("--------------------");
}

// ---------------------------------------------------------------------------
// Cold-start repeat test
// Enable boost from fully discharged, hold 100ms, disable, repeat 5 times.
// ---------------------------------------------------------------------------
static void coldStartRepeatTest() {
  Serial.println("==== Cold-Start Repeat Test (5 cycles) ====");
  Serial.print("Precharge delay: ");
  Serial.print(g_prechargeMs);
  Serial.println(" ms");

  for (int cycle = 0; cycle < 5; cycle++) {
    Serial.print("CYCLE ");
    Serial.print(cycle + 1);
    Serial.println("/5:");

    // 1. Disable boost
    disableBoost();
    g_boostOn = false;
    Serial.println("  boost OFF");

    // 2. Wait 2 seconds for 22V rail to discharge
    Serial.println("  waiting 2s for 22V rail discharge...");
    delay(2000);

    // 3. Enable boost
    enableBoost();
    g_boostOn = true;
    Serial.print("  boost ON at ");
    Serial.print(millis());
    Serial.println(" ms");

    // 4. Wait precharge delay
    delay(g_prechargeMs);

    // 5. Print "boost ON" timestamp
    Serial.print("  precharge done at ");
    Serial.print(millis());
    Serial.println(" ms");

    // 6. Hold enabled for 100ms
    delay(100);

    // 7. Disable boost
    disableBoost();
    g_boostOn = false;
    Serial.print("  boost OFF at ");
    Serial.print(millis());
    Serial.println(" ms");

    // Read battery voltage after each cycle
    const float vbat = readBatteryVoltage();
    Serial.print("  VBAT=");
    Serial.print(vbat, 2);
    Serial.println(" V");

    delay(100);  // brief pause between cycles
  }

  Serial.println("---- Cold-Start Repeat Test Done ----");
}

// ---------------------------------------------------------------------------
// Burst load test
// Enable boost + precharge, set DRV_N=HIGH + REL_N=LOW (transmit North),
// fire 10 bursts at 120ms intervals, clear directions, disable boost.
// ---------------------------------------------------------------------------
static void burstLoadTest() {
  Serial.println("==== Burst Load Test (10 bursts) ====");

  // 1. Enable boost + precharge
  enableBoost();
  g_boostOn = true;
  Serial.print("boost ON, waiting precharge ");
  Serial.print(g_prechargeMs);
  Serial.println(" ms...");
  delay(g_prechargeMs);

  // 2. Set DRV_N=HIGH, REL_N=LOW (transmit mode for North)
  setTxTransmit('N');
  Serial.println("DRV_N=HIGH, REL_N=LOW (North transmit mode)");

  // 3. Fire 10 bursts at 120ms intervals
  for (int i = 0; i < 10; i++) {
    BurstResult br = sendBurst40kHz(12);
    Serial.print("  burst ");
    if (i < 9) Serial.print(" ");
    Serial.print(i + 1);
    Serial.print("/10  firstEdge=");
    Serial.print(br.firstEdgeUs);
    Serial.print("us  lastEdge=");
    Serial.print(br.lastEdgeUs);
    Serial.println("us");
    delay(120);
  }

  // 4. Clear directions
  clearAllDirections();
  Serial.println("all DRV/REL cleared (idle)");

  // 5. Disable boost
  disableBoost();
  g_boostOn = false;
  Serial.println("boost OFF");

  Serial.println("---- Burst Load Test Done ----");
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static void printMenu() {
  Serial.println();
  Serial.println("=== V3 Boost Cold-Start (Phase 3) ===");
  Serial.println("  e = Enable boost (hard enable + precharge)");
  Serial.println("  d = Disable boost");
  Serial.println("  s = Soft-start boost (gradual pulses)");
  Serial.println("  c = Cold-start repeat test (5 cycles)");
  Serial.println("  b = Fire 10 bursts to load 22V rail");
  Serial.println("  v = Read battery voltage");
  Serial.println("  p = Print pin states");
  Serial.println("  P = Set precharge delay (ms)");
  Serial.println("  ? = Menu");
  Serial.println();
  Serial.print("Boost: ");
  Serial.print(g_boostOn ? "ON" : "OFF");
  Serial.print("  Precharge: ");
  Serial.print(g_prechargeMs);
  Serial.println(" ms");
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
#ifdef DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(800);

  // Print reset reason (detect brownout)
  printResetReason();

  // Safe state: all outputs configured, boost OFF, RX disabled, PWR_HOLD HIGH
  initSafeState();

  // Configure battery ADC
  pinMode(BAT_ADC_PIN, INPUT);
  analogReadResolution(12);

  // TOF_EDGE input
  pinMode(PIN_TOF_EDGE, INPUT);

  // If boost was warm before reboot, re-enable immediately
  if (g_boostWarmFlag) {
    Serial.println("BOOST WARM: re-enabling boost (cap pre-charged from previous attempt)");
    enableBoost();
    g_boostOn = true;
    delay(g_prechargeMs);
    delay(50);  // extra settling
    Serial.println("BOOST WARM: boost ready");
  }

  Serial.println();
  Serial.println("=== V3 Boost Cold-Start (Phase 3) ===");
  Serial.println("!!! U49-equipped boards ONLY !!!");
  Serial.println("!!! Do NOT use on U49-bypassed boards !!!");
  Serial.println();

  printMenu();
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
  if (Serial.available() <= 0) {
    delay(20);
    return;
  }

  const char cmd = static_cast<char>(Serial.read());

  switch (cmd) {
    case 'e': {
      // Hard enable boost + precharge
      Serial.println("Enabling boost (hard enable)...");
      Serial.println("  May brownout and reboot — RTC flag will auto-resume");
      Serial.flush();
      delay(10);
      g_boostWarmFlag = true;
      enableBoost();
      g_boostOn = true;
      delay(g_prechargeMs);
      Serial.print("Boost ON (precharge ");
      Serial.print(g_prechargeMs);
      Serial.println(" ms done)");
      break;
    }

    case 'd': {
      // Disable boost
      disableBoost();
      g_boostOn = false;
      g_boostWarmFlag = false;
      Serial.println("Boost OFF (TX_22V_EN_N=HIGH, safe default)");
      break;
    }

    case 's': {
      // Soft-start boost
      Serial.println("Starting soft-start sequence...");
      g_boostWarmFlag = true;
      softStartBoost();
      g_boostOn = true;
      Serial.println("Soft-start complete — boost ON");
      break;
    }

    case 'c': {
      // Cold-start repeat test
      coldStartRepeatTest();
      break;
    }

    case 'b': {
      // Burst load test
      burstLoadTest();
      break;
    }

    case 'v': {
      // Read battery voltage
      const float vbat = readBatteryVoltage();
      Serial.print("Battery voltage: ");
      Serial.print(vbat, 2);
      Serial.println(" V");
      break;
    }

    case 'p': {
      // Print pin states
      printPinStates();
      break;
    }

    case 'P': {
      // Set precharge delay
      Serial.print("Current precharge: ");
      Serial.print(g_prechargeMs);
      Serial.println(" ms");
      Serial.println("Enter new precharge delay (ms):");

      // Read number from serial
      String input = "";
      while (true) {
        if (Serial.available() > 0) {
          char c = static_cast<char>(Serial.read());
          if (c == '\n' || c == '\r') {
            break;
          }
          input += c;
        }
        delay(10);
      }
      input.trim();
      uint32_t newMs = input.toInt();
      if (newMs > 0 && newMs <= 10000) {
        g_prechargeMs = newMs;
        setBoostPrechargeMs(newMs);
        Serial.print("Precharge set to ");
        Serial.print(g_prechargeMs);
        Serial.println(" ms");
      } else {
        Serial.println("Invalid value (1-10000 ms)");
      }
      break;
    }

    case '?': {
      printMenu();
      break;
    }

    default:
      break;
  }
}