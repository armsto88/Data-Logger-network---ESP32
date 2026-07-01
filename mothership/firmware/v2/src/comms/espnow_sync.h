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
// Announce a new sync schedule (SET_SYNC_SCHED) to the fleet over the
// broadcast peer during a sync window. Used to hand a changed schedule to
// sleeping nodes at the moment they are awake on the OLD schedule.
void broadcastSyncScheduleNow(int syncIntervalMinutes, uint32_t phaseUnix);

// Announce the recording/wake interval (SET_SCHEDULE) to the fleet over the
// broadcast peer during a sync window. SET_SYNC_SCHED does NOT carry the wake
// interval, so this must be broadcast separately for a recording-interval
// change to reach deployed nodes. Nodes apply it only when it differs, so it
// is safe to broadcast every window.
void broadcastWakeIntervalNow(int intervalMinutes);
void registerReceiveCallback(EspNowRecvCallback cb);
void espnowSyncLoop();
void initSnapQueue(int depth);
int drainSnapQueue(EspNowSnapSlot* outSlots, int maxSlots);
uint32_t getSnapDropCount();
void deinitEspNowSync();
