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

// Context for the small text-body downloads (manifest / signature). Carries a
// budget measured from the FIRST body byte received, not from call time: the
// modem driver's connection setup (NTP sync, AT+CCHSTART, AT+CCHOPEN, the
// chunked CCHSEND of the request) all happen inside httpsGetStream() before
// this sink is ever invoked, and can easily consume several real seconds of
// AT-command/network round-trip on a live cellular link. A deadline captured
// before that call (as this originally shipped) gets front-run by setup
// latency and can abort a transfer before a single body byte arrives — this
// was caught on-air: a 327-byte manifest.json aborted at 100 bytes delivered
// under an 8s from-call-time budget. Measuring from first-byte-received keeps
// the intended protection (bound the small-body TRANSFER, distinct from the
// already-tolerant 20s idle timeout) without being sensitive to connection
// setup time. The ~1 MB image download's ImageCtx budget is unaffected by
// this — it's already correctly measured from the wake's session start.
struct SmallCtx {
  String*  out;
  uint32_t firstByteAtMs;   // 0 = not yet received a byte (set explicitly at construction)
  uint32_t budgetMs;        // measured from firstByteAtMs, once set
};

bool appendSink(const uint8_t* d, size_t n, void* ctx) {
  SmallCtx* c = static_cast<SmallCtx*>(ctx);
  const uint32_t now = millis();
  if (c->firstByteAtMs == 0) c->firstByteAtMs = now;
  if (now - c->firstByteAtMs > c->budgetMs) return false;   // transfer-phase cap
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
// Cap on the BODY-TRANSFER phase of each small (<1 KB) manifest/signature
// fetch, measured from the first body byte received (see SmallCtx) — not from
// call time, since connection setup (NTP sync, CCHOPEN, CCHSEND) happens
// before any body byte and can itself take several real seconds on a live
// cellular link. The 20 s passed to httpsGetStream is only an idle gap
// timeout, not an end-to-end bound; 8 s is ample for <1 KB of transfer time
// even on a slow link.
constexpr uint32_t kOtaSmallFetchDeadlineMs = 8000;
// Image download chunk size for Range-request downloading. Bench-observed
// truncation (2026-07-21/22) happened at a consistent 1,292,354 bytes
// (~1.23 MiB) regardless of client-side receive strategy (auto-push vs
// flow-controlled manual pull) — a session-scoped limit, not an ESP32-side
// pacing issue. 512 KiB per chunk gives comfortable margin under that.
constexpr uint32_t kOtaImageChunkBytes = 512UL * 1024UL;
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
  SmallCtx mctx{&manifestBody, 0, kOtaSmallFetchDeadlineMs};
  HttpsGetStreamResult mr = modem.httpsGetStream(manifestUrl, appendSink, &mctx, 20000);
  if (!mr.success)
    return retryable(mr.httpStatus == 200 ? "DOWNLOAD_TRUNCATED" : "DOWNLOAD_FAILED",
                     mr.httpStatus == 200 ? FW_DOWNLOAD_TRUNCATED : FW_DOWNLOAD_FAILED);
  SmallCtx sctx{&sigBody, 0, kOtaSmallFetchDeadlineMs};
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

  // 4. DOWNLOAD image, in bounded Range-request chunks, each streamed straight
  // into the install core. On-air evidence (2026-07-21/22 bench) ruled out
  // flow control as the cause of a persistent truncation: switching the whole
  // transport from auto-push to a flow-controlled manual AT+CCHRECV pull loop
  // made ZERO difference — the connection was torn down at the exact same
  // byte count (1,292,354 of 1,311,520) either way. That rules out an
  // ESP32-side UART/back-pressure problem and points at a session-scoped
  // limit (modem CCH/TLS buffer, or a server/CDN-side cap) that no amount of
  // client-side pacing can avoid. Splitting the download into HTTP Range
  // requests, each its own CCHOPEN/CCHSEND/CCHCLOSE session, sidesteps either
  // cause: no single session ever asks for more than kOtaImageChunkBytes.
  // A 60s idle timeout (vs 20s for the small manifest/sig fetches) tolerates
  // a transient LTE stall within a chunk; the session-limit budget check in
  // imageSink still bounds the overall time across all chunks.
  out.state = OtaLifecycleState::DOWNLOADING;
  ImageCtx ictx{sessionStartMs, sessionLimitMs, FW_NONE, false};
  MothershipOtaStatus st0 = mothershipOtaGetStatus();
  out.expectedSize = st0.expectedSize;

  // Pre-erase the target partition BEFORE opening any download session. Left to
  // run lazily on the first imageSink write, esp_ota_begin's full-partition erase
  // (~200 ms) stalls the modem receive loop and the A7670G drops the TLS session
  // mid-chunk — bench-proven 2026-07-22, where every chunk size truncated on
  // chunk 0. Running it here moves the erase out of the session window, after
  // which every in-session flash write measured <=1 ms and the full 1.3 MB image
  // downloaded cleanly (see CLOUD_OTA_BENCH_TEST_RESULTS_2026-07-22.md, Test 4).
  {
    FwReason br = mothershipOtaImageBegin();
    if (br != FW_NONE) {
      bool t = false; const char* reason = otaFwReasonToBrief(br, &t);
      return t ? retryable(reason, br) : terminal(reason, br);
    }
  }

  for (uint32_t chunkStart = 0; chunkStart < st0.expectedSize; chunkStart += kOtaImageChunkBytes) {
    const uint32_t chunkEndExclusive =
        min(chunkStart + kOtaImageChunkBytes, st0.expectedSize);
    const uint32_t chunkLen = chunkEndExclusive - chunkStart;
    String range = "bytes=" + String(chunkStart) + "-" + String(chunkEndExclusive - 1);

    HttpsGetStreamResult ir = modem.httpsGetStream(imageUrl, imageSink, &ictx, 60000,
                                                   "", range);
    MothershipOtaStatus st1 = mothershipOtaGetStatus();
    out.bytesWritten = st1.written;

    if (ictx.budgetHit) return deferred("DEFERRED_BUSY", FW_DEFERRED_BUSY);
    if (ictx.lastFwReason != FW_NONE) {
      bool t = false; const char* reason = otaFwReasonToBrief(ictx.lastFwReason, &t);
      const FwReason fw = static_cast<FwReason>(ictx.lastFwReason);
      return t ? retryable(reason, fw) : terminal(reason, fw);
    }
    // Server ignored the Range request and sent something other than the
    // requested slice (a properly-ranged 206's Content-Length is the SLICE
    // length, not the whole resource) — chunking cannot help against a server
    // that won't honor Range, so don't spin retrying it forever.
    if (ir.httpStatus != 206 || ir.declaredContentLength != chunkLen) {
      Serial.printf("[OTA] Range not honored: status=%d declared=%lu expected=%lu\n",
                    ir.httpStatus, (unsigned long)ir.declaredContentLength,
                    (unsigned long)chunkLen);
      return terminal("DOWNLOAD_FAILED", FW_DOWNLOAD_FAILED);
    }
    if (!ir.success) {
      // Transport ended before this chunk's declared body -> truncated
      // (retryable; the whole install restarts from chunk 0 next wake — the
      // install core has no partial-image resume, same as before chunking).
      return retryable(ir.aborted ? "DOWNLOAD_TIMEOUT" : "DOWNLOAD_TRUNCATED",
                       ir.aborted ? FW_DOWNLOAD_TIMEOUT : FW_DOWNLOAD_TRUNCATED);
    }
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
