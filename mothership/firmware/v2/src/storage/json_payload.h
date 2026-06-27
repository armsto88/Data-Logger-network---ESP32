#pragma once

#include <Arduino.h>
#include "storage/upload_queue.h"
#include "config/transmission_settings.h"

// JSON upload payload builder for the Mothership V2 modem upload path.
//
// Produces JSON documents matching docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md
// using manual String concatenation (no ArduinoJson dependency).  The
// builder is a pure serialisation concern: all runtime data that it cannot
// gather from shared headers (node_registry.h) is passed in via
// JsonUploadContext, so the module has zero dependency on config_server.cpp
// internals.

// ---------------------------------------------------------------------------
// Context — populated by the caller (performModemUpload in main.cpp)
// ---------------------------------------------------------------------------
struct JsonUploadContext {
  // meta section
  const char* deviceId;        // DEVICE_ID
  const char* fwVersion;       // FW_VERSION
  const char* fwBuild;         // FW_BUILD
  uint32_t    uploadTimeUnix;  // getRTCTime() at call time
  const char* uploadReason;    // "scheduled" | "manual" | "config_change"

  // transmission settings (status.transmission + URL building)
  const TransmissionSettings& tx;

  // upload subsystem state (status.upload)
  const UploadCursor& cursor;
  uint32_t pendingBytes;
  uint32_t pendingRows;

  // flash / LittleFS stats (status.upload.flash*)
  uint64_t flashTotalBytes;
  uint64_t flashUsedBytes;

  // dataLog stats (status.dataLog)
  uint32_t dataLogRecords;
  uint64_t dataLogCsvBytes;
  String   lastConfirmedSyncIso;  // datetime of last CSV row, or ""

  // sync schedule globals (status top-level) — passed in so the builder
  // does not depend on config_server.cpp file-scope globals.
  int    wakeIntervalMin;
  int    syncIntervalMin;
  int    syncMode;            // SYNC_MODE_INTERVAL / SYNC_MODE_DAILY
  int    syncDailyHour;
  int    syncDailyMinute;
  String syncDailyTimeHHMM;  // pre-formatted "HH:MM"
  String nextSyncLocalIso;   // pre-computed by caller

  // Dashboard status fields (status.batVoltage /
  // lastUploadResult).  Populated by main.cpp.
  float       batVoltage;        // mothership battery voltage (volts)
  const char* lastUploadResult;  // "success" | "failed" | "partial" | "pending"
  uint32_t    projectStartedUnix;  // first-ever boot timestamp (0 = unknown)
};

// ---------------------------------------------------------------------------
// Result of a build attempt
// ---------------------------------------------------------------------------
struct JsonPayload {
  String   body;             // full JSON document
  uint32_t byteLength;       // body.length()
  uint16_t rowCount;         // readings emitted
  bool     ok;               // false = build failed (heap/parse)
  uint32_t csvBytesConsumed; // byte offset within csvChunk of the first
                             // un-parsed row (for cursor advancement)
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Build a single JSON upload document containing meta + (optional status) +
// up to maxReadings rows drawn from csvChunk.  csvChunk is the raw CSV text
// (header + rows) as produced by UploadQueue::getNewData().
//
// includeStatus: true on the first POST of a multi-POST session, false on
// subsequent chunks (status is large and only sent once per sync cycle).
JsonPayload buildJsonUpload(const JsonUploadContext& ctx,
                            const String& csvChunk,
                            uint16_t maxReadings,
                            bool includeStatus);

// Build only the meta + status document (no readings) for a manual/config
// sync where there is no new data but the dashboard still needs status.
JsonPayload buildJsonStatusOnly(const JsonUploadContext& ctx);

// ---------------------------------------------------------------------------
// DataLog stats helper — reads /datalog.csv for record count, byte size,
// and the datetime of the last row.  Used by main.cpp to populate the
// JsonUploadContext.dataLog* fields without duplicating file-scan logic.
// ---------------------------------------------------------------------------
struct DataLogStats {
  uint32_t records;
  uint64_t csvBytes;
  String   lastConfirmedSyncIso;
};
DataLogStats readDataLogStats();