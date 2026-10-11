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
// Secure connection-key provisioning (versioned "FM1" / "FM2" payloads)
// ---------------------------------------------------------------------------

// FM1 carries the permanent connection key itself and is applied directly.
// FM2 carries only a short-lived, single-use exchange token: the FieldHub
// redeems it over its own LTE link and receives the connection key in return,
// so a dashboard QR never contains a permanent credential. FM1 stays accepted
// until the dashboard has switched to FM2 and the fleet runs this firmware.
enum ProvisioningKind {
  PROV_KIND_NONE = 0,
  PROV_KIND_DIRECT_KEY,  // FM1: endpoint + connectionKey are populated
  PROV_KIND_EXCHANGE,    // FM2 or a bare token: redemptionEndpoint + exchangeToken
};

struct ProvisioningPayload {
  int              version = 0;
  ProvisioningKind kind = PROV_KIND_NONE;
  String           endpoint;            // FM1
  String           connectionKey;       // FM1
  String           redemptionEndpoint;  // FM2
  String           exchangeToken;       // FM2
};

enum ProvisionParseResult {
  PROV_OK = 0,
  PROV_BAD_PREFIX,            // not "FM1."/"FM2." prefixed and not a bare token
  PROV_BAD_BASE64,           // payload after the prefix is not valid base64url
  PROV_BAD_JSON,             // decoded bytes are not a valid JSON object
  PROV_BAD_VERSION,          // missing/unknown "v", or "v" disagrees with the prefix
  PROV_MISSING_FIELDS,       // a required field is absent/empty
  PROV_ENDPOINT_NOT_ALLOWED, // endpoint not on the approved allow-list
  PROV_BAD_TOKEN,            // exchange token is not "fmp_" + 43 base64url characters
  PROV_HARDWARE_MISMATCH,    // code was issued for a different FieldHub's MAC
};

// Parse + validate a provisioning code. Accepted forms:
//   FM1.<base64url-json>   {"v":1,"endpoint":"https://...","connectionKey":"..."}
//   FM2.<base64url-json>   {"v":2,"redemptionEndpoint":"https://...",
//                           "token":"fmp_...","hardwareMac":"AA:BB:CC:DD:EE:FF"}
//   fmp_<43 characters>    bare exchange token (manual paste); redeemed at
//                          hwRedemptionEndpoint() under this hub's own MAC
// `fragment` is the URL-fragment content (no leading '#'). On PROV_OK, `out` is
// populated and out.kind says which fields are set; on any other result `out`
// is left untouched. Neither the connection key nor the exchange token is ever
// logged by this function.
//
// An FM2 payload is rejected unless its hardwareMac is `deviceMac` (separators
// and case ignored), so a code issued for another hub fails here instead of
// spending an LTE session on a redemption the server would refuse.
ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out,
                                         const String& deviceMac);

// As above, against this hub's factory MAC (hwMacString()).
ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out);

// Human-readable, secret-free label for a parse result (safe to log/serial).
const char* hwProvisionResultStr(ProvisionParseResult r);

// The one approved redemption URL:
//   https://<FM_APPROVED_ENDPOINT_HOST>/functions/v1/redeem-device-provisioning
String hwRedemptionEndpoint();

// True if `endpoint` is exactly the approved redemption URL (host compared
// case-insensitively). The exchange token is a credential, so unlike the
// ingest allow-list this pins the path as well as the host. A development
// build may widen this by compiling with -D FM_ALLOW_ANY_ENDPOINT=1.
bool hwRedemptionEndpointAllowed(const String& endpoint);

// ---------------------------------------------------------------------------
// FM2 redemption exchange (request body + response validation)
// ---------------------------------------------------------------------------

struct RedeemedCredential {
  String endpoint;       // ingest endpoint, allow-list checked
  String connectionKey;  // permanent "fm_..." key; never log this
};

enum RedeemParseResult {
  REDEEM_OK = 0,
  REDEEM_BAD_JSON,              // body is not a JSON object
  REDEEM_NOT_SUCCESS,           // "success" is not literally true
  REDEEM_MISSING_FIELDS,        // credential.endpoint or credential.key absent/empty
  REDEEM_BAD_KEY,               // key is not "fm_" + 20..125 base64url characters
  REDEEM_ENDPOINT_NOT_ALLOWED,  // returned ingest endpoint is off the allow-list
};

// JSON body for the redemption POST:
//   {"token":"fmp_...","hardwareMac":"AA:BB:CC:DD:EE:FF"}
String hwBuildRedemptionRequest(const String& exchangeToken,
                                const String& deviceMac);

// Validate a 2xx redemption response of the form
//   {"success":true,"credential":{"endpoint":"https://...","key":"fm_..."}}
// On REDEEM_OK, `out` is populated; on any other result it is left untouched,
// so a malformed or hostile response can never yield a stored credential. The
// key is never logged by this function.
RedeemParseResult hwParseRedemptionResponse(const String& body,
                                            RedeemedCredential& out);

// Human-readable, secret-free label for a redemption result (safe to log).
const char* hwRedeemResultStr(RedeemParseResult r);

// True if `endpoint` is an approved FieldMesh HTTPS endpoint. Requires https://
// and a host matching FM_APPROVED_ENDPOINT_HOST. A development build may widen
// this by compiling with -D FM_ALLOW_ANY_ENDPOINT=1.
bool hwEndpointAllowed(const String& endpoint);

// The single approved endpoint host (FM_APPROVED_ENDPOINT_HOST). Used to build
// release-artifact URLs so an OTA download can only ever target the approved
// host, then be re-validated with hwEndpointAllowed().
const char* hwApprovedEndpointHost();
