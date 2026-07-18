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

// Parse the HTTP bytes carried inside one or more A7670G +CCHRECV frames.
// Transport completeness is deliberately separate from status: a truncated
// HTTP 200 is not a successful response.
HttpResponseParseResult parseHttpResponseBytes(const String& httpBytes,
                                               bool peerClosed);
