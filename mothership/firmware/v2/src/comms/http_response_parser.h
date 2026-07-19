#pragma once

#include <Arduino.h>

struct HttpResponseParseResult {
  int statusCode = -1;
  bool headersComplete = false;
  bool bodyComplete = false;
  bool chunked = false;
  int32_t contentLength = -1;
  String body;
  String error;
};

// Just the response head: status line + framing headers, stopping at the
// blank line that ends the headers. `bodyStart` is the offset of the first
// body byte within the input (valid only when headersComplete). This carries
// no body, so a streaming reader (a large image download) can parse framing
// from the first bytes without ever buffering the whole response.
struct HttpResponseHead {
  int statusCode = -1;
  bool headersComplete = false;
  bool chunked = false;
  int32_t contentLength = -1;
  size_t bodyStart = 0;
  String error;
};

// Parse the response head. Behaviour matches the header-parsing half of
// parseHttpResponseBytes() exactly (same checks, same error strings).
HttpResponseHead parseHttpResponseHead(const String& httpBytes);

// Parse the HTTP bytes carried inside one or more A7670G +CCHRECV frames.
// Transport completeness is deliberately separate from status: a truncated
// HTTP 200 is not a successful response.
HttpResponseParseResult parseHttpResponseBytes(const String& httpBytes,
                                               bool peerClosed);

// Extract exactly the byte count declared by every
// +CCHRECV: DATA,0,<length> frame. UART control URCs that follow the payload
// are excluded, and an incomplete declared frame is rejected.
bool extractA7670CchPayload(const String& uartBytes, String& httpBytes,
                           uint32_t& declaredBytes, String& error);
