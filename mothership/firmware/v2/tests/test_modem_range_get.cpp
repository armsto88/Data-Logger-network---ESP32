// Bench test: chunked HTTP Range download (no flash writes).
//
// This test downloads a large firmware image in 512 KiB chunks using HTTP Range
// requests, each its own HTTPS session. No flash writes — just stream to a sink
// that counts bytes and computes SHA-256. This proves whether chunking sidesteps
// the observed 1,292,354-byte truncation (bench 2026-07-21/22).
//
// Build and run:
//   pio run -e mothership-v2-test-modem-range-get -t upload -t monitor --upload-port COM4 --monitor-port COM4
//

#include <Arduino.h>
#include <mbedtls/sha256.h>
#include "comms/modem_driver.h"

// Test artifact from the bench release (2026.07.0b)
static const char* kTestUrl = "https://unhzttnuayrgqrzeqetz.supabase.co/storage/v1/object/public/releases/mothership/fieldmesh-bench-2026.07.0b/image.bin";
static const uint32_t kTestSize = 1311520;
static const char* kTestSha256 = "ac642de3925dd6f74db0c9d96b69b930adb13e87ac0bd6d738250dab685919b8";
static const uint32_t kChunkSize = 512 * 1024;  // 512 KiB

static mbedtls_sha256_context g_sha;
static uint32_t g_bytes = 0;
static uint32_t g_lastLogged = 0;
static int g_chunksFailed = 0;

// Streaming sink: hash + count only, never store.
static bool sink(const uint8_t* d, size_t n, void*) {
  mbedtls_sha256_update(&g_sha, d, n);
  g_bytes += n;
  if (g_bytes - g_lastLogged >= 65536) {   // progress every 64 KB
    Serial.printf("  ...%lu bytes\n", (unsigned long)g_bytes);
    g_lastLogged = g_bytes;
  }
  return true;
}

static String hex32(const uint8_t* h) {
  static const char* k = "0123456789abcdef";
  String s; s.reserve(64);
  for (int i = 0; i < 32; ++i) { s += k[h[i] >> 4]; s += k[h[i] & 0xF]; }
  return s;
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[TEST] chunked Range download (512 KiB chunks, no flash)");
  Serial.printf("  URL:    %s\n", kTestUrl);
  Serial.printf("  size:   %u bytes\n", (unsigned)kTestSize);
  Serial.printf("  chunks: %u bytes each\n", (unsigned)kChunkSize);
  Serial.printf("  sha256: %s\n", kTestSha256);

  ModemDriver modem;
  modem.init();
  if (!modem.powerOn())            { Serial.println("FAIL modem powerOn"); return; }
  if (!modem.waitForNetwork(60000)){ Serial.println("FAIL waitForNetwork"); modem.gracefulShutdown(); return; }

  mbedtls_sha256_init(&g_sha);
  mbedtls_sha256_starts(&g_sha, 0);
  g_bytes = 0;
  g_lastLogged = 0;
  g_chunksFailed = 0;

  const uint32_t t0 = millis();

  // Download in chunks, each a fresh HTTP Range request.
  for (uint32_t chunkStart = 0; chunkStart < kTestSize; chunkStart += kChunkSize) {
    const uint32_t chunkEndExclusive = (chunkStart + kChunkSize < kTestSize)
                                        ? (chunkStart + kChunkSize)
                                        : kTestSize;
    const uint32_t chunkLen = chunkEndExclusive - chunkStart;

    String range = "bytes=" + String(chunkStart) + "-" + String(chunkEndExclusive - 1);
    Serial.printf("\n  chunk [%lu-%lu] (%lu bytes, range: %s)\n",
                  (unsigned long)chunkStart, (unsigned long)(chunkEndExclusive - 1),
                  (unsigned long)chunkLen, range.c_str());

    HttpsGetStreamResult r = modem.httpsGetStream(kTestUrl, sink, nullptr, 60000, "", range);

    Serial.printf("    http=%d delivered=%lu declared=%lu complete=%d aborted=%d\n",
                  r.httpStatus, (unsigned long)r.bytesDelivered,
                  (unsigned long)r.declaredContentLength, r.complete, r.aborted);

    if (r.httpStatus != 206) {
      Serial.printf("    ERROR: expected HTTP 206, got %d\n", r.httpStatus);
      g_chunksFailed++;
      break;
    }
    if (r.declaredContentLength != chunkLen) {
      Serial.printf("    ERROR: expected declared=%lu, got %lu\n",
                    (unsigned long)chunkLen, (unsigned long)r.declaredContentLength);
      g_chunksFailed++;
      break;
    }
    if (!r.complete || r.aborted) {
      Serial.printf("    ERROR: transfer incomplete or aborted\n");
      g_chunksFailed++;
      break;
    }
    if (r.bytesDelivered != chunkLen) {
      Serial.printf("    ERROR: delivered=%lu != expected=%lu\n",
                    (unsigned long)r.bytesDelivered, (unsigned long)chunkLen);
      g_chunksFailed++;
      break;
    }
  }

  const uint32_t dt = millis() - t0;

  uint8_t digest[32];
  mbedtls_sha256_finish(&g_sha, digest);
  mbedtls_sha256_free(&g_sha);
  const String gotSha = hex32(digest);

  Serial.printf("\n  total bytes: %lu  took: %lu ms\n", (unsigned long)g_bytes, (unsigned long)dt);
  Serial.printf("  sha256=%s\n", gotSha.c_str());

  int failures = 0;
  auto ck = [&](bool c, const char* l){ Serial.printf("%s %s\n", c?"ok  ":"FAIL", l); if(!c) failures++; };
  ck(g_chunksFailed == 0, "all chunks succeeded");
  ck(g_bytes == kTestSize, "total byte count matches expected");
  ck(gotSha == String(kTestSha256), "sha256 matches expected");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
  modem.gracefulShutdown();
}

void loop() {}
