#pragma once
#include <Arduino.h>
#include "fw_reason.h"

// ===== Mothership local self-update (plan §7.1) =====
//
// Two-step local flow driven by the config UI:
//   1) POST the signed manifest  -> mothershipOtaVerifyManifest()
//        Ed25519 signature + role/hardware/anti-downgrade checks. On success
//        the expected size + SHA-256 of the mothership artifact are recorded.
//   2) POST the matching binary  -> mothershipOtaImageChunk() (streamed)
//                                -> mothershipOtaImageFinish()
//        Streams into the inactive slot, gated on size + SHA-256 + ESP image
//        validity, then arms the boot slot. Caller reboots to apply.
//
// Boot safety uses the deferred-verify pattern: main.cpp defines
// verifyRollbackLater()=true and calls mothershipOtaFirstBootCheck() only after
// its subsystems init, so a bad image that hangs at boot auto-rolls back.

// Verify a signed manifest and stage its mothership artifact for install.
// allowDowngrade defaults to false (full anti-downgrade enforcement) — the
// cloud OTA path always uses the default. Only the deliberate local-AP
// emergency-rollback path passes true, which bypasses ONLY the anti-downgrade
// sequence gate (signature/role/hardware checks still apply).
FwReason mothershipOtaVerifyManifest(const uint8_t* json, size_t len,
                                     const uint8_t sig[64],
                                     bool allowDowngrade = false);

// Begin the install (allocate the inactive slot + ERASE it) up front, before any
// image bytes arrive. The cloud OTA path MUST call this after a successful verify
// and BEFORE opening the image download session. esp_ota_begin() erases the whole
// target partition (~200 ms); if left to run lazily inside the first
// mothershipOtaImageChunk() call, that erase stalls the modem receive loop long
// enough that the A7670G drops the TLS session mid-download (bench-proven
// 2026-07-22 — every chunk size truncated on chunk 0). Running it here moves the
// erase out of the session window so every in-session flash write is <=1 ms.
// Idempotent (no-op if already begun); requires a prior successful
// mothershipOtaVerifyManifest().
FwReason mothershipOtaImageBegin();

// Call repeatedly with ordered image bytes. Lazily begins the install on the
// first call if mothershipOtaImageBegin() was not called first (keeps the local
// self-update path working unchanged).
FwReason mothershipOtaImageChunk(const uint8_t* data, size_t len);

// Finalise: size + SHA-256 + image validation, then set the boot partition.
// FW_NONE means armed — the caller should reboot to apply. On success the
// release is recorded ARMED in the NVS release store for post-reboot promotion.
FwReason mothershipOtaImageFinish();

// releaseSequence of the manifest currently staged (0 if none verified). The
// cloud-fetch orchestrator reads this to record the ARMED release alongside its
// releaseId.
uint32_t mothershipOtaTargetSequence();

void mothershipOtaAbort();

// Set the module's last-reason (the value surfaced as status.firmware.otaReason).
// The cloud-fetch orchestrator calls this on every exit path — including its
// download-phase transport failures and deferrals, which the verify/chunk/finish
// core never touches — so the dashboard sees a truthful current OTA reason
// instead of a stale one. A subsequent successful mothershipOtaVerifyManifest()
// resets it to FW_NONE, so a completed install still reports "NONE".
void mothershipOtaSetLastReason(FwReason r);

// Pre-built status.firmware{} JSON for the cloud upload payload: identity
// (version/build/hw/protocol), running slot, OTA state, and the A/B slots[]
// table (label/version/buildId/state/active/nextBoot/present per slot).
String mothershipFirmwareStatusJson();

// One row of the A/B slot table. Exposed for fixture-based JSON-shape tests.
struct OtaSlotInfo {
  char        label[8];
  char        version[24];
  char        buildId[24];
  const char* state;
  bool        active;
  bool        nextBoot;
  bool        present;
};
// Serialise a slots[] array (pure string assembly; no esp_ota dependency).
String mothershipFwSlotsJson(const OtaSlotInfo* slots, int n);

struct MothershipOtaStatus {
  bool     manifestReady;
  uint32_t expectedSize;
  uint32_t written;
  FwReason lastReason;
  char     targetVersion[16];
  char     targetReleaseId[40];
};
MothershipOtaStatus mothershipOtaGetStatus();

// Run late in setup(), after subsystems init. If the running image is on
// probation, confirm it (subsystems reached a healthy state). If this is never
// reached (hang/crash during init), the bootloader rolls back automatically.
// Returns true if a pending image was confirmed this boot.
bool mothershipOtaFirstBootCheck();
