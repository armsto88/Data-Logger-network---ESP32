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
    s.endpointUrl        = String(DEFAULT_ENDPOINT_URL);
    s.authToken          = String("");
    s.apiKey             = String(DEFAULT_API_KEY);
    s.mothershipId       = String(DEFAULT_MOTHERSHIP_ID);
    s.projectId          = String(DEFAULT_PROJECT_ID);
    s.siteId             = String("");
    s.deploymentId       = String("");
    s.uploadIntervalMin  = 0;
    s.uploadPhaseUnix    = 0;
    s.minBatteryMv       = DEFAULT_MIN_BAT_MV;
    s.maxBytesPerSession = DEFAULT_MAX_BYTES;
    s.maxRetriesPerWindow = DEFAULT_MAX_RETRIES;
    s.allowManualUpload  = DEFAULT_ALLOW_MANUAL;
    s.useJsonUpload      = DEFAULT_USE_JSON;
    return;
  }

  s.enabled            = prefs.getBool("enabled", DEFAULT_TX_ENABLED);
  char stringBuf[256] = {};
  prefs.getString("url", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.endpointUrl = String(stringBuf);
  // If URL is empty or malformed, use the hardcoded default so users don't
  // have to type the long Apps Script URL on their phone.
  if (s.endpointUrl.length() < 10 || !s.endpointUrl.startsWith("https://")) {
    s.endpointUrl = String(DEFAULT_ENDPOINT_URL);
    Serial.println("[TX] URL invalid or empty — using hardcoded default");
  }
  // Migrate units that still have the legacy Google Apps Script URL stored in
  // NVS over to the Supabase ingest endpoint.  (The corrected value is
  // persisted on the next settings save.)
  if (s.endpointUrl.indexOf("script.google.com") >= 0) {
    s.endpointUrl = String(DEFAULT_ENDPOINT_URL);
    Serial.println("[TX] Migrated legacy Apps Script URL to Supabase endpoint");
  }
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("token", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.authToken = String(stringBuf);
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("api_key", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.apiKey = String(stringBuf);
  if (s.apiKey.length() == 0) {
    s.apiKey = String(DEFAULT_API_KEY);
  }
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("mothership_id", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.mothershipId = String(stringBuf);
  if (s.mothershipId.length() == 0) {
    s.mothershipId = String(DEFAULT_MOTHERSHIP_ID);
  }
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("project_id", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.projectId = String(stringBuf);
  if (s.projectId.length() == 0) {
    s.projectId = String(DEFAULT_PROJECT_ID);
  }
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("site_id", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.siteId = String(stringBuf);
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("deploy_id", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.deploymentId = String(stringBuf);
  s.uploadIntervalMin  = prefs.getUShort("upload_min", 0);
  s.uploadPhaseUnix    = prefs.getUInt("phase_unix", 0);
  s.minBatteryMv       = prefs.getUShort("min_bat_mv", DEFAULT_MIN_BAT_MV);
  s.maxBytesPerSession = prefs.getUInt("max_bytes", DEFAULT_MAX_BYTES);
  s.maxRetriesPerWindow = prefs.getUChar("max_retries", DEFAULT_MAX_RETRIES);
  s.allowManualUpload  = prefs.getBool("allow_manual", DEFAULT_ALLOW_MANUAL);
  // Force JSON upload on.  A previous config-save bug could persist a stale
  // `false` for the use_json key (or leave it absent on units migrating from
  // firmware that predates the field).  JSON is the primary upload path and
  // CSV is only a fallback, so there is no reason to disable it in normal
  // operation.  The corrected value will be persisted on the next save.
  s.useJsonUpload      = true;

  prefs.end();

  // Sanity clamp: stale NVS values from the config UI (e.g. old 256 KB
  // default) are larger than the ESP32 free heap can support in a single
  // session.  Anything above 128 KB is almost certainly a leftover and is
  // clamped down to the 96 KB default.  The correction is persisted back to
  // NVS so the clamp message does not repeat on every boot.
  if (s.maxBytesPerSession > 131072UL) {
    Serial.printf("[TX] maxBytesPerSession %u too large — clamping to %u\n",
                  static_cast<unsigned>(s.maxBytesPerSession),
                  static_cast<unsigned>(DEFAULT_MAX_BYTES));
    s.maxBytesPerSession = DEFAULT_MAX_BYTES;
    // Reopen namespace in read-write mode to persist the correction.
    if (prefs.begin(kTxNamespace, false)) {
      prefs.putUInt("max_bytes", s.maxBytesPerSession);
      prefs.end();
    } else {
      Serial.println("[TX] NVS begin(rw) failed - clamp not persisted");
    }
  }

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
  prefs.putString("api_key", s.apiKey);
  prefs.putString("mothership_id", s.mothershipId);
  prefs.putString("project_id", s.projectId);
  prefs.putString("site_id", s.siteId);
  prefs.putString("deploy_id", s.deploymentId);
  prefs.putUShort("upload_min", s.uploadIntervalMin);
  prefs.putUInt("phase_unix", s.uploadPhaseUnix);
  prefs.putUShort("min_bat_mv", s.minBatteryMv);
  prefs.putUInt("max_bytes", s.maxBytesPerSession);
  prefs.putUChar("max_retries", s.maxRetriesPerWindow);
  prefs.putBool("allow_manual", s.allowManualUpload);
  prefs.putBool("use_json", s.useJsonUpload);

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
  j += "\"apiKey\":\"" + esc(s.apiKey) + "\",";
  j += "\"mothershipId\":\"" + esc(s.mothershipId) + "\",";
  j += "\"projectId\":\"" + esc(s.projectId) + "\",";
  j += "\"siteId\":\"" + esc(s.siteId) + "\",";
  j += "\"deploymentId\":\"" + esc(s.deploymentId) + "\",";
  j += "\"uploadIntervalMin\":" + String(s.uploadIntervalMin) + ",";
  j += "\"uploadPhaseUnix\":" + String(s.uploadPhaseUnix) + ",";
  j += "\"minBatteryMv\":" + String(s.minBatteryMv) + ",";
  j += "\"maxBytesPerSession\":" + String(s.maxBytesPerSession) + ",";
  j += "\"maxRetriesPerWindow\":" + String(s.maxRetriesPerWindow) + ",";
  j += "\"allowManualUpload\":" + String(s.allowManualUpload ? "true" : "false") + ",";
  j += "\"useJsonUpload\":" + String(s.useJsonUpload ? "true" : "false");
  j += "}";
  return j;
}

// ---------------------------------------------------------------------------
// buildUploadUrl
// ---------------------------------------------------------------------------
static String urlEncodeParam(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    // RFC 3986 unreserved characters: A-Z a-z 0-9 - _ . ~
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      // Percent-encode everything else
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

String buildUploadUrl(const TransmissionSettings& s) {
  String url = s.endpointUrl;
  if (url.length() == 0) return url;

  // Supabase path: authentication is header-only (Authorization: Bearer
  // <apiKey>) and the endpoint takes no query params.  When an API key is set
  // we return the endpoint untouched.  Query params are only appended on the
  // legacy Google Apps Script path (apiKey empty, authToken set).
  if (s.apiKey.length() > 0) return url;

  // Append query params.  Use ? for the first param, & for the rest.
  char sep = '?';
  // If the URL already contains a '?', use '&' for all params.
  if (url.indexOf('?') >= 0) sep = '&';

  if (s.authToken.length() > 0) {
    url += sep;
    url += "token=";
    url += urlEncodeParam(s.authToken);
    sep = '&';
  }
  if (s.siteId.length() > 0) {
    url += sep;
    url += "siteId=";
    url += urlEncodeParam(s.siteId);
    sep = '&';
  }
  if (s.deploymentId.length() > 0) {
    url += sep;
    url += "deploymentId=";
    url += urlEncodeParam(s.deploymentId);
    sep = '&';
  }
  return url;
}
