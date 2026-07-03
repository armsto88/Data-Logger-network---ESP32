#pragma once

#include <Arduino.h>

// JSON upload payload builder for the Mothership V2 modem upload path.
//
// Emits the Supabase ingest batch format:
//   {"readings":[ {reading}, ... ], "meta":{"firmwareVersion":"..."}}
// up to 100 reading objects per POST. Reading field keys are identical to the
// CSV column headers (camelCase + spectral_NNN); project_id/mothership_id are
// derived server-side from the device API key and are NOT in the payload.
// Built with manual String concatenation (no ArduinoJson dependency).

// ---------------------------------------------------------------------------
// Status context — mothership-level telemetry sent alongside readings.
// Populated by the caller (handleSyncWake / handleManualUpload) and emitted
// as the "status" object in the batch POST body.  The backend stores it in
// mothership_status automatically.
// ---------------------------------------------------------------------------
struct StatusContext {
  float    batVoltage;          // status.batVoltage (volts)
  uint32_t flashUsagePct;       // status.upload.flashUsagePct
  uint64_t flashTotalBytes;     // status.upload.flashTotalBytes
  uint64_t flashUsedBytes;      // status.upload.flashUsedBytes
  const char* uploadReason;     // meta.uploadReason: "scheduled" or "manual"
  String   nextSyncLocal;       // status.nextSyncLocal (display string)
  int      wakeIntervalMinutes; // status.wakeIntervalMinutes
  int      syncIntervalMinutes; // status.syncIntervalMinutes
  const char* syncMode;         // status.syncMode: "interval" or "daily"
  uint16_t fleetTotal;          // status.fleet.total
  uint16_t fleetDeployed;       // status.fleet.deployed
  uint16_t fleetPaired;         // status.fleet.paired
  uint16_t fleetUnpaired;       // status.fleet.unpaired
  uint32_t pendingRows;         // status.upload.pendingRows
  uint32_t rowsUploaded;        // status.upload.rowsUploaded
  uint8_t  retryCount;          // status.upload.retryCount
  uint32_t lastUploadUnix;      // status.upload.lastUploadUnix
  const char* firmwareVersion;  // meta.firmwareVersion
  const char* firmwareBuild;    // meta.firmwareBuild
  uint32_t rtcUnix;             // status.rtcUnix (also meta.uploadTimeUnix)
  String   deviceId;            // status.deviceId (MAC string)
  uint16_t fleetPending;        // status.fleet.pending
  bool     uploadEnabled;       // status.upload.enabled
  uint32_t dataLogRecords;      // status.dataLog.records
  uint64_t dataLogCsvBytes;     // status.dataLog.csvSizeBytes
  String   dataLogLastSync;     // status.dataLog.lastConfirmedSync
  // --- Extended fields (full backend spec) ---
  String   nodesJson;           // status.nodes[] — pre-built "[...]" array
  String   transmissionJson;    // status.transmission{} — pre-built "{...}"
  String   syncDailyTime;       // status.syncDailyTime ("HH:MM", "" in interval mode)
  uint32_t pendingBytes;        // status.upload.pendingBytes
  uint32_t projectStartedUnix;  // status.projectStarted (0 = unknown)
  const char* lastUploadResult; // status.lastUploadResult ("success"/"failed"/"pending")
  String   modemJson;           // status.modem{} — pre-built "{...}" ("" = omit)
  String   diagnosticsJson;     // status.diagnostics{} — pre-built "{...}" ("" = omit)
  uint16_t fleetPaused;         // status.fleet.paused (deployed nodes in standby)
};

// ---------------------------------------------------------------------------
// Result of a build attempt
// ---------------------------------------------------------------------------
struct JsonPayload {
  String   body;             // full batch document: {"readings":[...],"meta":{...}}
  uint32_t byteLength;       // body.length()
  uint16_t rowCount;         // readings emitted
  bool     ok;               // false = build failed (heap)
  uint32_t csvBytesConsumed; // bytes of csvChunk consumed through the last
                             // emitted row (for cursor advancement)
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Build a batch document with up to maxReadings reading objects drawn from
// csvChunk (clamped to the backend's 100/POST limit). csvChunk is the raw CSV
// text (header + rows) from UploadQueue::getNewData(). fwVersion is reported in
// meta.firmwareVersion. csvBytesConsumed reports the byte offset within
// csvChunk of the first un-parsed data row so the caller can advance the upload
// cursor precisely.
//
// rtcFallbackUnix: the mothership RTC time (unix seconds). The backend rejects
// a null datetime, so any reading whose CSV datetime is missing/"unknown" is
// emitted with this RTC time (ISO-8601 + Z) instead of null — preventing a
// single bad row from 400-ing (and stalling) the whole batch.
JsonPayload buildJsonUpload(const String& csvChunk,
                            uint16_t maxReadings,
                            const String& fwVersion,
                            const StatusContext* status = nullptr,
                            uint32_t rtcFallbackUnix = 0);
