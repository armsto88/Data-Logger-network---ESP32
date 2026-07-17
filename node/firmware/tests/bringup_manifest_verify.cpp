// bringup_manifest_verify.cpp — Ed25519 manifest verify + compatibility proof.
//
// Fixtures below were produced by scripts/release_sign.py against the node
// firmware.bin (keygen -> make). The private key stays in the scratchpad; only
// the 32-byte public key is embedded, exactly as a shipped device would carry
// it. Proves: good signature accepts, tampered manifest rejects, parse reads
// the right fields, and the compatibility policy returns the right reason for
// each device/role/hardware/sequence case.

#include <Arduino.h>
#include "firmware_manifest.h"

// --- Release verification public key (Ed25519) from release_sign.py pubkey ---
static const uint8_t kReleasePubKey[32] = {
  0x29, 0x69, 0xce, 0xb7, 0x91, 0x1a, 0x2c, 0x63, 0xb6, 0x4e, 0x11, 0x52,
  0x70, 0xb9, 0xb7, 0x5c, 0xd8, 0x63, 0x32, 0x2b, 0x12, 0xf3, 0x55, 0xb5,
  0x64, 0xea, 0x47, 0x8f, 0x44, 0xe4, 0x1f, 0x3e };

// The EXACT manifest.json bytes that were signed (no trailing newline).
static const char kManifest[] =
  R"({"artifacts":[{"buildId":"nogit","hwTargets":["node-v3"],"minMothershipVersion":"0.1.0","protocolVersion":2,"role":"node","sha256":"e3f189eea6d193019dff19173b526fd78edc31a39ea2789bbbb72bc81d9730d6","size":853584,"version":"0.2.0"}],"releaseId":"fieldmesh-test.0","releaseSequence":3,"schemaVersion":1})";

// Detached signature (hex) over kManifest.
static const char kSigHex[] =
  "1e6401d359d3c7c77caec06f57f003f0430a9823d60d4027a8398ab05764f1f3"
  "af6dd554cce48ce55e0935ed544f314ae9b9461747163b68319c22cdf7c7bd0a";

static int gPass = 0, gFail = 0;
static void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (ok) gPass++; else gFail++;
}

static FirmwareIdentity mkId(const char* role, const char* semver, const char* hw) {
  FirmwareIdentity id{};
  id.role = role; id.semver = semver; id.buildId = "test";
  id.hwTarget = hw; id.protocolVersion = 2; id.identityVersion = 1;
  return id;
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n\n### FieldMesh manifest verify test ###");

  const uint8_t* json = (const uint8_t*)kManifest;
  const size_t   len  = strlen(kManifest);
  Serial.printf("manifest length: %u bytes\n", (unsigned)len);

  uint8_t sig[64];
  bool sigOk = fwHexToBytes(kSigHex, sig, sizeof sig);
  check("signature hex decodes", sigOk);

  // 1) Good signature.
  check("valid signature accepted",
        manifestVerifySignature(json, len, sig, kReleasePubKey));

  // 2) Tampered manifest (flip one byte in a copy) must be rejected.
  {
    static uint8_t tampered[sizeof kManifest];
    memcpy(tampered, json, len);
    tampered[20] ^= 0x01;
    check("tampered manifest rejected",
          !manifestVerifySignature(tampered, len, sig, kReleasePubKey));
  }

  // 3) Tampered signature must be rejected.
  {
    uint8_t badSig[64];
    memcpy(badSig, sig, 64);
    badSig[0] ^= 0x01;
    check("tampered signature rejected",
          !manifestVerifySignature(json, len, badSig, kReleasePubKey));
  }

  // 4) Parse fields.
  Manifest m;
  bool parsed = manifestParse(json, len, m);
  check("manifest parses", parsed);
  if (parsed) {
    Serial.printf("  releaseId=%s seq=%u artifacts=%u\n",
                  m.releaseId, (unsigned)m.releaseSequence, m.artifactCount);
    const ManifestArtifact* a = manifestArtifactForRole(m, "node");
    check("node artifact present", a != nullptr);
    if (a) {
      Serial.printf("  node: v%s hw0=%s size=%u sha=%.16s...\n",
                    a->version, a->hwTargets[0], (unsigned)a->size, a->sha256);
      check("version parsed",  strcmp(a->version, "0.2.0") == 0);
      check("hwTarget parsed",  strcmp(a->hwTargets[0], "node-v3") == 0);
      check("size parsed",      a->size == 853584);
      check("sha256 parsed",    strncmp(a->sha256, "e3f189ee", 8) == 0);
    }
  }

  // 5) Compatibility matrix.
  const ManifestArtifact* matched = nullptr;
  FwReason r;

  r = manifestCheckCompatibility(m, mkId("node", "0.1.0", "node-v3"), 1, &matched);
  check("node-v3 fresh -> NONE", r == FW_NONE && matched != nullptr);

  r = manifestCheckCompatibility(m, mkId("node", "0.1.0", "node-v3"), 5, &matched);
  check("older release seq -> DOWNGRADE_REJECTED", r == FW_DOWNGRADE_REJECTED);

  r = manifestCheckCompatibility(m, mkId("node", "0.1.0", "node-v2"), 1, &matched);
  check("wrong hardware -> INCOMPATIBLE_HARDWARE", r == FW_INCOMPATIBLE_HARDWARE);

  r = manifestCheckCompatibility(m, mkId("mothership", "0.1.0", "mothership-v1"), 1, &matched);
  check("no artifact for role -> NO_ARTIFACT_FOR_DEVICE", r == FW_NO_ARTIFACT_FOR_DEVICE);

  r = manifestCheckCompatibility(m, mkId("node", "0.2.0", "node-v3"), 3, &matched);
  check("same seq+version -> ALREADY_INSTALLED", r == FW_ALREADY_INSTALLED);

  Serial.printf("\n=== RESULT: %d passed, %d failed ===\n", gPass, gFail);
}

void loop() { delay(1000); }
