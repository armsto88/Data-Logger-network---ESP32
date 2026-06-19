#pragma once

#include <Arduino.h>

// Config-mode WiFi AP + web server for Mothership V1.
// Slim extract from production main.cpp — adapted to use V1's rtc_alarm.h
// and flash_logger.h instead of production's rtc_manager.h / sd_manager.h.

// Sync globals (defined in config_server.cpp, loaded/saved from NVS).
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

int computeAutoSyncMin(int wakeMin);

extern volatile bool gShutdownRequested;

void startConfigServer();
void configServerLoop();