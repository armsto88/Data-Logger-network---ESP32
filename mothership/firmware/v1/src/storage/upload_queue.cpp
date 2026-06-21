#include "storage/upload_queue.h"
#include <Preferences.h>

static const char* kTxNamespace = "tx";
static const char* kDataFile    = "/datalog.csv";
static const char* kTempFile    = "/datalog_tmp.csv";

// ---------------------------------------------------------------------------
// Construction / init
// ---------------------------------------------------------------------------
UploadQueue::UploadQueue()
    : m_initialised(false) {
  m_cursor.byteOffset    = 0;
  m_cursor.rowsUploaded   = 0;
  m_cursor.lastUploadUnix = 0;
  m_cursor.retryCount     = 0;
  m_cursor.wakeCounter    = 0;
}

bool UploadQueue::init() {
  loadCursor();
  validateCursor();
  m_initialised = true;
  Serial.printf("[UQ] init: offset=%u rows=%u wakes=%u\n",
                (unsigned)m_cursor.byteOffset,
                (unsigned)m_cursor.rowsUploaded,
                (unsigned)m_cursor.wakeCounter);
  return true;
}

// ---------------------------------------------------------------------------
// NVS load / save
// ---------------------------------------------------------------------------
void UploadQueue::loadCursor() {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, true)) {   // read-only
    Serial.println("[UQ] NVS begin(\"tx\") failed — cursor defaults to 0");
    return;
  }
  m_cursor.byteOffset    = prefs.getUInt("cursor_offset", 0);
  m_cursor.rowsUploaded   = prefs.getUInt("rows_uploaded", 0);
  m_cursor.lastUploadUnix = prefs.getUInt("last_upload_unix", 0);
  m_cursor.retryCount     = prefs.getUChar("retry_count", 0);
  m_cursor.wakeCounter    = prefs.getUInt("wake_counter", 0);
  prefs.end();
}

void UploadQueue::saveCursor() {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, false)) {   // read-write
    Serial.println("[UQ] NVS begin(\"tx\") failed — cursor not saved");
    return;
  }
  prefs.putUInt("cursor_offset", m_cursor.byteOffset);
  prefs.putUInt("rows_uploaded", m_cursor.rowsUploaded);
  prefs.putUInt("last_upload_unix", m_cursor.lastUploadUnix);
  prefs.putUChar("retry_count", m_cursor.retryCount);
  prefs.putUInt("wake_counter", m_cursor.wakeCounter);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
uint32_t UploadQueue::headerEndOffset() const {
  // Header line length + '\n' (println adds \n).
  return (uint32_t)(strlen(kUploadCSVHeader) + 1);
}

void UploadQueue::validateCursor() {
  File f = LittleFS.open(kDataFile, "r");
  if (!f) {
    Serial.println("[UQ] validateCursor: cannot open datalog.csv — resetting");
    m_cursor.byteOffset = headerEndOffset();
    return;
  }
  size_t fileSize = f.size();
  f.close();

  if (m_cursor.byteOffset > fileSize || m_cursor.byteOffset < headerEndOffset()) {
    Serial.printf("[UQ] validateCursor: offset %u > file %u — resetting to header end\n",
                  (unsigned)m_cursor.byteOffset, (unsigned)fileSize);
    m_cursor.byteOffset = headerEndOffset();
    saveCursor();
  }
}

uint32_t UploadQueue::getPendingBytes() const {
  File f = LittleFS.open(kDataFile, "r");
  if (!f) return 0;
  size_t fileSize = f.size();
  f.close();
  if (fileSize <= m_cursor.byteOffset) return 0;
  return (uint32_t)(fileSize - m_cursor.byteOffset);
}

uint32_t UploadQueue::getPendingRows() const {
  File f = LittleFS.open(kDataFile, "r");
  if (!f) return 0;
  if (!f.seek(m_cursor.byteOffset)) {
    f.close();
    return 0;
  }
  uint32_t rows = 0;
  while (f.available()) {
    if (f.read() == '\n') rows++;
  }
  f.close();
  return rows;
}

// ---------------------------------------------------------------------------
// getNewData
// ---------------------------------------------------------------------------
UploadPayload UploadQueue::getNewData(uint32_t maxBytes) {
  UploadPayload payload;
  payload.csvData    = String();
  payload.byteLength  = 0;
  payload.startOffset = m_cursor.byteOffset;
  payload.rowEstimate = 0;

  File f = LittleFS.open(kDataFile, "r");
  if (!f) {
    Serial.println("[UQ] getNewData: cannot open datalog.csv");
    return payload;
  }

  if (!f.seek(m_cursor.byteOffset)) {
    Serial.println("[UQ] getNewData: seek failed — offset past EOF");
    f.close();
    return payload;
  }

  // Prepend CSV header.
  payload.csvData  = String(kUploadCSVHeader);
  payload.csvData += "\n";

  // Read up to maxBytes, ending on a '\n' boundary.
  uint32_t bytesRead = 0;
  bool hitNewline = false;
  while (f.available() && bytesRead < maxBytes) {
    int c = f.read();
    if (c < 0) break;
    payload.csvData += (char)c;
    bytesRead++;
    if (c == '\n') {
      hitNewline = true;
      payload.rowEstimate++;
      // If we've read at least maxBytes we can stop now that we're on a boundary.
      if (bytesRead >= maxBytes) break;
    }
  }

  // If we stopped mid-row (no trailing newline) and hit maxBytes, continue
  // reading until the next '\n' so we don't split a row.
  if (!hitNewline && bytesRead >= maxBytes && f.available()) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      payload.csvData += (char)c;
      bytesRead++;
      if (c == '\n') {
        payload.rowEstimate++;
        break;
      }
    }
  }

  f.close();
  payload.byteLength = bytesRead;
  return payload;
}

// ---------------------------------------------------------------------------
// advanceCursor
// ---------------------------------------------------------------------------
bool UploadQueue::advanceCursor(uint32_t newOffset, uint32_t timestampUnix) {
  m_cursor.byteOffset    = newOffset;
  m_cursor.lastUploadUnix = timestampUnix;
  // Count rows in the just-uploaded chunk for bookkeeping.
  // (rowEstimate is provided by the caller via getNewData; we don't recount
  //  here to avoid re-reading the file.  rowsUploaded is updated by the
  //  caller if needed — but we can increment by the pending rows delta.)
  // For simplicity, increment rowsUploaded by the number of rows between
  // the old and new offset.
  // Caller should set rowsUploaded if exact tracking is needed.
  saveCursor();
  Serial.printf("[UQ] advanceCursor: offset=%u ts=%u\n",
                (unsigned)newOffset, (unsigned)timestampUnix);
  return true;
}

// ---------------------------------------------------------------------------
// purgeUploaded — streaming rewrite
// ---------------------------------------------------------------------------
bool UploadQueue::purgeUploaded() {
  File rf = LittleFS.open(kDataFile, "r");
  if (!rf) {
    Serial.println("[UQ] purgeUploaded: cannot open datalog.csv");
    return false;
  }
  File wf = LittleFS.open(kTempFile, "w", true);
  if (!wf) {
    Serial.println("[UQ] purgeUploaded: cannot open datalog_tmp.csv");
    rf.close();
    return false;
  }

  // Write header to temp.
  wf.println(kUploadCSVHeader);

  // Seek read file to cursor.
  if (!rf.seek(m_cursor.byteOffset)) {
    Serial.println("[UQ] purgeUploaded: seek failed — aborting");
    rf.close();
    wf.close();
    LittleFS.remove(kTempFile);
    return false;
  }

  // Copy remaining bytes in 512-byte chunks.
  uint8_t buf[512];
  size_t totalCopied = 0;
  while (rf.available()) {
    int n = rf.read(buf, sizeof(buf));
    if (n <= 0) break;
    wf.write(buf, n);
    totalCopied += n;
  }
  wf.close();
  rf.close();

  // Swap files.
  LittleFS.remove(kDataFile);
  if (!LittleFS.rename(kTempFile, kDataFile)) {
    Serial.println("[UQ] purgeUploaded: rename failed — data may be lost!");
    return false;
  }

  // Reset cursor to header end.
  m_cursor.byteOffset = headerEndOffset();
  saveCursor();

  Serial.printf("[UQ] purgeUploaded: copied %u bytes, cursor reset to %u\n",
                (unsigned)totalCopied, (unsigned)m_cursor.byteOffset);
  return true;
}

// ---------------------------------------------------------------------------
// emergencyPurgeIfFull
// ---------------------------------------------------------------------------
bool UploadQueue::emergencyPurgeIfFull(uint8_t thresholdPct) {
  uint32_t total = LittleFS.totalBytes();
  uint32_t used  = LittleFS.usedBytes();
  if (total == 0) return true;   // avoid div-by-zero

  uint32_t usedPct = (used * 100) / total;
  if (usedPct <= thresholdPct) return true;   // no purge needed

  Serial.printf("[UQ] emergencyPurge: usage %u%% > threshold %u%%\n",
                (unsigned)usedPct, (unsigned)thresholdPct);

  // Count total data rows.
  File rf = LittleFS.open(kDataFile, "r");
  if (!rf) {
    Serial.println("[UQ] emergencyPurge: cannot open datalog.csv");
    return false;
  }

  // First, find the byte offset of each newline so we can compute skip/keep
  // boundaries without holding the whole file in RAM.
  // We need: totalDataRows, and the byte offset after rowsToSkip data rows.
  uint32_t totalDataRows = 0;
  // Skip header line.
  String header = rf.readStringUntil('\n');
  uint32_t firstDataOffset = rf.position();

  // Collect newline offsets (relative to file start) for data rows.
  // To avoid unbounded memory, we only need the offset of the last row we
  // will skip.  So: count rows, then re-scan to find the boundary.
  while (rf.available()) {
    if (rf.read() == '\n') totalDataRows++;
  }
  rf.close();

  if (totalDataRows == 0) {
    Serial.println("[UQ] emergencyPurge: no data rows — nothing to purge");
    return true;
  }

  uint32_t rowsToKeep = totalDataRows / 2;        // keep newest 50%
  uint32_t rowsToSkip = totalDataRows - rowsToKeep; // discard oldest 50%

  Serial.printf("[UQ] emergencyPurge: totalRows=%u skip=%u keep=%u\n",
                (unsigned)totalDataRows, (unsigned)rowsToSkip, (unsigned)rowsToKeep);

  // Re-open and find the byte offset after skipping rowsToSkip data rows.
  rf = LittleFS.open(kDataFile, "r");
  if (!rf) return false;
  rf.readStringUntil('\n');   // consume header

  uint32_t skipBoundaryOffset = rf.position();   // offset after header
  uint32_t skipped = 0;
  while (rf.available() && skipped < rowsToSkip) {
    int c = rf.read();
    if (c < 0) break;
    skipBoundaryOffset++;
    if (c == '\n') skipped++;
  }
  // skipBoundaryOffset is now the byte position of the first row to keep.

  // Was the cursor in the skipped region?
  bool cursorInSkippedRegion = (m_cursor.byteOffset < skipBoundaryOffset);

  // Open temp file and write header.
  File wf = LittleFS.open(kTempFile, "w", true);
  if (!wf) {
    Serial.println("[UQ] emergencyPurge: cannot open temp file");
    rf.close();
    return false;
  }
  wf.println(kUploadCSVHeader);

  // Copy from skipBoundaryOffset to EOF.
  if (!rf.seek(skipBoundaryOffset)) {
    Serial.println("[UQ] emergencyPurge: seek to skip boundary failed");
    rf.close();
    wf.close();
    LittleFS.remove(kTempFile);
    return false;
  }

  uint8_t buf[512];
  size_t totalCopied = 0;
  while (rf.available()) {
    int n = rf.read(buf, sizeof(buf));
    if (n <= 0) break;
    wf.write(buf, n);
    totalCopied += n;
  }
  wf.close();
  rf.close();

  // Swap files.
  LittleFS.remove(kDataFile);
  if (!LittleFS.rename(kTempFile, kDataFile)) {
    Serial.println("[UQ] emergencyPurge: rename failed — data may be lost!");
    return false;
  }

  // Adjust cursor.
  if (cursorInSkippedRegion) {
    // Cursor was in the purged region — reset to header end (data lost).
    Serial.println("[UQ] emergencyPurge: cursor was in purged region — resetting");
    m_cursor.byteOffset = headerEndOffset();
  } else {
    // Cursor was in the kept region — shift it by the number of bytes purged
    // before it (skipBoundaryOffset - firstDataOffset).
    uint32_t purgedBytes = skipBoundaryOffset - firstDataOffset;
    if (m_cursor.byteOffset > purgedBytes + headerEndOffset()) {
      m_cursor.byteOffset -= purgedBytes;
    } else {
      m_cursor.byteOffset = headerEndOffset();
    }
  }
  saveCursor();

  Serial.printf("[UQ] emergencyPurge: done, copied %u bytes, cursor=%u\n",
                (unsigned)totalCopied, (unsigned)m_cursor.byteOffset);
  return true;
}

// ---------------------------------------------------------------------------
// Wake / retry policy
// ---------------------------------------------------------------------------
bool UploadQueue::shouldUploadThisWake(uint8_t policyIntervalWakes) const {
  if (policyIntervalWakes == 0) return true;   // every wake
  return (m_cursor.wakeCounter % policyIntervalWakes) == 0;
}

void UploadQueue::incrementWakeCounter() {
  m_cursor.wakeCounter++;
  saveCursor();
}

void UploadQueue::resetRetryCount() {
  m_cursor.retryCount = 0;
  saveCursor();
}

void UploadQueue::incrementRetryCount() {
  m_cursor.retryCount++;
  saveCursor();
}

bool UploadQueue::maxRetriesExceeded(uint8_t maxRetries) const {
  return m_cursor.retryCount >= maxRetries;
}