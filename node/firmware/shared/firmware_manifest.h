#pragma once
#include <Arduino.h>
#include <string.h>
#include <Ed25519.h>       // rweather/Crypto
#include <ArduinoJson.h>   // bblanchon/ArduinoJson v7
#include "firmware_identity.h"
#include "fw_reason.h"     // FwReason, fwReasonStr, fwHexToBytes

// ===== Release manifest verify + compatibility (shared by both roles) =====
//
// Trust chain (plan §5, §11):
//   Ed25519 signature (over the exact manifest.json bytes) valid
//     -> trust the manifest
//       -> trust its per-artifact SHA-256 (checked against the download)
//         -> compatibility policy (role / hardware / anti-downgrade)
//           -> only then set the boot partition.
//
// The signature is DETACHED and covers the exact bytes downloaded, so there is
// no canonicalisation step on device (§5.2). The verification public key is
// embedded in firmware; the private key never leaves the build machine.

#ifndef FW_MANIFEST_MAX_ARTIFACTS
#define FW_MANIFEST_MAX_ARTIFACTS 3
#endif
#ifndef FW_MANIFEST_MAX_HWTARGETS
#define FW_MANIFEST_MAX_HWTARGETS 4
#endif

struct ManifestArtifact {
  char     role[12];
  char     version[16];
  char     buildId[24];
  char     hwTargets[FW_MANIFEST_MAX_HWTARGETS][16];
  uint8_t  hwTargetCount;
  uint16_t protocolVersion;
  uint32_t size;
  char     sha256[65];   // 64 hex + NUL
  char     minMothershipVersion[16];
};

struct Manifest {
  uint8_t         schemaVersion;
  char            releaseId[40];
  uint32_t        releaseSequence;
  ManifestArtifact artifacts[FW_MANIFEST_MAX_ARTIFACTS];
  uint8_t         artifactCount;
};

// --- Step 1: signature over the EXACT downloaded manifest bytes ---
static inline bool manifestVerifySignature(const uint8_t* json, size_t len,
                                           const uint8_t sig64[64],
                                           const uint8_t pub32[32]) {
  return Ed25519::verify(sig64, pub32, json, len);
}

// --- Step 2: parse (call ONLY after the signature verified) ---
static inline bool manifestParse(const uint8_t* json, size_t len, Manifest& m) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  memset(&m, 0, sizeof m);
  m.schemaVersion   = doc["schemaVersion"] | 0;
  strlcpy(m.releaseId, doc["releaseId"] | "", sizeof m.releaseId);
  m.releaseSequence = doc["releaseSequence"] | 0UL;

  JsonArrayConst arts = doc["artifacts"].as<JsonArrayConst>();
  for (JsonObjectConst a : arts) {
    if (m.artifactCount >= FW_MANIFEST_MAX_ARTIFACTS) break;
    ManifestArtifact& t = m.artifacts[m.artifactCount];
    strlcpy(t.role,    a["role"]    | "", sizeof t.role);
    strlcpy(t.version, a["version"] | "", sizeof t.version);
    strlcpy(t.buildId, a["buildId"] | "", sizeof t.buildId);
    strlcpy(t.sha256,  a["sha256"]  | "", sizeof t.sha256);
    strlcpy(t.minMothershipVersion, a["minMothershipVersion"] | "", sizeof t.minMothershipVersion);
    t.protocolVersion = a["protocolVersion"] | 0;
    t.size            = a["size"] | 0UL;
    for (JsonVariantConst h : a["hwTargets"].as<JsonArrayConst>()) {
      if (t.hwTargetCount >= FW_MANIFEST_MAX_HWTARGETS) break;
      strlcpy(t.hwTargets[t.hwTargetCount], h.as<const char*>() ? h.as<const char*>() : "",
              sizeof t.hwTargets[0]);
      t.hwTargetCount++;
    }
    m.artifactCount++;
  }
  return m.artifactCount > 0;
}

// Find the artifact intended for this device's role.
static inline const ManifestArtifact* manifestArtifactForRole(const Manifest& m,
                                                              const char* role) {
  for (uint8_t i = 0; i < m.artifactCount; i++)
    if (strcmp(m.artifacts[i].role, role) == 0) return &m.artifacts[i];
  return nullptr;
}

// --- Step 3: compatibility policy (plan §5.3/§5.4/§7.4) ---
// installedSequence: the release sequence currently installed on this device
// (0 if unknown / factory). On FW_NONE, *matched points at the chosen artifact.
// allowDowngrade: opt-in bypass of the anti-downgrade gate ONLY. Reserved for
// the deliberate local-AP emergency-rollback path (an operator physically at the
// hub choosing to reflash an older known-good build); the cloud OTA path always
// passes false so a remote DEPLOY_RELEASE can never move a fleet backwards.
// Signature/role/hardware checks are NOT bypassed — only the sequence check.
static inline FwReason manifestCheckCompatibility(const Manifest& m,
                                                  const FirmwareIdentity& self,
                                                  uint32_t installedSequence,
                                                  const ManifestArtifact** matched,
                                                  bool allowDowngrade = false) {
  if (matched) *matched = nullptr;

  const ManifestArtifact* art = manifestArtifactForRole(m, self.role);
  if (!art) return FW_NO_ARTIFACT_FOR_DEVICE;   // release has nothing for this role

  bool hwOk = false;
  for (uint8_t i = 0; i < art->hwTargetCount; i++)
    if (strcmp(art->hwTargets[i], self.hwTarget) == 0) { hwOk = true; break; }
  if (!hwOk) return FW_INCOMPATIBLE_HARDWARE;

  // Anti-downgrade: never move to an older release sequence via routine OTA.
  // An explicit local-operator override (allowDowngrade) skips only this gate.
  if (!allowDowngrade && installedSequence != 0 && m.releaseSequence < installedSequence)
    return FW_DOWNGRADE_REJECTED;

  // Same sequence + same version = nothing to do.
  if (m.releaseSequence == installedSequence &&
      strcmp(art->version, self.semver) == 0)
    return FW_ALREADY_INSTALLED;

  if (matched) *matched = art;
  return FW_NONE;
}
