// On-device assertion test for the cloud-OTA orchestration's PURE helpers:
// URL construction, preflight gating, and FwReason -> brief-vocabulary mapping
// (incl. transient vs terminal classification). The full IO orchestration
// (modem GET + esp_ota install) is proven on-air in the bench plan.
//
//   pio run -e mothership-v2-test-ota-cloud-fetch -t upload && pio device monitor
//
#include <Arduino.h>
#include "ota/mothership_ota_cloud_fetch.h"
#include "system/hardware_identity.h"
#include "fw_reason.h"

static int failures = 0;
static void ok(bool c, const char* label) {
  if (c) Serial.printf("ok   %s\n", label);
  else { Serial.printf("FAIL %s\n", label); failures++; }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[TEST] OTA cloud fetch (pure helpers)");

  const char* rel = "fieldmesh-2026.08.0";

  // --- URL construction ---
  const String mUrl = otaBuildManifestUrl(rel);
  const String sUrl = otaBuildManifestSigUrl(rel);
  const String iUrl = otaBuildImageUrl(rel);
  Serial.printf("  manifest: %s\n  sig: %s\n  image: %s\n",
                mUrl.c_str(), sUrl.c_str(), iUrl.c_str());
  ok(mUrl.startsWith("https://"), "manifest URL is https");
  ok(mUrl.indexOf(hwApprovedEndpointHost()) > 0, "manifest URL uses approved host");
  ok(mUrl.indexOf(rel) > 0, "manifest URL contains releaseId");
  // Supabase Storage public-read URL layout (matches the backend's release
  // bucket + immutable cache headers). Locking the prefix here guards the
  // contract the backend team settled on (see FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md B.2).
  ok(mUrl.indexOf("/storage/v1/object/public/releases/mothership/") > 0,
     "manifest URL uses Supabase Storage public-read prefix");
  ok(mUrl.endsWith("/manifest.json"), "manifest URL filename");
  ok(sUrl.endsWith("/manifest.json.sig"), "sig URL filename");
  ok(iUrl.endsWith("/image.bin"), "image URL filename");
  // The derived URLs must pass the same approved-host gate the fetch uses.
  ok(hwEndpointAllowed(mUrl), "manifest URL passes hwEndpointAllowed");
  ok(hwEndpointAllowed(iUrl), "image URL passes hwEndpointAllowed");

  // --- Preflight gating ---
  ok(strcmp(otaPreflightReason(3.9f, 200000UL), "NONE") == 0, "preflight ok when healthy");
  ok(strcmp(otaPreflightReason(3.2f, 200000UL), "DEFERRED_LOW_BATTERY") == 0, "low battery deferred");
  ok(strcmp(otaPreflightReason(3.9f, 10000UL), "DEFERRED_BUSY") == 0, "low budget deferred");
  ok(strcmp(otaPreflightReason(0.0f, 200000UL), "NONE") == 0, "unknown battery does not block");
  // Battery gate takes precedence over budget when both fail.
  ok(strcmp(otaPreflightReason(3.0f, 10000UL), "DEFERRED_LOW_BATTERY") == 0, "battery gate first");

  // --- FwReason -> brief mapping + transient classification ---
  bool t;
  ok(strcmp(otaFwReasonToBrief(FW_SIGNATURE_INVALID, &t), "SIGNATURE_INVALID") == 0 && !t, "sig invalid terminal");
  ok(strcmp(otaFwReasonToBrief(FW_HASH_MISMATCH, &t), "HASH_MISMATCH") == 0 && !t, "hash mismatch terminal");
  ok(strcmp(otaFwReasonToBrief(FW_DOWNGRADE_REJECTED, &t), "DOWNGRADE_REJECTED") == 0 && !t, "downgrade terminal");
  ok(strcmp(otaFwReasonToBrief(FW_INCOMPATIBLE_HARDWARE, &t), "INCOMPATIBLE_HARDWARE") == 0 && !t, "incompat hw terminal");
  ok(strcmp(otaFwReasonToBrief(FW_NO_ARTIFACT_FOR_DEVICE, &t), "INCOMPATIBLE_ROLE") == 0 && !t, "no-artifact -> role terminal");
  ok(strcmp(otaFwReasonToBrief(FW_DOWNLOAD_TIMEOUT, &t), "DOWNLOAD_TIMEOUT") == 0 && t, "download timeout transient");
  ok(strcmp(otaFwReasonToBrief(FW_DOWNLOAD_TRUNCATED, &t), "DOWNLOAD_TRUNCATED") == 0 && t, "download truncated transient");
  ok(strcmp(otaFwReasonToBrief(FW_MODEM_UNAVAILABLE, &t), "MODEM_UNAVAILABLE") == 0 && t, "modem unavailable transient");
  ok(strcmp(otaFwReasonToBrief(FW_ALREADY_INSTALLED, &t), "NONE") == 0 && !t, "already installed -> NONE");

  // --- lifecycle state strings match brief §5.5 ---
  ok(strcmp(otaLifecycleStateStr(OtaLifecycleState::DOWNLOADING), "DOWNLOADING") == 0, "state DOWNLOADING");
  ok(strcmp(otaLifecycleStateStr(OtaLifecycleState::READY_TO_REBOOT), "READY_TO_REBOOT") == 0, "state READY_TO_REBOOT");
  ok(strcmp(otaLifecycleStateStr(OtaLifecycleState::ROLLED_BACK), "ROLLED_BACK") == 0, "state ROLLED_BACK");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
