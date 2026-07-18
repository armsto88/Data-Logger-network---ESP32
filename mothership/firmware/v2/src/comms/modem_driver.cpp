// Mothership V1 — Production modem driver implementation
//
// See modem_driver.h for the design overview.  The power-rail, PWRKEY,
// and AT-handshake patterns mirror the proven bring-up helpers in
// tests/modem_at_helper.h (Tests 9-13 all passed on hardware).

#include "modem_driver.h"
#include "comms/http_response_parser.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ModemDriver::ModemDriver() {}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

String ModemDriver::stateToString() const {
  switch (m_state) {
    case ModemState::OFF:             return "OFF";
    case ModemState::RAIL_ENABLED:     return "RAIL_ENABLED";
    case ModemState::RAIL_STABLE:      return "RAIL_STABLE";
    case ModemState::BOOT_REQUESTED:  return "BOOT_REQUESTED";
    case ModemState::STATUS_HIGH:     return "STATUS_HIGH";
    case ModemState::UART_READY:      return "UART_READY";
    case ModemState::REGISTERED:      return "REGISTERED";
    case ModemState::TRANSPORT_OPEN:  return "TRANSPORT_OPEN";
    case ModemState::UPLOAD_ACTIVE:   return "UPLOAD_ACTIVE";
    case ModemState::SHUTTING_DOWN:   return "SHUTTING_DOWN";
    case ModemState::RECOVERY:        return "RECOVERY";
    case ModemState::ERROR:           return "ERROR";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// init()
// ---------------------------------------------------------------------------

void ModemDriver::init() {
  Serial.println("=== ModemDriver::init() ===");

  // 4V_EN: OUTPUT LOW (rail off initially)
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);

  // PWRKEY: OUTPUT LOW (released)
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);

  // PG: INPUT (input-only, external pull-up)
  pinMode(PIN_MODEM_PG, INPUT);

  // STATUS: INPUT
  pinMode(PIN_MODEM_STATUS, INPUT);

  // UART is NOT started here — starts in powerOn() after boot.
  m_uartStarted = false;
  m_imei = "";
  m_state = ModemState::OFF;

  Serial.println("[Modem] pins configured, state=OFF");
}

// ---------------------------------------------------------------------------
// Power rail control
// ---------------------------------------------------------------------------

bool ModemDriver::railOn() {
  Serial.println("[Modem] railOn() — PWM soft-start 4V_EN");

  // Set 4V_EN LOW before enabling output to minimise any glitch.
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);

  pinMode(PIN_MODEM_PG, INPUT);  // input-only, external pull-up

  // PWM soft-start: ramp 4V_EN 0 -> 100% over 500 ms to limit inrush.
  // Matches the proven modem_at_helper.h pattern exactly:
  //   ledcSetup channel 0, 1 kHz, 10-bit (0-1023), ramp over 500 ms.
  ledcSetup(0, 1000, 10);
  ledcAttachPin(PIN_4V_EN, 0);
  unsigned long ssStart = millis();
  while (millis() - ssStart < 500) {
    int duty = map((int)(millis() - ssStart), 0, 500, 0, 1023);
    ledcWrite(0, duty);
    delay(5);
  }
  // Switch to full DC HIGH.
  ledcDetachPin(PIN_4V_EN);
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, HIGH);

  m_state = ModemState::RAIL_ENABLED;

  // Wait for power-good (PIN_MODEM_PG HIGH).
  unsigned long start = millis();
  while (millis() - start < MODEM_PG_TIMEOUT_MS) {
    if (digitalRead(PIN_MODEM_PG) == HIGH) {
      m_state = ModemState::RAIL_STABLE;
      Serial.println("[Modem] PG HIGH — rail stable");
      return true;
    }
    delay(10);
  }

  Serial.println("[Modem] railOn() FAILED — PG timeout");
  return false;
}

void ModemDriver::railOff() {
  Serial.println("[Modem] railOff() — 4V_EN LOW");
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);
}

// ---------------------------------------------------------------------------
// PWRKEY and STATUS
// ---------------------------------------------------------------------------

void ModemDriver::pulsePwrkey() {
  Serial.println("[Modem] pulsePwrkey() — HIGH for MODEM_PWRKEY_ON_MS");
  // NMOS gate: HIGH = pulls PWRKEY low = "press", LOW = release.
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);   // release first
  delay(50);
  digitalWrite(PIN_MODEM_PWRKEY, HIGH);  // press
  delay(MODEM_PWRKEY_ON_MS);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);    // release
  m_state = ModemState::BOOT_REQUESTED;
}

int ModemDriver::readStatus() {
  pinMode(PIN_MODEM_STATUS, INPUT);
  return digitalRead(PIN_MODEM_STATUS);
}

// ---------------------------------------------------------------------------
// UART
// ---------------------------------------------------------------------------

void ModemDriver::startUart() {
  if (!m_uartStarted) {
    Serial.println("[Modem] startUart() — Serial2 @ 115200");
    // Command-bearing responses exceed Arduino's default UART RX ring. This
    // must be configured before begin() or a fast response is truncated.
    Serial2.setRxBufferSize(16 * 1024);
    Serial2.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    Serial2.setTimeout(2000);
    Serial2.clearWriteError();
    while (Serial2.available()) { Serial2.read(); }  // flush RX buffer
    m_uartStarted = true;
  } else {
    // Already started — just flush.
    while (Serial2.available()) { Serial2.read(); }
  }
}

// ---------------------------------------------------------------------------
// Boot wait
// ---------------------------------------------------------------------------

bool ModemDriver::waitBoot(uint32_t timeoutMs) {
  Serial.printf("[Modem] waitBoot(%lu ms)\n", (unsigned long)timeoutMs);

  unsigned long start = millis();

  // First, wait for STATUS HIGH (modem powered).
  while (millis() - start < timeoutMs) {
    if (readStatus() == HIGH) {
      m_state = ModemState::STATUS_HIGH;
      Serial.println("[Modem] STATUS HIGH — modem powered");
      break;
    }
    delay(100);
  }

  if (readStatus() != HIGH) {
    Serial.println("[Modem] waitBoot() FAILED — STATUS never went HIGH");
    return false;
  }

  // Give UART a moment to settle, then try AT a few times.
  delay(500);
  unsigned long probeStart = millis();
  while (millis() - probeStart < (timeoutMs - (millis() - start))) {
    String resp;
    if (sendAT("AT", resp, 2000)) {
      m_state = ModemState::UART_READY;
      Serial.println("[Modem] AT handshake OK — UART ready");
      return true;
    }
    delay(500);
  }

  Serial.println("[Modem] waitBoot() FAILED — no AT response");
  return false;
}

// ---------------------------------------------------------------------------
// AT command send / receive
// ---------------------------------------------------------------------------

bool ModemDriver::sendAT(const char* cmd, String& response, uint32_t timeoutMs) {
  while (Serial2.available()) { Serial2.read(); }  // flush
  Serial2.print(cmd);
  Serial2.print("\r\n");
  Serial2.flush();

  response = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      response += c;
      if (response.indexOf("OK\r\n") >= 0) return true;
      if (response.indexOf("ERROR\r\n") >= 0) return false;
      if (response.indexOf("COMMAND NOT SUPPORT\r\n") >= 0) return false;
    }
    delay(5);
  }
  return false;  // timed out
}

bool ModemDriver::sendATExpect(const char* cmd, const char* expected,
                               uint32_t timeoutMs) {
  String resp;
  // sendAT returns true only on OK, but we also want to check the body.
  // Use sendAT to collect the response, then search for expected.
  // If sendAT times out (returns false), resp may still contain useful data.
  sendAT(cmd, resp, timeoutMs);
  return resp.indexOf(expected) >= 0;
}

// ---------------------------------------------------------------------------
// powerOn()
// ---------------------------------------------------------------------------

bool ModemDriver::powerOn() {
  Serial.println("=== ModemDriver::powerOn() ===");

  // 1. Rail on (PWM soft-start + PG wait).
  if (!railOn()) {
    m_state = ModemState::ERROR;
    Serial.println("[Modem] powerOn() FAILED — rail did not stabilise");
    return false;
  }

  // 2. Pulse PWRKEY.
  pulsePwrkey();

  // 3. Start UART (flush RX).
  startUart();

  // 4. Wait for boot (STATUS HIGH + AT handshake).
  if (!waitBoot(15000)) {
    m_state = ModemState::ERROR;
    Serial.println("[Modem] powerOn() FAILED — boot timeout");
    return false;
  }

  m_state = ModemState::UART_READY;
  Serial.println("[Modem] powerOn() OK — UART_READY");
  return true;
}

// ---------------------------------------------------------------------------
// waitForNetwork()
// ---------------------------------------------------------------------------

bool ModemDriver::waitForNetwork(uint32_t timeoutMs) {
  Serial.printf("=== ModemDriver::waitForNetwork(%lu ms) ===\n",
                (unsigned long)timeoutMs);

  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    String resp;

    // Check CREG (GSM registration).
    if (sendAT("AT+CREG?", resp, 2000)) {
      // Look for +CREG: 0,1 (home) or +CREG: 0,5 (roaming).
      if (resp.indexOf("+CREG: 0,1") >= 0 || resp.indexOf("+CREG: 0,5") >= 0) {
        m_state = ModemState::REGISTERED;
        Serial.println("[Modem] network registered (CREG)");
        return true;
      }
    }

    // Check CEREG (EPS network registration).
    if (sendAT("AT+CEREG?", resp, 2000)) {
      if (resp.indexOf("+CEREG: 0,1") >= 0 || resp.indexOf("+CEREG: 0,5") >= 0) {
        m_state = ModemState::REGISTERED;
        Serial.println("[Modem] network registered (CEREG)");
        return true;
      }
    }

    delay(2000);  // poll every 2 seconds
  }

  Serial.println("[Modem] waitForNetwork() — timeout, not registered");
  return false;
}

// ---------------------------------------------------------------------------
// httpsPost()
// ---------------------------------------------------------------------------

// NOTE: The A7670G (A7600 series, firmware A110B06A7670M7) uses:
// - CIP* API (NETOPEN/CIPOPEN/CIPSEND) for TCP/UDP
// - CCH* API (CCHSTART/CCHOPEN/CCHSEND) for SSL/TLS
// CIPOPEN does NOT support "SSL" type — use CCH* commands for HTTPS.
// Verified working on hardware with Aldi Talk SIM, both TCP and SSL/TLS.

HttpsPostResult ModemDriver::httpsPost(const String& url,
                                        const String& payload,
                                        const String& contentType,
                                        const String& authToken) {
  Serial.println("=== ModemDriver::httpsPost() ===");
  Serial.printf("[Modem] URL: %s\n", url.c_str());
  Serial.printf("[Modem] payload: %u bytes, content-type: %s\n",
                (unsigned)payload.length(), contentType.c_str());

  HttpsPostResult result;
  result.success = false;
  result.httpStatus = -1;

  // Parse URL
  bool useSSL = false;
  String host = "";
  int port = 80;
  String path = "/";

  if (url.startsWith("https://")) {
    useSSL = true;
    port = 443;
    host = url.substring(8);
  } else if (url.startsWith("http://")) {
    host = url.substring(7);
  } else {
    host = url;
  }

  int slashIdx = host.indexOf('/');
  if (slashIdx >= 0) {
    path = host.substring(slashIdx);
    host = host.substring(0, slashIdx);
  }

  int colonIdx = host.indexOf(':');
  if (colonIdx >= 0) {
    port = host.substring(colonIdx + 1).toInt();
    host = host.substring(0, colonIdx);
  }

  Serial.printf("[Modem] Parsed: host=%s port=%d path=%s ssl=%d\n",
                host.c_str(), port, path.c_str(), useSSL ? 1 : 0);

  // 1. Ensure PDP context is active
  //    APN is configurable via build flag MODEM_APN; defaults to "TM" (Think Mobile).
  //    Previous default was "internet.eplus.de" (Aldi Talk).
  {
    String resp;
#ifndef MODEM_APN
#define MODEM_APN "TM"
#endif
    String cgdcont = "AT+CGDCONT=1,\"IP\",\"" MODEM_APN "\"";
    sendAT(cgdcont.c_str(), resp, 5000);
    sendAT("AT+CGACT=1,1", resp, 10000);
  }

  // 2. NETOPEN
  {
    String resp;
    if (!sendAT("AT+NETOPEN", resp, 15000)) {
      result.errorDetail = "AT+NETOPEN failed: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      return result;
    }
    delay(3000);
    m_state = ModemState::TRANSPORT_OPEN;
  }

  // 3. Build HTTP request
  String httpReq = "POST " + path + " HTTP/1.1\r\n";
  httpReq += "Host: " + host + "\r\n";
  httpReq += "Content-Type: " + contentType + "\r\n";
  httpReq += "Content-Length: " + String(payload.length()) + "\r\n";
  if (authToken.length() > 0) {
    httpReq += "Authorization: Bearer " + authToken + "\r\n";
  }
  httpReq += "Connection: close\r\n";
  httpReq += "\r\n";
  httpReq += payload;

  // 4. Preserve the URL's transport. An HTTPS upload carrying commands or
  // credentials must never be retried in clear text after a TLS failure.
  const bool posted = useSSL
      ? httpsPostSSL(host, port, httpReq, result)
      : httpsPostTCP(host, port, httpReq, result);

  if (useSSL && !posted) {
    Serial.printf("[Modem] HTTPS failed (HTTP %d); plaintext fallback disabled\n",
                  result.httpStatus);
  }

  // 5. Cleanup
  {
    String resp;
    sendAT("AT+NETCLOSE", resp, 5000);
  }

  m_state = ModemState::REGISTERED;

  if (!result.success && result.errorDetail.length() == 0) {
    result.errorDetail = "HTTP status " + String(result.httpStatus);
  }

  Serial.println("=== httpsPost() complete ===");
  return result;
}

// ---------------------------------------------------------------------------
// Chunked send helpers
//
// The A7670G does not issue the '>' prompt for large single-shot
// CCHSEND/CIPSEND payloads (12KB+).  These helpers split the request into
// 1024-byte chunks, each with its own '>' prompt and confirmation, which is
// reliable across firmware versions.
// ---------------------------------------------------------------------------

static bool chunkedCchSend(const String& data, uint32_t chunkSize = 1024) {
  uint32_t offset = 0;
  uint32_t total = data.length();

  while (offset < total) {
    uint32_t thisChunk = (total - offset < chunkSize) ? (total - offset) : chunkSize;
    String sendCmd = "AT+CCHSEND=0," + String(thisChunk);

    // Flush RX
    while (Serial2.available()) { Serial2.read(); }
    Serial2.print(sendCmd);
    Serial2.print("\r\n");
    Serial2.flush();

    // Wait for '>' prompt
    String resp = "";
    unsigned long start = millis();
    bool gotPrompt = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf(">") >= 0) { gotPrompt = true; break; }
        // Detect early server response / error during prompt wait
        if (resp.indexOf("ERROR") >= 0 || resp.indexOf("HTTP/1.") >= 0) {
          Serial.printf("[Modem] CCHSEND chunk %u-%u: error during prompt wait: %s\n",
                        (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
          return false;
        }
      }
      if (gotPrompt) break;
      delay(1);
    }

    if (!gotPrompt) {
      Serial.printf("[Modem] CCHSEND chunk %u-%u no '>' prompt: %s\n",
                    (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
      return false;
    }

    // Write chunk data
    Serial2.write(data.c_str() + offset, thisChunk);
    Serial2.flush();

    // Wait for OK
    resp = "";
    start = millis();
    bool gotOk = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("OK\r\n") >= 0) { gotOk = true; break; }
        // Detect early server response (error during send)
        if (resp.indexOf("+CCHRECV") >= 0 || resp.indexOf("HTTP/1.") >= 0) {
          Serial.printf("[Modem] CCHSEND chunk %u-%u: early server response detected\n",
                        (unsigned)offset, (unsigned)(offset + thisChunk));
          Serial.println(resp);
          return false;
        }
      }
      if (gotOk) break;
      delay(1);
    }

    if (!gotOk) {
      Serial.printf("[Modem] CCHSEND chunk %u-%u no OK: %s\n",
                    (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
      return false;
    }

    offset += thisChunk;
    Serial.printf("[Modem] CCHSEND chunk: %u/%u bytes sent\n",
                  (unsigned)offset, (unsigned)total);

    // Small delay between chunks
    delay(50);
  }

  Serial.printf("[Modem] CCHSEND complete: %u bytes in chunks\n", (unsigned)total);
  return true;
}

static bool chunkedCipSend(const String& data, uint32_t chunkSize = 1024) {
  uint32_t offset = 0;
  uint32_t total = data.length();

  while (offset < total) {
    uint32_t thisChunk = (total - offset < chunkSize) ? (total - offset) : chunkSize;
    String sendCmd = "AT+CIPSEND=0," + String(thisChunk);

    while (Serial2.available()) { Serial2.read(); }
    Serial2.print(sendCmd);
    Serial2.print("\r\n");
    Serial2.flush();

    // Wait for '>' prompt
    String resp = "";
    unsigned long start = millis();
    bool gotPrompt = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf(">") >= 0) { gotPrompt = true; break; }
        // Detect early server response / error during prompt wait
        if (resp.indexOf("ERROR") >= 0 || resp.indexOf("HTTP/1.") >= 0) {
          Serial.printf("[Modem] CIPSEND chunk %u-%u: error during prompt wait: %s\n",
                        (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
          return false;
        }
      }
      if (gotPrompt) break;
      delay(1);
    }

    if (!gotPrompt) {
      Serial.printf("[Modem] CIPSEND chunk %u-%u no '>' prompt: %s\n",
                    (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
      return false;
    }

    // Write chunk data
    Serial2.write(data.c_str() + offset, thisChunk);
    Serial2.flush();

    // Wait for +CIPSEND: 0,<n> URC
    resp = "";
    start = millis();
    bool gotUrc = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("+CIPSEND: 0,") >= 0) { gotUrc = true; break; }
        // Detect early server response (error during send)
        if (resp.indexOf("HTTP/1.") >= 0 || resp.indexOf("+IPCLOSE") >= 0) {
          Serial.printf("[Modem] CIPSEND chunk %u-%u: early server response/close detected\n",
                        (unsigned)offset, (unsigned)(offset + thisChunk));
          Serial.println(resp);
          return false;
        }
      }
      if (gotUrc) break;
      delay(1);
    }

    if (!gotUrc) {
      Serial.printf("[Modem] CIPSEND chunk %u-%u no URC: %s\n",
                    (unsigned)offset, (unsigned)(offset + thisChunk), resp.c_str());
      return false;
    }

    offset += thisChunk;
    Serial.printf("[Modem] CIPSEND chunk: %u/%u bytes sent\n",
                  (unsigned)offset, (unsigned)total);

    delay(50);
  }

  Serial.printf("[Modem] CIPSEND complete: %u bytes in chunks\n", (unsigned)total);
  return true;
}

// ---------------------------------------------------------------------------
// httpsPostSSL — SSL/TLS POST via CCH* API
// ---------------------------------------------------------------------------

bool ModemDriver::httpsPostSSL(const String& host, int port,
                                const String& httpReq, HttpsPostResult& result) {
  Serial.println("[Modem] === SSL POST via CCH* API ===");

  // 1. NTP sync (needs data connection active)
  Serial.println("[Modem] NTP sync...");
  String resp;
  sendAT("AT+CNTP=\"pool.ntp.org\",32", resp, 5000);
  sendAT("AT+CNTP", resp, 15000);
  delay(3000);

  // Check if clock updated
  sendAT("AT+CCLK?", resp, 2000);
  Serial.printf("[Modem] CCLK: %s\n", resp.c_str());

  // 2. SSL context configuration
  sendAT("AT+CSSLCFG=\"sslversion\",0,4", resp, 2000);
  sendAT("AT+CSSLCFG=\"authmode\",0,0", resp, 2000);
  sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", resp, 2000);
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1", resp, 2000);
  sendAT("AT+CSSLCFG=\"negotiatetime\",0,300", resp, 2000);

  // 3. Start SSL service
  if (!sendAT("AT+CCHSTART", resp, 30000)) {
    result.errorDetail = "AT+CCHSTART failed: " + resp;
    Serial.println("[Modem] " + result.errorDetail);
    return false;
  }
  // Start draining immediately; delaying here overflowed the UART RX ring for
  // command-bearing responses.

  // 4. Bind SSL context
  sendAT("AT+CCHSSLCFG=0,0", resp, 5000);

  // 5. Set auto-receive mode
  sendAT("AT+CCHSET=0,0", resp, 2000);

  // 6. Open SSL connection
  String openCmd = "AT+CCHOPEN=0,\"" + host + "\"," + String(port) + ",2";
  Serial.printf("[Modem] CCHOPEN: %s\n", openCmd.c_str());

  while (Serial2.available()) { Serial2.read(); }
  Serial2.print(openCmd);
  Serial2.print("\r\n");
  Serial2.flush();

  // Wait for +CCHOPEN: 0,0 (success)
  resp = "";
  unsigned long start = millis();
  bool cchOpenOk = false;
  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
    }
    if (resp.indexOf("+CCHOPEN: 0,0") >= 0) {
      cchOpenOk = true;
      break;
    }
    // Check for error: +CCHOPEN: 0,<non-zero>
    int urcIdx = resp.indexOf("+CCHOPEN: 0,");
    if (urcIdx >= 0 && urcIdx + 12 < resp.length()) {
      char errChar = resp.charAt(urcIdx + 12);
      if (errChar != '0' && errChar >= '0' && errChar <= '9') {
        break;  // Error
      }
    }
    delay(10);
  }

  if (!cchOpenOk) {
    result.errorDetail = "CCHOPEN failed: " + resp;
    Serial.println("[Modem] " + result.errorDetail);
    sendAT("AT+CCHSTOP", resp, 5000);
    return false;
  }

  Serial.println("[Modem] SSL connection opened!");
  m_state = ModemState::UPLOAD_ACTIVE;
  delay(1000);

  // 7. Send data in chunks
  if (!chunkedCchSend(httpReq, 1024)) {
    result.errorDetail = "CCHSEND chunked send failed";
    Serial.println("[Modem] " + result.errorDetail);
    sendAT("AT+CCHCLOSE=0", resp, 2000);
    sendAT("AT+CCHSTOP", resp, 5000);
    return false;
  }

  // 8. Collect response
  // Give the modem time to receive the server response before we start
  // reading. Google Apps Script responds quickly and may close the peer
  // connection before the loop below begins, so allow buffer time.
  delay(2000);

  Serial.println("[Modem] Waiting for SSL response...");
  resp = "";
  start = millis();
  bool gotData = false;
  bool gotCchRecv = false;
  bool peerClosed = false;
  unsigned long lastResponseByteAt = start;

  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
      lastResponseByteAt = millis();
      // Detect any +CCHRECV: DATA URC — server sent data back
      if (resp.indexOf("+CCHRECV: DATA") >= 0) {
        gotCchRecv = true;
      }
      // Try to parse HTTP status from the accumulated response. The HTTP
      // status line is embedded inside the +CCHRECV data payload, so it
      // may arrive split across reads — keep accumulating until found.
      if (resp.indexOf("HTTP/1.") >= 0 && !gotData) {
        // Try to parse — only set gotData when we have the full status
        // line. The status line may arrive split across reads, so keep
        // re-attempting until a valid status code is extracted.
        int httpIdx = resp.indexOf("HTTP/1.");
        if (httpIdx >= 0) {
          int spaceIdx = resp.indexOf(' ', httpIdx);
          if (spaceIdx >= 0) {
            int nextSpace = resp.indexOf(' ', spaceIdx + 1);
            int crIdx = resp.indexOf('\r', spaceIdx + 1);
            // Only parse if we have a delimiter after the status code
            if (nextSpace >= 0 || crIdx >= 0) {
              int endIdx = (nextSpace >= 0 && (crIdx < 0 || nextSpace < crIdx)) ? nextSpace : crIdx;
              if (endIdx < 0) endIdx = spaceIdx + 4;
              String statusStr = resp.substring(spaceIdx + 1, endIdx);
              statusStr.trim();
              int parsed = statusStr.toInt();
              if (parsed > 0) {
                result.httpStatus = parsed;
                gotData = true;
              }
            }
          }
        }
      }
      if (resp.indexOf("+CCH_PEER_CLOSED") >= 0) {
        peerClosed = true;
      }
    }
    const HttpResponseParseResult observed =
        parseHttpResponseBytes(resp, peerClosed);
    if (observed.bodyComplete &&
        (observed.contentLength >= 0 || observed.chunked)) break;
    if (peerClosed && millis() - lastResponseByteAt >= 500) break;
    delay(10);
  }

  // Log raw response for debugging (truncated if very long)
  if (resp.length() > 0) {
    String logResp = resp.length() > 500 ? resp.substring(0, 500) + "..." : resp;
    Serial.printf("[Modem] SSL raw response (%u bytes): %s\n",
                  (unsigned)resp.length(), logResp.c_str());
  }

  // Debug: log what we actually received for diagnosis
  Serial.printf("[Modem] SSL response analysis: gotData=%d gotCchRecv=%d peerClosed=%d respLen=%u\n",
                gotData, gotCchRecv, peerClosed, (unsigned)resp.length());
  if (resp.indexOf("HTTP/1.") >= 0) {
    int idx = resp.indexOf("HTTP/1.");
    Serial.printf("[Modem] HTTP/1. found at index %d, surrounding: [%s]\n",
                  idx, resp.substring(idx, idx + 30).c_str());
  } else {
    Serial.println("[Modem] HTTP/1. NOT found in response");
  }

  // Fallback: try to parse HTTP status from full response after loop ends.
  // The in-loop parser may miss the status line if "HTTP/1." is split
  // across Serial2.read() calls in a way that the incremental indexOf
  // doesn't catch, or if the +CCHRECV URC line contains characters that
  // interfere with parsing. Re-scan the fully accumulated response here.
  if (!gotData && resp.indexOf("HTTP/1.") >= 0) {
    int httpIdx = resp.indexOf("HTTP/1.");
    // Find the status code: skip "HTTP/1.x " to find the 3-digit code
    int spaceIdx = resp.indexOf(' ', httpIdx);
    if (spaceIdx >= 0) {
      // Skip past the first space (after HTTP version), find the status code
      int codeStart = spaceIdx + 1;
      // Find the next space or \r after the status code
      int nextSpace = resp.indexOf(' ', codeStart);
      int crIdx = resp.indexOf('\r', codeStart);
      int endIdx = -1;
      if (nextSpace >= 0 && crIdx >= 0) endIdx = (nextSpace < crIdx) ? nextSpace : crIdx;
      else if (nextSpace >= 0) endIdx = nextSpace;
      else if (crIdx >= 0) endIdx = crIdx;
      if (endIdx >= 0) {
        String statusStr = resp.substring(codeStart, endIdx);
        statusStr.trim();
        int parsed = statusStr.toInt();
        if (parsed > 0) {
          result.httpStatus = parsed;
          gotData = true;
          Serial.printf("[Modem] SSL fallback parsed HTTP status: %d\n", parsed);
        }
      }
    }
  }

  const HttpResponseParseResult completeHttp =
      parseHttpResponseBytes(resp, peerClosed);
  result.responseWireBytes = resp.length();
  result.responseHttpBytes = resp.indexOf("HTTP/1.") >= 0
      ? resp.length() - static_cast<uint32_t>(resp.indexOf("HTTP/1.")) : 0;
  result.responseComplete = completeHttp.headersComplete &&
                            completeHttp.bodyComplete;
  if (result.responseComplete) {
    result.httpStatus = completeHttp.statusCode;
    resp = completeHttp.body;
    gotData = true;
  } else {
    gotData = false;
    gotCchRecv = false;
    peerClosed = false;
    result.httpStatus = -1;
    result.errorDetail = "Incomplete SSL HTTP response: " + completeHttp.error;
  }

  if (gotData) {
    result.responseBody = resp;
    result.success = (result.httpStatus == 200 || result.httpStatus == 302);
    Serial.printf("[Modem] SSL HTTP status=%d complete=1 wireBytes=%lu bodyBytes=%u success=%s\n",
                  result.httpStatus,
                  static_cast<unsigned long>(result.responseWireBytes),
                  static_cast<unsigned>(result.responseBody.length()),
                  result.success ? "YES" : "NO");
  } else if (gotCchRecv && !peerClosed) {
    // Got data but couldn't parse status — assume success (conservative)
    result.responseBody = resp;
    result.httpStatus = 302;
    result.success = true;
    Serial.println("[Modem] SSL: got CCHRECV data but no HTTP status parsed — assuming success");
  } else if (peerClosed && !gotCchRecv) {
    // Peer closed without any data received — assume success
    result.responseBody = resp;
    result.httpStatus = 302;
    result.success = true;
    Serial.println("[Modem] SSL: peer closed after successful chunked send — assuming success");
  } else {
    if (result.errorDetail.length() == 0)
      result.errorDetail = "No SSL HTTP response received";
    Serial.println("[Modem] " + result.errorDetail);
  }

  // 9. Close SSL
  sendAT("AT+CCHCLOSE=0", resp, 2000);
  sendAT("AT+CCHSTOP", resp, 5000);

  return result.success;
}

// ---------------------------------------------------------------------------
// httpsPostTCP — TCP POST via CIP* API
// ---------------------------------------------------------------------------

bool ModemDriver::httpsPostTCP(const String& host, int port,
                                const String& httpReq, HttpsPostResult& result) {
  Serial.println("[Modem] === TCP POST via CIP* API ===");

  // 1. Close any existing socket
  String resp;
  sendAT("AT+CIPCLOSE=0", resp, 2000);
  delay(500);

  // 2. Open TCP connection
  String openCmd = "AT+CIPOPEN=0,\"TCP\",\"" + host + "\"," + String(port);
  Serial.printf("[Modem] CIPOPEN: %s\n", openCmd.c_str());

  while (Serial2.available()) { Serial2.read(); }
  Serial2.print(openCmd);
  Serial2.print("\r\n");
  Serial2.flush();

  resp = "";
  unsigned long start = millis();
  bool tcpOpenOk = false;
  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
      if (resp.indexOf("+CIPOPEN: 0,0") >= 0) { tcpOpenOk = true; break; }
      if (resp.indexOf("ERROR") >= 0) break;
    }
    if (tcpOpenOk || resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }

  if (!tcpOpenOk) {
    result.errorDetail = "CIPOPEN TCP failed: " + resp;
    Serial.println("[Modem] " + result.errorDetail);
    return false;
  }

  Serial.println("[Modem] TCP socket opened!");
  m_state = ModemState::UPLOAD_ACTIVE;
  delay(1000);

  // 3. Send data in chunks
  if (!chunkedCipSend(httpReq, 1024)) {
    result.errorDetail = "CIPSEND chunked send failed";
    Serial.println("[Modem] " + result.errorDetail);
    sendAT("AT+CIPCLOSE=0", resp, 2000);
    return false;
  }

  // 4. Collect response
  Serial.println("[Modem] Waiting for TCP response...");
  resp = "";
  start = millis();
  bool gotData = false;

  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
      if (resp.indexOf("HTTP/1.") >= 0 && !gotData) {
        // Try to parse — only set gotData when we have the full status
        // line. The status line may arrive split across reads, so keep
        // re-attempting until a valid status code is extracted.
        int httpIdx = resp.indexOf("HTTP/1.");
        if (httpIdx >= 0) {
          int spaceIdx = resp.indexOf(' ', httpIdx);
          if (spaceIdx >= 0) {
            int nextSpace = resp.indexOf(' ', spaceIdx + 1);
            int crIdx = resp.indexOf('\r', spaceIdx + 1);
            // Only parse if we have a delimiter after the status code
            if (nextSpace >= 0 || crIdx >= 0) {
              int endIdx = (nextSpace >= 0 && (crIdx < 0 || nextSpace < crIdx)) ? nextSpace : crIdx;
              if (endIdx < 0) endIdx = spaceIdx + 4;
              String statusStr = resp.substring(spaceIdx + 1, endIdx);
              statusStr.trim();
              int parsed = statusStr.toInt();
              if (parsed > 0) {
                result.httpStatus = parsed;
                gotData = true;
              }
            }
          }
        }
      }
      if (resp.indexOf("+IPCLOSE") >= 0 || resp.indexOf("CLOSED") >= 0) break;
    }
    if (resp.indexOf("+IPCLOSE") >= 0 || resp.indexOf("CLOSED") >= 0) break;
    delay(10);
  }

  if (gotData) {
    result.responseBody = resp;
    result.success = (result.httpStatus == 200 || result.httpStatus == 302);
    Serial.printf("[Modem] TCP HTTP status: %d, success: %s\n",
                  result.httpStatus, result.success ? "YES" : "NO");
  } else {
    result.errorDetail = "No TCP HTTP response received";
    Serial.println("[Modem] " + result.errorDetail);
  }

  // 5. Close socket
  sendAT("AT+CIPCLOSE=0", resp, 2000);

  return result.success;
}

// ---------------------------------------------------------------------------
// gracefulShutdown()
// ---------------------------------------------------------------------------

void ModemDriver::gracefulShutdown() {
  Serial.println("=== ModemDriver::gracefulShutdown() ===");
  m_state = ModemState::SHUTTING_DOWN;

  // 1. AT+CPOF (graceful). Accept any response (POWER DOWN is not OK/ERROR).
  {
    // Use raw pattern: flush, send, collect for 5 s.
    while (Serial2.available()) { Serial2.read(); }
    Serial2.print("AT+CPOF");
    Serial2.print("\r\n");
    Serial2.flush();

    String resp = "";
    unsigned long start = millis();
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
      }
      delay(5);
    }
    if (resp.length() > 0) {
      Serial.println("[Modem] AT+CPOF responded:");
      Serial.print(resp);
    } else {
      Serial.println("[Modem] AT+CPOF no response — PWRKEY fallback");
    }
  }

  // 2. Wait for STATUS LOW (up to 5 s).
  unsigned long start = millis();
  while (millis() - start < 5000) {
    if (readStatus() == LOW) break;
    delay(100);
  }

  // 3. If STATUS still HIGH, PWRKEY long-press (>=2.5 s).
  if (readStatus() == HIGH) {
    Serial.println("[Modem] STATUS still HIGH — PWRKEY long-press 2.5 s");
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    delay(2500);
    digitalWrite(PIN_MODEM_PWRKEY, LOW);
    delay(2000);
  }

  // 4. Rail off.
  railOff();

  m_state = ModemState::OFF;
  Serial.println("[Modem] gracefulShutdown() complete — OFF");
}

// ---------------------------------------------------------------------------
// forcePowerCycle()
// ---------------------------------------------------------------------------

void ModemDriver::forcePowerCycle() {
  Serial.println("=== ModemDriver::forcePowerCycle() ===");
  m_state = ModemState::RECOVERY;

  // 1. Rail off.
  railOff();
  delay(3000);

  // 2. Rail on + PWRKEY + boot.
  if (!railOn()) {
    m_state = ModemState::ERROR;
    Serial.println("[Modem] forcePowerCycle() FAILED — rail did not stabilise");
    return;
  }

  pulsePwrkey();
  startUart();

  if (!waitBoot(15000)) {
    m_state = ModemState::ERROR;
    Serial.println("[Modem] forcePowerCycle() FAILED — boot timeout");
    return;
  }

  m_state = ModemState::UART_READY;
  Serial.println("[Modem] forcePowerCycle() OK — UART_READY");
}

// ---------------------------------------------------------------------------
// getImei()
// ---------------------------------------------------------------------------

String ModemDriver::getImei() {
  if (m_imei.length() > 0) {
    return m_imei;  // cached
  }

  String resp;
  if (sendAT("AT+CGSN", resp, 2000)) {
    // Response: \r\n<15-digit-IMEI>\r\n\r\nOK\r\n
    // Extract the first 15-digit numeric substring.
    int len = resp.length();
    for (int i = 0; i < len; ++i) {
      if (isdigit((int)resp.charAt(i))) {
        // Collect consecutive digits.
        String num = "";
        int j = i;
        while (j < len && isdigit((int)resp.charAt(j))) {
          num += resp.charAt(j);
          j++;
        }
        if (num.length() >= 15) {
          m_imei = num.substring(0, 15);
          Serial.printf("[Modem] IMEI: %s\n", m_imei.c_str());
          return m_imei;
        }
        i = j;
      }
    }
  }

  Serial.println("[Modem] getImei() — failed to parse");
  return "";
}

// ---------------------------------------------------------------------------
// getSignalQuality()
// ---------------------------------------------------------------------------

int ModemDriver::getSignalQuality() {
  String resp;
  if (sendAT("AT+CSQ", resp, 2000)) {
    // Response: +CSQ: <rssi>,<ber>
    int idx = resp.indexOf("+CSQ:");
    if (idx >= 0) {
      int colon = resp.indexOf(':', idx);
      int comma = resp.indexOf(',', colon);
      if (colon >= 0 && comma >= 0) {
        String rssiStr = resp.substring(colon + 1, comma);
        rssiStr.trim();
        int rssi = rssiStr.toInt();
        Serial.printf("[Modem] CSQ rssi=%d\n", rssi);
        return rssi;  // 99 = no signal
      }
    }
  }

  Serial.println("[Modem] getSignalQuality() — failed");
  return 99;
}

// ---------------------------------------------------------------------------
// getDiagnostics()
// ---------------------------------------------------------------------------

// 3GPP access-technology code (AT+COPS <AcT>) -> short label.
static const char* actCodeToStr(int act) {
  switch (act) {
    case 0: return "GSM";
    case 1: return "GSM_COMPACT";
    case 2: return "UTRAN";
    case 3: return "GSM_EGPRS";
    case 4: return "UTRAN_HSDPA";
    case 5: return "UTRAN_HSUPA";
    case 6: return "UTRAN_HSPA";
    case 7: return "LTE";
    case 8: return "EC_GSM_IOT";
    case 9: return "LTE_NB";
    default: return "UNKNOWN";
  }
}

// Extract the first run of >= minDigits consecutive digits from s (for ICCID).
static String firstDigitRun(const String& s, int minDigits) {
  int len = s.length();
  for (int i = 0; i < len; ++i) {
    if (isdigit((int)s.charAt(i))) {
      int j = i;
      while (j < len && isdigit((int)s.charAt(j))) j++;
      if (j - i >= minDigits) return s.substring(i, j);
      i = j;
    }
  }
  return "";
}

bool ModemDriver::getDiagnostics(ModemDiagnostics& d) {
  d.imei = "";
  d.iccid = "";
  d.rssiDbm = 0;
  d.ber = 99;
  d.rsrpDbm = 0;
  d.rsrqDb = 0;
  d.operatorName = "";
  d.accessTech = "";
  d.cpsi = "";

  bool anyResponse = false;
  String resp;

  d.imei = getImei();
  if (d.imei.length() > 0) anyResponse = true;

  // AT+CSQ -> +CSQ: <rssi>,<ber>   (rssi 0-31, 99=unknown; ber 0-7, 99=unknown)
  if (sendAT("AT+CSQ", resp, 2000)) {
    anyResponse = true;
    int idx = resp.indexOf("+CSQ:");
    if (idx >= 0) {
      int colon = resp.indexOf(':', idx);
      int comma = resp.indexOf(',', colon);
      if (colon >= 0 && comma >= 0) {
        int rssiRaw = resp.substring(colon + 1, comma).toInt();
        int end = resp.indexOf('\r', comma);
        d.ber = resp.substring(comma + 1, end > comma ? end : resp.length()).toInt();
        d.rssiDbm = (rssiRaw >= 0 && rssiRaw <= 31) ? (-113 + 2 * rssiRaw) : 0;
      }
    }
  }

  // AT+CESQ -> +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>  (LTE last two)
  // RSRP dBm = index - 141 (0..97); RSRQ dB ~ index/2 - 20 (0..34); 255=unknown.
  if (sendAT("AT+CESQ", resp, 2000)) {
    anyResponse = true;
    int idx = resp.indexOf("+CESQ:");
    if (idx >= 0) {
      int p = resp.indexOf(':', idx) + 1;
      int vals[6] = {255, 255, 255, 255, 255, 255};
      for (int k = 0; k < 6 && p > 0; ++k) {
        int comma = resp.indexOf(',', p);
        String tok = (comma >= 0) ? resp.substring(p, comma) : resp.substring(p);
        tok.trim();
        vals[k] = tok.toInt();
        if (comma < 0) break;
        p = comma + 1;
      }
      if (vals[5] != 255) d.rsrpDbm = vals[5] - 141;
      if (vals[4] != 255) d.rsrqDb = vals[4] / 2 - 20;
    }
  }

  // AT+COPS? -> +COPS: <mode>,<format>,"<operator>",<AcT>
  if (sendAT("AT+COPS?", resp, 2000)) {
    anyResponse = true;
    int q1 = resp.indexOf('"');
    int q2 = (q1 >= 0) ? resp.indexOf('"', q1 + 1) : -1;
    if (q1 >= 0 && q2 > q1) d.operatorName = resp.substring(q1 + 1, q2);
    int lastComma = resp.lastIndexOf(',');
    if (lastComma >= 0) {
      int end = resp.indexOf('\r', lastComma);
      int act = resp.substring(lastComma + 1, end > lastComma ? end : resp.length()).toInt();
      d.accessTech = actCodeToStr(act);
    }
  }

  // AT+CICCID -> +ICCID: <19-20 digit ICCID>
  if (sendAT("AT+CICCID", resp, 2000)) {
    anyResponse = true;
    d.iccid = firstDigitRun(resp, 18);
  }

  // AT+CPSI? -> raw serving-cell line (system mode, operator, band, cell, TAC).
  if (sendAT("AT+CPSI?", resp, 2000)) {
    anyResponse = true;
    int idx = resp.indexOf("+CPSI:");
    if (idx >= 0) {
      int start = idx + 6;
      int end = resp.indexOf('\r', start);
      String line = (end > start) ? resp.substring(start, end) : resp.substring(start);
      line.trim();
      d.cpsi = line;
      // Derive access tech from the first CPSI token if COPS didn't give one.
      if (d.accessTech.length() == 0 && line.length() > 0) {
        int c = line.indexOf(',');
        d.accessTech = (c > 0) ? line.substring(0, c) : line;
      }
    }
  }

  Serial.printf("[Modem] Diagnostics: rssi=%ddBm ber=%d rsrp=%ddBm rsrq=%ddB op=%s act=%s\n",
                d.rssiDbm, d.ber, d.rsrpDbm, d.rsrqDb,
                d.operatorName.c_str(), d.accessTech.c_str());
  return anyResponse;
}

String modemDiagnosticsToJson(const ModemDiagnostics& d, uint32_t regTimeMs) {
  auto esc = [](const String& v) -> String {
    String out;
    out.reserve(v.length() + 8);
    for (size_t i = 0; i < v.length(); i++) {
      char c = v[i];
      if (c == '"' || c == '\\') { out += '\\'; out += c; }
      else if (c == '\n' || c == '\r' || c == '\t') { out += ' '; }
      else { out += c; }
    }
    return out;
  };

  // The backend wants `null` (not 0) for unmeasured values, so it can tell
  // "not measured" from a real reading. Sentinels: rssi/rsrp/rsrq 0 (never a
  // real dBm/dB here), ber 99, regTimeMs 0xFFFFFFFF (untracked, e.g. manual).
  String j;
  j.reserve(256);
  j += "{";
  j += "\"imei\":\"" + esc(d.imei) + "\",";
  j += "\"iccid\":\"" + esc(d.iccid) + "\",";
  j += "\"rssiDbm\":" + (d.rssiDbm == 0 ? String("null") : String(d.rssiDbm)) + ",";
  j += "\"ber\":" + (d.ber == 99 ? String("null") : String(d.ber)) + ",";
  j += "\"rsrpDbm\":" + (d.rsrpDbm == 0 ? String("null") : String(d.rsrpDbm)) + ",";
  j += "\"rsrqDb\":" + (d.rsrqDb == 0 ? String("null") : String(d.rsrqDb)) + ",";
  j += "\"operator\":\"" + esc(d.operatorName) + "\",";
  j += "\"accessTech\":\"" + esc(d.accessTech) + "\",";
  j += "\"cpsi\":\"" + esc(d.cpsi) + "\",";
  j += "\"regTimeMs\":" +
       (regTimeMs == 0xFFFFFFFFUL ? String("null") : String(regTimeMs));
  j += "}";
  return j;
}

// ---------------------------------------------------------------------------
// isResponsive()
// ---------------------------------------------------------------------------

bool ModemDriver::isResponsive() {
  String resp;
  return sendAT("AT", resp, 2000);
}
