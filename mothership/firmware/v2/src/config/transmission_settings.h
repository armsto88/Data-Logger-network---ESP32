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

// Default mothership UUID issued by the backend.  Sent as "mothership_id" in
// every reading object so Supabase can route data to the correct project.
// Stored in NVS key "mothership_id" so it can be changed per deployment.
static constexpr const char* DEFAULT_MOTHERSHIP_ID =
    "f68a7546-6727-42a0-948f-5eae5f521f66";

// Default project UUID.  The Supabase schema requires "project_id" on every
// reading.  Stored in NVS key "project_id" so it can be changed per deployment.
static constexpr const char* DEFAULT_PROJECT_ID =
    "65a72fab-6014-4159-9198-575d695db66a";

// Default device API key (sent as Authorization: Bearer).  Pre-loaded so the
// unit can upload without typing the key on a phone.  A key entered via the
// Settings page / QR string overrides this.
static constexpr const char* DEFAULT_API_KEY =
    "fm_bkyd_001_b3d189ae-8b1a-4c2a-8dc8-2b6730d90567";

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