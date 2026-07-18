// On-device assertion tests for the canonical FieldHub hardware identity and
// the FM1 connection-key provisioning payload parser.
//
// Covers: MAC byte formatting, hardware-code six-hex suffix, non-secret
// registration URI shape, /api/identity field wiring, and — most importantly —
// that malformed / unversioned / disallowed-endpoint provisioning payloads are
// rejected without ever yielding credentials.
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

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
