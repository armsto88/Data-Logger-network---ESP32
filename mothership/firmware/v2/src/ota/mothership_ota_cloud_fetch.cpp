#include "ota/mothership_ota_cloud_fetch.h"
#include "ota/mothership_selfupdate.h"
#include "ota/mothership_ota_release_store.h"
#include "system/hardware_identity.h"
#include "firmware_identity.h"
#include "protocol.h"        // NODE_PROTOCOL_VERSION
#include "fw_reason.h"

const char* otaLifecycleStateStr(OtaLifecycleState s) {
  switch (s) {
    case OtaLifecycleState::QUEUED:                  return "QUEUED";
    case OtaLifecycleState::ACCEPTED:                return "ACCEPTED";
    case OtaLifecycleState::PREFLIGHT:               return "PREFLIGHT";
    case OtaLifecycleState::DOWNLOADING:             return "DOWNLOADING";
    case OtaLifecycleState::VERIFYING:               return "VERIFYING";
    case OtaLifecycleState::READY_TO_REBOOT:         return "READY_TO_REBOOT";
    case OtaLifecycleState::PENDING_BOOT_VALIDATION: return "PENDING_BOOT_VALIDATION";
    case OtaLifecycleState::CONFIRMED:               return "CONFIRMED";
    case OtaLifecycleState::ROLLED_BACK:             return "ROLLED_BACK";
    case OtaLifecycleState::FAILED:                  return "FAILED";
  }
  return "FAILED";
}

// ---- Pure helpers ----

static String otaReleaseBase(const char* releaseId) {
  // https://<approved-host>/storage/v1/object/public/releases/<role>/<releaseId>
  //
  // Supabase Storage public-read path. Edge Functions live under
  // /functions/v1/... and Storage public reads under /storage/v1/object/public/...
  // — both on FM_APPROVED_ENDPOINT_HOST. We use the Storage public URL because
  // it serves the exact uploaded bytes verbatim (no re-serialization -> no
  // SIGNATURE_INVALID), is CDN-cached + immutable (Cache-Control: max-age=31536000
  // set at upload), and keeps the edge function out of the LTE hot path (no
  // cold start, no per-request timeout across a multi-wake ~1 MB download). The
  // dashboard never sends this URL — the firmware still derives it from
  // releaseId alone; only the prefix differs from the bare-root template.
  String base = "https://";
  base += hwApprovedEndpointHost();
  base += "/storage/v1/object/public/releases/";
  base += fwIdentity(NODE_PROTOCOL_VERSION).role;   // "mothership"
  base += "/";
  base += releaseId;
  return base;
}

String otaBuildManifestUrl(const char* releaseId)    { return otaReleaseBase(releaseId) + "/manifest.json"; }
String otaBuildManifestSigUrl(const char* releaseId) { return otaReleaseBase(releaseId) + "/manifest.json.sig"; }
String otaBuildImageUrl(const char* releaseId)       { return otaReleaseBase(releaseId) + "/image.bin"; }

const char* otaPreflightReason(float batteryV, uint32_t remainingBudgetMs) {
  if (batteryV > 0.0f && batteryV < OTA_MIN_BATTERY_V) return "DEFERRED_LOW_BATTERY";
  if (remainingBudgetMs < OTA_MIN_BUDGET_MS) return "DEFERRED_BUSY";
  return "NONE";
}

const char* otaFwReasonToBrief(int fwReason, bool* isTransient) {
  const FwReason r = static_cast<FwReason>(fwReason);
  bool transient = false;
  const char* s;
  switch (r) {
    case FW_DOWNLOAD_FAILED:
    case FW_DOWNLOAD_TIMEOUT:
    case FW_DOWNLOAD_TRUNCATED:
    case FW_MODEM_UNAVAILABLE:
      transient = true; s = fwReasonStr(r); break;
    case FW_NO_ARTIFACT_FOR_DEVICE:
      s = "INCOMPATIBLE_ROLE"; break;   // brief §5.5 vocabulary
    case FW_ALREADY_INSTALLED:
      s = "NONE"; break;                // already running it — no-op success
    default:
      s = fwReasonStr(r); break;        // MANIFEST/SIGNATURE/HASH/SIZE/... terminal
  }
  if (isTransient) *isTransient = transient;
  return s;
}

// ---- Full orchestration ----

namespace {

// Context for the small text-body downloads (manifest / signature). Carries an
// overall wall-clock deadline: httpsGetStream()'s own "timeout" is only an
// idle/inactivity gap timeout (resets on every byte), so a modem trickling
// bytes just under that gap could stall a small download near-indefinitely.
// The ~1 MB image download is already deadline-bounded via ImageCtx; this gives
// the two <1 KB pre-download fetches the same protection.
struct SmallCtx {
  String*  out;
  uint32_t deadlineMs;   // absolute millis() past which the fetch aborts
};

bool appendSink(const uint8_t* d, size_t n, void* ctx) {
  SmallCtx* c = static_cast<SmallCtx*>(ctx);
  if (millis() > c->deadlineMs) return false;   // overall wall-clock cap
  for (size_t i = 0; i < n; ++i) *c->out += (char)d[i];
  return c->out->length() < 4096;   // guard: these bodies are < ~1 KB
}

// Context for the image chunk trampoline: forwards to the install core and
// aborts if the session budget is running out.
struct ImageCtx {
  uint32_t sessionStartMs;
  uint32_t sessionLimitMs;
  int      lastFwReason;    // FwReason from the last chunk write
  bool     budgetHit;
};

bool imageSink(const uint8_t* d, size_t n, void* ctx) {
  ImageCtx* c = static_cast<ImageCtx*>(ctx);
  if (millis() - c->sessionStartMs > c->sessionLimitMs) { c->budgetHit = true; return false; }
  const FwReason r = mothershipOtaImageChunk(d, n);
  c->lastFwReason = r;
  return r == FW_NONE;   // stop the stream on any write failure
}

// Retry policy for genuinely-failed attempts (transport/verify/finish
// transients). Pure deferrals (low battery, session budget, backoff) do NOT
// count as attempts. After kOtaMaxAttempts failures the release is given up
// (terminal RETRY_LIMIT_EXCEEDED) so a persistently flaky link stops
// re-downloading the full ~1 MB image every wake forever.
constexpr uint8_t kOtaMaxAttempts = 8;
// Overall wall-clock cap for each small (<1 KB) manifest/signature fetch. The
// 20 s passed to httpsGetStream is only an idle gap timeout, not an end-to-end
// bound; 8 s is ample for <1 KB even on a slow link.
constexpr uint32_t kOtaSmallFetchDeadlineMs = 8000;
uint8_t otaBackoffWakes(uint8_t attempts) {   // wakes to skip before next attempt
  if (attempts == 0) return 0;                // first attempt: immediate
  uint8_t shift = static_cast<uint8_t>(attempts - 1);
  if (shift > 5) shift = 5;                   // cap the spacing at 32 wakes
  return static_cast<uint8_t>(1u << shift);   // 1, 2, 4, 8, 16, 32, 32, ...
}

}  // namespace

OtaCloudFetchResult mothershipOtaCloudFetchAndInstall(ModemDriver& modem,
                                                      float batteryV,
                                                      uint32_t sessionStartMs,
                                                      uint32_t sessionLimitMs) {
  OtaCloudFetchResult out{};
  char releaseId[40] = {0};
  if (!otaReleaseStoreGetPending(releaseId, sizeof(releaseId))) {
    out.state = OtaLifecycleState::QUEUED;   // nothing staged
    return out;
  }

  auto terminal = [&](const char* reason, FwReason fw) {
    out.state = OtaLifecycleState::FAILED;
    out.reasonStr = reason;
    out.transient = false;
    mothershipOtaSetLastReason(fw);   // surface in status.firmware.otaReason
    mothershipOtaAbort();
    otaReleaseStoreClearPending();   // backend must issue a fresh command
    return out;
  };
  auto retryable = [&](const char* reason, FwReason fw) {
    out.state = OtaLifecycleState::FAILED;
    out.reasonStr = reason;
    out.transient = true;
    mothershipOtaSetLastReason(fw);
    mothershipOtaAbort();
    // Keep the pending intent staged — retried automatically next wake — but
    // count this failed attempt toward the backoff/terminal cap.
    otaReleaseStoreRecordPendingAttempt(nullptr);
    return out;
  };
  // A pure "not now" defer (battery/budget/backoff). Retried next wake, intent
  // stays staged, and — unlike retryable() — does NOT consume the retry budget.
  auto deferred = [&](const char* reason, FwReason fw) {
    out.state = OtaLifecycleState::PREFLIGHT;
    out.reasonStr = reason;
    out.transient = true;
    mothershipOtaSetLastReason(fw);
    mothershipOtaAbort();   // no-op unless a mid-download budget-hit left an install open
    return out;
  };

  // 0. Retry backoff + terminal cap (before touching the modem). Only genuine
  // failed attempts count; deferrals below never reach otaReleaseStoreRecordPendingAttempt.
  const uint8_t attempts = otaReleaseStorePendingAttempts();
  if (attempts >= kOtaMaxAttempts) {
    return terminal("RETRY_LIMIT_EXCEEDED", FW_RETRY_LIMIT_EXCEEDED);
  }
  if (otaReleaseStorePendingWakesSinceAttempt() < otaBackoffWakes(attempts)) {
    otaReleaseStoreNotePendingWakeSkipped();
    return deferred("DEFERRED_BACKOFF", FW_DEFERRED_BACKOFF);
  }

  // 1. PREFLIGHT.
  out.state = OtaLifecycleState::PREFLIGHT;
  const uint32_t elapsed = millis() - sessionStartMs;
  const uint32_t remaining = (elapsed < sessionLimitMs) ? sessionLimitMs - elapsed : 0;
  const char* pf = otaPreflightReason(batteryV, remaining);
  if (strcmp(pf, "NONE") != 0) {
    const FwReason pfFw = (strcmp(pf, "DEFERRED_LOW_BATTERY") == 0)
        ? FW_DEFERRED_LOW_BATTERY : FW_DEFERRED_BUSY;
    return deferred(pf, pfFw);
  }

  // 2. DOWNLOAD manifest + signature (small).
  out.state = OtaLifecycleState::DOWNLOADING;
  const String manifestUrl = otaBuildManifestUrl(releaseId);
  const String sigUrl = otaBuildManifestSigUrl(releaseId);
  const String imageUrl = otaBuildImageUrl(releaseId);
  if (!hwEndpointAllowed(manifestUrl) || !hwEndpointAllowed(sigUrl) ||
      !hwEndpointAllowed(imageUrl)) {
    return terminal("MANIFEST_INVALID", FW_MANIFEST_INVALID);   // URL escaped the approved host
  }

  String manifestBody, sigBody;
  SmallCtx mctx{&manifestBody, millis() + kOtaSmallFetchDeadlineMs};
  HttpsGetStreamResult mr = modem.httpsGetStream(manifestUrl, appendSink, &mctx, 20000);
  if (!mr.success)
    return retryable(mr.httpStatus == 200 ? "DOWNLOAD_TRUNCATED" : "DOWNLOAD_FAILED",
                     mr.httpStatus == 200 ? FW_DOWNLOAD_TRUNCATED : FW_DOWNLOAD_FAILED);
  SmallCtx sctx{&sigBody, millis() + kOtaSmallFetchDeadlineMs};
  HttpsGetStreamResult sr = modem.httpsGetStream(sigUrl, appendSink, &sctx, 20000);
  if (!sr.success)
    return retryable(sr.httpStatus == 200 ? "DOWNLOAD_TRUNCATED" : "DOWNLOAD_FAILED",
                     sr.httpStatus == 200 ? FW_DOWNLOAD_TRUNCATED : FW_DOWNLOAD_FAILED);

  // The signature file is 128 hex chars (64 bytes).
  sigBody.trim();
  uint8_t sig[64];
  if (sigBody.length() < 128 || !fwHexToBytes(sigBody.c_str(), sig, 64)) {
    return terminal("SIGNATURE_INVALID", FW_SIGNATURE_INVALID);
  }

  // 3. VERIFY manifest (Ed25519 + role/hw/protocol + anti-downgrade).
  out.state = OtaLifecycleState::VERIFYING;
  FwReason vr = mothershipOtaVerifyManifest(
      reinterpret_cast<const uint8_t*>(manifestBody.c_str()), manifestBody.length(), sig);
  if (vr == FW_ALREADY_INSTALLED) {
    // Nothing to do — clear the intent and report the running release.
    out.state = OtaLifecycleState::CONFIRMED;
    out.reasonStr = "NONE";
    mothershipOtaSetLastReason(FW_NONE);
    otaReleaseStoreClearPending();
    return out;
  }
  if (vr != FW_NONE) {
    bool t = false; const char* reason = otaFwReasonToBrief(vr, &t);
    return t ? retryable(reason, vr) : terminal(reason, vr);
  }

  // 4. DOWNLOAD image, streamed straight into the install core.
  out.state = OtaLifecycleState::DOWNLOADING;
  ImageCtx ictx{sessionStartMs, sessionLimitMs, FW_NONE, false};
  MothershipOtaStatus st0 = mothershipOtaGetStatus();
  out.expectedSize = st0.expectedSize;
  HttpsGetStreamResult ir = modem.httpsGetStream(imageUrl, imageSink, &ictx, 20000);
  MothershipOtaStatus st1 = mothershipOtaGetStatus();
  out.bytesWritten = st1.written;

  if (ictx.budgetHit) return deferred("DEFERRED_BUSY", FW_DEFERRED_BUSY);
  if (ictx.lastFwReason != FW_NONE) {
    bool t = false; const char* reason = otaFwReasonToBrief(ictx.lastFwReason, &t);
    const FwReason fw = static_cast<FwReason>(ictx.lastFwReason);
    return t ? retryable(reason, fw) : terminal(reason, fw);
  }
  if (!ir.success) {
    // Transport ended before the full declared body -> truncated (retryable).
    return retryable(ir.aborted ? "DOWNLOAD_TIMEOUT" : "DOWNLOAD_TRUNCATED",
                     ir.aborted ? FW_DOWNLOAD_TIMEOUT : FW_DOWNLOAD_TRUNCATED);
  }

  // 5. FINISH (size + SHA-256 + esp_ota_end + set-boot). Arms the release.
  out.state = OtaLifecycleState::VERIFYING;
  FwReason fr = mothershipOtaImageFinish();
  if (fr != FW_NONE) {
    const char* reason = otaFwReasonToBrief(fr, nullptr);
    // A complete-but-bad image is terminal; treat everything here as terminal.
    return terminal(reason, fr);
  }

  // Success: image flashed + boot-armed. The release store already recorded it
  // ARMED (in mothershipOtaImageFinish). Clear the fetch intent; the next boot
  // confirms it via mothershipOtaFirstBootCheck -> promote to INSTALLED.
  otaReleaseStoreClearPending();
  out.state = OtaLifecycleState::READY_TO_REBOOT;
  out.reasonStr = "NONE";
  out.rebootPending = true;
  return out;
}
