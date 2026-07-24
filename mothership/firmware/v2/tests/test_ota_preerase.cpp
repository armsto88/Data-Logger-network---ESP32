// Bench Test 4: pre-erase the OTA partition BEFORE opening the modem session,
// then chunked Range download with real flash writes + per-write timing.
//
// Hypothesis (from the 2026-07-22 chunk-size sweep): the truncation is driven by
// flash-erase cache-disable stalls. esp_ota_begin() with a real image size erases
// the whole ~1.4 MB target partition up front — and in the production path that
// call happens lazily on the FIRST image chunk, i.e. WHILE the modem TLS session
// is already open. That multi-second erase stalls the AT+CCHRECV pull loop and the
// modem drops the session. Every sweep run died on chunk 0, consistent with this.
//
// This test forces the erase to complete BEFORE any modem session (a zero-length
// mothershipOtaImageChunk() call triggers otaInstallBegin -> esp_ota_begin), so
// the download window contains only short esp_ota_write() calls. It times the
// pre-erase and every write to prove whether any in-session stall remains.
//
// Single-variable change vs Test 3 (same 512 KiB chunks). If this completes where
// Test 3 failed on chunk 0, the up-front erase was the killer and cloud OTA works
// on current hardware with no SD card.
//
//   pio run -e mothership-v2-test-ota-preerase -t upload -t monitor --upload-port COM4 --monitor-port COM4
//

#include <Arduino.h>
#include "comms/modem_driver.h"
#include "ota/mothership_selfupdate.h"
#include "ota/mothership_ota_release_store.h"

static const char* kTestReleaseId = "fieldmesh-bench-2026.07.0b";

#ifndef TEST_CHUNK_KIB
#define TEST_CHUNK_KIB 512
#endif
static const uint32_t kChunkSize = (uint32_t)TEST_CHUNK_KIB * 1024;

// Log any single flash write that stalls longer than this (ms).
static const uint32_t kLongWriteMs = 10;

static int      g_chunksFailed = 0;
static uint32_t g_bytesWritten = 0;
static uint32_t g_maxWriteMs = 0;
static uint32_t g_longWrites = 0;
static uint32_t g_writeCount = 0;

static bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  if (strlen(hex) != outLen * 2) return false;
  for (size_t i = 0; i < outLen; ++i) {
    char hi = hex[i * 2], lo = hex[i * 2 + 1];
    int h = (hi >= '0' && hi <= '9') ? hi - '0'
          : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
          : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
    int l = (lo >= '0' && lo <= '9') ? lo - '0'
          : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
          : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

struct SmallCtx { String* out; uint32_t firstByteAtMs; uint32_t budgetMs; };
static bool appendSink(const uint8_t* d, size_t n, void* ctx) {
  SmallCtx* c = static_cast<SmallCtx*>(ctx);
  const uint32_t now = millis();
  if (c->firstByteAtMs == 0) c->firstByteAtMs = now;
  if (now - c->firstByteAtMs > c->budgetMs) return false;
  for (size_t i = 0; i < n; ++i) *c->out += (char)d[i];
  return c->out->length() < 4096;
}

struct ImageCtx { int lastFwReason; };
static bool imageSink(const uint8_t* d, size_t n, void* ctx) {
  ImageCtx* c = static_cast<ImageCtx*>(ctx);
  const uint32_t t0 = millis();
  const FwReason r = mothershipOtaImageChunk(d, n);
  const uint32_t dt = millis() - t0;
  g_writeCount++;
  if (dt > g_maxWriteMs) g_maxWriteMs = dt;
  if (dt >= kLongWriteMs) {
    g_longWrites++;
    Serial.printf("    LONG write: %lu ms for %u bytes at offset %lu\n",
                  (unsigned long)dt, (unsigned)n, (unsigned long)g_bytesWritten);
  }
  c->lastFwReason = r;
  if (r != FW_NONE) {
    Serial.printf("    OTA write failed: FwReason=%d at %lu bytes\n",
                  (int)r, (unsigned long)g_bytesWritten);
    return false;
  }
  g_bytesWritten += n;
  return true;
}

static const String kBase =
  "https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/";

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[TEST 4] OTA pre-erase + chunked download + write timing");
  Serial.printf("  releaseId: %s\n  chunk: %u bytes  long-write threshold: %lu ms\n",
                kTestReleaseId, (unsigned)kChunkSize, (unsigned long)kLongWriteMs);

  ModemDriver modem;
  modem.init();
  if (!modem.powerOn())             { Serial.println("FAIL modem powerOn"); return; }
  if (!modem.waitForNetwork(60000)) { Serial.println("FAIL waitForNetwork"); modem.gracefulShutdown(); return; }

  // 1-3: manifest + signature + verify (identical to Test 3).
  Serial.println("\n[STEP 1-3] manifest + signature + verify...");
  String manifestBody, sigBody;
  SmallCtx mctx{&manifestBody, 0, 8000};
  HttpsGetStreamResult mr = modem.httpsGetStream(kBase + kTestReleaseId + "/manifest.json", appendSink, &mctx, 20000);
  if (!mr.success) { Serial.println("FAIL manifest"); modem.gracefulShutdown(); return; }
  SmallCtx sctx{&sigBody, 0, 8000};
  HttpsGetStreamResult sr = modem.httpsGetStream(kBase + kTestReleaseId + "/manifest.json.sig", appendSink, &sctx, 20000);
  if (!sr.success) { Serial.println("FAIL signature"); modem.gracefulShutdown(); return; }
  sigBody.trim();
  uint8_t sig[64];
  if (sigBody.length() < 128 || !hexToBytes(sigBody.c_str(), sig, 64)) { Serial.println("FAIL sig parse"); modem.gracefulShutdown(); return; }
  FwReason vr = mothershipOtaVerifyManifest(
      reinterpret_cast<const uint8_t*>(manifestBody.c_str()), manifestBody.length(), sig);
  if (vr != FW_NONE) { Serial.printf("FAIL verify: %d\n", (int)vr); modem.gracefulShutdown(); return; }
  MothershipOtaStatus st = mothershipOtaGetStatus();
  Serial.printf("  verified OK. expected size: %lu\n", (unsigned long)st.expectedSize);

  // 4: PRE-ERASE — force otaInstallBegin (esp_ota_begin -> full up-front erase)
  //    to run here, with NO modem session open. A zero-length chunk triggers the
  //    begin without writing any image bytes.
  Serial.println("\n[STEP 4] Pre-erasing OTA partition (no modem session open)...");
  const uint8_t dummy = 0;
  const uint32_t tErase = millis();
  FwReason pe = mothershipOtaImageChunk(&dummy, 0);   // begins install => erases partition
  const uint32_t eraseMs = millis() - tErase;
  Serial.printf("  pre-erase (esp_ota_begin) took %lu ms, result=%d\n", (unsigned long)eraseMs, (int)pe);
  if (pe != FW_NONE) { Serial.printf("FAIL pre-erase: %d\n", (int)pe); mothershipOtaAbort(); modem.gracefulShutdown(); return; }

  // 5: chunked Range download — writes only, no erase in the session window.
  Serial.println("\n[STEP 5] Downloading image in chunks (writes only)...");
  const String imageUrl = kBase + kTestReleaseId + "/image.bin";
  g_bytesWritten = 0;
  ImageCtx ictx{FW_NONE};
  const uint32_t tDl = millis();

  for (uint32_t start = 0; start < st.expectedSize; start += kChunkSize) {
    const uint32_t endExcl = (start + kChunkSize < st.expectedSize) ? start + kChunkSize : st.expectedSize;
    const uint32_t chunkLen = endExcl - start;
    String range = "bytes=" + String(start) + "-" + String(endExcl - 1);
    Serial.printf("\n  chunk [%lu-%lu] (%lu bytes)\n", (unsigned long)start, (unsigned long)(endExcl - 1), (unsigned long)chunkLen);

    HttpsGetStreamResult ir = modem.httpsGetStream(imageUrl, imageSink, &ictx, 60000, "", range);
    Serial.printf("    status=%d delivered=%lu declared=%lu complete=%d aborted=%d\n",
                  ir.httpStatus, (unsigned long)ir.bytesDelivered,
                  (unsigned long)ir.declaredContentLength, ir.complete, ir.aborted);

    if (ir.httpStatus != 206 || ir.declaredContentLength != chunkLen || !ir.success || ictx.lastFwReason != FW_NONE) {
      Serial.println("    ERROR: chunk failed");
      g_chunksFailed++;
      break;
    }
  }
  const uint32_t dlMs = millis() - tDl;

  // 6: finish (only if the whole image landed).
  Serial.println("\n[STEP 6] Finishing...");
  if (g_chunksFailed == 0) {
    FwReason fr = mothershipOtaImageFinish();
    if (fr != FW_NONE) { Serial.printf("  FAIL finish: %d\n", (int)fr); g_chunksFailed++; }
    else Serial.println("  OK finish (image hashed + slot armed)");
  } else {
    mothershipOtaAbort();
    Serial.println("  (skipped — aborted partial install)");
  }

  Serial.printf("\n  === TIMING ===\n");
  Serial.printf("  pre-erase:        %lu ms\n", (unsigned long)eraseMs);
  Serial.printf("  download+write:   %lu ms\n", (unsigned long)dlMs);
  Serial.printf("  bytes written:    %lu / %lu\n", (unsigned long)g_bytesWritten, (unsigned long)st.expectedSize);
  Serial.printf("  writes:           %lu total, %lu >= %lu ms, max %lu ms\n",
                (unsigned long)g_writeCount, (unsigned long)g_longWrites,
                (unsigned long)kLongWriteMs, (unsigned long)g_maxWriteMs);

  int failures = 0;
  auto ck = [&](bool c, const char* l){ Serial.printf("%s %s\n", c?"ok  ":"FAIL", l); if(!c) failures++; };
  ck(g_chunksFailed == 0, "all chunks succeeded");
  ck(g_bytesWritten == st.expectedSize, "total bytes written matches expected");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
  modem.gracefulShutdown();
}

void loop() {}
