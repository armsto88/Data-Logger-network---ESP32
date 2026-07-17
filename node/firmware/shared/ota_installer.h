#pragma once
#include <Arduino.h>
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <SHA256.h>   // rweather/Crypto (same lib as Ed25519 manifest verify)
#include "fw_reason.h"   // FwReason, fwHexToBytes (no JSON/crypto deps)

// ===== Streaming OTA image installer (shared by both roles) =====
//
// Writes a firmware image into the INACTIVE ota slot while hashing it, and only
// activates it if size + SHA-256 match the (already signature-verified) manifest
// AND the ESP image itself validates. Never touches the running slot on any
// failure path. Source of bytes is transport-agnostic: HTTP body (mothership
// self-update / node download) or flash (bench clone).
//
// Boot safety uses the deferred-verify pattern proven on the bench: the app
// must define `extern "C" bool verifyRollbackLater() { return true; }` so the
// Arduino core does not auto-confirm, then call otaConfirmImage() only after a
// first-boot self-test passes. A crash/reset before that auto-rolls back.

struct OtaInstall {
  esp_ota_handle_t       handle;
  const esp_partition_t* target;
  SHA256                 sha;
  uint32_t               expectedSize;
  uint32_t               written;
  uint8_t                expectedSha[32];
  bool                   active;
};

// expectedSha256Hex: 64 lowercase hex chars from the verified manifest artifact.
static inline FwReason otaInstallBegin(OtaInstall& o, uint32_t expectedSize,
                                       const char* expectedSha256Hex) {
  o.active = false;
  o.written = 0;
  o.expectedSize = expectedSize;
  if (!fwHexToBytes(expectedSha256Hex, o.expectedSha, 32)) return FW_MANIFEST_INVALID;

  o.target = esp_ota_get_next_update_partition(NULL);
  if (!o.target) return FW_FLASH_WRITE_FAILED;
  if (expectedSize == 0 || expectedSize > o.target->size) return FW_IMAGE_TOO_LARGE;

  o.sha.reset();
  if (esp_ota_begin(o.target, expectedSize, &o.handle) != ESP_OK) return FW_FLASH_WRITE_FAILED;
  o.active = true;
  return FW_NONE;
}

// Feed the next ordered chunk. Rejects an overrun past the declared size.
static inline void otaInstallAbort(OtaInstall& o) {
  if (o.active) { esp_ota_abort(o.handle); o.active = false; }
}

static inline FwReason otaInstallWrite(OtaInstall& o, const uint8_t* data, size_t len) {
  if (!o.active) return FW_IMAGE_INVALID;
  if ((uint64_t)o.written + len > o.expectedSize) { otaInstallAbort(o); return FW_SIZE_MISMATCH; }
  if (esp_ota_write(o.handle, data, len) != ESP_OK) { otaInstallAbort(o); return FW_FLASH_WRITE_FAILED; }
  o.sha.update(data, len);
  o.written += len;
  return FW_NONE;
}

// Finalise: size check -> SHA-256 check -> ESP image validation -> set boot slot.
// Returns FW_NONE on success (image is armed but NOT yet confirmed; caller
// persists state and reboots). Never changes the boot slot on any failure.
static inline FwReason otaInstallFinish(OtaInstall& o) {
  if (!o.active) return FW_IMAGE_INVALID;
  o.active = false;

  if (o.written != o.expectedSize) { esp_ota_abort(o.handle); return FW_SIZE_MISMATCH; }

  uint8_t got[32];
  o.sha.finalize(got, sizeof got);
  if (memcmp(got, o.expectedSha, 32) != 0) { esp_ota_abort(o.handle); return FW_HASH_MISMATCH; }

  if (esp_ota_end(o.handle) != ESP_OK) return FW_IMAGE_INVALID;       // ESP image checks
  if (esp_ota_set_boot_partition(o.target) != ESP_OK) return FW_FLASH_WRITE_FAILED;
  return FW_NONE;
}

// --- Boot-side (first-boot validation / rollback) ---

static inline bool otaIsPendingVerify() {
  const esp_partition_t* r = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return r && esp_ota_get_state_partition(r, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

// Call after the first-boot self-test PASSES. Cancels the pending rollback.
static inline esp_err_t otaConfirmImage() {
  return esp_ota_mark_app_valid_cancel_rollback();
}

// Call when the first-boot self-test FAILS. Reverts to the previous slot.
static inline esp_err_t otaRejectImageAndReboot() {
  return esp_ota_mark_app_invalid_rollback_and_reboot();
}
