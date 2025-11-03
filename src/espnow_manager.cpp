#include "espnow_manager.h"
#include <esp_now.h>
#include <WiFi.h>

volatile bool newDataReceived = false;
String receivedCSV = "";

void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    receivedCSV = "";
    for (int i = 0; i < len; ++i) receivedCSV += (char)data[i];
    newDataReceived = true;
    Serial.print("Received ESP-NOW data: ");
    Serial.println(receivedCSV);
}

void setupESPNOW() {
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed!");
        return;
    }
    esp_now_register_recv_cb(onDataReceive);
    Serial.println("ESP-NOW ready (receiver)");
}

void espnow_loop() {
    if (newDataReceived) {
        // Add timestamp and log to SD card
        String row = getTimestamp() + "," + receivedCSV;
        logCSVRow(row);
        Serial.println("Logged row to SD: " + row);
        newDataReceived = false;
    }
}
