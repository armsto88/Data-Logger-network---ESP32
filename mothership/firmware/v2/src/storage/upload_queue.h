#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include "storage/csv_schema.h"

// Upload cursor / queue manager for the Mothership V1 modem upload subsystem.
//
// Tracks how much of /datalog.csv has been successfully uploaded to the
// remote endpoint. The cursor (byte offset) is persisted in NVS namespace
// "tx" so it survives deep-sleep / power cycles.
//
// Upload acknowledgement advances a cursor but does not delete local history.
// LittleFS is a bounded rolling store: at a safe high-water mark, the oldest
// rows are removed with a crash-safe streaming rewrite.

static constexpr uint8_t kLittleFsRetentionHighWaterPct = 65;
static constexpr uint8_t kLittleFsRetentionKeepPct = 40;

// ---------------------------------------------------------------------------
// CSV header (must match flash_logger.cpp)
// ---------------------------------------------------------------------------
static constexpr const char* kUploadCSVHeader = kCurrentCSVHeader35;

// True while /datalog.csv still carries a pre-epoch header, i.e. queued rows
// exist that have no deploymentEpoch column. Those rows would follow the
// backend's fallback into whatever deployment is active at ingest, so any
// operation that changes which deployment that is must be blocked until they
// drain. pendingRowsOut (optional) receives an estimated row count for the
// operator-facing message.
bool uploadQueueHasLegacyRows(uint32_t* pendingRowsOut);

// ---------------------------------------------------------------------------
// Cursor — persisted in NVS
// ---------------------------------------------------------------------------
struct UploadCursor {
  uint32_t byteOffset;     // byte position in /datalog.csv after last uploaded row
  uint32_t rowsUploaded;    // cumulative count of rows successfully uploaded
  uint32_t lastUploadUnix;  // timestamp of last successful upload
  uint8_t  retryCount;      // current retry count within this window
  uint32_t wakeCounter;     // sync-wake counter for upload policy scheduling
  uint32_t nextAttemptUnix; // earliest retry time after reaching retry limit
  uint32_t rowsRemovedLocally; // cumulative rows evicted from bounded history
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

  // Recover the data file, load the cursor from NVS, and validate it against
  // the current file. Returns false if recovery or the cursor load failed, in
  // which case the queue is NOT initialised and every accessor below is
  // reporting defaults rather than stored state.
  //
  // Callers must honour the return value. A failed init leaves a zero cursor,
  // and a zero cursor is indistinguishable from "nothing uploaded yet" — so a
  // caller that ignores this either re-uploads history or renders a convincing
  // zero. Idempotent: once initialised it returns true without redoing the work
  // (this is what keeps a page load from re-running init), and after a failure a
  // later call retries.
  bool init();

  // True once init() has succeeded. Accessor values are only meaningful then.
  bool isInitialised() const { return m_initialised; }

  // Read new data from /datalog.csv starting at the cursor, up to maxBytes.
  // The payload is prefixed with the CSV header.  Reading stops at a row
  // boundary (next '\n') so rows are never split.
  UploadPayload getNewData(uint32_t maxBytes);

  // Advance the cursor after a successful upload.
  // newOffset  — byte offset of the first un-uploaded byte.
  // timestampUnix — RTC timestamp to store as lastUploadUnix (0 if unknown).
  // rowsUploadedDelta — rows actually uploaded in this chunk; added to the
  //   cumulative rowsUploaded counter (0 for malformed-skip advances).
  bool advanceCursor(uint32_t newOffset, uint32_t timestampUnix,
                     uint32_t rowsUploadedDelta = 0);

  // Streaming rewrite: keep only the un-uploaded portion of /datalog.csv.
  // Writes header + rows from cursor to EOF into /datalog_tmp.csv, then
  // swaps files and resets the cursor to the header end.
  bool purgeUploaded();

  // One-time compatibility compaction only. A legacy 25/30-column file must be
  // rewritten after its pending rows drain so new epoch-bearing rows can use
  // the current header. Current-schema files are deliberately left intact.
  bool purgeUploadedIfLegacyDrained();

  // If LittleFS usage exceeds thresholdPct, retain a bounded portion of the
  // newest rows via streaming rewrite. Adjusts the cursor if it was in the
  // removed region.
  bool emergencyPurgeIfFull(uint8_t thresholdPct);

  bool recoverDataFile();
  void validateCursor();

  // Accessors
  UploadCursor getCursor() const { return m_cursor; }
  uint32_t getPendingBytes() const;
  uint32_t getPendingRows() const;

  // Wake / retry policy helpers
  bool shouldUploadThisWake(uint8_t policyIntervalWakes) const;
  void incrementWakeCounter();
  void resetRetryCount();
  void incrementRetryCount();
  void incrementRetryCount(uint32_t nowUnix, uint32_t cooldownSec);
  bool maxRetriesExceeded(uint8_t maxRetries) const;
  bool maxRetriesExceeded(uint8_t maxRetries, uint32_t nowUnix) const;

  // --- Poison-row tracking --------------------------------------------------
  // A non-retryable rejection does not advance the cursor, so one unparseable
  // row wedges the queue forever and the emergency purge eventually eats the
  // buffer from the oldest end. Counting consecutive rejections at the SAME
  // offset lets the caller skip past the offending chunk after a bounded number
  // of attempts, turning a silent permanent stall into a visible bounded loss.
  // Durable, because the hub cold-boots between wakes.
  uint8_t noteNonRetryableFailure(uint32_t offset);
  void    clearNonRetryableFailures();
  uint8_t nonRetryableFailureCount(uint32_t offset) const;

#ifdef UQ_TEST_INIT_FAILURE_HOOK
  // Test-only failure injection for init(). Compiled ONLY into the upload-queue
  // test environment, never into production firmware. This fails the QUEUE
  // cursor load; it is unrelated to the transmission-settings save hook in
  // config/transmission_settings.h, which fails a different namespace at a
  // different layer.
  static void testForceInitFailure(bool on);
#endif

 private:
  // Persist m_cursor to NVS namespace "tx".
  void saveCursor();
  // Load m_cursor from NVS. Returns false if the namespace could not be opened,
  // i.e. the cursor in RAM is defaults rather than what is stored.
  bool loadCursor();
  // Persist the poison-row counters to NVS namespace "tx".
  void savePoisonState();
  // Byte offset of the first data row (end of header line).
  uint32_t headerEndOffset() const;

  UploadCursor m_cursor;
  bool m_initialised;
  uint32_t m_poisonOffset;   // cursor offset the failures are counted against
  uint8_t  m_poisonCount;    // consecutive non-retryable rejections there
};
