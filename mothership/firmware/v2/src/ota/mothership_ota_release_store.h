#pragma once
#include <Arduino.h>

// ===== Persistent OTA release state (NVS) =====
//
// Small NVS-backed store for the three pieces of OTA release state that must
// survive the mothership's hard power-cut between wakes (every wake is a cold
// boot — see main.cpp PWR_HOLD handling):
//
//   1. INSTALLED  — the releaseId + monotonic releaseSequence of the image
//                   currently confirmed-running. This is the authoritative
//                   source for anti-downgrade (installedReleaseSequence()),
//                   replacing the old hardcoded `return 0` stub.
//   2. ARMED      — a release that has been flashed to the inactive slot and
//                   set as the next boot target, but not yet confirmed on its
//                   first boot. Written just before reboot; on the next boot,
//                   mothershipOtaFirstBootCheck() promotes ARMED -> INSTALLED
//                   only if the image self-confirms. If the image fails to boot
//                   the bootloader rolls back and ARMED stays set but unmatched
//                   by the running version — that mismatch is how a rollback is
//                   detected and reported.
//   3. PENDING    — a fetch intent: the releaseId the backend asked us to
//                   install. Staged when a DEPLOY_RELEASE command is accepted,
//                   kept across wakes so a transient download failure retries
//                   automatically without the backend re-issuing the command,
//                   cleared on terminal success/failure.
//
// Storage mirrors backend_command_ingest.cpp's proven approach: a single
// versioned, FNV-1a-checksummed record written to alternating A/B keys with a
// read-back verify, so a power loss mid-write never yields a torn/half record.

// Load state from NVS (defaults: everything empty, installed sequence 0).
void otaReleaseStoreInit();

// ---- INSTALLED (anti-downgrade source) ----
uint32_t otaReleaseStoreInstalledSequence();
// Copies the installed releaseId (empty string if none). Returns false if
// there is no recorded installed release.
bool     otaReleaseStoreInstalledReleaseId(char* out, size_t outLen);
// Directly record an installed release (used e.g. to seed the first release, or
// by tests). Normal operation promotes ARMED -> INSTALLED instead.
bool     otaReleaseStoreRecordInstalled(const char* releaseId, uint32_t sequence);

// ---- PENDING (fetch intent) ----
bool     otaReleaseStoreSetPending(const char* releaseId);
// Copies the pending releaseId; returns false (and empties out) if none staged.
bool     otaReleaseStoreGetPending(char* out, size_t outLen);
bool     otaReleaseStoreClearPending();

// ---- ARMED (flashed + set-boot, awaiting first-boot confirmation) ----
bool     otaReleaseStoreSetArmed(const char* releaseId, uint32_t sequence);
// Copies the armed releaseId + sequence; returns false if nothing armed.
bool     otaReleaseStoreGetArmed(char* out, size_t outLen, uint32_t* outSequence);
// Promote ARMED -> INSTALLED (installedSequence/Id := armed*), then clear ARMED.
// Call only after the just-booted image self-confirms. No-op returns false if
// nothing was armed.
bool     otaReleaseStorePromoteArmed();
bool     otaReleaseStoreClearArmed();

// Test-only: wipe the whole namespace back to defaults.
void     otaReleaseStoreResetForTest();
