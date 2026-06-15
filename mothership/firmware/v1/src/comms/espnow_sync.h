#pragma once

#include <Arduino.h>
#include <esp_now.h>

// ESP-NOW sync-only module for Mothership V1.
// Provides receive-only ESP-NOW on a fixed channel without WiFi AP.
// Used during RTC-alarm sync windows to collect node data.

// Callback type for received ESP-NOW data
typedef void (*EspNowRecvCallback)(const uint8_t* mac, const uint8_t* data, int len);

bool initEspNowSyncOnly(int channel);
void broadcastSyncWindowOpen();
void registerReceiveCallback(EspNowRecvCallback cb);
void espnowSyncLoop();