#pragma once

#include <Arduino.h>

// Transmission settings for the Mothership V1 modem upload subsystem.
// Stored in NVS namespace "tx".  Loaded at boot and after web UI saves.
//
// Endpoint: Google Cloud Function (raw CSV POST, token in URL query param).

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

// ---------------------------------------------------------------------------
// Settings struct
// ---------------------------------------------------------------------------
struct TransmissionSettings {
  bool     enabled;
  String   endpointUrl;        // upload endpoint (Supabase ingest function)
  String   authToken;          // legacy token appended as ?token=xxx
  String   apiKey;             // device API key (sent as Bearer)
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
void loadTransmissionSettings(TransmissionSettings& s);

// Save settings to NVS namespace "tx".
void saveTransmissionSettings(const TransmissionSettings& s);

// Serialise settings as JSON for the web UI.
String transmissionSettingsToJson(const TransmissionSettings& s);

// Serialise the subset of settings the backend wants in status.transmission.
// Excludes secrets (apiKey/authToken) and server-derived IDs
// (mothershipId/projectId) — only the 9 fields the ingest schema stores.
String buildTransmissionStatusJson(const TransmissionSettings& s);

// Build the full upload URL: endpointUrl + ?token=xxx&siteId=yyy&deploymentId=zzz
String buildUploadUrl(const TransmissionSettings& s);