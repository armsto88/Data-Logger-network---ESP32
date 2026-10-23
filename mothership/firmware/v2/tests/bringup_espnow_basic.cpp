// Mothership V1 bringup: ESP-NOW init without AP
// Validates that ESP-NOW can receive on channel 11 without an active WiFi AP.
// This is the core receive path for the sync-window model.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#ifndef PIN_PWR_HOLD
#define PIN_PWR_HOLD 26
#endif
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 11
#endif

// Receive callback (ESP-IDF 4.4 API: mac_addr, data, len)
void onEspNowRecv(const uint8_t* mac_addr, const uint8_t* data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5]);

  Serial.printf("[ESP-NOW] RX from %s | len=%d | data: ", macStr, len);
  // Print first 32 bytes as hex, or full payload if shorter
  int printLen = (len > 32) ? 32 : len;
  for (int i = 0; i < printLen; i++) {
    Serial.printf("%02X ", data[i]);
  }
  if (len > 32) {
    Serial.printf("... (%d more bytes)", len - 32);
  }
  Serial.println();
}

void setup() {
  // CRITICAL: assert PWR_HOLD immediately
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 ESP-NOW Basic Bring-up ===");
  Serial.printf("Channel: %d\n", ESPNOW_CHANNEL);

  // Init WiFi in STA mode (no AP)
  WiFi.mode(WIFI_STA);
  // Set channel — must match node channel
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[WiFi] STA mode, channel %d\n", ESPNOW_CHANNEL);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] init FAILED");
    while (true) { delay(1000); }
  }
  Serial.println("[ESP-NOW] init OK");

  // Register receive callback
  if (esp_now_register_recv_cb(onEspNowRecv) != ESP_OK) {
    Serial.println("[ESP-NOW] register recv callback FAILED");
    while (true) { delay(1000); }
  }
  Serial.println("[ESP-NOW] receive callback registered");
  Serial.println("[ESP-NOW] Listening for packets... Send from a node to test.");
}

void loop() {
  // Nothing to do — callback handles incoming data
  delay(1000);
  Serial.printf("t=%lu ms | ESP-NOW listening on ch%d\n", millis(), ESPNOW_CHANNEL);
}