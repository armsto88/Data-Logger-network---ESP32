#pragma once

#include <Arduino.h>
#include "control/backend_command_ingest.h"

// Config-mode WiFi AP + web server for Mothership V1.
// Slim extract from production main.cpp — adapted to use V1's rtc_alarm.h
// and flash_logger.h instead of production's rtc_manager.h / sd_manager.h.

// Sync globals (defined in config_server.cpp, loaded/saved from NVS).
enum SyncMode {
  SYNC_MODE_DAILY = 0,
  SYNC_MODE_INTERVAL = 1,
};

extern int gWakeIntervalMin;
extern int gSyncIntervalMin;
extern int gSyncMode;
extern int gSyncDailyHour;
extern int gSyncDailyMinute;
extern unsigned long gLastSyncBroadcastUnix;
extern const int kAllowedIntervals[];

// NVS loaders (call once at boot before startConfigServer()).
void loadWakeIntervalFromNVS();
void loadSyncModeFromNVS();
void loadDailySyncTimeFromNVS();
void loadSyncRuntimeGuardsFromNVS();
void saveSyncRuntimeGuardsToNVS();

// Durable control-protocol-2 FieldHub-wide schedule transaction. The executor
// persists the global setting and every assigned node's desired config before
// the dispatcher can expose ACCEPTED.
void configInitRecordingIntervalControl();
BackendCommandApplyResult configApplyBackendRecordingInterval(
    const Command& command);
bool configMarkRecordingIntervalNodeConverged(const char* nodeId,
                                              uint16_t configVersion);

int computeAutoSyncMin(int wakeMin);

extern volatile bool gShutdownRequested;

// Device identification (defined in config_server.cpp, used by json_payload
// builder via JsonUploadContext populated in main.cpp).
extern const char* DEVICE_ID;
extern const char* FW_VERSION;
extern const char* FW_BUILD;

// Sync schedule helpers (defined in config_server.cpp).
String formatSyncTimeHHMM(int hh, int mm);
String computeNextSyncIsoLocal();

void startConfigServer();
void configServerLoop();
