#include <Arduino.h>

#include "comms/http_response_parser.h"

namespace {
int passed = 0;
int failed = 0;

void check(const char* name, bool ok) {
  Serial.printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}

void runSuite() {
  Serial.println("\n--- HTTP response completeness suite ---");
  const String json = "{\"controlProtocolVersion\":2,\"nextCursor\":1,\"commands\":[]}";
  String fixed = "+CCHRECV: DATA,0,123\r\nHTTP/1.1 200 OK\r\n";
  fixed += "Content-Type: application/json\r\nContent-Length: ";
  fixed += String(json.length());
  fixed += "\r\n\r\n" + json + "\r\n+CCH_PEER_CLOSED: 0\r\n";
  HttpResponseParseResult r = parseHttpResponseBytes(fixed, true);
  check("Content-Length body complete", r.statusCode == 200 &&
        r.bodyComplete && r.body == json);
  String extracted;
  String frameError;
  uint32_t declared = 0;
  String framed = "+CCHRECV: DATA,0," + String(fixed.length()) + "\r\n" +
                  fixed + "\r\n+CCH_PEER_CLOSED: 0\r\n";
  check("CCH frame excludes following peer-close URC",
        extractA7670CchPayload(framed, extracted, declared, frameError) &&
        extracted == fixed && declared == fixed.length());
  framed = "+CCHRECV: DATA,0,10\r\nshort";
  check("truncated CCH frame rejected",
        !extractA7670CchPayload(framed, extracted, declared, frameError));

  const String truncated = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort";
  r = parseHttpResponseBytes(truncated, true);
  check("truncated HTTP 200 rejected", r.statusCode == 200 && !r.bodyComplete);

  const String chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
  r = parseHttpResponseBytes(chunked, false);
  check("chunked body decoded", r.bodyComplete && r.chunked &&
        r.body == "hello world");

  const String splitChunk = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "5\r\nhel";
  r = parseHttpResponseBytes(splitChunk, true);
  check("split chunk rejected until complete", !r.bodyComplete);

  const String closeDelimited = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{}";
  check("close-delimited waits for peer close",
        !parseHttpResponseBytes(closeDelimited, false).bodyComplete &&
        parseHttpResponseBytes(closeDelimited, true).bodyComplete);
  Serial.printf("HTTP parser result: %d passed, %d failed\n", passed, failed);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  runSuite();
}

void loop() { delay(1000); }
