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

FwReason mothershipOtaVerifyManifest(const uint8_t* json, size_t len,
                                     const uint8_t sig[64]);

// Call repeatedly with ordered image bytes. First call begins the install.
FwReason mothershipOtaImageChunk(const uint8_t* data, size_t len);

// Finalise: size + SHA-256 + image validation, then set the boot partition.
// FW_NONE means armed — the caller should reboot to apply.
FwReason mothershipOtaImageFinish();

void mothershipOtaAbort();

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
