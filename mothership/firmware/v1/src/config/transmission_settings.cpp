#include "config/transmission_settings.h"
#include <Preferences.h>

// NVS namespace for all transmission / upload settings.
static const char* kTxNamespace = "tx";

// ---------------------------------------------------------------------------
// loadTransmissionSettings
// ---------------------------------------------------------------------------
void loadTransmissionSettings(TransmissionSettings& s) {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, true)) {   // read-only
    Serial.println("[TX] NVS begin(\"tx\") failed — using defaults");
    s.enabled            = DEFAULT_TX_ENABLED;
    s.endpointUrl        = String("");
    s.authToken          = String("");
    s.siteId             = String("");
    s.deploymentId       = String("");
    s.uploadIntervalMin  = 0;
    s.uploadPhaseUnix    = 0;
    s.minBatteryMv       = DEFAULT_MIN_BAT_MV;
    s.maxBytesPerSession = DEFAULT_MAX_BYTES;
    s.maxRetriesPerWindow = DEFAULT_MAX_RETRIES;
    s.allowManualUpload  = DEFAULT_ALLOW_MANUAL;
    return;
  }

  s.enabled            = prefs.getBool("enabled", DEFAULT_TX_ENABLED);
  s.endpointUrl        = prefs.getString("url", String(""));
  s.authToken          = prefs.getString("token", String(""));
  s.siteId             = prefs.getString("site_id", String(""));
  s.deploymentId       = prefs.getString("deploy_id", String(""));
  s.uploadIntervalMin  = prefs.getUShort("upload_min", 0);
  s.uploadPhaseUnix    = prefs.getUInt("phase_unix", 0);
  s.minBatteryMv       = prefs.getUShort("min_bat_mv", DEFAULT_MIN_BAT_MV);
  s.maxBytesPerSession = prefs.getUInt("max_bytes", DEFAULT_MAX_BYTES);
  s.maxRetriesPerWindow = prefs.getUChar("max_retries", DEFAULT_MAX_RETRIES);
  s.allowManualUpload  = prefs.getBool("allow_manual", DEFAULT_ALLOW_MANUAL);

  prefs.end();
  Serial.println("[TX] Settings loaded from NVS");
}

// ---------------------------------------------------------------------------
// saveTransmissionSettings
// ---------------------------------------------------------------------------
void saveTransmissionSettings(const TransmissionSettings& s) {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, false)) {   // read-write
    Serial.println("[TX] NVS begin(\"tx\") failed — cannot save");
    return;
  }

  prefs.putBool("enabled", s.enabled);
  prefs.putString("url", s.endpointUrl);
  prefs.putString("token", s.authToken);
  prefs.putString("site_id", s.siteId);
  prefs.putString("deploy_id", s.deploymentId);
  prefs.putUShort("upload_min", s.uploadIntervalMin);
  prefs.putUInt("phase_unix", s.uploadPhaseUnix);
  prefs.putUShort("min_bat_mv", s.minBatteryMv);
  prefs.putUInt("max_bytes", s.maxBytesPerSession);
  prefs.putUChar("max_retries", s.maxRetriesPerWindow);
  prefs.putBool("allow_manual", s.allowManualUpload);

  prefs.end();
  Serial.println("[TX] Settings saved to NVS");
}

// ---------------------------------------------------------------------------
// transmissionSettingsToJson
// ---------------------------------------------------------------------------
String transmissionSettingsToJson(const TransmissionSettings& s) {
  // Escape quotes in strings (basic, sufficient for URLs / IDs).
  auto esc = [](const String& v) -> String {
    String out;
    out.reserve(v.length() + 8);
    for (size_t i = 0; i < v.length(); i++) {
      char c = v[i];
      if (c == '"' || c == '\\') { out += '\\'; out += c; }
      else { out += c; }
    }
    return out;
  };

  String j;
  j.reserve(512);
  j += "{";
  j += "\"enabled\":" + String(s.enabled ? "true" : "false") + ",";
  j += "\"endpointUrl\":\"" + esc(s.endpointUrl) + "\",";
  j += "\"authToken\":\"" + esc(s.authToken) + "\",";
  j += "\"siteId\":\"" + esc(s.siteId) + "\",";
  j += "\"deploymentId\":\"" + esc(s.deploymentId) + "\",";
  j += "\"uploadIntervalMin\":" + String(s.uploadIntervalMin) + ",";
  j += "\"uploadPhaseUnix\":" + String(s.uploadPhaseUnix) + ",";
  j += "\"minBatteryMv\":" + String(s.minBatteryMv) + ",";
  j += "\"maxBytesPerSession\":" + String(s.maxBytesPerSession) + ",";
  j += "\"maxRetriesPerWindow\":" + String(s.maxRetriesPerWindow) + ",";
  j += "\"allowManualUpload\":" + String(s.allowManualUpload ? "true" : "false");
  j += "}";
  return j;
}

// ---------------------------------------------------------------------------
// buildUploadUrl
// ---------------------------------------------------------------------------
String buildUploadUrl(const TransmissionSettings& s) {
  String url = s.endpointUrl;
  if (url.length() == 0) return url;

  // Append query params.  Use ? for the first param, & for the rest.
  char sep = '?';
  // If the URL already contains a '?', use '&' for all params.
  if (url.indexOf('?') >= 0) sep = '&';

  if (s.authToken.length() > 0) {
    url += sep;
    url += "token=";
    url += s.authToken;
    sep = '&';
  }
  if (s.siteId.length() > 0) {
    url += sep;
    url += "siteId=";
    url += s.siteId;
    sep = '&';
  }
  if (s.deploymentId.length() > 0) {
    url += sep;
    url += "deploymentId=";
    url += s.deploymentId;
    sep = '&';
  }
  return url;
}