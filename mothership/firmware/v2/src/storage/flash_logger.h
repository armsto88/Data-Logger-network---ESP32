#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include "protocol.h"  // node_snapshot_t, node_snapshot_v2_t, SENSOR_ID_*

// Flash (LittleFS) logging module for Mothership V1.
// Mirrors the sd_logger interface so the firmware can fall back to
// on-chip flash when the SD card is unavailable (PCB MOSI routing bug).
//
// Stores data as /datalog.csv in LittleFS — same filename as the SD
// version so downstream tooling is compatible.

// ---------------------------------------------------------------------------
// DecodedSnapshot — common representation for V1 and V2 snapshots
// ---------------------------------------------------------------------------
// Phase 3 of the V2 snapshot migration: both V1 (node_snapshot_t) and V2
// (node_snapshot_v2_t) wire packets decode into this common struct so the
// logging / upload / ACK paths can be protocol-version agnostic.

struct DecodedReading {
    uint16_t sensorId;
    float    value;
};

struct DecodedSnapshot {
    char     nodeId[16];
    uint32_t nodeTimestamp;
    uint32_t seqNum;
    uint16_t qualityFlags;
    uint16_t configVersion;
    uint8_t  protocolVersion;
    // V1-only: bitmask of channels that were present in the original packet.
    // For V2 snapshots this is synthesised from the readings so existing
    // CSV columns (sensorPresent) stay populated.
    uint16_t sensorPresent;
    DecodedReading readings[MAX_READINGS_PER_SNAPSHOT];
    size_t   readingCount;

    // Helper: find a reading by sensor ID
    const float* find(uint16_t sensorId) const {
        for (size_t i = 0; i < readingCount; ++i) {
            if (readings[i].sensorId == sensorId) return &readings[i].value;
        }
        return nullptr;
    }

    // Helper: check if a sensor is present
    bool hasSensor(uint16_t sensorId) const {
        return find(sensorId) != nullptr;
    }
};

// Decode a V1 node_snapshot_t into the common DecodedSnapshot form.
void decodeV1(const node_snapshot_t& snap, DecodedSnapshot& out);

// Decode V2 wire bytes (header + readings) into DecodedSnapshot.
// Returns false if the packet fails validation.
bool decodeV2(const uint8_t* data, int len, DecodedSnapshot& out);

// Convert a DecodedSnapshot back into a V1 node_snapshot_t so legacy
// sync-mode queue/drain paths (which expect node_snapshot_t) can handle
// V2 snapshots without restructuring. Sensors absent from the decoded
// snapshot are left as NaN / zero in the V1 struct.
void decodedToV1(const DecodedSnapshot& decoded, node_snapshot_t& out);

bool initFlash();
bool logSnapshotRow(const node_snapshot_t* snap);
bool logSnapshotBatch(const node_snapshot_t* snapshots, int count);
bool logDecodedSnapshot(const DecodedSnapshot& decoded);
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
