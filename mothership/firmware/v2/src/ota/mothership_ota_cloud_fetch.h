#pragma once
#include <Arduino.h>
#include "comms/modem_driver.h"

// ===== Cloud-triggered mothership OTA: fetch + verify + install =====
//
// Turns a staged DEPLOY_RELEASE intent (a releaseId in the OTA release store)
// into an installed, boot-armed firmware image. Runs once per wake, in the LTE
// upload phase, AFTER the normal status/control upload has succeeded and only
// when the session budget allows.
//
// Trust boundary: the manifest's Ed25519 signature + per-artifact SHA-256 are
// the authority (TLS on this modem does not verify server certs). URLs are
// derived by firmware from the pinned approved host + releaseId — never
// supplied by the backend — and re-checked with hwEndpointAllowed().
//
// The verify/chunk/finish core (mothership_selfupdate) is shared unchanged with
// the local-AP install path; this module is just the cloud driver for it.

// Lifecycle states — string forms MUST match dashboard brief §5.5 exactly.
enum class OtaLifecycleState : uint8_t {
  QUEUED, ACCEPTED, PREFLIGHT, DOWNLOADING, VERIFYING,
  READY_TO_REBOOT, PENDING_BOOT_VALIDATION, CONFIRMED, ROLLED_BACK, FAILED
};
const char* otaLifecycleStateStr(OtaLifecycleState s);

struct OtaCloudFetchResult {
  OtaLifecycleState state = OtaLifecycleState::QUEUED;
  const char*       reasonStr = "NONE";   // brief §5.5 reason vocabulary
  uint32_t          bytesWritten = 0;
  uint32_t          expectedSize = 0;
  bool              rebootPending = false; // install armed; caller may reboot
  bool              transient = false;     // failure is retryable next wake
};

// ---- Pure helpers (no IO — unit-tested in tests/test_ota_cloud_fetch.cpp) ----

// Build the release artifact URLs from the pinned approved host + role +
// releaseId. Layout: https://<host>/releases/<role>/<releaseId>/<file>.
String otaBuildManifestUrl(const char* releaseId);
String otaBuildManifestSigUrl(const char* releaseId);
String otaBuildImageUrl(const char* releaseId);

// Preflight gate decision. Returns a brief §5.5 reason string:
//   "NONE"                -> proceed
//   "DEFERRED_LOW_BATTERY"-> battery below threshold (retry next wake)
//   "DEFERRED_BUSY"       -> too little session budget left (retry next wake)
// batteryV<=0 disables the battery gate (unknown reading -> don't block).
const char* otaPreflightReason(float batteryV, uint32_t remainingBudgetMs);

// Map an install-core FwReason to (reasonString, isTransient). Transient means
// "retry the whole command next wake" (leave the pending intent staged);
// terminal means "clear the intent; the backend must issue a fresh command".
const char* otaFwReasonToBrief(int fwReason, bool* isTransient);

// Battery threshold (V) below which an OTA is deferred. Build-flag overridable.
#ifndef OTA_MIN_BATTERY_V
#define OTA_MIN_BATTERY_V 3.5f
#endif
// Minimum session budget (ms) that must remain to even start a fetch.
#ifndef OTA_MIN_BUDGET_MS
#define OTA_MIN_BUDGET_MS 90000UL
#endif

// ---- Full orchestration (IO — proven on-air in the bench plan) ----

// Drive PREFLIGHT -> fetch manifest+sig -> verify -> stream image -> finish.
// `batteryV` gates preflight (pass <=0 to skip). sessionStartMs/sessionLimitMs
// bound the whole operation against the wake's shared budget.
OtaCloudFetchResult mothershipOtaCloudFetchAndInstall(ModemDriver& modem,
                                                      float batteryV,
                                                      uint32_t sessionStartMs,
                                                      uint32_t sessionLimitMs);
