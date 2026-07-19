#pragma once
#include <Arduino.h>

// ===== Shared OTA reason vocabulary + small helpers (no heavy deps) =====
//
// Kept dependency-free (no ArduinoJson/crypto) so lightweight modules like the
// installer can use the reason codes without pulling in the manifest parser.
// Subset/superset of the plan §10.3 reason vocabulary.

enum FwReason : uint8_t {
  FW_NONE = 0,
  FW_MANIFEST_INVALID,
  FW_SIGNATURE_INVALID,
  FW_NO_ARTIFACT_FOR_DEVICE,
  FW_INCOMPATIBLE_HARDWARE,
  FW_INCOMPATIBLE_PROTOCOL,
  FW_DOWNGRADE_REJECTED,
  FW_ALREADY_INSTALLED,
  // Download / install
  FW_IMAGE_TOO_LARGE,
  FW_SIZE_MISMATCH,
  FW_HASH_MISMATCH,
  FW_IMAGE_INVALID,
  FW_FLASH_WRITE_FAILED,
  // Cloud-fetch transport (appended — never renumber the above; these are
  // adjacent to persisted/reported values). DOWNLOAD_TIMEOUT/DOWNLOAD_TRUNCATED
  // reuse the exact strings from the dashboard integration brief §5.5;
  // DOWNLOAD_FAILED/MODEM_UNAVAILABLE are firmware-internal diagnostics (the
  // orchestration layer surfaces a brief-compliant lifecycle reason string
  // separately — see mothership_ota_cloud_fetch).
  FW_DOWNLOAD_FAILED,
  FW_DOWNLOAD_TIMEOUT,
  FW_DOWNLOAD_TRUNCATED,
  FW_MODEM_UNAVAILABLE,
};

static inline const char* fwReasonStr(FwReason r) {
  switch (r) {
    case FW_NONE:                   return "NONE";
    case FW_MANIFEST_INVALID:       return "MANIFEST_INVALID";
    case FW_SIGNATURE_INVALID:      return "SIGNATURE_INVALID";
    case FW_NO_ARTIFACT_FOR_DEVICE: return "NO_ARTIFACT_FOR_DEVICE";
    case FW_INCOMPATIBLE_HARDWARE:  return "INCOMPATIBLE_HARDWARE";
    case FW_INCOMPATIBLE_PROTOCOL:  return "INCOMPATIBLE_PROTOCOL";
    case FW_DOWNGRADE_REJECTED:     return "DOWNGRADE_REJECTED";
    case FW_ALREADY_INSTALLED:      return "ALREADY_INSTALLED";
    case FW_IMAGE_TOO_LARGE:        return "IMAGE_TOO_LARGE";
    case FW_SIZE_MISMATCH:          return "SIZE_MISMATCH";
    case FW_HASH_MISMATCH:          return "HASH_MISMATCH";
    case FW_IMAGE_INVALID:          return "IMAGE_INVALID";
    case FW_FLASH_WRITE_FAILED:     return "FLASH_WRITE_FAILED";
    case FW_DOWNLOAD_FAILED:        return "DOWNLOAD_FAILED";
    case FW_DOWNLOAD_TIMEOUT:       return "DOWNLOAD_TIMEOUT";
    case FW_DOWNLOAD_TRUNCATED:     return "DOWNLOAD_TRUNCATED";
    case FW_MODEM_UNAVAILABLE:      return "MODEM_UNAVAILABLE";
    default:                        return "??";
  }
}

// Parse a fixed-length lowercase-hex string into bytes. False on any non-hex
// nibble; reads exactly outLen*2 chars.
static inline bool fwHexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < outLen; i++) {
    int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}
