#include "config/transmission_settings.h"
#include <Preferences.h>

#include "system/hardware_identity.h"   // hwEndpointAllowed()

// NVS namespace for all transmission / upload settings.
static const char* kTxNamespace = "tx";

// Durable re-provisioning marker keys. NVS keys are capped at 15 characters, so
// the conceptual name (fieldmesh_reprovision_required) is abbreviated here.
static const char* kReprovReqKey    = "reprov_req";
static const char* kReprovReasonKey = "reprov_why";
static const char* kReprovHostKey   = "reprov_host";
static const char* kReprovKeyPfxKey = "reprov_kpfx";

// Host portion of an https:// URL ("" if it isn't one). Non-secret.
static String endpointHostOf(const String& url) {
  if (!url.startsWith("https://")) return String("");
  const int hostStart = 8;   // strlen("https://")
  int hostEnd = url.indexOf('/', hostStart);
  if (hostEnd < 0) hostEnd = url.length();
  return url.substring(hostStart, hostEnd);
}

#ifdef TX_TEST_NVS_FAILURE_HOOK
static bool s_txTestForceSaveFailure = false;
void txTestForceSaveFailure(bool on) { s_txTestForceSaveFailure = on; }
bool txTestSaveFailureForced() { return s_txTestForceSaveFailure; }
#endif

const char* txDestinationModeName(TxDestinationMode mode) {
  switch (mode) {
    case TX_DEST_FIELDMESH:    return "fieldmesh";
    case TX_DEST_CUSTOM_HTTPS: return "custom_https";
    case TX_DEST_LOCAL_ONLY:
    default:                   return "local_only";
  }
}

// ---------------------------------------------------------------------------
// Durable "re-provisioning required" marker
// ---------------------------------------------------------------------------
static String reprovStringKey(const char* key) {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, true)) return String("");
  char buf[128] = {};
  prefs.getString(key, buf, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  prefs.end();
  return String(buf);
}

bool txReprovisionRequired() {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, true)) return false;
  const bool required = prefs.getBool(kReprovReqKey, false);
  prefs.end();
  return required;
}

String txReprovisionReason()    { return reprovStringKey(kReprovReasonKey); }
String txReprovisionHost()      { return reprovStringKey(kReprovHostKey); }
String txReprovisionKeyPrefix() { return reprovStringKey(kReprovKeyPfxKey); }

void txSetReprovisionRequired(const char* reason,
                              const String& host,
                              const String& keyPrefix) {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, false)) {
    Serial.println("[TX] NVS begin(\"tx\") failed — re-provision marker NOT set");
    return;
  }
  prefs.putBool(kReprovReqKey, true);
  prefs.putString(kReprovReasonKey, String(reason ? reason : ""));
  prefs.putString(kReprovHostKey, host);
  prefs.putString(kReprovKeyPfxKey, keyPrefix);
  prefs.end();
  Serial.printf("[TX] re-provisioning required (reason=%s host=%s)\n",
                reason ? reason : "", host.c_str());
}

void txClearReprovisionRequired() {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, false)) return;
  if (!prefs.getBool(kReprovReqKey, false)) { prefs.end(); return; }
  prefs.putBool(kReprovReqKey, false);
  prefs.putString(kReprovReasonKey, String(""));
  prefs.putString(kReprovHostKey, String(""));
  prefs.putString(kReprovKeyPfxKey, String(""));
  prefs.end();
  Serial.println("[TX] re-provisioning marker cleared");
}

// ---------------------------------------------------------------------------
// loadTransmissionSettings
// ---------------------------------------------------------------------------
void loadTransmissionSettings(TransmissionSettings& s) {
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, true)) {   // read-only
    s.destinationMode    = DEFAULT_DESTINATION_MODE;
    Serial.println("[TX] NVS begin(\"tx\") failed — using defaults");
    s.enabled            = DEFAULT_TX_ENABLED;
    s.endpointUrl        = String(DEFAULT_ENDPOINT_URL);
    s.authToken          = String("");
    s.apiKey             = String(DEFAULT_API_KEY);
    s.customEndpointUrl  = String("");
    s.customBearerToken  = String("");
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
  prefs.getString("custom_url", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.customEndpointUrl = String(stringBuf);
  memset(stringBuf, 0, sizeof(stringBuf));
  prefs.getString("custom_token", stringBuf, sizeof(stringBuf));
  stringBuf[sizeof(stringBuf) - 1] = '\0';
  s.customBearerToken = String(stringBuf);
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
  s.uploadPhaseUnix    = prefs.getUInt("phase_unix", 0);
  // These four are no longer user-editable — they're fixed at sensible defaults
  // (see transmission_settings.h) and forced here so any stale NVS value or old
  // UI override is ignored. Upload every sync (0), 1S Li-ion brownout guard, the
  // 96 KB CSV-path cap (inert on the JSON path), and 3 retries per window.
  s.uploadIntervalMin   = 0;
  s.minBatteryMv        = DEFAULT_MIN_BAT_MV;
  s.maxBytesPerSession  = DEFAULT_MAX_BYTES;
  s.maxRetriesPerWindow = DEFAULT_MAX_RETRIES;
  s.allowManualUpload  = prefs.getBool("allow_manual", DEFAULT_ALLOW_MANUAL);
  // Force JSON upload on.  A previous config-save bug could persist a stale
  // `false` for the use_json key (or leave it absent on units migrating from
  // firmware that predates the field).  JSON is the primary upload path and
  // CSV is only a fallback, so there is no reason to disable it in normal
  // operation.  The corrected value will be persisted on the next save.
  s.useJsonUpload      = true;

  // Upgrade older settings without changing their working behaviour. Before
  // destination mode existed, an enabled uploader or saved device key meant a
  // FieldMesh-provisioned unit; otherwise it was effectively local-only.
  const uint8_t storedMode = prefs.getUChar("dest_mode", 0xFF);
  if (storedMode <= static_cast<uint8_t>(TX_DEST_CUSTOM_HTTPS)) {
    s.destinationMode = static_cast<TxDestinationMode>(storedMode);
  } else {
    s.destinationMode = (s.enabled || s.apiKey.length() > 0)
        ? TX_DEST_FIELDMESH : TX_DEST_LOCAL_ONLY;
  }

  prefs.end();

  // Local-only is authoritative. A stale pre-upgrade `enabled=true` value must
  // not power the modem after the operator explicitly chooses local storage.
  if (s.destinationMode == TX_DEST_LOCAL_ONLY) {
    s.enabled = false;
  } else if (s.destinationMode == TX_DEST_CUSTOM_HTTPS &&
             !s.customEndpointUrl.startsWith("https://")) {
    s.enabled = false;
  }

  // Fail-closed remediation for a poisoned FieldMesh endpoint.
  //
  // A hub that was pointed at an unapproved host (historically possible through
  // the request-controlled endpoint writers on /save-settings, now removed) has
  // handed its device key to that host. Firmware cannot un-leak the key, so it
  // does the two things it can: stop using it, and keep saying so until the
  // operator revokes it on the dashboard and re-provisions.
  //
  // Custom HTTPS is an explicit operator-owned sink and is exempt: it is
  // supposed to point somewhere off the allow-list.
  if (s.destinationMode == TX_DEST_FIELDMESH && !hwEndpointAllowed(s.endpointUrl)) {
    const String badHost = endpointHostOf(s.endpointUrl);
    // Non-secret prefix only, so the operator can identify which key to revoke.
    const String keyPrefix = (s.apiKey.length() > 6) ? s.apiKey.substring(0, 6)
                                                      : String("");
    Serial.printf("[TX] FieldMesh endpoint host '%s' is not approved — "
                  "disabling upload and clearing the stored key\n",
                  badHost.c_str());
    s.destinationMode = TX_DEST_LOCAL_ONLY;
    s.enabled         = false;
    s.apiKey          = String("");
    s.endpointUrl     = String(DEFAULT_ENDPOINT_URL);

    Preferences fc;
    if (fc.begin(kTxNamespace, false)) {
      fc.putUChar("dest_mode", static_cast<uint8_t>(TX_DEST_LOCAL_ONLY));
      fc.putBool("enabled", false);
      fc.putString("api_key", String(""));
      fc.putString("url", s.endpointUrl);
      fc.end();
    } else {
      Serial.println("[TX] NVS begin(rw) failed — fail-closed state not persisted");
    }
    // Separate durable marker: the condition above is false from here on, so
    // without this the warning would disappear after this one load.
    txSetReprovisionRequired("endpoint_not_allowed", badHost, keyPrefix);
  }

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
bool saveTransmissionSettings(const TransmissionSettings& s) {
#ifdef TX_TEST_NVS_FAILURE_HOOK
  if (txTestSaveFailureForced()) {
    Serial.println("[TX] TEST HOOK: forcing save failure");
    return false;
  }
#endif
  Preferences prefs;
  if (!prefs.begin(kTxNamespace, false)) {   // read-write
    Serial.println("[TX] NVS begin(\"tx\") failed — cannot save");
    return false;
  }

  // Every put*() is checked. Preferences returns the number of bytes stored (0
  // on failure), so a silent partial write can no longer be reported as a save.
  bool ok = true;
  auto note = [&ok](size_t written, const char* key) {
    if (written == 0) {
      ok = false;
      Serial.printf("[TX] NVS write failed for key \"%s\"\n", key);
    }
  };

  note(prefs.putBool("enabled", s.enabled), "enabled");
  note(prefs.putUChar("dest_mode", static_cast<uint8_t>(s.destinationMode)), "dest_mode");
  note(prefs.putUShort("upload_min", s.uploadIntervalMin), "upload_min");
  note(prefs.putUInt("phase_unix", s.uploadPhaseUnix), "phase_unix");
  note(prefs.putUShort("min_bat_mv", s.minBatteryMv), "min_bat_mv");
  note(prefs.putUInt("max_bytes", s.maxBytesPerSession), "max_bytes");
  note(prefs.putUChar("max_retries", s.maxRetriesPerWindow), "max_retries");
  note(prefs.putBool("allow_manual", s.allowManualUpload), "allow_manual");
  note(prefs.putBool("use_json", s.useJsonUpload), "use_json");

  // putString() of an EMPTY value legitimately stores 0 bytes, so those writes
  // are checked against remove-or-store semantics instead of a byte count.
  auto putStr = [&](const char* key, const String& value) {
    if (value.length() == 0) {
      // Storing "" is how a field is cleared (e.g. the burned key). remove()
      // returns false when the key was already absent, which is success here.
      prefs.remove(key);
      if (prefs.isKey(key)) {
        ok = false;
        Serial.printf("[TX] NVS clear failed for key \"%s\"\n", key);
      }
      return;
    }
    note(prefs.putString(key, value), key);
  };

  // "url" belongs here, not on the byte-count path above: an empty endpointUrl
  // stores 0 bytes exactly like any other empty String, so checking it by count
  // would report a successful write as a failure — and that false failure would
  // send the caller into txRecoverAfterFailedSave(), disabling FieldMesh and
  // demanding re-provisioning over a save that actually worked. Clearing the key
  // is the correct representation: the loader already treats an absent/empty URL
  // as "use DEFAULT_ENDPOINT_URL".
  putStr("url", s.endpointUrl);
  putStr("token", s.authToken);
  putStr("api_key", s.apiKey);
  putStr("custom_url", s.customEndpointUrl);
  putStr("custom_token", s.customBearerToken);
  putStr("mothership_id", s.mothershipId);
  putStr("project_id", s.projectId);
  putStr("site_id", s.siteId);
  putStr("deploy_id", s.deploymentId);

  prefs.end();
  if (!ok) {
    Serial.println("[TX] Settings NOT saved — one or more NVS writes failed");
    return false;
  }
  Serial.println("[TX] Settings saved to NVS");
  return true;
}

// ---------------------------------------------------------------------------
// txRecoverAfterFailedSave
// ---------------------------------------------------------------------------
// There is no rollback: Preferences commits each key individually, so a failed
// save can leave the endpoint written and the key not (or the reverse). All the
// firmware can do is look at what actually landed and refuse to keep operating
// on an incoherent credential set.
bool txRecoverAfterFailedSave() {
  TransmissionSettings after;
  loadTransmissionSettings(after);   // may itself apply the fail-closed state

  if (txReprovisionRequired()) return true;   // already neutralised on load

  const bool incoherent =
      after.destinationMode == TX_DEST_FIELDMESH &&
      (!hwEndpointAllowed(after.endpointUrl) || after.apiKey.length() == 0);
  if (!incoherent) return false;

  Serial.println("[TX] Partial save left an incoherent FieldMesh credential set "
                 "— applying fail-closed state");
  Preferences prefs;
  if (prefs.begin(kTxNamespace, false)) {
    prefs.putUChar("dest_mode", static_cast<uint8_t>(TX_DEST_LOCAL_ONLY));
    prefs.putBool("enabled", false);
    prefs.remove("api_key");
    prefs.end();
  }
  txSetReprovisionRequired("save_failed", endpointHostOf(after.endpointUrl),
                           String(""));
  return true;
}

// ---------------------------------------------------------------------------
// buildTransmissionStatusJson — status.transmission{} for the upload payload
// ---------------------------------------------------------------------------
String buildTransmissionStatusJson(const TransmissionSettings& s) {
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
  j.reserve(384);
  j += "{";
  const String activeEndpoint = (s.destinationMode == TX_DEST_CUSTOM_HTTPS)
      ? s.customEndpointUrl : s.endpointUrl;
  j += "\"endpointUrl\":\"" + esc(activeEndpoint) + "\",";
  j += "\"siteId\":\"" + esc(s.siteId) + "\",";
  j += "\"deploymentId\":\"" + esc(s.deploymentId) + "\",";
  j += "\"uploadIntervalMin\":" + String(s.uploadIntervalMin) + ",";
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
  String url = (s.destinationMode == TX_DEST_CUSTOM_HTTPS)
      ? s.customEndpointUrl : s.endpointUrl;
  if (url.length() == 0) return url;

  // Supabase path: authentication is header-only (Authorization: Bearer
  // <apiKey>) and the endpoint takes no query params.  When an API key is set
  // we return the endpoint untouched.  Query params are only appended on the
  // legacy Google Apps Script path (apiKey empty, authToken set).
  if (s.destinationMode == TX_DEST_CUSTOM_HTTPS || s.apiKey.length() > 0) return url;

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

String buildUploadAuth(const TransmissionSettings& s) {
  if (s.destinationMode == TX_DEST_FIELDMESH) return s.apiKey;
  if (s.destinationMode == TX_DEST_CUSTOM_HTTPS) return s.customBearerToken;
  return String();
}
