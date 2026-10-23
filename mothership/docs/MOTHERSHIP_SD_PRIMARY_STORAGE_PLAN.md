# Mothership SD-Primary Storage — Plan

Status: **proposed, nothing implemented.** Written 2026-10-23, after the FieldHub v2 r6
bring-up proved the SD hardware (`hardware/FieldHub_v2/revisions/2026-10-10-r6/BRINGUP_HARNESS.md`).

Related notes:

- `FIELDHUB_STANDALONE_OPERATION.md` — the local-first storage contract this plan changes.
- `MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md` — the hub boot-time budget this plan has to stay inside (§6.9 below).
- `MOTHERSHIP_LOCAL_SENSOR_COLLECTION_DESIGN_NOTE.md` — hub-local readings use the same write path (§6.10 below).

## 1. Summary

Make the SD card the hub's system of record for readings, and the source for uploads, downloads
and the Data page. Keep every reading until the card is nearly full, then remove the oldest.
Keep writing to LittleFS as well until the power-cut tests have passed.

The reason is capacity. Uploads read only from LittleFS today, so an LTE outage longer than the
LittleFS buffer loses un-uploaded readings even though the SD archive holds them (§3). The reason
the change is bigger than "point the queue at the card" is that much of the current code assumes
one small file: it scans, counts and rewrites it (§4).

## 2. Requirements and decisions

From the owner, 2026-10-23:

- The SD card is the main store, not flash.
- It is permanent: data is removed only when the card is full, as LittleFS does now (the oldest
  goes first).
- LittleFS keeps dual-writing for now.

Proposed here, and not objected to: **monthly files with a size cap** (§6.2). Not decided: the
items in §11.

## 3. How storage works today (verified in code)

| Piece | Behaviour | Where |
|---|---|---|
| Write | Each accepted snapshot is appended to LittleFS `/datalog.csv`, and, if a card is mounted, to SD `/fieldmesh_readings.csv`. The node's ACK says "persisted" if either write worked | `processSnapshot()` in `main.cpp`; `sdLogCSVRow()` in `storage/sd_logger.cpp` |
| Row append | One open, println, close per row, on both stores | `flashLogCSVRow()`, `sdLogCSVRow()` |
| Upload | Reads `/datalog.csv` from a byte-offset cursor kept in NVS (`tx` namespace, key `cursor_offset`) | `storage/upload_queue.cpp` |
| LittleFS retention | At 65 % of the partition, a streaming rewrite keeps the newest 40 % of rows, whether uploaded or not | `emergencyPurgeIfFull()`, constants in `upload_queue.h` |
| SD retention | None. The file only grows; a full card sets a write-error flag | `sd_logger.cpp` |
| Download | Prefers the SD file, streams it as one CSV | `handleDownloadCSV()` in `config/config_server.cpp` |
| Data page | Prefers the SD file, reads it line by line to count rows and find the first and last timestamps | `buildDataStatusSectionHtml()` |
| Status to the backend | `flashPct`, `fsTotal`, `fsUsed`, `dataLogRecords`, file size, all from LittleFS | `StatusContext` in `main.cpp`, `json_payload.cpp` |
| Dead code | `storage/sd_manager.cpp` and `comms/espnow_manager.cpp` also write SD `/datalog.csv`, but both are excluded by the production `build_src_filter` | `platformio.ini` |

The LittleFS partition is 768 KB (`partitions.csv`, `spiffs`, `0xC0000`) and also holds other
files. At 65 % that is about 500 KB, or about **2,400 rows** at roughly 210 bytes a row.

| Fleet | Rows per hour | Time to fill LittleFS |
|---|---|---|
| 4 nodes, 1 per minute | 240 | about 10 hours |
| 4 nodes, every 15 minutes | 16 | about 6 days |
| 40 nodes, 1 per minute | 2,400 | **about 1 hour** |

An LTE outage longer than that silently drops un-uploaded rows from the upload path.

## 4. Why the queue cannot simply be pointed at the card

Each of these works on 768 KB and fails on gigabytes:

1. **Row counting scans the file.** `getPendingRows()` and `getTotalRows()` read from the cursor or
   the header to the end, counting newlines. The upload loop calls `getPendingRows()` once per chunk
   (`while (uploadQueue.getPendingRows() > 0 ...)` in `main.cpp`). With 300 MB pending, one call
   takes about 4 minutes at the measured 1.4 MB/s read speed.
2. **The Data page reads the whole file on every load**, with `readStringUntil('\n')` per line.
3. **Retention is a rewrite.** Trimming a file to its newest 40 % means writing 40 % of it back. At
   the measured 354 KiB/s write speed, 1 GB takes about 50 minutes and a full 16 GB card takes
   hours, with the hub awake and the web server stalled.
4. **The cursor is one 32-bit offset into one file**, and 4 GB is also the FAT32 file-size limit.
5. **Eviction would invalidate the cursor.** The existing purge handles this by adjusting the
   cursor, which only works for a file that is rewritten.
6. **One open, append and close per row** costs an unmeasured amount on SD. It matters inside the
   rendezvous window (§6.9).
7. **Nothing here has automated coverage for purge or drain.** `test_upload_queue.cpp` says purge
   and drain are "covered by the bench acceptance run instead".

## 5. Measured on HUB-R6-01 (16 GB SDHC, 2026-10-23)

| Measurement | Result |
|---|---|
| Mount with card present | 7 ms at 40 MHz, 38 ms at 400 kHz (worst of 3) |
| Mount attempt with no card | 628 ms worst case, no crash over 25 reboots |
| Sequential write, 128 KiB file | 354 KiB/s at 40 MHz |
| Sequential read, 128 KiB file | 1,376 KiB/s at 40 MHz |
| Reboots with card, full archive test | 100 of 100 passed |
| Stock SD library | cannot mount this card; the vendored `lib/SD` fix is in `main` |

Not measured yet, and needed before the design is final (§7, phase 0):

- the time of one open, append and close of a row, against a held-open handle with a flush;
- the time of `SD.usedBytes()` on a card that is nearly full (FatFs may scan the FAT);
- the time to delete a 256 MB and a 1 GB file on FAT32;
- the time of a directory listing with a few hundred files;
- behaviour through repeated power cuts during appends (the Stage F items still open).

## 6. Design

### 6.1 Roles

| Store | Role |
|---|---|
| SD, when mounted and healthy | System of record. Written first. Source for uploads, the Data page and downloads. |
| LittleFS | Dual-written safety copy while the power-cut tests are open. The fallback upload queue when no card is mounted. Keeps today's 65 %/40 % behaviour. |
| NVS | Cursor and a small state record (§6.4). |

When the card is absent, the hub behaves as it does today. A reading written only to LittleFS
during an SD outage is not copied to the card afterwards (see §11, question 3).

### 6.2 Layout

```
/fieldmesh/
  readings_2026-10.csv           one file per calendar month (UTC, from the hub RTC)
  readings_2026-10_p2.csv        continues the month once a file reaches 256 MB
  deployments.csv                unchanged contents, new location
  hub.id                         the hub MAC, for card-ownership checks (§11, question 4)
  index.json                     advisory summary (§6.7)
```

- Every readings file starts with the current canonical header (`kCurrentCSVHeader35`), so any
  single file opens on its own on a PC.
- A new file starts at a month change or at the size cap, whichever comes first. The cap keeps any
  file far below the FAT32 limit and keeps the eviction unit bounded.
- A segment id is a number, `YYYYMM * 100 + part`, for example `20261001`.
- The existing `/fieldmesh_readings.csv` and `/fieldmesh_deployments.csv` are legacy. The reading
  archive becomes the oldest segment (id 0): it is left in place, never rewritten, included in
  downloads, and evicted first.

### 6.3 Write path

1. `processSnapshot()` builds the stamped row once. It writes to the SD segment, then to LittleFS.
2. `persisted` for the node ACK is true if either write succeeded, as now.
3. Rows go through a new single function, `readingsStoreAppend(row)`. The hub-local snapshot path
   from the sensing plan calls the same function, so that work adds no second write path.
4. At mount, the tail segment is checked: the last 4 KB is read, and anything after the last
   newline is truncated. A power cut can leave a torn final row, and the upload reader already
   refuses a row with no newline.
5. A write error sets the existing `sdHadWriteError` flag and does not stop the LittleFS write.

Whether to hold the segment open during a window, with a flush after each row, is decided by the
phase 0 measurement (§7).

### 6.4 Upload queue and cursor

Keep the `UploadQueue` interface and its eight `advanceCursor()` call sites (six in `main.cpp`, two
in `config_server.cpp`), which all pass `payload.startOffset + consumedBytes`. Change what is
behind it:

- Introduce a `ReadingsStore` interface. A LittleFS implementation wraps today's logic
  unchanged, so existing tests keep passing. A segment implementation reads the SD segments.
- The SD cursor is `(segmentId, offsetInSegment)`. Two new NVS keys hold the segment id and the
  offset; the existing `cursor_offset` keeps its meaning for the LittleFS queue.
- `getNewData()` reads from the current segment only. When a segment is fully uploaded and a later
  one exists, the cursor moves to the start of the next segment.
- **No scans.** Pending bytes come from sizes minus the offset. Pending and total rows are
  counters (§6.7), with an estimate (bytes divided by the average row length) when a counter is
  stale. The upload loop tests "more to send" with bytes.
- **Which queue a wake uses:** the SD queue when a card is mounted and healthy, otherwise the
  LittleFS queue. Each has its own cursor. Switching between them can re-send rows. This is
  accepted, because the backend deduplicates on `(nodeId, datetime, seqNum)` and retries are
  idempotent (`docs/FIELDMESH_PAYLOAD_REFERENCE.md`).
- A cursor that points at a segment that no longer exists, whether evicted or a swapped card,
  moves to the start of the oldest segment that does.

### 6.5 Retention

- **Trigger:** at the end of the wake, after the upload, never in the boot path or the rendezvous
  window. This is the same rule as rendezvous Stage 0d for the purge.
- **Condition:** free space below a reserve, by default the larger of 2 % of the card and
  128 MB. The reserve keeps FAT writes from ever hitting a true out-of-space condition.
- **Action:** delete the oldest segment, then check again, until free space is above the reserve
  plus a margin. The current segment is never deleted.
- **Uploaded or not:** oldest first regardless, as LittleFS does today. Rows removed before they
  were uploaded are counted separately and shown (§6.8), so a loss is visible and not silent.
- **If only the current segment is left and the card is still full:** stop appending to SD, set
  the error flag, and carry on with LittleFS.
- **Order of capacity:** 40 nodes recording every minute write about 12 MB a day, so a 16 GB card
  lasts over three years before the first eviction. Eviction is a safety net.

### 6.6 Failure behaviour

| Condition | Behaviour |
|---|---|
| No card at boot | One mount attempt (about 0.6 s), then LittleFS-only as today. Status shows "no card". |
| Card inserted while running | Not seen until the next boot (no card-detect line on r6). |
| Write error on SD | Flag set, LittleFS copy still written, next wake retries the mount. |
| Torn final row | Truncated at the next mount (§6.3). |
| Card fails to mount after a power cut | Treated as no card. The UI says to check the card on a PC; the hub never reformats it. |
| Card full, one segment left | See §6.5. |
| Card swapped | Cursor falls back to the oldest segment present. `hub.id` mismatch is handled as in §11, question 4. |
| Read-only or locked card | Treated as a write error. Reading and uploading still work. |

### 6.7 Download, Data page and counters

- **Download** streams the segments in id order under a single header, so the user still receives
  one CSV. A download by month is a small addition to the same route.
- **Data page** and **status** read `index.json`, not the data. The index holds, per segment: id,
  bytes, rows, first and last timestamp. It is advisory: rewritten at the end of each wake from
  counters held in RAM during the wake, and rebuilt by a scan only on request (a button in config
  mode, with progress) or when it is missing. A scan costs about 3 minutes per 256 MB segment.
- Rows appended during a wake are counted in RAM. If power is lost before the index is written,
  the index is short by that many rows and says "approximate" until rebuilt.

### 6.8 Status and the backend

New, additive fields: SD present, used and total bytes, segment count, oldest sample time, rows
removed before upload, write-error count. Existing fields keep their names. `flashPct` and
`fsUsed` keep describing LittleFS.

Not verified: whether the ingest function tolerates unknown `status` keys. The backend repo is not in
this checkout. Confirm before shipping the new fields.

### 6.9 Boot time and the rendezvous

- SD mounts before the sync window, as now. Measured: 7 ms with a card, 0.6 s without. That fits
  inside the rendezvous latency budget that the sync sweep is measuring.
- Nothing proportional to stored data runs at boot: no directory scan, no row count, no
  free-space walk. The segment list comes from NVS, with `SD.exists()` checks for the next ids.
- Eviction and index writes run after the window and the upload, as above.
- Per-row append cost lands inside the grant loop, which today costs about 2 s per record on the
  node side. It becomes visible once rendezvous Stage 2a cuts that to about 0.3 s, which is why it
  is measured first.

### 6.10 Hub-local sensing

Hub-collected readings call `readingsStoreAppend()`. No new CSV columns, as the sensing plan
already requires.

## 7. Work plan

| Phase | What | Gate to leave |
|---|---|---|
| 0. Measure | A bring-up sketch for the five unmeasured items in §5 | Numbers recorded in `results.csv`; §6.3 and §6.5 finalised |
| 1. Seam | `ReadingsStore` interface; LittleFS implementation is today's code unchanged | All existing suites pass, production image size noted |
| 2. Write path | Segment store, rolling, torn-row repair, `readingsStoreAppend()`, legacy archive as segment 0 | On-device suite: roll over a month and a size cap, reboot, torn row |
| 3. Upload from SD | `(segment, offset)` cursor, byte-based pending, queue choice per wake | Upload suite across a segment boundary; card removed mid-backlog |
| 4. Pages | `index.json`, streamed multi-file download, status fields | Data page loads in under a second with 1 GB of segments |
| 5. Retention | Eviction, counters, UI | Eviction suite on a small partition (below) |
| 6. Power-cut campaign | Repeated cuts during appends at many offsets, filesystem checked on a PC after each | No lost committed row, card mounts every time. Only then may LittleFS dual-write be removed |

Testing eviction without filling 16 GB: format a 64–128 MB partition on a card and set the reserve
to a few MB.

## 8. Tests

Existing and kept: `mothership-v2-test-sd-logger`, `mothership-v2-test-upload-queue`.

New, on-device (this repo has no host test environment):

- segment rollover at a month change and at the size cap; reboot between rows;
- cursor movement across a segment boundary; cursor repair after eviction and after a card swap;
- torn final row at mount;
- eviction order, never touching the current segment, and the removed-before-upload counter;
- queue choice with the card present, absent and failing mid-wake;
- Data page and download against a large synthetic segment set (timed);
- the existing purge and drain behaviour, which has no coverage today.

## 9. Risks

- **FAT is not power-safe.** The single biggest risk to making SD primary, and the reason for the
  dual-write and phase 6. A cut during a cluster allocation can corrupt the filesystem, not only the
  last row.
- **Eviction deletes data that may not have uploaded.** Accepted by the owner ("as LittleFS does
  now"), made visible by the counter.
- **Duplicate uploads** after switching queues. Harmless to the backend, and visible in "rows
  uploaded".
- **The production image is 90.2 % full** (1,419,049 of 1,572,864 bytes). The segment store and the
  index add code. Measure early in phase 1.
- **Per-row latency in the rendezvous**, if the measurement is poor, is fixed with a held-open
  handle, at the cost of a wider power-cut window.

## 10. Out of scope

Reformatting or repairing a card from the hub, encryption, compression, a card-detect line (a
hardware change, listed in the FieldHub next-revision notes), a changed CSV schema, and any
backend work other than the additive status fields.

## 11. Questions for the owner

1. **Reserve size.** Is "larger of 2 % and 128 MB free" a good definition of full?
2. **Month boundary.** Use UTC, or the display offset the hub UI already has? UTC is simpler and
   matches the RTC; local months are friendlier for a scientist reading file names.
3. **SD outage backfill.** Rows that went only to LittleFS while the card was absent: copy them to
   the card when it returns, or leave them? Copying needs a way to avoid duplicating rows that did
   reach the card. The proposal here is to leave them.
4. **Card ownership.** A card moved from another hub would have its backlog uploaded under this
   hub's credentials. Proposed: `hub.id` holds the hub MAC; on a mismatch, do not upload the
   existing backlog, show a warning, and keep appending.
