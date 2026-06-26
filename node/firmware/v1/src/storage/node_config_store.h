#pragma once

#include <Arduino.h>

struct NodeConfigStoreRecord {
  uint8_t mothershipMac[6];
  uint8_t state;
  bool rtcSynced;
  bool deployed;
  bool rtcPowerLost;
  uint8_t recoveryReason;
  uint8_t wakeIntervalMin;
  uint16_t syncIntervalMin;
  uint32_t syncPhaseUnix;
  uint32_t lastTimeSyncUnix;
  uint32_t lastSyncSlot;
  uint16_t appliedConfigVersion;
};

enum class NodeConfigLoadStatus : uint8_t {
  LoadedPrimary = 0,
  LoadedSecondary,
  MigratedLegacy,
  NoValidConfig
};

bool nodeConfigStoreLoad(NodeConfigStoreRecord& out,
                         NodeConfigLoadStatus* statusOut = nullptr);
bool nodeConfigStoreSave(const NodeConfigStoreRecord& record);

#ifdef NODE_CONFIG_STORE_TESTING
void nodeConfigStoreResetForTest();
bool nodeConfigStoreCorruptActiveForTest();
#endif

