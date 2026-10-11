#include "system/hardware_identity.h"

#include <ArduinoJson.h>
#include <esp_mac.h>

// The single approved FieldMesh ingest host. A wiped/reflashed hub will only
// accept a provisioning payload pointing here, so a scanned QR cannot redirect
// a hub's uploads to an unapproved endpoint. Overridable for local development
// builds (-D FM_APPROVED_ENDPOINT_HOST=\"...\" or -D FM_ALLOW_ANY_ENDPOINT=1).
#ifndef FM_APPROVED_ENDPOINT_HOST
#define FM_APPROVED_ENDPOINT_HOST "unhzttnuayrgqrzeqetz.supabase.co"
#endif

// ---------------------------------------------------------------------------
// Canonical identity
// ---------------------------------------------------------------------------

String hwFormatMac(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String hwMacString() {
  uint8_t mac[6] = {0};
  // Factory STA MAC from eFuse — the permanent hardware identity. This matches
  // WiFi.macAddress() (STA interface) but does not require the radio to be up.
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  return hwFormatMac(mac);
}

String hwCodeFromMac(const String& mac) {
  // Collect the hex nibbles, ignoring colons or any other separators, then take
  // the final six and upper-case them.
  String hex;
  hex.reserve(12);
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    const bool isHex = (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
    if (isHex) hex += c;
  }
  String suffix = (hex.length() >= 6) ? hex.substring(hex.length() - 6) : hex;
  suffix.toUpperCase();
  return String("FieldMesh-") + suffix;
}

String hwCode() {
  return hwCodeFromMac(hwMacString());
}

String hwApSsid() {
  String bare = hwMacString();   // "AA:BB:CC:DD:EE:FF", already uppercase
  bare.replace(":", "");
  return String("FieldHub(") + bare + ")";
}

String hwRegisterUri(const String& mac) {
  // Percent-encode only the colons; the MAC is otherwise hex + separators.
  String encoded;
  encoded.reserve(mac.length() + 12);
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if (c == ':') encoded += "%3A";
    else encoded += c;
  }
  return String("fieldmesh://fieldhub/register?mac=") + encoded;
}

// ---------------------------------------------------------------------------
// base64url decode (RFC 4648 §5, padding optional)
// ---------------------------------------------------------------------------

static int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;  // '=' padding or any invalid character
}

static bool base64UrlDecode(const String& in, String& out) {
  out = "";
  out.reserve((in.length() * 3) / 4 + 1);
  int acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '=' ) break;             // padding — nothing meaningful follows
    if (c == '\r' || c == '\n') continue;
    const int v = b64Value(c);
    if (v < 0) return false;          // invalid base64url character
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += (char)((acc >> bits) & 0xFF);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Endpoint allow-list
// ---------------------------------------------------------------------------

const char* hwApprovedEndpointHost() {
  return FM_APPROVED_ENDPOINT_HOST;
}

bool hwEndpointAllowed(const String& endpoint) {
#ifdef FM_ALLOW_ANY_ENDPOINT
  return endpoint.length() > 0;
#else
  if (!endpoint.startsWith("https://")) return false;
  // Extract the host: between "https://" and the next '/'.
  const int hostStart = 8;  // strlen("https://")
  int hostEnd = endpoint.indexOf('/', hostStart);
  if (hostEnd < 0) hostEnd = endpoint.length();
  String host = endpoint.substring(hostStart, hostEnd);
  host.toLowerCase();
  String approved = String(FM_APPROVED_ENDPOINT_HOST);
  approved.toLowerCase();
  return host == approved;
#endif
}

// ---------------------------------------------------------------------------
// Provisioning payloads (FM1 direct key, FM2 one-time exchange)
// ---------------------------------------------------------------------------

static const char kRedemptionPath[] = "/functions/v1/redeem-device-provisioning";
static const char kExchangeTokenPrefix[] = "fmp_";
static const size_t kExchangeTokenTail = 43;  // 32 random bytes, base64url, unpadded
static const char kConnectionKeyPrefix[] = "fm_";

static bool isBase64UrlChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// True if `value` is `prefix` followed by minTail..maxTail base64url characters.
static bool hasSecretShape(const String& value, const char* prefix,
                           size_t minTail, size_t maxTail) {
  const size_t prefixLen = strlen(prefix);
  if (!value.startsWith(prefix)) return false;
  const size_t tail = value.length() - prefixLen;
  if (tail < minTail || tail > maxTail) return false;
  for (size_t i = prefixLen; i < value.length(); ++i) {
    if (!isBase64UrlChar(value[i])) return false;
  }
  return true;
}

// Uppercase hex nibbles only, so "aa:bb:.." and "AABB.." compare equal.
static String macHexOnly(const String& mac) {
  String hex;
  hex.reserve(12);
  for (size_t i = 0; i < mac.length(); ++i) {
    const char c = mac[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex += c;
    else if (c >= 'a' && c <= 'f') hex += (char)(c - 'a' + 'A');
  }
  return hex;
}

static const char* jsonStringOrEmpty(JsonVariantConst v) {
  const char* s = v.is<const char*>() ? v.as<const char*>() : nullptr;
  return s ? s : "";
}

String hwRedemptionEndpoint() {
  return String("https://") + FM_APPROVED_ENDPOINT_HOST + kRedemptionPath;
}

bool hwRedemptionEndpointAllowed(const String& endpoint) {
#ifdef FM_ALLOW_ANY_ENDPOINT
  return endpoint.length() > 0;
#else
  if (!hwEndpointAllowed(endpoint)) return false;
  const int pathStart = endpoint.indexOf('/', 8);  // first '/' after "https://"
  return pathStart > 0 && endpoint.substring(pathStart) == kRedemptionPath;
#endif
}

ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out,
                                         const String& deviceMac) {
  // Bare exchange token (manual paste). It names no endpoint and no hardware,
  // so it can only ever be redeemed at the approved URL under this hub's MAC.
  if (fragment.startsWith(kExchangeTokenPrefix)) {
    if (!hasSecretShape(fragment, kExchangeTokenPrefix, kExchangeTokenTail,
                        kExchangeTokenTail)) {
      return PROV_BAD_TOKEN;
    }
    ProvisioningPayload parsed;
    parsed.version = 2;
    parsed.kind = PROV_KIND_EXCHANGE;
    parsed.redemptionEndpoint = hwRedemptionEndpoint();
    parsed.exchangeToken = fragment;
    out = parsed;
    return PROV_OK;
  }

  int prefixVersion = 0;
  if (fragment.startsWith("FM1.")) prefixVersion = 1;
  else if (fragment.startsWith("FM2.")) prefixVersion = 2;
  else return PROV_BAD_PREFIX;

  const String b64 = fragment.substring(4);
  if (b64.length() == 0) return PROV_BAD_BASE64;

  String json;
  if (!base64UrlDecode(b64, json)) return PROV_BAD_BASE64;
  if (json.length() == 0) return PROV_BAD_BASE64;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err || !doc.is<JsonObject>()) return PROV_BAD_JSON;

  // The prefix and the payload's own "v" must agree: an FM1 body under an FM2
  // prefix (or the reverse) is not a payload either format defines.
  if (!doc["v"].is<int>()) return PROV_BAD_VERSION;
  const int v = doc["v"].as<int>();
  if (v != prefixVersion) return PROV_BAD_VERSION;

  ProvisioningPayload parsed;
  parsed.version = v;

  if (v == 1) {
    const char* ep = jsonStringOrEmpty(doc["endpoint"]);
    const char* ck = jsonStringOrEmpty(doc["connectionKey"]);
    if (!*ep || !*ck) return PROV_MISSING_FIELDS;

    parsed.kind = PROV_KIND_DIRECT_KEY;
    parsed.endpoint = ep;
    parsed.connectionKey = ck;
    if (!hwEndpointAllowed(parsed.endpoint)) return PROV_ENDPOINT_NOT_ALLOWED;
  } else {
    const char* re = jsonStringOrEmpty(doc["redemptionEndpoint"]);
    const char* tk = jsonStringOrEmpty(doc["token"]);
    const char* hm = jsonStringOrEmpty(doc["hardwareMac"]);
    if (!*re || !*tk || !*hm) return PROV_MISSING_FIELDS;

    parsed.kind = PROV_KIND_EXCHANGE;
    parsed.redemptionEndpoint = re;
    parsed.exchangeToken = tk;
    if (!hwRedemptionEndpointAllowed(parsed.redemptionEndpoint)) {
      return PROV_ENDPOINT_NOT_ALLOWED;
    }
    if (!hasSecretShape(parsed.exchangeToken, kExchangeTokenPrefix,
                        kExchangeTokenTail, kExchangeTokenTail)) {
      return PROV_BAD_TOKEN;
    }
    const String want = macHexOnly(deviceMac);
    if (want.length() != 12 || macHexOnly(hm) != want) {
      return PROV_HARDWARE_MISMATCH;
    }
  }

  out = parsed;
  return PROV_OK;
}

ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out) {
  return hwParseProvisioning(fragment, out, hwMacString());
}

const char* hwProvisionResultStr(ProvisionParseResult r) {
  switch (r) {
    case PROV_OK:                   return "OK";
    case PROV_BAD_PREFIX:           return "bad-prefix";
    case PROV_BAD_BASE64:           return "bad-base64";
    case PROV_BAD_JSON:             return "bad-json";
    case PROV_BAD_VERSION:          return "bad-version";
    case PROV_MISSING_FIELDS:       return "missing-fields";
    case PROV_ENDPOINT_NOT_ALLOWED: return "endpoint-not-allowed";
    case PROV_BAD_TOKEN:            return "bad-token";
    case PROV_HARDWARE_MISMATCH:    return "hardware-mismatch";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// FM2 redemption exchange
// ---------------------------------------------------------------------------

String hwBuildRedemptionRequest(const String& exchangeToken,
                                const String& deviceMac) {
  JsonDocument doc;
  doc["token"] = exchangeToken;
  doc["hardwareMac"] = deviceMac;
  String body;
  serializeJson(doc, body);
  return body;
}

RedeemParseResult hwParseRedemptionResponse(const String& body,
                                            RedeemedCredential& out) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonObject>()) return REDEEM_BAD_JSON;

  if (!doc["success"].is<bool>() || !doc["success"].as<bool>()) {
    return REDEEM_NOT_SUCCESS;
  }

  const char* ep = jsonStringOrEmpty(doc["credential"]["endpoint"]);
  const char* key = jsonStringOrEmpty(doc["credential"]["key"]);
  if (!*ep || !*key) return REDEEM_MISSING_FIELDS;

  // Bounds match what the settings store can hold (255 characters per value)
  // and the ingest endpoint's own bearer rule ("fm_" + at least 20 characters).
  if (!hasSecretShape(key, kConnectionKeyPrefix, 20, 125)) return REDEEM_BAD_KEY;
  if (strlen(ep) > 255 || !hwEndpointAllowed(ep)) return REDEEM_ENDPOINT_NOT_ALLOWED;

  RedeemedCredential parsed;
  parsed.endpoint = ep;
  parsed.connectionKey = key;
  out = parsed;
  return REDEEM_OK;
}

const char* hwRedeemResultStr(RedeemParseResult r) {
  switch (r) {
    case REDEEM_OK:                   return "OK";
    case REDEEM_BAD_JSON:             return "bad-json";
    case REDEEM_NOT_SUCCESS:          return "not-success";
    case REDEEM_MISSING_FIELDS:       return "missing-fields";
    case REDEEM_BAD_KEY:              return "bad-key";
    case REDEEM_ENDPOINT_NOT_ALLOWED: return "endpoint-not-allowed";
  }
  return "unknown";
}
