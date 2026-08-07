#include "config/sim_settings.h"
#include <Preferences.h>

static const char* kSimNamespace = "sim";

bool simSettingsFieldValid(const String& value, size_t maxLen) {
  if (value.length() > maxLen) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    // <= 0x20 covers NUL, CR, LF, tab and space; '"' would close the quoted
    // AT argument. Either lets the caller write a second AT command.
    if ((uint8_t)c <= 0x20 || c == '"') return false;
  }
  return true;
}

void loadSimSettings(SimSettings& s) {
  s.apn     = String(DEFAULT_SIM_APN);
  s.apnUser = String("");
  s.apnPass = String("");

  Preferences prefs;
  if (!prefs.begin(kSimNamespace, true)) {   // read-only
    Serial.println("[SIM] NVS begin(\"sim\") failed — using default APN");
    return;
  }

  char buf[128] = {};
  prefs.getString("apn", buf, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  String apn(buf);
  if (apn.length() > 0) s.apn = apn;

  memset(buf, 0, sizeof(buf));
  prefs.getString("apn_user", buf, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  s.apnUser = String(buf);

  memset(buf, 0, sizeof(buf));
  prefs.getString("apn_pass", buf, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  s.apnPass = String(buf);

  prefs.end();

  // Belt and braces. A value that predates the validation below, or one written
  // by some future path that forgets to validate, must never reach an AT command.
  if (!simSettingsFieldValid(s.apn, SIM_APN_MAX_LEN) || s.apn.length() == 0) {
    Serial.println("[SIM] Stored APN is unusable — falling back to the default");
    s.apn = String(DEFAULT_SIM_APN);
  }
  if (!simSettingsFieldValid(s.apnUser, SIM_AUTH_MAX_LEN)) s.apnUser = String("");
  if (!simSettingsFieldValid(s.apnPass, SIM_AUTH_MAX_LEN)) s.apnPass = String("");
}

bool saveSimSettings(const SimSettings& s) {
  if (s.apn.length() == 0 || !simSettingsFieldValid(s.apn, SIM_APN_MAX_LEN) ||
      !simSettingsFieldValid(s.apnUser, SIM_AUTH_MAX_LEN) ||
      !simSettingsFieldValid(s.apnPass, SIM_AUTH_MAX_LEN)) {
    Serial.println("[SIM] Refusing to save: field failed AT-argument validation");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kSimNamespace, false)) {   // read-write
    Serial.println("[SIM] NVS begin(\"sim\") failed — cannot save");
    return false;
  }

  bool ok = true;
  if (prefs.putString("apn", s.apn) == 0) {
    ok = false;
    Serial.println("[SIM] NVS write failed for key \"apn\"");
  }
  // An empty credential is stored by removing the key: putString("") writes 0
  // bytes, which is indistinguishable from a failure by return value alone.
  auto putOptional = [&](const char* key, const String& value) {
    if (value.length() == 0) {
      prefs.remove(key);
      if (prefs.isKey(key)) {
        ok = false;
        Serial.printf("[SIM] NVS clear failed for key \"%s\"\n", key);
      }
      return;
    }
    if (prefs.putString(key, value) == 0) {
      ok = false;
      Serial.printf("[SIM] NVS write failed for key \"%s\"\n", key);
    }
  };
  putOptional("apn_user", s.apnUser);
  putOptional("apn_pass", s.apnPass);

  prefs.end();
  if (!ok) {
    Serial.println("[SIM] Settings NOT saved — one or more NVS writes failed");
    return false;
  }
  Serial.printf("[SIM] Settings saved (apn=%s auth=%s)\n",
                s.apn.c_str(), s.apnUser.length() > 0 ? "yes" : "no");
  return true;
}
