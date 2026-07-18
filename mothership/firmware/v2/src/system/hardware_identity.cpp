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
// FM1 provisioning payload
// ---------------------------------------------------------------------------

ProvisionParseResult hwParseProvisioning(const String& fragment,
                                         ProvisioningPayload& out) {
  static const char kPrefix[] = "FM1.";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (!fragment.startsWith(kPrefix)) return PROV_BAD_PREFIX;

  const String b64 = fragment.substring(prefixLen);
  if (b64.length() == 0) return PROV_BAD_BASE64;

  String json;
  if (!base64UrlDecode(b64, json)) return PROV_BAD_BASE64;
  if (json.length() == 0) return PROV_BAD_BASE64;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err || !doc.is<JsonObject>()) return PROV_BAD_JSON;

  if (!doc["v"].is<int>()) return PROV_BAD_VERSION;
  const int v = doc["v"].as<int>();
  if (v != 1) return PROV_BAD_VERSION;

  const char* ep = doc["endpoint"].is<const char*>() ? doc["endpoint"].as<const char*>() : "";
  const char* ck = doc["connectionKey"].is<const char*>() ? doc["connectionKey"].as<const char*>() : "";
  if (!ep || !*ep || !ck || !*ck) return PROV_MISSING_FIELDS;

  ProvisioningPayload parsed;
  parsed.version = v;
  parsed.endpoint = ep;
  parsed.connectionKey = ck;
  if (!hwEndpointAllowed(parsed.endpoint)) return PROV_ENDPOINT_NOT_ALLOWED;

  out = parsed;
  return PROV_OK;
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
  }
  return "unknown";
}
