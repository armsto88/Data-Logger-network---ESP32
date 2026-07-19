#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// ===== Binary-safe incremental reader for the A7670G CCH auto-receive stream =====
//
// The modem, in auto-receive mode (AT+CCHSET=0,0), pushes the HTTPS response as
// a sequence of "+CCHRECV: DATA,0,<len>\r\n<payload>" frames interleaved with
// plain-ASCII URC lines. This reader pulls the payload out frame-by-frame and
// forwards body bytes to a sink callback, WITHOUT ever buffering the whole
// response — so a ~1 MB firmware image never lives in RAM.
//
// Binary-safety: between frames the modem emits only ASCII URC text, so
// scanning there for the frame marker and the peer-closed URC is safe; frame
// payloads are consumed strictly by the declared byte count, so an image byte
// of 0x00 (or bytes that happen to spell the marker) never confuse parsing.
//
// This is deliberately free of any Arduino/Serial dependency so the framing
// logic — the trickiest part of the download path — can be unit-tested with
// synthetic frames on-device (see tests/test_cch_stream_reader.cpp) without a
// modem, a SIM, or a network.

class CchStreamReader {
 public:
  // Returns false to abort the transfer (e.g. the OTA install rejected a chunk).
  using BodySink = bool (*)(const uint8_t* data, size_t len, void* ctx);

  // Feed a block of freshly-read UART bytes. Body bytes are forwarded to `sink`
  // in contiguous runs. Returns false if the sink asked to abort (also latches
  // aborted()).
  bool feed(const uint8_t* data, size_t len, BodySink sink, void* ctx) {
    // Function-local statics: single shared copy across TUs (inline function),
    // no out-of-line definition needed, no ODR clash.
    static const char kMarker[] = "+CCHRECV: DATA,0,";
    static const char kClosed[] = "+CCH_PEER_CLOSED";
    for (size_t i = 0; i < len; ++i) {
      const uint8_t b = data[i];
      switch (mode_) {
        case SEEK: {
          mkPos_ = (b == kMarker[mkPos_]) ? mkPos_ + 1 : (b == kMarker[0] ? 1 : 0);
          clPos_ = (b == kClosed[clPos_]) ? clPos_ + 1 : (b == kClosed[0] ? 1 : 0);
          if (kClosed[clPos_] == '\0') { peerClosed_ = true; clPos_ = 0; }
          if (kMarker[mkPos_] == '\0') { mode_ = LEN; mkPos_ = 0; lenLen_ = 0; }
          break;
        }
        case LEN: {
          if (b >= '0' && b <= '9') {
            if (lenLen_ < sizeof(lenBuf_) - 1) lenBuf_[lenLen_++] = (char)b;
          } else if (b == '\n') {
            lenBuf_[lenLen_] = '\0';
            frameLen_ = (uint32_t)strtoul(lenBuf_, nullptr, 10);
            frameGot_ = 0;
            mode_ = frameLen_ ? PAYLOAD : SEEK;
          }  // ignore '\r' / spaces
          break;
        }
        case PAYLOAD: {
          bool isBody = false;
          onPayloadByte(b, isBody);
          if (isBody) {
            const uint32_t remaining = frameLen_ - frameGot_;   // includes this byte
            size_t run = (size_t)remaining;
            if (run > len - i) run = len - i;                   // only what's in this block
            if (sink && !sink(&data[i], run, ctx)) { aborted_ = true; }
            bodyDelivered_ += run;
            frameGot_ += run;
            i += run - 1;                                       // caller ++i lands past run
          } else {
            frameGot_++;
          }
          if (frameGot_ >= frameLen_) mode_ = SEEK;
          break;
        }
      }
      if (aborted_) return false;
    }
    return true;
  }

  bool     headComplete() const { return headComplete_; }
  const char* head()      const { return head_; }
  size_t   headLen()      const { return headLen_; }
  bool     peerClosed()   const { return peerClosed_; }
  bool     aborted()      const { return aborted_; }
  uint32_t bodyDelivered() const { return bodyDelivered_; }

 private:
  enum Mode { SEEK, LEN, PAYLOAD };

  void onPayloadByte(uint8_t b, bool& isBody) {
    isBody = false;
    if (headComplete_) { isBody = true; return; }
    if (headLen_ < kMaxHead) head_[headLen_++] = (char)b;
    // Roll the 4-byte terminator window over EVERY header byte seen, not just
    // the ones that fit in head_. If the scan looked at the last 4 *written*
    // bytes instead, then once head_ fills to kMaxHead those bytes freeze and
    // \r\n\r\n could never be (re)detected — wedging the reader in "still in
    // headers" forever, so every body byte gets misclassified as head and
    // silently dropped. Supabase Storage responses can carry CORS +
    // Cache-Control + ETag + security headers, so head_ is generous (kMaxHead),
    // but terminator detection must not depend on the byte fitting in it.
    tail_[0] = tail_[1]; tail_[1] = tail_[2]; tail_[2] = tail_[3]; tail_[3] = (char)b;
    if (tail_[0] == '\r' && tail_[1] == '\n' &&
        tail_[2] == '\r' && tail_[3] == '\n') {
      headComplete_ = true;
    }
  }

  // Generous cap so realistic response headers are captured in full for the
  // status/Content-Length parse. If a header block ever exceeds this, head_ is
  // truncated (missing its \r\n\r\n), so parseHttpResponseHead reports
  // "headers incomplete" and the transfer fails cleanly as retryable — never
  // the old silent infinite-header wedge (guaranteed by the rolling tail_).
  static constexpr size_t kMaxHead = 8192;

  Mode     mode_ = SEEK;
  size_t   mkPos_ = 0;
  size_t   clPos_ = 0;
  uint32_t frameLen_ = 0;
  uint32_t frameGot_ = 0;
  char     lenBuf_[12] = {0};
  size_t   lenLen_ = 0;
  bool     peerClosed_ = false;
  bool     aborted_ = false;
  uint32_t bodyDelivered_ = 0;

  char     head_[kMaxHead];
  size_t   headLen_ = 0;
  bool     headComplete_ = false;
  char     tail_[4] = {0, 0, 0, 0};   // rolling \r\n\r\n scan, independent of head_ capacity
};
