#pragma once

#include <Arduino.h>

// Canonical FieldHub hardware identity + secure connection-key provisioning.
//
// The factory Wi-Fi STA MAC (read from eFuse) is the permanent, customer-facing
// FieldHub identity. Every identity surface — the local portal card, the
// /api/identity endpoint, the hardware-registration QR, the config-mode AP SSID
// suffix, and the upload status deviceId — derives from the same source here so
// they can never disagree. This helper never emits API keys, project IDs, or
// connection keys.

// ---------------------------------------------------------------------------
// Canonical identity
// ---------------------------------------------------------------------------

// "AA:BB:CC:DD:EE:FF" — uppercase, colon-separated — from the given 6 bytes.
String hwFormatMac(const uint8_t mac[6]);

// Factory STA MAC (esp_read_mac ESP_MAC_WIFI_STA), formatted as above.
String hwMacString();

// "FieldMesh-ABCDEF" where ABCDEF is the final six uppercase hex characters of
// `mac`. `mac` may be colon-separated or bare; non-hex characters are ignored.
String hwCodeFromMac(const String& mac);

// hwCodeFromMac(hwMacString()).
String hwCode();

// Config-mode AP SSID: "FieldHub(AABBCCDDEEFF)" — the full factory MAC,
// uppercase, no separators. Lets a user pick the correct hub from their phone's
// Wi-Fi list. Same MAC source as every other identity surface.
String hwApSsid();

// Non-secret hardware-registration deep link for the dashboard to scan:
//   fieldmesh://fieldhub/register?mac=AA%3ABB%3ACC%3ADD%3AEE%3AFF
// Colons are percent-encoded (%3A). Contains no credentials.
String hwRegisterUri(const String& mac);

// ---------------------------------------------------------------------------
// Secure connection-key provisioning (versioned "FM1" payload)
// ---------------------------------------------------------------------------

struct ProvisioningPayload {
  int    version = 0;
  String endpoint;
  String connectionKey;
};

enum ProvisionParseResult {
  PROV_OK = 0,
  PROV_BAD_PREFIX,            // not "FM1." prefixed
  PROV_BAD_BASE64,           // payload after the prefix is not valid base64url
  PROV_BAD_JSON,             // decoded bytes are not a valid JSON object
  PROV_BAD_VERSION,          // missing/unknown "v"
  PROV_MISSING_FIELDS,       // endpoint or connectionKey absent/empty
  PROV_ENDPOINT_NOT_ALLOWED, // endpoint host not on the approved allow-list
};

// Parse + validate a versioned provisioning payload of the form
//   FM1.<base64url-json>
// where the JSON is {"v":1,"endpoint":"https://...","connectionKey":"..."}.
// `fragment` is the URL-fragment content (no leading '#'). On PROV_OK, `out` is
// populated. The plaintext connection key is never logged by this function.
ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out);

// Human-readable, secret-free label for a parse result (safe to log/serial).
const char* hwProvisionResultStr(ProvisionParseResult r);

// True if `endpoint` is an approved FieldMesh HTTPS endpoint. Requires https://
// and a host matching FM_APPROVED_ENDPOINT_HOST. A development build may widen
// this by compiling with -D FM_ALLOW_ANY_ENDPOINT=1.
bool hwEndpointAllowed(const String& endpoint);
