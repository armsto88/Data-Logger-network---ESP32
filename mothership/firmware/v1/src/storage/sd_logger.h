#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// SD card logging module for Mothership V1.
// Manages SPI SD card init and CSV logging of node snapshots.

// Snapshot structure for logging (matches protocol.h sensor_data_message_t)
struct NodeSnapshot {
  char     nodeId[16];
  char     sensorType[16];
  char     sensorLabel[24];
  uint16_t sensorId;
  float    value;
  unsigned long nodeTimestamp;
  uint16_t qualityFlags;
};

bool initSD();
bool logSnapshot(const NodeSnapshot* snap);
bool logCSVRow(const String& row);
String getCSVStats();
bool createCSVHeader();
bool sdIsReady();