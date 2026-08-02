#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// Durable field archive. LittleFS remains the upload cursor/cache and fallback;
// when a card is mounted, the same canonical reading row is also appended here
// and is never removed after upload.

struct DeploymentEvent;

bool initSD();
bool sdIsReady();
bool sdHadWriteError();
bool sdLogCSVRow(const String& row);
bool sdAppendDeploymentEvent(const char* nodeId, const DeploymentEvent& event);
const char* sdReadingsPath();
const char* sdDeploymentsPath();
uint64_t sdReadingsFileSize();
uint64_t sdTotalBytes();
uint64_t sdUsedBytes();
