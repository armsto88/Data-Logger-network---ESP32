// On-AIR bringup test for the streaming HTTPS GET (httpsGetStream).
//
// This is the one OTA-download leg that needs real hardware end-to-end: a
// powered A7670G modem, a SIM with data, an antenna, and a real HTTPS-hosted
// test file. It powers the modem, registers on the network, streams the file
// while counting bytes + computing a running SHA-256, and compares against the
// expected size/hash. It NEVER buffers the whole file — proving the ~1 MB image
// path stays within RAM.
//
// Configure via build flags (see the mothership-v1-modem-https-get env):
//   -D OTA_GET_URL="https://host/path/file.bin"   (HTTPS only)
//   -D OTA_GET_SIZE=1048576                        (expected byte count)
//   -D OTA_GET_SHA256="<64 lowercase hex>"         (expected SHA-256, optional)
//
//   pio run -e mothership-v1-modem-https-get -t upload && pio device monitor
//
#include <Arduino.h>
#include <mbedtls/sha256.h>
#include "comms/modem_driver.h"

#ifndef OTA_GET_URL
#define OTA_GET_URL "https://example.com/replace-me.bin"
#endif
#ifndef OTA_GET_SIZE
#define OTA_GET_SIZE 0
#endif
#ifndef OTA_GET_SHA256
#define OTA_GET_SHA256 ""
#endif

static mbedtls_sha256_context g_sha;
static uint32_t g_bytes = 0;
static uint32_t g_lastLogged = 0;

// Streaming sink: hash + count only, never store. Mirrors exactly how the OTA
// path feeds mothershipOtaImageChunk() without holding the image in RAM.
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
  Serial.println("\n[TEST] streaming HTTPS GET (on-air)");
  Serial.printf("  URL:  %s\n  size: %u  sha: %s\n",
                OTA_GET_URL, (unsigned)OTA_GET_SIZE, OTA_GET_SHA256);

  ModemDriver modem;
  modem.init();
  if (!modem.powerOn())            { Serial.println("FAIL modem powerOn"); return; }
  if (!modem.waitForNetwork(60000)){ Serial.println("FAIL waitForNetwork"); modem.gracefulShutdown(); return; }

  mbedtls_sha256_init(&g_sha);
  mbedtls_sha256_starts(&g_sha, 0);
  g_bytes = 0; g_lastLogged = 0;

  const uint32_t t0 = millis();
  HttpsGetStreamResult r = modem.httpsGetStream(OTA_GET_URL, sink, nullptr, 20000);
  const uint32_t dt = millis() - t0;

  uint8_t digest[32];
  mbedtls_sha256_finish(&g_sha, digest);
  mbedtls_sha256_free(&g_sha);
  const String gotSha = hex32(digest);

  Serial.printf("\n  http=%d delivered=%lu declared=%lu complete=%d aborted=%d took=%lums\n",
                r.httpStatus, (unsigned long)r.bytesDelivered,
                (unsigned long)r.declaredContentLength, r.complete, r.aborted, dt);
  Serial.printf("  sha256=%s\n", gotSha.c_str());
  Serial.printf("  detail=%s\n", r.errorDetail.c_str());

  int failures = 0;
  auto ck = [&](bool c, const char* l){ Serial.printf("%s %s\n", c?"ok  ":"FAIL", l); if(!c) failures++; };
  ck(r.httpStatus == 200, "HTTP 200");
  ck(r.complete, "transfer complete");
  ck(!r.aborted, "not aborted");
  if (OTA_GET_SIZE) ck(r.bytesDelivered == (uint32_t)OTA_GET_SIZE, "byte count matches expected");
  ck(r.bytesDelivered == g_bytes, "sink count == result count");
  if (String(OTA_GET_SHA256).length() == 64) ck(gotSha == OTA_GET_SHA256, "sha256 matches expected");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
  modem.gracefulShutdown();
}

void loop() {}
