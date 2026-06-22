#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include "protocol.h"  // node_snapshot_t

// Flash (LittleFS) logging module for Mothership V1.
// Mirrors the sd_logger interface so the firmware can fall back to
// on-chip flash when the SD card is unavailable (PCB MOSI routing bug).
//
// Stores data as /datalog.csv in LittleFS — same filename as the SD
// version so downstream tooling is compatible.

bool initFlash();
bool logSnapshotRow(const node_snapshot_t* snap);
bool logSnapshotBatch(const node_snapshot_t* snapshots, int count);
bool flashLogCSVRow(const String& row);
String flashGetCSVStats();
bool flashCreateCSVHeader();
bool flashIsReady();
bool flashMountFailed();
bool flashFormatExplicit();

// CSV download helpers (for future WiFi AP web server integration).
String readCSVFile();          // whole file as a String
size_t getCSVFileSize();       // bytes
int getCSVRecordCount();       // data rows (excludes header)
