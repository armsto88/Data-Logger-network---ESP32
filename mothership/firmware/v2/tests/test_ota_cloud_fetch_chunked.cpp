// Bench test: chunked HTTP Range download WITH OTA flash writes.
//
// Full end-to-end chunked download test: fetches manifest, verifies signature,
// downloads image in 512 KiB Range-request chunks, streams each into the OTA
// install core with real esp_ota_write() calls. This mirrors the production
// cloud-OTA path exactly, but on-device without backend state/retries.
//
// Build and run:
//   pio run -e mothership-v2-test-ota-cloud-fetch-chunked -t upload -t monitor --upload-port COM4 --monitor-port COM4
//

#include <Arduino.h>
#include <mbedtls/sha256.h>
#include "comms/modem_driver.h"
#include "ota/mothership_selfupdate.h"
#include "ota/mothership_ota_release_store.h"
#include "ota/mothership_ota_cloud_fetch.h"

// Test artifact from the bench release (2026.07.0b)
static const char* kTestReleaseId = "fieldmesh-bench-2026.07.0b";

// Chunk size is a build knob so the sweep (512/256/128/64 KiB) needs no edits:
//   PLATFORMIO_BUILD_FLAGS="-D TEST_CHUNK_KIB=256" pio run -e ... -t upload
#ifndef TEST_CHUNK_KIB
#define TEST_CHUNK_KIB 512
#endif
static const uint32_t kChunkSize = (uint32_t)TEST_CHUNK_KIB * 1024;

static int g_chunksFailed = 0;
static uint32_t g_bytesWritten = 0;

// Helper: hex string to bytes (for signature parsing)
static bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  if (strlen(hex) != outLen * 2) return false;
  for (size_t i = 0; i < outLen; ++i) {
    char high = hex[i * 2];
    char low = hex[i * 2 + 1];
    int h = (high >= '0' && high <= '9') ? (high - '0')
          : (high >= 'a' && high <= 'f') ? (high - 'a' + 10)
          : (high >= 'A' && high <= 'F') ? (high - 'A' + 10)
          : -1;
    int l = (low >= '0' && low <= '9') ? (low - '0')
          : (low >= 'a' && low <= 'f') ? (low - 'a' + 10)
          : (low >= 'A' && low <= 'F') ? (low - 'A' + 10)
          : -1;
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

// Sink for small fetches (manifest, signature)
struct SmallCtx {
  String*  out;
  uint32_t firstByteAtMs;
  uint32_t budgetMs;
};

static bool appendSink(const uint8_t* d, size_t n, void* ctx) {
  SmallCtx* c = static_cast<SmallCtx*>(ctx);
  const uint32_t now = millis();
  if (c->firstByteAtMs == 0) c->firstByteAtMs = now;
  if (now - c->firstByteAtMs > c->budgetMs) return false;
  for (size_t i = 0; i < n; ++i) *c->out += (char)d[i];
  return c->out->length() < 4096;
}

// Sink for large image chunks (with real OTA writes)
struct ImageCtx {
  uint32_t sessionStartMs;
  uint32_t sessionLimitMs;
  int      lastFwReason;
  bool     budgetHit;
};

static bool imageSink(const uint8_t* d, size_t n, void* ctx) {
  ImageCtx* c = static_cast<ImageCtx*>(ctx);
  if (millis() - c->sessionStartMs > c->sessionLimitMs) {
    c->budgetHit = true;
    Serial.printf("    Budget hit! elapsed=%lu limit=%lu\n",
                  (unsigned long)(millis() - c->sessionStartMs),
                  (unsigned long)c->sessionLimitMs);
    return false;
  }
  const FwReason r = mothershipOtaImageChunk(d, n);
  c->lastFwReason = r;
  if (r != FW_NONE) {
    Serial.printf("    OTA write failed: FwReason=%d after %lu bytes\n",
                  (int)r, (unsigned long)g_bytesWritten);
  }
  g_bytesWritten += n;
  if (g_bytesWritten % (64*1024) == 0) {
    Serial.printf("    wrote %lu bytes\n", (unsigned long)g_bytesWritten);
  }
  return r == FW_NONE;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[TEST] OTA cloud fetch chunked (with real flash writes)");
  Serial.printf("  releaseId: %s\n", kTestReleaseId);
  Serial.printf("  chunk:     %u bytes\n", (unsigned)kChunkSize);

  ModemDriver modem;
  modem.init();
  if (!modem.powerOn())            {
    Serial.println("FAIL modem powerOn");
    return;
  }
  if (!modem.waitForNetwork(60000)){
    Serial.println("FAIL waitForNetwork");
    modem.gracefulShutdown();
    return;
  }

  const uint32_t sessionStartMs = millis();
  const uint32_t sessionLimitMs = 600000;  // 10 minute timeout for the whole test

  // 1. Fetch manifest
  Serial.println("\n[STEP 1] Fetching manifest...");
  const String manifestUrl = "https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/" +
                            String(kTestReleaseId) + "/manifest.json";
  String manifestBody;
  SmallCtx mctx{&manifestBody, 0, 8000};
  HttpsGetStreamResult mr = modem.httpsGetStream(manifestUrl, appendSink, &mctx, 20000);
  Serial.printf("  status=%d delivered=%lu\n", mr.httpStatus, (unsigned long)mr.bytesDelivered);
  if (!mr.success) {
    Serial.println("FAIL manifest download");
    modem.gracefulShutdown();
    return;
  }

  // 2. Fetch signature
  Serial.println("\n[STEP 2] Fetching signature...");
  const String sigUrl = "https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/" +
                       String(kTestReleaseId) + "/manifest.json.sig";
  String sigBody;
  SmallCtx sctx{&sigBody, 0, 8000};
  HttpsGetStreamResult sr = modem.httpsGetStream(sigUrl, appendSink, &sctx, 20000);
  Serial.printf("  status=%d delivered=%lu\n", sr.httpStatus, (unsigned long)sr.bytesDelivered);
  if (!sr.success) {
    Serial.println("FAIL signature download");
    modem.gracefulShutdown();
    return;
  }

  // 3. Parse signature
  Serial.println("\n[STEP 3] Parsing signature...");
  sigBody.trim();
  uint8_t sig[64];
  if (sigBody.length() < 128 || !hexToBytes(sigBody.c_str(), sig, 64)) {
    Serial.println("FAIL signature parse");
    modem.gracefulShutdown();
    return;
  }
  Serial.printf("  sig: %s\n", sigBody.substring(0, 16).c_str());

  // 4. Verify manifest
  Serial.println("\n[STEP 4] Verifying manifest...");
  FwReason vr = mothershipOtaVerifyManifest(
      reinterpret_cast<const uint8_t*>(manifestBody.c_str()),
      manifestBody.length(),
      sig);
  Serial.printf("  verify result: %d\n", (int)vr);
  if (vr != FW_NONE) {
    Serial.println("FAIL manifest verify");
    modem.gracefulShutdown();
    return;
  }

  MothershipOtaStatus st = mothershipOtaGetStatus();
  Serial.printf("  expected size: %lu\n", (unsigned long)st.expectedSize);

  // 5. Download image in chunks
  Serial.println("\n[STEP 5] Downloading image in chunks...");
  const String imageUrl = "https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/" +
                         String(kTestReleaseId) + "/image.bin";
  g_bytesWritten = 0;
  ImageCtx ictx{sessionStartMs, sessionLimitMs, FW_NONE, false};

  for (uint32_t chunkStart = 0; chunkStart < st.expectedSize; chunkStart += kChunkSize) {
    const uint32_t chunkEndExclusive = (chunkStart + kChunkSize < st.expectedSize)
                                        ? (chunkStart + kChunkSize)
                                        : st.expectedSize;
    const uint32_t chunkLen = chunkEndExclusive - chunkStart;

    String range = "bytes=" + String(chunkStart) + "-" + String(chunkEndExclusive - 1);
    Serial.printf("\n  chunk [%lu-%lu] (%lu bytes)\n",
                  (unsigned long)chunkStart, (unsigned long)(chunkEndExclusive - 1),
                  (unsigned long)chunkLen);

    HttpsGetStreamResult ir = modem.httpsGetStream(imageUrl, imageSink, &ictx, 60000, "", range);

    Serial.printf("    status=%d delivered=%lu declared=%lu complete=%d\n",
                  ir.httpStatus, (unsigned long)ir.bytesDelivered,
                  (unsigned long)ir.declaredContentLength, ir.complete);

    if (ir.httpStatus != 206) {
      Serial.printf("    ERROR: expected HTTP 206, got %d\n", ir.httpStatus);
      g_chunksFailed++;
      break;
    }
    if (ir.declaredContentLength != chunkLen) {
      Serial.printf("    ERROR: expected declared=%lu, got %lu\n",
                    (unsigned long)chunkLen, (unsigned long)ir.declaredContentLength);
      g_chunksFailed++;
      break;
    }
    if (!ir.success) {
      Serial.printf("    ERROR: transfer failed\n");
      g_chunksFailed++;
      break;
    }
    if (ictx.lastFwReason != FW_NONE) {
      Serial.printf("    ERROR: OTA write failed: %d\n", (int)ictx.lastFwReason);
      g_chunksFailed++;
      break;
    }
  }

  // 6. Finish the install
  Serial.println("\n[STEP 6] Finishing OTA install...");
  if (g_chunksFailed == 0) {
    FwReason fr = mothershipOtaImageFinish();
    if (fr != FW_NONE) {
      Serial.printf("  FAIL finish: %d\n", (int)fr);
      g_chunksFailed++;
    } else {
      Serial.println("  OK finish (image hashed + slot armed)");
    }
  } else {
    mothershipOtaAbort();   // clean up the partial install so the next run is fresh
    Serial.println("  (skipped — aborted partial install)");
  }

  // Summary
  Serial.printf("\n  total bytes written: %lu\n", (unsigned long)g_bytesWritten);
  Serial.printf("  chunks failed: %d\n", g_chunksFailed);

  int failures = 0;
  auto ck = [&](bool c, const char* l){ Serial.printf("%s %s\n", c?"ok  ":"FAIL", l); if(!c) failures++; };
  ck(g_chunksFailed == 0, "all chunks succeeded");
  ck(g_bytesWritten == st.expectedSize, "total bytes written matches expected");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
  modem.gracefulShutdown();
}

void loop() {}
