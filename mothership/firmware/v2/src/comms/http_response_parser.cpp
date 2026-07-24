#include "comms/http_response_parser.h"

#include <stdlib.h>
#include <string.h>

namespace {

bool parseChunkSize(const String& line, uint32_t& out) {
  String value = line;
  const int extension = value.indexOf(';');
  if (extension >= 0) value.remove(extension);
  value.trim();
  if (value.length() == 0 || value.length() > 8) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    uint8_t digit = 0;
    if (c >= '0' && c <= '9') digit = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') digit = static_cast<uint8_t>(10 + c - 'a');
    else if (c >= 'A' && c <= 'F') digit = static_cast<uint8_t>(10 + c - 'A');
    else return false;
    if (parsed > (UINT32_MAX - digit) / 16U) return false;
    parsed = parsed * 16U + digit;
  }
  out = parsed;
  return true;
}

bool decodeChunked(const String& encoded, String& decoded, String& error) {
  decoded = "";
  decoded.reserve(encoded.length());
  size_t cursor = 0;
  while (cursor < encoded.length()) {
    const int lineEnd = encoded.indexOf('\n', cursor);
    if (lineEnd < 0) return false;
    uint32_t chunkBytes = 0;
    String sizeLine = encoded.substring(cursor, lineEnd);
    if (sizeLine.endsWith("\r")) sizeLine.remove(sizeLine.length() - 1);
    if (!parseChunkSize(sizeLine, chunkBytes)) {
      error = "invalid chunk size";
      return false;
    }
    cursor = static_cast<size_t>(lineEnd + 1);
    if (chunkBytes == 0) {
      if (cursor + 2 <= encoded.length() &&
          encoded.substring(cursor, cursor + 2) == "\r\n") return true;
      if (cursor < encoded.length() && encoded[cursor] == '\n') return true;
      return encoded.indexOf("\r\n\r\n", cursor) >= 0 ||
             encoded.indexOf("\n\n", cursor) >= 0;
    }
    if (chunkBytes > encoded.length() - cursor) return false;
    decoded += encoded.substring(cursor, cursor + chunkBytes);
    cursor += chunkBytes;
    if (cursor >= encoded.length()) return false;
    if (cursor + 2 <= encoded.length() &&
        encoded.substring(cursor, cursor + 2) == "\r\n") {
      cursor += 2;
    } else if (encoded[cursor] == '\n') {
      cursor += 1;
    } else {
      error = "missing chunk terminator";
      return false;
    }
  }
  return false;
}

}  // namespace

bool extractA7670CchPayload(const String& uartBytes, String& httpBytes,
                           uint32_t& declaredBytes, String& error) {
  static constexpr const char* kMarker = "+CCHRECV: DATA,0,";
  httpBytes = "";
  declaredBytes = 0;
  error = "";
  size_t cursor = 0;
  bool found = false;
  while (cursor < uartBytes.length()) {
    const int marker = uartBytes.indexOf(kMarker, cursor);
    if (marker < 0) break;
    found = true;
    const int lineEnd = uartBytes.indexOf('\n', marker);
    if (lineEnd < 0) {
      error = "CCHRECV header incomplete";
      return false;
    }
    String lengthText = uartBytes.substring(
        marker + static_cast<int>(strlen(kMarker)), lineEnd);
    lengthText.trim();
    if (lengthText.length() == 0) {
      error = "CCHRECV length missing";
      return false;
    }
    for (size_t i = 0; i < lengthText.length(); ++i) {
      if (lengthText[i] < '0' || lengthText[i] > '9') {
        error = "CCHRECV length invalid";
        return false;
      }
    }
    const uint64_t frameLength = strtoull(lengthText.c_str(), nullptr, 10);
    if (frameLength > 16 * 1024 ||
        declaredBytes + frameLength > 16 * 1024) {
      error = "CCHRECV payload too large";
      return false;
    }
    const size_t payloadStart = static_cast<size_t>(lineEnd + 1);
    if (frameLength > uartBytes.length() - payloadStart) {
      error = "CCHRECV payload incomplete";
      return false;
    }
    httpBytes += uartBytes.substring(
        payloadStart, payloadStart + static_cast<size_t>(frameLength));
    declaredBytes += static_cast<uint32_t>(frameLength);
    cursor = payloadStart + static_cast<size_t>(frameLength);
  }
  if (!found) {
    error = "CCHRECV frame missing";
    return false;
  }
  return true;
}

HttpResponseHead parseHttpResponseHead(const String& httpBytes) {
  HttpResponseHead out{};
  const int statusStart = httpBytes.indexOf("HTTP/1.");
  if (statusStart < 0) {
    out.error = "HTTP status line missing";
    return out;
  }
  const int statusSpace = httpBytes.indexOf(' ', statusStart);
  const int statusEnd = statusSpace >= 0
      ? httpBytes.indexOf(' ', statusSpace + 1) : -1;
  if (statusSpace < 0 || statusEnd < 0) {
    out.error = "HTTP status line incomplete";
    return out;
  }
  const String statusText = httpBytes.substring(statusSpace + 1, statusEnd);
  if (statusText.length() != 3) {
    out.error = "HTTP status code invalid";
    return out;
  }
  out.statusCode = statusText.toInt();
  if (out.statusCode < 100 || out.statusCode > 599) {
    out.statusCode = -1;
    out.error = "HTTP status code invalid";
    return out;
  }

  const int headerEnd = httpBytes.indexOf("\r\n\r\n", statusStart);
  if (headerEnd < 0) {
    out.error = "HTTP headers incomplete";
    return out;
  }
  out.headersComplete = true;
  out.bodyStart = static_cast<size_t>(headerEnd + 4);
  String headers = httpBytes.substring(statusStart, headerEnd);
  headers.toLowerCase();
  out.chunked = headers.indexOf("transfer-encoding: chunked") >= 0;

  const int contentHeader = headers.indexOf("content-length:");
  if (contentHeader >= 0) {
    int valueStart = contentHeader + static_cast<int>(strlen("content-length:"));
    int valueEnd = headers.indexOf("\r\n", valueStart);
    if (valueEnd < 0) valueEnd = headers.length();
    String value = headers.substring(valueStart, valueEnd);
    value.trim();
    if (value.length() == 0) {
      out.error = "Content-Length invalid";
      return out;
    }
    for (size_t i = 0; i < value.length(); ++i) {
      if (value[i] < '0' || value[i] > '9') {
        out.error = "Content-Length invalid";
        return out;
      }
    }
    const uint64_t parsed = strtoull(value.c_str(), nullptr, 10);
    if (parsed > INT32_MAX) {
      out.error = "Content-Length too large";
      return out;
    }
    out.contentLength = static_cast<int32_t>(parsed);
  }
  return out;
}

HttpResponseParseResult parseHttpResponseBytes(const String& httpBytes,
                                               bool peerClosed) {
  HttpResponseParseResult out{};
  const HttpResponseHead head = parseHttpResponseHead(httpBytes);
  out.statusCode      = head.statusCode;
  out.headersComplete = head.headersComplete;
  out.chunked         = head.chunked;
  out.contentLength   = head.contentLength;
  out.error           = head.error;
  // Any head-level failure (missing/incomplete status line, incomplete
  // headers, bad Content-Length) short-circuits with the same error the
  // monolithic parser produced.
  if (!head.headersComplete || head.error.length() > 0) return out;

  const String encodedBody = httpBytes.substring(head.bodyStart);
  if (out.chunked) {
    out.bodyComplete = decodeChunked(encodedBody, out.body, out.error);
    if (!out.bodyComplete && out.error.length() == 0)
      out.error = "chunked body incomplete";
    return out;
  }
  if (out.contentLength >= 0) {
    if (encodedBody.length() < static_cast<size_t>(out.contentLength)) {
      out.error = "Content-Length body incomplete";
      return out;
    }
    out.body = encodedBody.substring(0, static_cast<size_t>(out.contentLength));
    out.bodyComplete = true;
    return out;
  }

  if (peerClosed) {
    out.body = encodedBody;
    out.bodyComplete = true;
  } else {
    out.error = "close-delimited body still open";
  }
  return out;
}
