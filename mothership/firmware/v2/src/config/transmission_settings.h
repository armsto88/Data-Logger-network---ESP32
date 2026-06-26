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
static constexpr uint16_t DEFAULT_MIN_BAT_MV    = 3700;
static constexpr uint32_t DEFAULT_MAX_BYTES     = 98304;    // 96 KB (supports ~30 nodes)
static constexpr uint8_t  DEFAULT_MAX_RETRIES    = 3;
static constexpr bool     DEFAULT_ALLOW_MANUAL   = true;
static constexpr bool     DEFAULT_USE_JSON       = true;

// Hardcoded default endpoint URL — Google Apps Script web app.  Used when NVS
// has no URL stored or the stored value is malformed, so users don't have to
// type the long URL on their phone during initial setup.
static constexpr const char* DEFAULT_ENDPOINT_URL =
    "https://script.google.com/macros/s/AKfycbwpvmqXDtS-nmr4L8JwxFo22fJBrKFODYT_i6CAewKjzeHpyyusSnJPlzzflc8gAEvD/exec";

// ---------------------------------------------------------------------------
// Settings struct
// ---------------------------------------------------------------------------
struct TransmissionSettings {
  bool     enabled;
  String   endpointUrl;        // Google Cloud Function URL
  String   authToken;          // token appended as ?token=xxx
  String   apiKey;             // fm_xxxxxxxx API key (sent as Bearer)
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

// Build the full upload URL: endpointUrl + ?token=xxx&siteId=yyy&deploymentId=zzz
String buildUploadUrl(const TransmissionSettings& s);