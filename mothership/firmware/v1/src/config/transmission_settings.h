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
static constexpr uint32_t DEFAULT_MAX_BYTES     = 262144;   // 256 KB
static constexpr uint8_t  DEFAULT_MAX_RETRIES    = 3;
static constexpr bool     DEFAULT_ALLOW_MANUAL   = true;

// ---------------------------------------------------------------------------
// Settings struct
// ---------------------------------------------------------------------------
struct TransmissionSettings {
  bool     enabled;
  String   endpointUrl;        // Google Cloud Function URL
  String   authToken;          // token appended as ?token=xxx
  String   siteId;
  String   deploymentId;
  uint16_t uploadIntervalMin;   // 0 = every sync wake
  uint32_t uploadPhaseUnix;     // phase alignment reference
  uint16_t minBatteryMv;        // default 3700
  uint32_t maxBytesPerSession;  // default 262144 (256 KB)
  uint8_t  maxRetriesPerWindow; // default 3
  bool     allowManualUpload;   // default true
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