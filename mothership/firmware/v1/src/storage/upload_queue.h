#pragma once

#include <Arduino.h>
#include <LittleFS.h>

// Upload cursor / queue manager for the Mothership V1 modem upload subsystem.
//
// Tracks how much of /datalog.csv has been successfully uploaded to the
// Google Cloud Function endpoint.  The cursor (byte offset) is persisted in
// NVS namespace "tx" so it survives deep-sleep / power cycles.
//
// Purge strategy: single-file streaming rewrite (no temp index files).
// Emergency purge: when LittleFS usage exceeds a threshold, discard the
// oldest 50% of data rows.

// ---------------------------------------------------------------------------
// CSV header (must match flash_logger.cpp)
// ---------------------------------------------------------------------------
static constexpr const char* kUploadCSVHeader =
    "datetime,nodeId,seqNum,sensorPresent,qualityFlags,configVersion,"
    "batVoltage,airTemp,airHumidity,"
    "spectral_415,spectral_445,spectral_480,spectral_515,"
    "spectral_555,spectral_590,spectral_630,spectral_680,"
    "windSpeed,windDir,soil1Vwc,soil1Temp,soil2Vwc,soil2Temp,aux1,aux2";

// ---------------------------------------------------------------------------
// Cursor — persisted in NVS
// ---------------------------------------------------------------------------
struct UploadCursor {
  uint32_t byteOffset;     // byte position in /datalog.csv after last uploaded row
  uint32_t rowsUploaded;    // cumulative count of rows successfully uploaded
  uint32_t lastUploadUnix;  // timestamp of last successful upload
  uint8_t  retryCount;      // current retry count within this window
  uint32_t wakeCounter;     // sync-wake counter for upload policy scheduling
};

// ---------------------------------------------------------------------------
// Payload — produced by getNewData()
// ---------------------------------------------------------------------------
struct UploadPayload {
  String  csvData;       // CSV header + rows from cursor to EOF (or maxBytes)
  uint32_t byteLength;   // payload size in bytes (data portion, excluding header)
  uint32_t startOffset;  // byte offset where this payload starts (= cursor)
  uint32_t rowEstimate;  // approximate row count in data portion
};

// ---------------------------------------------------------------------------
// UploadQueue
// ---------------------------------------------------------------------------
class UploadQueue {
 public:
  UploadQueue();

  // Load cursor from NVS and validate against the current file.
  // If the file is smaller than the stored offset, reset to header end.
  bool init();

  // Read new data from /datalog.csv starting at the cursor, up to maxBytes.
  // The payload is prefixed with the CSV header.  Reading stops at a row
  // boundary (next '\n') so rows are never split.
  UploadPayload getNewData(uint32_t maxBytes);

  // Advance the cursor after a successful upload.
  // newOffset  — byte offset of the first un-uploaded byte.
  // timestampUnix — RTC timestamp to store as lastUploadUnix (0 if unknown).
  bool advanceCursor(uint32_t newOffset, uint32_t timestampUnix);

  // Streaming rewrite: keep only the un-uploaded portion of /datalog.csv.
  // Writes header + rows from cursor to EOF into /datalog_tmp.csv, then
  // swaps files and resets the cursor to the header end.
  bool purgeUploaded();

  // If LittleFS usage exceeds thresholdPct, purge the oldest 50% of data
  // rows via streaming rewrite.  Adjusts the cursor if it was in the
  // purged region.
  bool emergencyPurgeIfFull(uint8_t thresholdPct);

  // Accessors
  UploadCursor getCursor() const { return m_cursor; }
  uint32_t getPendingBytes() const;
  uint32_t getPendingRows() const;

  // Wake / retry policy helpers
  bool shouldUploadThisWake(uint8_t policyIntervalWakes) const;
  void incrementWakeCounter();
  void resetRetryCount();
  void incrementRetryCount();
  bool maxRetriesExceeded(uint8_t maxRetries) const;

 private:
  // Persist m_cursor to NVS namespace "tx".
  void saveCursor();
  // Load m_cursor from NVS.
  void loadCursor();
  // If file size < cursor offset, reset cursor to header end.
  void validateCursor();
  // Byte offset of the first data row (end of header line).
  uint32_t headerEndOffset() const;

  UploadCursor m_cursor;
  bool m_initialised;
};