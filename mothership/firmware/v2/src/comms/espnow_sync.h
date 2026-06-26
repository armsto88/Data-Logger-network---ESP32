#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include "protocol.h"

// ESP-NOW sync-only module for Mothership V1.
// Provides receive-only ESP-NOW on a fixed channel without WiFi AP.
// Used during RTC-alarm sync windows to collect node data.

// Callback type for received ESP-NOW data
typedef void (*EspNowRecvCallback)(const uint8_t* mac, const uint8_t* data, int len);

struct EspNowSnapSlot {
  uint8_t mac[6];
  node_snapshot_t snap;
};

bool initEspNowSyncOnly(int channel);
void broadcastSyncWindowOpen();
void registerReceiveCallback(EspNowRecvCallback cb);
void espnowSyncLoop();
void initSnapQueue(int depth);
int drainSnapQueue(EspNowSnapSlot* outSlots, int maxSlots);
uint32_t getSnapDropCount();
void deinitEspNowSync();
