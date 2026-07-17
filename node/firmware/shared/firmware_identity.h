#pragma once
#include <Arduino.h>

// ===== Firmware identity (single source of truth, shared by both roles) =====
//
// Answers "what exactly is running on this device?" for OTA compatibility
// decisions (plan §5.3, §8.1) and status reporting. The fields are the OTA
// vocabulary:
//   role + hwTarget  -> is this image applicable to this device?
//   semver + buildId -> is it newer, older, or the same build?
//   protocolVersion  -> can old/new devices still talk?
//
// These come from build flags injected at compile time:
//   FW_ROLE, FW_HW_TARGET, FW_SEMVER  -> set per-environment in platformio.ini
//   FW_GIT                            -> set by scripts/fw_version.py (git hash
//                                        + "-dirty"); do NOT use __DATE__/__TIME__,
//                                        which freeze across cached reflashes.
// Fallbacks keep bench/unit builds compiling when a flag is absent.

#ifndef FW_ROLE
#define FW_ROLE "unknown"
#endif
#ifndef FW_HW_TARGET
#define FW_HW_TARGET "unknown"
#endif
#ifndef FW_SEMVER
#define FW_SEMVER "0.0.0-dev"
#endif
#ifndef FW_GIT
#define FW_GIT "nogit"
#endif

// Bump when the meaning/layout of FirmwareIdentity changes.
#define FW_IDENTITY_VERSION 1

struct FirmwareIdentity {
  const char* role;             // "node" | "mothership"
  const char* semver;           // "2.4.0"
  const char* buildId;          // git short hash, optionally "-dirty"
  const char* hwTarget;         // "node-v3", "mothership-v1"
  uint16_t    protocolVersion;  // role's wire protocol version
  uint8_t     identityVersion;  // FW_IDENTITY_VERSION
};

// Each role passes its own wire protocol version (node: NODE_PROTOCOL_VERSION).
static inline FirmwareIdentity fwIdentity(uint16_t protocolVersion) {
  FirmwareIdentity id;
  id.role            = FW_ROLE;
  id.semver          = FW_SEMVER;
  id.buildId         = FW_GIT;
  id.hwTarget        = FW_HW_TARGET;
  id.protocolVersion = protocolVersion;
  id.identityVersion = FW_IDENTITY_VERSION;
  return id;
}

static inline void fwIdentityPrint(const FirmwareIdentity& id) {
  Serial.printf("[fwid] role=%s ver=%s build=%s hw=%s proto=%u idv=%u\n",
                id.role, id.semver, id.buildId, id.hwTarget,
                (unsigned)id.protocolVersion, (unsigned)id.identityVersion);
}
