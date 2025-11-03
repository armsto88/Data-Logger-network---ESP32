#include "espnow_manager.h"
#include <esp_now.h>
#include <WiFi.h>
#include "config.h"

// Structure to receive data - must match sender structure
typedef struct struct_message {
    char sensorData[64];
    float value;
    unsigned long timestamp;
} struct_message;

struct_message incomingData;

// Callback when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingBytes, int len) {
    Serial.print("ESP-NOW data received from: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", mac[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.println();
    
    memcpy(&incomingData, incomingBytes, sizeof(incomingData));
    Serial.print("Sensor Data: ");
    Serial.println(incomingData.sensorData);
    Serial.print("Value: ");
    Serial.println(incomingData.value);
    
    // Here you could log to SD card using sd_manager
    // String logEntry = String(incomingData.sensorData) + "," + String(incomingData.value);
    // logCSVRow(logEntry);
}

void setupESPNOW() {
    // Set device as a Wi-Fi Station
    WiFi.mode(WIFI_STA);
    
    // Init ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    
    // Register for receive callback
    esp_now_register_recv_cb(OnDataRecv);
    
    Serial.println("ESP-NOW initialized successfully");
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
}

void espnow_loop() {
    // ESP-NOW is event-driven, no need for continuous polling
    // Data reception is handled by the callback function
}