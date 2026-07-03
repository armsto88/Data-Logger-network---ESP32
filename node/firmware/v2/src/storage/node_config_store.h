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
  bool     recordingPaused;   // standby: deployed + syncing but not recording
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

// Expected/configured sensor mask (SNAP_PRESENT_* bits + NODE_SENSOR_MASK_VALID).
// Stored as a standalone NVS key — deliberately NOT part of the checksummed A/B
// record above, so adding it never changes the record length or risks
// invalidating a node's persisted deploy state on a firmware upgrade. Returns 0
// (auto / unset) when no mask has been configured.
uint16_t nodeSensorMaskLoad();
bool     nodeSensorMaskSave(uint16_t mask);

#ifdef NODE_CONFIG_STORE_TESTING
void nodeConfigStoreResetForTest();
bool nodeConfigStoreCorruptActiveForTest();
#endif

