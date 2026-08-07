#pragma once

#include <Arduino.h>

// Cellular SIM / APN settings for the Mothership V1 modem.
//
// The APN used to be a compile-time default (MODEM_APN, hardcoded twice in
// modem_driver.cpp and never set as a build flag anywhere), so changing SIM or
// carrier meant editing source and reflashing a deployed hub. It is a setting
// now, stored in NVS namespace "sim".
//
// All keys are within NVS's 15-character limit.

// Carrier default. A hub with nothing saved behaves exactly as it did before
// this setting existed — this is the value that was compiled in.
static constexpr const char* DEFAULT_SIM_APN = "TM";

// Bounds. The APN limit follows 3GPP's 100-character APN; the credential limit
// is generous for carrier PAP/CHAP logins. Both are also the injection guard:
// see simSettingsFieldValid().
static constexpr size_t SIM_APN_MAX_LEN  = 100;
static constexpr size_t SIM_AUTH_MAX_LEN = 64;

struct SimSettings {
  String apn;       // defaults to DEFAULT_SIM_APN
  String apnUser;   // optional — blank means no carrier authentication
  String apnPass;   // optional
};

// Load from NVS namespace "sim". Missing/blank APN falls back to the default.
void loadSimSettings(SimSettings& s);

// Save to NVS namespace "sim". Returns false if the namespace could not be
// opened or any individual write failed.
//
// This returns bool from the outset deliberately: saveTransmissionSettings()
// shipped as void, reported success it had not verified, and needed a later fix.
bool saveSimSettings(const SimSettings& s);

// True if `value` is safe to splice into a quoted AT command argument and is
// within `maxLen`.
//
// This is a real command-injection guard, not a formality: these values are
// concatenated into AT+CGDCONT / AT+CGAUTH argument strings, where a '"' closes
// the quoted argument early and a CR or LF terminates the command line and
// starts another one. Anything at or below 0x20 (which covers CR, LF and NUL)
// and any double quote is rejected. An empty value is valid only where the
// field itself is optional; callers check that separately.
bool simSettingsFieldValid(const String& value, size_t maxLen);
