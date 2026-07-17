// bringup_ota_installer.cpp — proves the shared streaming OTA installer.
//
// Uses the REAL production installer (ota_installer.h): streams an image into
// the inactive slot, gates activation on size + SHA-256 + ESP image validity,
// and uses the deferred-verify rollback pattern. The "download" here is the
// running image read back from flash (transport-agnostic), so no SD/network.
//
// Menu @115200:
//   i  - install (correct SHA) -> should boot into the other slot PENDING_VERIFY
//   w  - install with a WRONG SHA -> must reject (HASH_MISMATCH), slot unchanged
//   v  - confirm running image (cancel rollback)
//   k  - reboot without confirming -> auto-rollback
//   b  - force boot back to slot0

#include <Arduino.h>
#include <SHA256.h>
#include "ota_installer.h"

// Defer confirmation so a freshly-installed image stays on probation.
extern "C" bool verifyRollbackLater() { return true; }

static char gRunningShaHex[65];  // SHA-256 of the running partition's full span

static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[i*2] = h[b[i] >> 4]; out[i*2+1] = h[b[i] & 0xf]; }
  out[n*2] = 0;
}

// Hash the full span of the running partition; this is the "expected" hash for
// a self-clone install (installer hashes the identical bytes it writes).
static void computeRunningSha() {
  const esp_partition_t* r = esp_ota_get_running_partition();
  SHA256 sha; sha.reset();
  static uint8_t buf[4096];
  for (uint32_t off = 0; off < r->size; off += sizeof buf) {
    size_t n = min((uint32_t)sizeof buf, r->size - off);
    esp_partition_read(r, off, buf, n);
    sha.update(buf, n);
    yield();
  }
  uint8_t digest[32]; sha.finalize(digest, sizeof digest);
  toHex(digest, 32, gRunningShaHex);
}

static void printInfo() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  Serial.println("\n===== OTA installer test =====");
  Serial.printf("running: %s @0x%06X size 0x%06X  pendingVerify=%s\n",
                run->label, run->address, run->size, otaIsPendingVerify() ? "YES" : "no");
  Serial.printf("running span sha256: %.16s...\n", gRunningShaHex);
  Serial.println("menu: i=install(good)  w=install(bad-sha)  v=confirm  k=reboot-no-confirm  b=slot0");
}

// Stream the running partition span through the installer.
static FwReason streamSelfImage(OtaInstall& o) {
  const esp_partition_t* r = esp_ota_get_running_partition();
  static uint8_t buf[4096];
  for (uint32_t off = 0; off < r->size; off += sizeof buf) {
    size_t n = min((uint32_t)sizeof buf, r->size - off);
    if (esp_partition_read(r, off, buf, n) != ESP_OK) return FW_FLASH_WRITE_FAILED;
    FwReason w = otaInstallWrite(o, buf, n);
    if (w != FW_NONE) return w;
    if ((off % 0x40000) == 0) Serial.printf("  %u%%\n", (unsigned)((off * 100) / r->size));
    yield();
  }
  return FW_NONE;
}

static void doInstall(bool corruptSha) {
  const esp_partition_t* r = esp_ota_get_running_partition();
  char shaHex[65];
  strcpy(shaHex, gRunningShaHex);
  if (corruptSha) { shaHex[0] = (shaHex[0] == 'a') ? 'b' : 'a'; }  // wreck expected hash

  Serial.printf("[install] begin size=0x%06X sha=%.12s%s\n",
                r->size, shaHex, corruptSha ? " (deliberately WRONG)" : "");
  OtaInstall o;
  FwReason rc = otaInstallBegin(o, r->size, shaHex);
  if (rc != FW_NONE) { Serial.printf("[install] begin failed: %s\n", fwReasonStr(rc)); return; }

  rc = streamSelfImage(o);
  if (rc != FW_NONE) { Serial.printf("[install] stream stopped: %s\n", fwReasonStr(rc)); return; }

  const esp_partition_t* bootBefore = esp_ota_get_boot_partition();
  rc = otaInstallFinish(o);
  const esp_partition_t* bootAfter = esp_ota_get_boot_partition();
  Serial.printf("[install] finish: %s  boot %s->%s\n", fwReasonStr(rc),
                bootBefore->label, bootAfter->label);

  if (rc == FW_NONE) {
    Serial.println("[install] armed. rebooting into new slot in 2s...");
    delay(2000);
    esp_restart();
  } else {
    Serial.println("[install] rejected — running slot untouched (correct).");
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n\n### FieldMesh OTA installer bring-up ###");
  computeRunningSha();
  printInfo();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  switch (Serial.read()) {
    case 'i': doInstall(false); break;
    case 'w': doInstall(true);  break;
    case 'v': Serial.printf("[confirm] %s\n", esp_err_to_name(otaConfirmImage())); break;
    case 'k': Serial.println("[reboot] no confirm -> expect rollback"); delay(500); esp_restart(); break;
    case 'b': {
      const esp_partition_t* p = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (p) Serial.printf("[recover] boot->ota_0: %s\n", esp_err_to_name(esp_ota_set_boot_partition(p)));
      break;
    }
    case '\n': case '\r': break;
    default: printInfo(); break;
  }
}
