#pragma once

#include <Arduino.h>

// Transmission settings for the Mothership V1 modem upload subsystem.
// Stored in NVS namespace "tx".  Loaded at boot and after web UI saves.
//
// Upload settings for local-only, FieldMesh, or an operator-owned HTTPS sink.

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
static constexpr bool     DEFAULT_TX_ENABLED    = false;
static constexpr uint16_t DEFAULT_MIN_BAT_MV    = 3500;  // 1S Li-ion brownout guard for modem TX
static constexpr uint32_t DEFAULT_MAX_BYTES     = 98304;    // 96 KB (supports ~30 nodes)
static constexpr uint8_t  DEFAULT_MAX_RETRIES    = 3;
static constexpr bool     DEFAULT_ALLOW_MANUAL   = true;
static constexpr bool     DEFAULT_USE_JSON       = true;

// Hardcoded default endpoint URL — Supabase Edge Function (ingest-fieldmesh).
// Used when NVS has no URL stored or the stored value is malformed, so users
// don't have to type the long URL on their phone during initial setup.
static constexpr const char* DEFAULT_ENDPOINT_URL =
    "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh";

// Provisioning credentials are BLANK by default. A newly manufactured or
// wiped/reflashed FieldHub must remain unprovisioned until the local
// connection-key provisioning flow (hardware QR → dashboard → provisioning QR)
// succeeds; it must never ship or fall back to a baked-in credential. Existing
// deployed hubs are unaffected — their values live in NVS, and a blank
// compile-time fallback is only consulted when NVS has no stored value.
//
// A development build may inject working defaults without editing this file, by
// compiling with -D FM_DEFAULT_MOTHERSHIP_ID=\"...\", -D FM_DEFAULT_PROJECT_ID,
// and -D FM_DEFAULT_API_KEY.
#ifndef FM_DEFAULT_MOTHERSHIP_ID
#define FM_DEFAULT_MOTHERSHIP_ID ""
#endif
#ifndef FM_DEFAULT_PROJECT_ID
#define FM_DEFAULT_PROJECT_ID ""
#endif
#ifndef FM_DEFAULT_API_KEY
#define FM_DEFAULT_API_KEY ""
#endif

// Mothership UUID sent as "mothership_id" in each reading (NVS key
// "mothership_id"). Blank until provisioned.
static constexpr const char* DEFAULT_MOTHERSHIP_ID = FM_DEFAULT_MOTHERSHIP_ID;

// Project UUID sent as "project_id" in each reading (NVS key "project_id").
// Blank until provisioned.
static constexpr const char* DEFAULT_PROJECT_ID = FM_DEFAULT_PROJECT_ID;

// Device API key / connection key (sent as Authorization: Bearer). Blank until
// the local provisioning flow applies a dashboard-issued key.
static constexpr const char* DEFAULT_API_KEY = FM_DEFAULT_API_KEY;

// Data destination is explicit. A blank connection key means "not connected to
// FieldMesh"; it must not mean "the FieldHub itself is not set up" because all
// local commissioning and recording functions work without a backend.
enum TxDestinationMode : uint8_t {
  TX_DEST_LOCAL_ONLY   = 0,
  TX_DEST_FIELDMESH    = 1,
  TX_DEST_CUSTOM_HTTPS = 2,
};

static constexpr TxDestinationMode DEFAULT_DESTINATION_MODE = TX_DEST_LOCAL_ONLY;

// ---------------------------------------------------------------------------
// Settings struct
// ---------------------------------------------------------------------------
struct TransmissionSettings {
  TxDestinationMode destinationMode;
  bool     enabled;
  String   endpointUrl;        // upload endpoint (Supabase ingest function)
  String   authToken;          // legacy token appended as ?token=xxx
  String   apiKey;             // device API key (sent as Bearer)
  String   customEndpointUrl;  // operator-owned HTTPS JSON ingest endpoint
  String   customBearerToken;  // optional Bearer value for custom endpoint
  String   mothershipId;       // UUID sent as "mothership_id" in each reading
  String   projectId;          // UUID sent as "project_id" in each reading
  String   siteId;
  String   deploymentId;
  uint16_t uploadIntervalMin;   // 0 = every sync wake
  uint32_t uploadPhaseUnix;     // phase alignment reference
  uint16_t minBatteryMv;        // default 3700
  uint32_t maxBytesPerSession;  // default 98304 (96 KB)
  uint8_t  maxRetriesPerWindow; // default 3
  bool     allowManualUpload;   // default true
  bool     useJsonUpload;       // default true — JSON path, false = CSV fallback
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Load settings from NVS namespace "tx" into s (with defaults).
//
// Fail-closed remediation: if the stored destination is FieldMesh but the stored
// endpoint is not on the approved allow-list, the hub is treated as poisoned —
// upload is disabled, the destination drops to local-only, the stored device key
// is erased, the endpoint is reset to the default, and the durable
// "re-provisioning required" marker below is set. All of that is persisted, so
// the neutralised state survives a reboot.
void loadTransmissionSettings(TransmissionSettings& s);

// Save settings to NVS namespace "tx". Returns false if the namespace could not
// be opened or ANY individual write failed.
//
// A false return does NOT mean nothing was written. Arduino Preferences commits
// each put*() individually, so a mid-way failure leaves NVS partially updated
// and there is nothing to roll back to. Callers must treat false as "the stored
// credential set is now of unknown coherence" and report failure to the
// operator; txRecoverAfterFailedSave() below applies the fail-closed state when
// what actually landed is incoherent.
bool saveTransmissionSettings(const TransmissionSettings& s);

// Re-load settings after a failed save and, if the persisted credential set is
// incoherent (FieldMesh mode with a disallowed endpoint or no key), apply the
// fail-closed state + re-provisioning marker. Returns true if it had to.
bool txRecoverAfterFailedSave();

// ---------------------------------------------------------------------------
// Durable "re-provisioning required" marker
// ---------------------------------------------------------------------------
//
// This is the fieldmesh_reprovision_required flag. It lives in its own NVS key
// and is deliberately NOT inferred from destinationMode: remediation itself
// flips the mode to local-only, so the condition that detected the poisoning is
// false on every later boot. Without a separate durable marker the operator
// warning would render once and then silently vanish.
//
// It is cleared in exactly one place: a successfully persisted, valid FM1
// provisioning payload.

// True while this hub is stuck pending re-provisioning.
bool txReprovisionRequired();

// Machine-readable reason for the marker, e.g. "endpoint_not_allowed".
// Empty when no marker is set.
String txReprovisionReason();

// Host of the disallowed endpoint that burned the credential, for the operator
// warning. Non-secret; must still be HTML-escaped at render time. May be empty.
String txReprovisionHost();

// Non-secret prefix of the burned key (first few characters only) if one was
// recorded, so the operator can tell which key to revoke. Never the full key.
String txReprovisionKeyPrefix();

// Set the marker (persisted). `host` and `keyPrefix` are optional context.
void txSetReprovisionRequired(const char* reason,
                              const String& host,
                              const String& keyPrefix);

// Clear the marker. Only call after a valid FM1 payload has been persisted.
void txClearReprovisionRequired();

#ifdef TX_TEST_NVS_FAILURE_HOOK
// Test-only failure injection for saveTransmissionSettings(). Compiled ONLY
// into the dedicated hook build environment, never into production firmware.
// Scoped to this (tx) module: it does not and must not affect the UploadQueue
// init hook, which fails a different NVS namespace at a different layer.
void txTestForceSaveFailure(bool on);
bool txTestSaveFailureForced();
#endif

// Serialise the subset of settings the backend wants in status.transmission.
// Excludes secrets (apiKey/authToken) and server-derived IDs
// (mothershipId/projectId) — only the 9 fields the ingest schema stores.
String buildTransmissionStatusJson(const TransmissionSettings& s);

// Build the active destination URL. Legacy FieldMesh-compatible settings may
// still append token/site query parameters; custom HTTPS secrets never do.
String buildUploadUrl(const TransmissionSettings& s);

// Bearer value for the active destination (blank means no Authorization
// header). Kept separate from buildUploadUrl so custom secrets never enter a
// query string.
String buildUploadAuth(const TransmissionSettings& s);

// Stable operator/API label for the persisted destination mode.
const char* txDestinationModeName(TxDestinationMode mode);
