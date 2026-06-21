// Mothership V1 — Production modem driver implementation
//
// See modem_driver.h for the design overview.  The power-rail, PWRKEY,
// and AT-handshake patterns mirror the proven bring-up helpers in
// tests/modem_at_helper.h (Tests 9-13 all passed on hardware).

#include "modem_driver.h"

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

// NOTE: The AT+HTTP* command sequence below is written to the SIMCom
// documented standard for the A7670G.  The exact syntax for firmware
// A110B06A7670M7 has not yet been verified with an antenna connected.
// When the antenna arrives, a bringup_modem_https test will confirm and
// adjust if needed.  Key areas to verify:
//   - AT+HTTPDATA prompt ("DOWNLOAD" vs "OK")
//   - AT+HTTPPARA="USERDATA" header support
//   - AT+HTTPACTION URC format and timing
//   - AT+HTTPREAD response framing

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

  // 1. HTTPINIT
  {
    String resp;
    if (!sendAT("AT+HTTPINIT", resp, 5000)) {
      result.errorDetail = "AT+HTTPINIT failed: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      return result;
    }
  }
  m_state = ModemState::TRANSPORT_OPEN;

  // 2. URL
  {
    String cmd = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
    String resp;
    if (!sendAT(cmd.c_str(), resp, 5000)) {
      result.errorDetail = "AT+HTTPPARA URL failed: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      return result;
    }
  }

  // 3. Content-Type
  {
    String cmd = "AT+HTTPPARA=\"CONTENT\",\"" + contentType + "\"";
    String resp;
    if (!sendAT(cmd.c_str(), resp, 5000)) {
      result.errorDetail = "AT+HTTPPARA CONTENT failed: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      return result;
    }
  }

  // 4. Auth token (optional). If this fails, continue — the token may be
  //    in the URL query param instead.
  if (authToken.length() > 0) {
    String cmd = "AT+HTTPPARA=\"USERDATA\",\"Authorization: Bearer " +
                 authToken + "\"";
    String resp;
    if (!sendAT(cmd.c_str(), resp, 5000)) {
      Serial.println("[Modem] WARNING: USERDATA header failed (non-fatal): " +
                      resp);
      // Continue — token may be in URL.
    }
  }

  // 5. HTTPDATA — send payload.
  //    AT+HTTPDATA=<len>,<timeout>  then wait for "DOWNLOAD" (or "OK")
  //    prompt, then write payload bytes.
  m_state = ModemState::UPLOAD_ACTIVE;
  {
    uint32_t dataLen = payload.length();
    String cmd = "AT+HTTPDATA=" + String(dataLen) + ",10000";
    String resp;

    // Send the HTTPDATA command and wait for the DOWNLOAD / OK prompt.
    // We use a raw send + collect pattern because the prompt is not
    // terminated by OK/ERROR.
    while (Serial2.available()) { Serial2.read(); }  // flush
    Serial2.print(cmd);
    Serial2.print("\r\n");
    Serial2.flush();

    // Wait for "DOWNLOAD" or "OK" prompt (up to 5 s).
    resp = "";
    unsigned long start = millis();
    bool gotPrompt = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("DOWNLOAD") >= 0 || resp.indexOf("OK") >= 0) {
          gotPrompt = true;
          break;
        }
      }
      if (gotPrompt) break;
      delay(5);
    }

    if (!gotPrompt) {
      result.errorDetail = "AT+HTTPDATA no DOWNLOAD/OK prompt: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      m_state = ModemState::TRANSPORT_OPEN;
      return result;
    }

    // Write payload bytes.
    Serial2.print(payload);
    Serial2.flush();

    // Wait for OK after data upload (up to 10 s).
    resp = "";
    start = millis();
    bool dataOk = false;
    while (millis() - start < 10000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("OK\r\n") >= 0) {
          dataOk = true;
          break;
        }
        if (resp.indexOf("ERROR\r\n") >= 0) break;
      }
      if (dataOk) break;
      delay(5);
    }

    if (!dataOk) {
      result.errorDetail = "AT+HTTPDATA upload did not get OK: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      m_state = ModemState::TRANSPORT_OPEN;
      return result;
    }
  }

  // 6. HTTPACTION=1 (POST).  Expect OK, then wait for the
  //    +HTTPACTION: 1,<status>,<responseLen> URC (up to 30 s).
  {
    String resp;
    if (!sendAT("AT+HTTPACTION=1", resp, 5000)) {
      result.errorDetail = "AT+HTTPACTION=1 failed: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      m_state = ModemState::TRANSPORT_OPEN;
      return result;
    }

    // Now wait for the +HTTPACTION URC.
    resp = "";
    unsigned long start = millis();
    bool gotUrc = false;
    int httpStatus = -1;
    int responseLen = 0;
    while (millis() - start < 30000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        int idx = resp.indexOf("+HTTPACTION: 1,");
        if (idx >= 0) {
          // Parse: +HTTPACTION: 1,<status>,<responseLen>
          // Find the comma after "1,".
          int comma1 = resp.indexOf(',', idx + 15);  // after "+HTTPACTION: 1"
          if (comma1 >= 0) {
            int comma2 = resp.indexOf(',', comma1 + 1);
            String statusStr, lenStr;
            if (comma2 >= 0) {
              statusStr = resp.substring(comma1 + 1, comma2);
              lenStr = resp.substring(comma2 + 1);
            } else {
              statusStr = resp.substring(comma1 + 1);
            }
            statusStr.trim();
            lenStr.trim();
            httpStatus = statusStr.toInt();
            responseLen = lenStr.toInt();
            gotUrc = true;
          }
          break;
        }
      }
      if (gotUrc) break;
      delay(10);
    }

    if (!gotUrc) {
      result.errorDetail = "AT+HTTPACTION URC timeout: " + resp;
      Serial.println("[Modem] " + result.errorDetail);
      sendAT("AT+HTTPTERM", resp, 2000);
      m_state = ModemState::TRANSPORT_OPEN;
      return result;
    }

    result.httpStatus = httpStatus;
    Serial.printf("[Modem] HTTPACTION URC: status=%d, responseLen=%d\n",
                  httpStatus, responseLen);

    // 7. If 200 and there is a response body, read it.
    if (httpStatus == 200 && responseLen > 0) {
      String readResp;
      if (sendAT("AT+HTTPREAD", readResp, 10000)) {
        // Response format: +HTTPREAD: <len>\r\n<body>\r\nOK\r\n
        // Extract the body between the first \r\n after +HTTPREAD: and OK.
        int hdrIdx = readResp.indexOf("+HTTPREAD:");
        if (hdrIdx >= 0) {
          int bodyStart = readResp.indexOf('\n', hdrIdx);
          if (bodyStart >= 0) {
            bodyStart++;  // skip the \n
            // Body ends before the final OK line.
            int okIdx = readResp.lastIndexOf("\r\nOK\r\n");
            if (okIdx >= bodyStart) {
              result.responseBody = readResp.substring(bodyStart, okIdx);
            } else {
              result.responseBody = readResp.substring(bodyStart);
            }
          }
        }
      } else {
        Serial.println("[Modem] WARNING: AT+HTTPREAD failed: " + readResp);
      }
    }
  }

  // 8. HTTPTERM
  {
    String resp;
    sendAT("AT+HTTPTERM", resp, 5000);
  }

  // 9. Build result.
  result.success = (result.httpStatus == 200);
  if (!result.success && result.errorDetail.length() == 0) {
    result.errorDetail = "HTTP status " + String(result.httpStatus);
  }

  m_state = ModemState::REGISTERED;  // back to registered after upload
  Serial.printf("[Modem] httpsPost() done: success=%d, status=%d\n",
                result.success ? 1 : 0, result.httpStatus);
  return result;
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
// isResponsive()
// ---------------------------------------------------------------------------

bool ModemDriver::isResponsive() {
  String resp;
  return sendAT("AT", resp, 2000);
}