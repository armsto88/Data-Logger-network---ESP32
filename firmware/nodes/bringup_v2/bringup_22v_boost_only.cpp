/*
 * 22V Boost Test — Direct GPIO Drive (U49 Bypass)
 * 
 * HARDWARE MOD: R173 removed, GPIO5 connected directly to EN_22.
 * This bypasses the inverter U49 which was oscillating due to
 * 3V3_SYS ripple from the MT3608 switching noise.
 * 
 * Logic is now INVERTED from original:
 *   GPIO5 HIGH → EN_22 HIGH → boost ON
 *   GPIO5 LOW  → EN_22 LOW  → boost OFF
 * 
 * WARNING: GPIO5 is a strapping pin that defaults HIGH at boot,
 * so the 22V boost will be ON during boot. This may cause brownout
 * on battery power without sufficient input capacitance.
 * 
 * Menu:
 *   d = Deep-sleep enable (sleep 3s, then wake)
 *   x = Disable boost
 *   v = Re-enable boost (cap may be pre-charged)
 *   b = Fire 10 bursts (load 22V rail)
 *   B = Fire 100 bursts (heavy load)
 *   ? = Print menu
 */

#include <Arduino.h>
#include <esp_sleep.h>

#define PIN_TX_22V_EN_N 5
#define PIN_PWR_HOLD 23
#define PIN_TX_BURST_PWM 25
#define PIN_DRV_N 26
#define PIN_REL_N 33

RTC_DATA_ATTR static bool g_boostWarmFlag = false;
RTC_DATA_ATTR static int g_bootCount = 0;

bool g_boostOn = false;

void printMenu();

void setup() {
  g_bootCount++;
  Serial.begin(115200);
  delay(500);

  // Check wake reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool wokeFromDeepSleep = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER);

  // Latch power
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  // Configure boost pin
  pinMode(PIN_TX_22V_EN_N, OUTPUT);
  digitalWrite(PIN_TX_22V_EN_N, LOW);  // safe default = OFF (inverted logic)

  Serial.println("=== 22V Boost Test ===");
  Serial.print("Boot count: ");
  Serial.println(g_bootCount);
  
  if (wokeFromDeepSleep) {
    Serial.println(">>> Woke from deep sleep (boost was enabled during sleep)");
    Serial.println(">>> 22V rail should be charged — check voltage on 22V_SYS");
  }

  if (g_boostWarmFlag) {
    Serial.println("BOOST WARM: re-enabling boost (cap pre-charged)");
    digitalWrite(PIN_TX_22V_EN_N, HIGH);  // boost ON (inverted logic)
    delay(100);
    g_boostOn = true;
    Serial.println("Boost ON (inverted logic — GPIO5 HIGH = boost ON)");
    Serial.println("Probe 22V_SYS with DMM, press 'b' to load rail");
  }

  printMenu();
}

void printMenu() {
  Serial.println("---");
  Serial.println("  d = Deep-sleep enable (RECOMMENDED — sleep 3s)");
  Serial.println("  x = Disable boost");
  Serial.println("  v = Re-enable boost (cap may be pre-charged)");
  Serial.println("  b = Fire 10 bursts (load 22V rail)");
  Serial.println("  B = Fire 100 bursts (heavy load)");
  Serial.println("  ? = Print menu");
  Serial.println("---");
  Serial.println("NOTE: Soft-start (s) does NOT work on battery power.");
  Serial.println("      Use deep-sleep (d) to enable boost.");
  Serial.println("---");
}

void deepSleepEnable() {
  Serial.println("========================================");
  Serial.println("DEEP SLEEP ENABLE — ESP32 will sleep 3s");
  Serial.println("Serial will disconnect — this is NORMAL");
  Serial.println("ESP32 draws ~10uA, MT3608 gets all battery current");
  Serial.println("  NOTE: GPIO5 HIGH at boot = boost ON (inverted logic)");
  Serial.println("After 3s, ESP32 wakes and re-enables boost");
  Serial.println("========================================");
  Serial.println("Watch for 'BOOST WARM' message after reconnect...");
  Serial.flush();
  delay(100);
  
  g_boostWarmFlag = true;
  digitalWrite(PIN_TX_22V_EN_N, HIGH);  // boost ON (inverted logic)
  esp_sleep_enable_timer_wakeup(3000000);  // 3 seconds
  esp_deep_sleep_start();
  // Never reaches here — ESP32 wakes and runs setup() again
}

void fireBursts(int count, int burstCycles) {
  if (!g_boostOn) {
    Serial.println("ERROR: Boost not ON — enable boost first (s/d/v)");
    return;
  }
  Serial.print("Firing ");
  Serial.print(count);
  Serial.print(" bursts (");
  Serial.print(burstCycles);
  Serial.println(" cycles each) to load 22V rail...");
  
  // Enable TX direction
  pinMode(PIN_DRV_N, OUTPUT);
  pinMode(PIN_REL_N, OUTPUT);
  pinMode(PIN_TX_BURST_PWM, OUTPUT);
  digitalWrite(PIN_DRV_N, HIGH);
  digitalWrite(PIN_REL_N, HIGH);
  digitalWrite(PIN_TX_BURST_PWM, LOW);
  
  for (int i = 0; i < count; i++) {
    // Fire burst
    const uint32_t halfPeriodUs = 12;  // ~40 kHz
    for (int c = 0; c < burstCycles; c++) {
      digitalWrite(PIN_TX_BURST_PWM, HIGH);
      delayMicroseconds(halfPeriodUs);
      digitalWrite(PIN_TX_BURST_PWM, LOW);
      delayMicroseconds(halfPeriodUs);
    }
    digitalWrite(PIN_TX_BURST_PWM, LOW);
    
    if (i % 10 == 9) {
      Serial.print("  burst ");
      Serial.print(i + 1);
      Serial.println(" done");
    }
    delay(50);  // 50ms between bursts
  }
  
  // Disable TX direction
  digitalWrite(PIN_DRV_N, LOW);
  digitalWrite(PIN_REL_N, LOW);
  
  Serial.println("Bursts done — check 22V_SYS voltage now");
}

void loop() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    
    switch (cmd) {
      case 'd':
        deepSleepEnable();
        break;
        
      case 'x':
        digitalWrite(PIN_TX_22V_EN_N, LOW);  // boost OFF (inverted logic)
        g_boostOn = false;
        g_boostWarmFlag = false;
        Serial.println("Boost OFF (inverted logic)");
        break;
        
      case 'v':
        Serial.println("Re-enabling boost (cap may be pre-charged)...");
        Serial.flush();
        delay(10);
        digitalWrite(PIN_TX_22V_EN_N, HIGH);  // boost ON (inverted logic)
        delay(200);
        g_boostOn = true;
        g_boostWarmFlag = true;
        Serial.println("Boost ON (inverted logic) — probe 22V_SYS");
        break;
        
      case 'b':
        fireBursts(10, 12);
        break;
        
      case 'B':
        fireBursts(100, 12);
        break;
        
      case '?':
        printMenu();
        break;
    }
  }
  
  // Heartbeat every 5 seconds
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    lastPrint = millis();
    bool boostOn = (digitalRead(PIN_TX_22V_EN_N) == HIGH);  // inverted logic
    Serial.print("[");
    Serial.print(millis() / 1000);
    Serial.print("s] Boost: ");
    Serial.println(boostOn ? "ON" : "OFF");
  }
}