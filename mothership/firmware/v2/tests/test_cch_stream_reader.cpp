// On-device assertion test for the CCH streaming frame reader.
//
// Exercises the binary-safe framing state machine deterministically with
// synthetic +CCHRECV frames — no modem, SIM, or network needed. This is the
// trickiest part of the OTA download path, so it is proven in isolation here
// before the on-air modem GET is trusted.
//
//   pio run -e mothership-v2-test-cch-stream -t upload && pio device monitor
//
#include <Arduino.h>
#include <vector>
#include "comms/cch_stream_reader.h"
#include "comms/http_response_parser.h"

static int failures = 0;
static void ok(bool c, const char* label) {
  if (c) Serial.printf("ok   %s\n", label);
  else { Serial.printf("FAIL %s\n", label); failures++; }
}

// Body sink that appends received bytes into a global vector so we can compare
// against the expected image exactly (binary-safe, incl. embedded NULs).
static std::vector<uint8_t> g_body;
static bool g_abortAfter = false;
static size_t g_abortAt = 0;
static bool sink(const uint8_t* d, size_t n, void*) {
  for (size_t i = 0; i < n; ++i) {
    if (g_abortAfter && g_body.size() >= g_abortAt) return false;
    g_body.push_back(d[i]);
  }
  return true;
}

// Wrap payload bytes into one "+CCHRECV: DATA,0,<len>\r\n<payload>" frame.
static void appendFrame(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
  char hdr[40];
  int hn = snprintf(hdr, sizeof(hdr), "\r\n+CCHRECV: DATA,0,%u\r\n", (unsigned)n);
  for (int i = 0; i < hn; ++i) out.push_back((uint8_t)hdr[i]);
  for (size_t i = 0; i < n; ++i) out.push_back(p[i]);
}

static void appendStr(std::vector<uint8_t>& out, const char* s) {
  for (const char* p = s; *p; ++p) out.push_back((uint8_t)*p);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[TEST] CCH stream reader");

  // Build a synthetic HTTP response: headers + a binary body containing NULs
  // and bytes that spell the frame marker, split arbitrarily across frames.
  const char* headers =
      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
      "Content-Length: 268\r\nConnection: close\r\n\r\n";
  std::vector<uint8_t> body;
  for (int i = 0; i < 268; ++i) body.push_back((uint8_t)(i & 0xFF));  // includes 0x00
  // Sprinkle marker-like bytes into the body to prove they don't confuse parsing.
  memcpy(&body[10], "+CCHRECV: DATA,0,", 17);

  std::vector<uint8_t> full;               // headers + body as one HTTP entity
  appendStr(full, headers);
  for (uint8_t b : body) full.push_back(b);

  // Frame it: first frame carries all headers + first 30 body-region bytes,
  // then several small frames, to force header/body split mid-frame and
  // multi-frame bodies.
  std::vector<uint8_t> uart;
  size_t off = 0, first = strlen(headers) + 30;
  appendFrame(uart, full.data(), first);
  off = first;
  while (off < full.size()) {
    size_t n = min((size_t)37, full.size() - off);   // odd size, not frame-aligned
    appendFrame(uart, full.data() + off, n);
    off += n;
  }
  appendStr(uart, "\r\n+CCH_PEER_CLOSED\r\n");

  // Each reader is ~8 KB (heap-allocated); keep only one live at a time by
  // scoping each test so its head buffer never lands on the loopTask stack.

  // --- Test 1: feed the whole UART stream in one go ---
  { g_body.clear(); g_abortAfter = false;
    CchStreamReader* r = new CchStreamReader();
    r->feed(uart.data(), uart.size(), sink, nullptr);
    ok(r->headComplete(), "headers detected");
    ok(r->peerClosed(), "peer-closed detected");
    ok(r->bodyDelivered() == 268, "268 body bytes delivered");
    ok(g_body.size() == 268, "sink got 268 bytes");
    ok(g_body == body, "body bytes match exactly (binary-safe, NULs + marker text)");
    String hs; for (size_t k=0;k<r->headLen();++k) hs += r->head()[k];
    HttpResponseHead h1 = parseHttpResponseHead(hs);
    ok(h1.statusCode == 200, "status 200 parsed from head");
    ok(h1.contentLength == 268, "content-length 268 parsed");
    delete r; }

  // --- Test 2: feed one byte at a time (worst-case fragmentation) ---
  { g_body.clear();
    CchStreamReader* r = new CchStreamReader();
    for (size_t i = 0; i < uart.size(); ++i) r->feed(&uart[i], 1, sink, nullptr);
    ok(r->bodyDelivered() == 268 && g_body == body, "byte-at-a-time yields identical body");
    delete r; }

  // --- Test 3: sink abort partway stops delivery ---
  { g_body.clear(); g_abortAfter = true; g_abortAt = 100;
    CchStreamReader* r = new CchStreamReader();
    bool cont = r->feed(uart.data(), uart.size(), sink, nullptr);
    ok(!cont && r->aborted(), "feed reports abort");
    ok(g_body.size() <= 101, "delivery stopped near abort point");
    g_abortAfter = false;
    delete r; }

  // --- Test 4: truncated stream (body ends early) ---
  { g_body.clear();
    std::vector<uint8_t> truncated;
    appendFrame(truncated, full.data(), strlen(headers) + 100);   // only 100 body bytes
    CchStreamReader* r = new CchStreamReader();
    r->feed(truncated.data(), truncated.size(), sink, nullptr);
    ok(r->headComplete(), "trunc: headers seen");
    ok(r->bodyDelivered() == 100, "trunc: only 100 body bytes");
    ok(!r->peerClosed(), "trunc: no peer close");
    // caller detects incompleteness: delivered(100) < content-length(268)
    delete r; }

  // --- Test 5: header block larger than the reader's head buffer (kMaxHead) ---
  // Regression guard. A real Supabase Storage response can carry CORS +
  // Cache-Control + ETag + security headers. Before the rolling-tail fix, once
  // head_ filled to kMaxHead the last-4-written bytes froze, so the \r\n\r\n
  // terminator could never be detected — the reader stayed stuck "in headers"
  // forever and every body byte was silently dropped. Prove a header block
  // well past kMaxHead (8192) is still detected and the body flows.
  { g_body.clear(); g_abortAfter = false;
    String bigHeaders = "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n";
    bigHeaders += "X-Padding: ";
    while (bigHeaders.length() < 9000) bigHeaders += "abcdefghij";  // > kMaxHead
    bigHeaders += "\r\n\r\n";
    std::vector<uint8_t> bigFull;
    for (size_t k = 0; k < bigHeaders.length(); ++k) bigFull.push_back((uint8_t)bigHeaders[k]);
    std::vector<uint8_t> bigBody;
    for (int i = 0; i < 64; ++i) bigBody.push_back((uint8_t)(i & 0xFF));
    for (uint8_t b : bigBody) bigFull.push_back(b);
    std::vector<uint8_t> bigUart;
    appendFrame(bigUart, bigFull.data(), bigFull.size());
    appendStr(bigUart, "\r\n+CCH_PEER_CLOSED\r\n");
    CchStreamReader* r = new CchStreamReader();
    r->feed(bigUart.data(), bigUart.size(), sink, nullptr);
    ok(r->headComplete(), "big-head: terminator detected past kMaxHead");
    ok(r->bodyDelivered() == 64, "big-head: 64 body bytes delivered (not swallowed as head)");
    ok(g_body.size() == 64 && g_body == bigBody, "big-head: body bytes match exactly");
    delete r; }

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
