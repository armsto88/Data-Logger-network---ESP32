// On-device assertion tests for the canonical FieldHub hardware identity and
// the connection-key provisioning parsers (FM1 direct key, FM2 one-time code).
//
// Covers: MAC byte formatting, hardware-code six-hex suffix, non-secret
// registration URI shape, /api/identity field wiring, and — most importantly —
// that malformed / unversioned / disallowed-endpoint / wrong-hardware
// provisioning payloads and malformed redemption replies are rejected without
// ever yielding credentials.
//
//   pio run -e mothership-v2-test-identity -t upload && pio device monitor
//
#include <Arduino.h>
#include <ArduinoJson.h>
#include "system/hardware_identity.h"

static int failures = 0;

static void expectEq(const String& got, const char* want, const char* label) {
  if (got != want) {
    Serial.printf("FAIL %-28s : got '%s' want '%s'\n", label, got.c_str(), want);
    failures++;
  } else {
    Serial.printf("ok   %-28s\n", label);
  }
}

static void expectTrue(bool cond, const char* label) {
  if (!cond) { Serial.printf("FAIL %-28s\n", label); failures++; }
  else       { Serial.printf("ok   %-28s\n", label); }
}

// base64url-encode a JSON string so tests can build payloads the way the
// dashboard would (used only to exercise the decoder; not shipped).
static String b64url(const String& in) {
  static const char* t =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  String out;
  int i = 0;
  while (i + 2 < (int)in.length()) {
    uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8) | (uint8_t)in[i+2];
    out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63];
    out += t[(n >> 6) & 63];  out += t[n & 63];
    i += 3;
  }
  int rem = in.length() - i;
  if (rem == 1) {
    uint32_t n = (uint8_t)in[i] << 16;
    out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63];  // no padding (base64url)
  } else if (rem == 2) {
    uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i+1] << 8);
    out += t[(n >> 18) & 63]; out += t[(n >> 12) & 63]; out += t[(n >> 6) & 63];
  }
  return out;
}

static String fm1(const String& json) { return String("FM1.") + b64url(json); }

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[TEST] hardware identity + provisioning");

  // --- MAC formatting ---
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  expectEq(hwFormatMac(mac), "AA:BB:CC:DD:EE:FF", "mac format upper colons");
  const uint8_t mac2[6] = {0x30, 0x76, 0xF5, 0x6C, 0x0A, 0x80};
  expectEq(hwFormatMac(mac2), "30:76:F5:6C:0A:80", "mac format leading zero");

  // --- hardware-code suffix (last six hex, uppercase) ---
  expectEq(hwCodeFromMac("AA:BB:CC:DD:EE:FF"), "FieldMesh-DDEEFF", "code from colon mac");
  expectEq(hwCodeFromMac("30:76:f5:6c:0a:80"), "FieldMesh-6C0A80", "code uppercases");
  expectEq(hwCodeFromMac("aabbccddeeff"),      "FieldMesh-DDEEFF", "code from bare mac");

  // --- registration URI (non-secret, colons percent-encoded) ---
  expectEq(hwRegisterUri("AA:BB:CC:DD:EE:FF"),
           "fieldmesh://fieldhub/register?mac=AA%3ABB%3ACC%3ADD%3AEE%3AFF",
           "register uri encoding");

  // --- live device identity: all surfaces agree ---
  const String liveMac  = hwMacString();
  const String liveCode = hwCode();
  expectTrue(liveMac.length() == 17, "live mac length 17");
  expectEq(liveCode, hwCodeFromMac(liveMac).c_str(), "code derives from live mac");
  const String ap = hwApSsid();
  expectTrue(ap.startsWith("FieldHub(") && ap.endsWith(")"), "ap ssid FieldHub(<mac>)");
  expectTrue(ap.indexOf(':') < 0, "ap ssid has no colons");
  Serial.printf("     device mac=%s code=%s ap=%s\n",
                liveMac.c_str(), liveCode.c_str(), ap.c_str());

  // --- endpoint allow-list ---
  const String approved =
      "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh";
  expectTrue(hwEndpointAllowed(approved),                         "allow approved host");
  expectTrue(!hwEndpointAllowed("https://evil.example.com/x"),    "reject other host");
  expectTrue(!hwEndpointAllowed("http://unhzttnuayrgqrzeqetz.supabase.co/x"),
             "reject non-https");

  // --- FM1 provisioning: happy path ---
  {
    ProvisioningPayload p;
    String payload = fm1(String("{\"v\":1,\"endpoint\":\"") + approved +
                         "\",\"connectionKey\":\"fm_conn_abc123\"}");
    ProvisionParseResult r = hwParseProvisioning(payload, p);
    expectTrue(r == PROV_OK, "valid payload accepted");
    expectEq(p.endpoint, approved.c_str(), "valid payload endpoint");
    expectEq(p.connectionKey, "fm_conn_abc123", "valid payload key");
  }

  // --- FM1 provisioning: rejections leave nothing usable ---
  auto rejects = [&](const String& frag, ProvisionParseResult want, const char* label) {
    ProvisioningPayload p;
    ProvisionParseResult r = hwParseProvisioning(frag, p);
    expectTrue(r == want, label);
    if (r != PROV_OK) expectTrue(p.connectionKey.length() == 0, "  no key leaked");
  };
  rejects("garbage-no-prefix", PROV_BAD_PREFIX, "reject missing prefix");
  rejects("FM1.!!!not-base64!!!", PROV_BAD_BASE64, "reject bad base64");
  rejects(fm1("{not json"), PROV_BAD_JSON, "reject bad json");
  rejects(fm1(String("{\"v\":9,\"endpoint\":\"") + approved +
              "\",\"connectionKey\":\"k\"}"), PROV_BAD_VERSION, "reject bad version");
  rejects(fm1(String("{\"v\":1,\"endpoint\":\"") + approved + "\"}"),
          PROV_MISSING_FIELDS, "reject missing key");
  rejects(fm1("{\"v\":1,\"endpoint\":\"https://evil.example.com\",\"connectionKey\":\"k\"}"),
          PROV_ENDPOINT_NOT_ALLOWED, "reject disallowed endpoint");

  // --- FM2 one-time exchange codes ---
  const String redeemUrl =
      "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/redeem-device-provisioning";
  const String token = "fmp_0123456789abcdefghijklmnopqrstuvwxyzABCDEFG";  // fmp_ + 43
  const String hubMac = "AA:BB:CC:DD:EE:FF";
  auto fm2Json = [&](const String& endpoint, const String& tok, const String& mac) {
    return String("{\"v\":2,\"redemptionEndpoint\":\"") + endpoint + "\",\"token\":\"" + tok +
           "\",\"hardwareMac\":\"" + mac + "\"}";
  };
  auto fm2 = [&](const String& json) { return String("FM2.") + b64url(json); };

  expectEq(hwRedemptionEndpoint(), redeemUrl.c_str(), "redemption endpoint derived from host");
  expectTrue(hwRedemptionEndpointAllowed(redeemUrl), "allow approved redemption url");
  expectTrue(!hwRedemptionEndpointAllowed(
                 "https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh"),
             "reject other path on approved host");
  expectTrue(!hwRedemptionEndpointAllowed(redeemUrl + "?next=https://evil.example.com"),
             "reject redemption url with a query");
  expectTrue(!hwRedemptionEndpointAllowed(
                 "https://evil.example.com/functions/v1/redeem-device-provisioning"),
             "reject redemption path on other host");

  // Golden vector: the exact fragment the dashboard's
  // createFieldHubProvisioningUrl() emits for this token and MAC. If this stops
  // parsing, the two repositories no longer agree on the FM2 format.
  {
    ProvisioningPayload p;
    ProvisionParseResult r = hwParseProvisioning(
        "FM2.eyJ2IjoyLCJyZWRlbXB0aW9uRW5kcG9pbnQiOiJodHRwczovL3VuaHp0dG51YXlyZ3FyemVxZXR6"
        "LnN1cGFiYXNlLmNvL2Z1bmN0aW9ucy92MS9yZWRlZW0tZGV2aWNlLXByb3Zpc2lvbmluZyIsInRva2Vu"
        "IjoiZm1wXzAxMjM0NTY3ODlhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ekFCQ0RFRkciLCJoYXJkd2Fy"
        "ZU1hYyI6IkFBOkJCOkNDOkREOkVFOkZGIn0",
        p, hubMac);
    expectTrue(r == PROV_OK, "dashboard FM2 vector accepted");
    expectTrue(p.kind == PROV_KIND_EXCHANGE, "FM2 is an exchange code");
    expectEq(p.redemptionEndpoint, redeemUrl.c_str(), "FM2 redemption endpoint");
    expectEq(p.exchangeToken, token.c_str(), "FM2 exchange token");
    expectTrue(p.connectionKey.length() == 0 && p.endpoint.length() == 0,
               "FM2 carries no permanent key");
  }
  {
    // The hub's own MAC is compared ignoring case and separators.
    ProvisioningPayload p;
    expectTrue(hwParseProvisioning(fm2(fm2Json(redeemUrl, token, "aabbccddeeff")), p,
                                   "aa:bb:cc:dd:ee:ff") == PROV_OK,
               "FM2 mac match ignores case/separators");
  }
  {
    // A bare token (manual paste) is redeemed at the approved URL only.
    ProvisioningPayload p;
    expectTrue(hwParseProvisioning(token, p, hubMac) == PROV_OK, "bare token accepted");
    expectTrue(p.kind == PROV_KIND_EXCHANGE, "bare token is an exchange code");
    expectEq(p.redemptionEndpoint, redeemUrl.c_str(), "bare token uses approved url");
    expectEq(p.exchangeToken, token.c_str(), "bare token preserved");
  }
  {
    ProvisioningPayload p;
    expectTrue(hwParseProvisioning(fm1(String("{\"v\":1,\"endpoint\":\"") + approved +
                                       "\",\"connectionKey\":\"fm_conn_abc123\"}"),
                                   p, hubMac) == PROV_OK &&
                   p.kind == PROV_KIND_DIRECT_KEY,
               "FM1 is a direct-key code");
  }

  auto rejects2 = [&](const String& frag, ProvisionParseResult want, const char* label) {
    ProvisioningPayload p;
    ProvisionParseResult r = hwParseProvisioning(frag, p, hubMac);
    expectTrue(r == want, label);
    expectTrue(p.kind == PROV_KIND_NONE && p.exchangeToken.length() == 0 &&
                   p.connectionKey.length() == 0,
               "  nothing usable returned");
  };
  rejects2(fm2(fm2Json(redeemUrl, token, "AA:BB:CC:DD:EE:00")), PROV_HARDWARE_MISMATCH,
           "reject code for another hub");
  rejects2(fm2(fm2Json("https://evil.example.com/functions/v1/redeem-device-provisioning",
                       token, hubMac)),
           PROV_ENDPOINT_NOT_ALLOWED, "reject FM2 other host");
  rejects2(fm2(fm2Json("https://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/other",
                       token, hubMac)),
           PROV_ENDPOINT_NOT_ALLOWED, "reject FM2 other path");
  rejects2(fm2(fm2Json(redeemUrl, "fm_0123456789abcdefghijklmnopqrstuvwxyzABCDEFGH", hubMac)),
           PROV_BAD_TOKEN, "reject permanent key as token");
  rejects2(fm2(fm2Json(redeemUrl, token.substring(0, token.length() - 1), hubMac)),
           PROV_BAD_TOKEN, "reject short token");
  rejects2(fm2(fm2Json(redeemUrl, token.substring(0, token.length() - 1) + "!", hubMac)),
           PROV_BAD_TOKEN, "reject token bad char");
  rejects2(fm2(String("{\"v\":2,\"redemptionEndpoint\":\"") + redeemUrl + "\",\"token\":\"" +
               token + "\"}"),
           PROV_MISSING_FIELDS, "reject FM2 without mac");
  rejects2(fm2(String("{\"v\":1,\"endpoint\":\"") + approved +
               "\",\"connectionKey\":\"fm_conn_abc123\"}"),
           PROV_BAD_VERSION, "reject FM1 body under FM2 prefix");
  rejects2(fm1(fm2Json(redeemUrl, token, hubMac)), PROV_BAD_VERSION,
           "reject FM2 body under FM1 prefix");
  rejects2(token + "A", PROV_BAD_TOKEN, "reject long bare token");
  rejects2("fmp_short", PROV_BAD_TOKEN, "reject short bare token");

  // --- FM2 redemption request + reply ---
  {
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, hwBuildRedemptionRequest(token, hubMac));
    expectTrue(!err && doc.size() == 2, "redemption request is a 2-field object");
    expectEq(String(doc["token"].as<const char*>()), token.c_str(), "redemption request token");
    expectEq(String(doc["hardwareMac"].as<const char*>()), hubMac.c_str(),
             "redemption request mac");
  }
  const String key = "fm_0123456789abcdefghijklmnopqrstuv";  // fm_ + 32, as issued
  auto reply = [&](const String& endpoint, const String& k) {
    return String("{\"success\":true,\"credential\":{\"id\":\"00000000-0000-0000-0000-000000000001\","
                  "\"key\":\"") + k + "\",\"keyPrefix\":\"fm_012345678\",\"endpoint\":\"" +
           endpoint + "\",\"createdAt\":\"2026-10-11T08:00:00Z\"}}";
  };
  {
    RedeemedCredential c;
    expectTrue(hwParseRedemptionResponse(reply(approved, key), c) == REDEEM_OK,
               "valid redemption reply accepted");
    expectEq(c.endpoint, approved.c_str(), "redeemed endpoint");
    expectEq(c.connectionKey, key.c_str(), "redeemed key");
  }
  auto rejectsReply = [&](const String& body, RedeemParseResult want, const char* label) {
    RedeemedCredential c;
    RedeemParseResult r = hwParseRedemptionResponse(body, c);
    expectTrue(r == want, label);
    expectTrue(c.connectionKey.length() == 0 && c.endpoint.length() == 0, "  no key leaked");
  };
  rejectsReply("<html>502 Bad Gateway</html>", REDEEM_BAD_JSON, "reject non-json reply");
  rejectsReply("[]", REDEEM_BAD_JSON, "reject non-object reply");
  rejectsReply("{\"success\":false,\"error\":\"Provisioning code is invalid\"}",
               REDEEM_NOT_SUCCESS, "reject unsuccessful reply");
  rejectsReply(String("{\"success\":\"true\",\"credential\":{\"key\":\"") + key +
                   "\",\"endpoint\":\"" + approved + "\"}}",
               REDEEM_NOT_SUCCESS, "reject non-boolean success");
  rejectsReply("{\"success\":true}", REDEEM_MISSING_FIELDS, "reject reply without credential");
  rejectsReply(String("{\"success\":true,\"credential\":{\"endpoint\":\"") + approved + "\"}}",
               REDEEM_MISSING_FIELDS, "reject reply without key");
  rejectsReply(reply(approved, token), REDEEM_BAD_KEY, "reject one-time code as key");
  rejectsReply(reply(approved, "fm_short"), REDEEM_BAD_KEY, "reject short key");
  rejectsReply(reply(approved, key + " x"), REDEEM_BAD_KEY, "reject key with bad char");
  rejectsReply(reply("https://evil.example.com/functions/v1/ingest-fieldmesh", key),
               REDEEM_ENDPOINT_NOT_ALLOWED, "reject reply naming other host");
  rejectsReply(reply("http://unhzttnuayrgqrzeqetz.supabase.co/functions/v1/ingest-fieldmesh", key),
               REDEEM_ENDPOINT_NOT_ALLOWED, "reject reply naming non-https");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
