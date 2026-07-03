#include "storage/upload_queue.h"
#include <Preferences.h>

static const char* kTxNamespace = "tx";
static const char* kDataFile    = "/datalog.csv";
static const char* kTempFile    = "/datalog_tmp.csv";
static const char* kBackupFile  = "/datalog_bak.csv";

static bool commitTempDataFile(const char* context) {
  LittleFS.remove(kBackupFile);

  const bool hadDataFile = LittleFS.exists(kDataFile);
  if (hadDataFile && !LittleFS.rename(kDataFile, kBackupFile)) {
    Serial.printf("[UQ] %s: failed to rename data file to backup\n", context);
    return false;
  }

  if (!LittleFS.rename(kTempFile, kDataFile)) {
    Serial.printf("[UQ] %s: temp-to-data rename failed; restoring backup\n", context);
    if (hadDataFile) LittleFS.rename(kBackupFile, kDataFile);
    return false;
  }

  if (hadDataFile && !LittleFS.remove(kBackupFile)) {
    Serial.printf("[UQ] %s: warning - backup cleanup deferred to recovery\n", context);
  }
  return true;
}

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
  m_cursor.nextAttemptUnix = 0;
}

bool UploadQueue::init() {
  if (!recoverDataFile()) {
    Serial.println("[UQ] init: data-file recovery failed");
    return false;
  }
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
  // Open read-write so we can remove a stale key left by older firmware.
  // NVS keys are limited to 15 characters; "last_upload_unix" (17 chars)
  // exceeds that limit and triggers KEY_TOO_LONG errors on every putUInt()
  // call in this namespace until it is removed.
  if (!prefs.begin(kTxNamespace, false)) {   // read-write
    Serial.println("[UQ] NVS begin(\"tx\") failed — cursor defaults to 0");
    return;
  }
  m_cursor.byteOffset    = prefs.getUInt("cursor_offset", 0);
  m_cursor.rowsUploaded   = prefs.getUInt("rows_uploaded", 0);
  m_cursor.lastUploadUnix = prefs.getUInt("last_upload", 0);
  m_cursor.retryCount     = prefs.getUChar("retry_count", 0);
  m_cursor.wakeCounter    = prefs.getUInt("wake_counter", 0);
  // NVS keys are limited to 15 characters; keep this key deliberately short.
  m_cursor.nextAttemptUnix = prefs.getUInt("next_attempt", 0);
  // Clean up stale key from older firmware (NVS keys max 15 chars).
  prefs.remove("last_upload_unix");
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
  prefs.putUInt("last_upload", m_cursor.lastUploadUnix);
  prefs.putUChar("retry_count", m_cursor.retryCount);
  prefs.putUInt("wake_counter", m_cursor.wakeCounter);
  prefs.putUInt("next_attempt", m_cursor.nextAttemptUnix);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read the first line (header) of /datalog.csv and return its byte length
// including the line terminator. Falls back to the compile-time header
// length when the file cannot be opened. This keeps the upload queue
// correct once the CSV header becomes dynamic (Phase 4) without changing
// the current format.
static uint32_t readFileHeaderLength() {
  File f = LittleFS.open(kDataFile, "r");
  if (!f) {
    return static_cast<uint32_t>(strlen(kUploadCSVHeader) + 2);
  }
  String header = f.readStringUntil('\n');
  f.close();
  // readStringUntil consumes up to but not including '\n'; add 1 for '\n'.
  // Arduino-ESP32 Print::println() writes CRLF, so the on-disk terminator
  // is 2 bytes — but readStringUntil('\n') stops at '\n' and leaves '\r'
  // inside `header` if present. Account for the consumed '\n' (1 byte) plus
  // any trailing '\r' already in the string.
  uint32_t len = (uint32_t)header.length() + 1;  // +1 for '\n'
  return len;
}

uint32_t UploadQueue::headerEndOffset() const {
  // Prefer the actual header length from the file so a future dynamic
  // header (Phase 4) is handled without code changes. Fall back to the
  // compile-time constant when the file is not yet present.
  const uint32_t fileLen = readFileHeaderLength();
  if (fileLen > 0) return fileLen;
  // Arduino-ESP32 Print::println() appends CRLF.
  return static_cast<uint32_t>(strlen(kUploadCSVHeader) + 2);
}

bool UploadQueue::recoverDataFile() {
  const bool hasData = LittleFS.exists(kDataFile);
  const bool hasTemp = LittleFS.exists(kTempFile);
  const bool hasBackup = LittleFS.exists(kBackupFile);

  if (hasData) {
    if (hasTemp) {
      Serial.println("[UQ] recovery: data+temp found; keeping data and removing temp");
      LittleFS.remove(kTempFile);
    }
    if (hasBackup) {
      Serial.println("[UQ] recovery: committed data+backup found; removing backup");
      LittleFS.remove(kBackupFile);
    }
    return true;
  }

  if (hasBackup) {
    Serial.println("[UQ] recovery: restoring backup as data file");
    if (!LittleFS.rename(kBackupFile, kDataFile)) return false;
    if (hasTemp) {
      Serial.println("[UQ] recovery: removing uncommitted temp file");
      LittleFS.remove(kTempFile);
    }
    return true;
  }

  if (hasTemp) {
    Serial.println("[UQ] recovery: promoting lone temp file");
    return LittleFS.rename(kTempFile, kDataFile);
  }

  return true;
}

void UploadQueue::validateCursor() {
  File f = LittleFS.open(kDataFile, "r");
  if (!f) {
    Serial.println("[UQ] validateCursor: cannot open datalog.csv — resetting");
    m_cursor.byteOffset = headerEndOffset();
    return;
  }
  size_t fileSize = f.size();

  if (m_cursor.byteOffset > fileSize || m_cursor.byteOffset < headerEndOffset()) {
    Serial.printf("[UQ] validateCursor: offset %u > file %u — resetting to header end\n",
                  (unsigned)m_cursor.byteOffset, (unsigned)fileSize);
    m_cursor.byteOffset = headerEndOffset();
    saveCursor();
    f.close();
    return;
  }

  if (m_cursor.byteOffset == headerEndOffset()) {
    f.close();
    return;
  }

  if (!f.seek(m_cursor.byteOffset - 1)) {
    m_cursor.byteOffset = headerEndOffset();
    saveCursor();
    f.close();
    return;
  }

  if (f.read() == '\n') {
    f.close();
    return;
  }

  Serial.printf("[UQ] validateCursor: offset %u is mid-row; scanning forward\n",
                static_cast<unsigned>(m_cursor.byteOffset));
  bool foundBoundary = false;
  if (f.seek(m_cursor.byteOffset)) {
    while (f.available()) {
      if (f.read() == '\n') {
        m_cursor.byteOffset = static_cast<uint32_t>(f.position());
        foundBoundary = true;
        break;
      }
    }
  }
  if (!foundBoundary) {
    m_cursor.byteOffset = headerEndOffset();
  }
  f.close();
  saveCursor();
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

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t requiredHeap = maxBytes + 4096U;
  uint32_t effectiveMaxBytes = maxBytes;
  if (freeHeap < requiredHeap) {
    // Clamp to what the heap can actually support instead of giving up.
    // 8 KB overhead accounts for the String reserve + 4 KB read buffer +
    // misc allocations during the read loop.
    effectiveMaxBytes = (freeHeap > 8192U) ? (freeHeap - 8192U) : 0U;
    if (effectiveMaxBytes < 1024U) {
      Serial.printf("[UPLOAD] Insufficient heap for payload after clamp: "
                    "free=%u requested=%u effective=%u (need >=1024 usable bytes)\n",
                    static_cast<unsigned>(freeHeap), static_cast<unsigned>(maxBytes),
                    static_cast<unsigned>(effectiveMaxBytes));
      return payload;
    }
    Serial.printf("[UPLOAD] Heap clamp: maxBytes %u -> %u (free=%u)\n",
                  static_cast<unsigned>(maxBytes), static_cast<unsigned>(effectiveMaxBytes),
                  static_cast<unsigned>(freeHeap));
  }

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

  const uint32_t reserveSize = effectiveMaxBytes + strlen(kUploadCSVHeader) + 258U;
  if (!payload.csvData.reserve(reserveSize)) {
    Serial.println("[UPLOAD] String allocation failed");
    f.close();
    return payload;
  }

  // Use the actual header from the file so the uploaded payload matches the
  // on-disk CSV exactly (forward-compatible with a future dynamic header).
  // Fall back to the compile-time constant if the file has no header line.
  {
    File hf = LittleFS.open(kDataFile, "r");
    if (hf) {
      String header = hf.readStringUntil('\n');
      hf.close();
      if (header.length() > 0) {
        payload.csvData = header;
        payload.csvData += "\n";
      } else {
        payload.csvData = String(kUploadCSVHeader);
        payload.csvData += "\n";
      }
    } else {
      payload.csvData = String(kUploadCSVHeader);
      payload.csvData += "\n";
    }
  }
  const uint32_t payloadHeaderLength = payload.csvData.length();

  uint8_t buf[4096];
  uint32_t bytesRead = 0;
  uint32_t lastBoundaryBytes = 0;
  bool allocationFailed = false;

  auto appendBytes = [&](const uint8_t* data, size_t count) -> bool {
    const uint32_t before = payload.csvData.length();
    if (!payload.csvData.concat(reinterpret_cast<const char*>(data), count) ||
        payload.csvData.length() != before + count) {
      Serial.println("[UPLOAD] String allocation failed");
      return false;
    }
    return true;
  };

  while (f.available() && bytesRead < effectiveMaxBytes) {
    const uint32_t remaining = effectiveMaxBytes - bytesRead;
    const size_t request = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const int count = f.read(buf, request);
    if (count <= 0) break;
    if (!appendBytes(buf, static_cast<size_t>(count))) {
      allocationFailed = true;
      break;
    }
    for (int i = 0; i < count; ++i) {
      if (buf[i] == '\n') {
        ++payload.rowEstimate;
        lastBoundaryBytes = bytesRead + static_cast<uint32_t>(i) + 1U;
      }
    }
    bytesRead += static_cast<uint32_t>(count);
  }

  // If effectiveMaxBytes lands inside a row, consume exactly through the next newline.
  if (!allocationFailed && bytesRead >= effectiveMaxBytes &&
      (bytesRead == 0 || payload.csvData[payload.csvData.length() - 1] != '\n')) {
    bool foundNewline = false;
    while (f.available() && !foundNewline) {
      const int count = f.read(buf, sizeof(buf));
      if (count <= 0) break;
      size_t appendCount = static_cast<size_t>(count);
      for (int i = 0; i < count; ++i) {
        if (buf[i] == '\n') {
          appendCount = static_cast<size_t>(i + 1);
          foundNewline = true;
          break;
        }
      }
      if (!appendBytes(buf, appendCount)) {
        allocationFailed = true;
        break;
      }
      bytesRead += static_cast<uint32_t>(appendCount);
      if (foundNewline) {
        ++payload.rowEstimate;
        lastBoundaryBytes = bytesRead;
      }
    }
  }

  f.close();
  if (allocationFailed) {
    payload.csvData = String();
    payload.byteLength = 0;
    payload.rowEstimate = 0;
    return payload;
  }

  // Never return a crash-truncated final row.
  if (bytesRead > lastBoundaryBytes) {
    payload.csvData.remove(payloadHeaderLength + lastBoundaryBytes);
    bytesRead = lastBoundaryBytes;
  }
  payload.byteLength = bytesRead;
  return payload;
}

// ---------------------------------------------------------------------------
// advanceCursor
// ---------------------------------------------------------------------------
bool UploadQueue::advanceCursor(uint32_t newOffset, uint32_t timestampUnix,
                                uint32_t rowsUploadedDelta) {
  m_cursor.byteOffset    = newOffset;
  m_cursor.lastUploadUnix = timestampUnix;
  // Accumulate the rows actually uploaded (the caller passes the chunk's row
  // count; malformed-skip advances pass 0). This is what the "N readings sent"
  // status reflects.
  m_cursor.rowsUploaded += rowsUploadedDelta;
  saveCursor();
  Serial.printf("[UQ] advanceCursor: offset=%u ts=%u rows+=%u total=%u\n",
                (unsigned)newOffset, (unsigned)timestampUnix,
                (unsigned)rowsUploadedDelta, (unsigned)m_cursor.rowsUploaded);
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

  // Write header to temp — copy the actual first line from the source file
  // so a future dynamic header is preserved verbatim. Fall back to the
  // compile-time constant if the source has no readable header.
  {
    String header = rf.readStringUntil('\n');
    if (header.length() > 0) {
      wf.println(header);
    } else {
      wf.println(kUploadCSVHeader);
    }
  }

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

  // Commit with backup-then-swap.
  if (!commitTempDataFile("purgeUploaded")) {
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

  // Open temp file and write header — copy the actual header from the
  // source file so a future dynamic header is preserved. The header was
  // already consumed above via readStringUntil('\n'); re-read it from a
  // fresh handle to keep this section self-contained.
  File wf = LittleFS.open(kTempFile, "w", true);
  if (!wf) {
    Serial.println("[UQ] emergencyPurge: cannot open temp file");
    rf.close();
    return false;
  }
  {
    File hf = LittleFS.open(kDataFile, "r");
    if (hf) {
      String header = hf.readStringUntil('\n');
      hf.close();
      if (header.length() > 0) {
        wf.println(header);
      } else {
        wf.println(kUploadCSVHeader);
      }
    } else {
      wf.println(kUploadCSVHeader);
    }
  }

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

  // Commit with backup-then-swap.
  if (!commitTempDataFile("emergencyPurge")) {
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
  m_cursor.nextAttemptUnix = 0;
  saveCursor();
}

void UploadQueue::incrementRetryCount() {
  if (m_cursor.retryCount < UINT8_MAX) ++m_cursor.retryCount;
  saveCursor();
}

void UploadQueue::incrementRetryCount(uint32_t nowUnix, uint32_t cooldownSec) {
  if (m_cursor.retryCount < UINT8_MAX) ++m_cursor.retryCount;
  m_cursor.nextAttemptUnix = nowUnix + cooldownSec;
  saveCursor();
}

bool UploadQueue::maxRetriesExceeded(uint8_t maxRetries) const {
  return m_cursor.retryCount >= maxRetries;
}

bool UploadQueue::maxRetriesExceeded(uint8_t maxRetries, uint32_t nowUnix) const {
  if (m_cursor.nextAttemptUnix > 0 && nowUnix >= m_cursor.nextAttemptUnix) {
    return false;
  }
  return m_cursor.retryCount >= maxRetries;
}
