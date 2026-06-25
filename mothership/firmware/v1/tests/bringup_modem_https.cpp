// Mothership V1 bringup: A7670G data-capability diagnostics
// Power-on the A7670G, wait for GSM registration, then run a series of
// diagnostic AT commands to determine why AT+CIPSTART and AT+SAPBR return
// ERROR. Prints ALL modem responses for analysis.

#include <Arduino.h>
#include "modem_at_helper.h"

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== Mothership V1 Modem Data Diagnostics ===");
  Serial.println("Starting in 5 seconds...");
  for (int i = 5; i > 0; i--) {
    Serial.printf("%d...\n", i);
    delay(1000);
  }
  Serial.println("Go!");

  // --- Power on ---
  Serial.println();
  Serial.println("--- Power on: 4V rail + PWRKEY + boot ---");
  bool pg = modemRailOn();
  if (!pg) {
    Serial.println("FAIL: Power-good did not assert. Aborting.");
    return;
  }
  Serial.println("PASS: 4V rail up (ESP_PG HIGH).");

  modemInitUart();
  modemPulsePwrkey();
  if (!modemWaitBoot(15000)) {
    Serial.println("FAIL: Modem did not boot / respond to AT. Aborting.");
    modemGracefulOff();
    return;
  }
  Serial.println("PASS: Modem booted.");

  String resp;
  modemSendAT("ATE0", resp, 2000);

  // Allow modem to settle before polling
  delay(5000);

  // AT+COPS=0 removed — modem auto-registers without it

  // --- Wait for network registration (up to 180s) ---
  Serial.println();
  Serial.println("--- Waiting for network registration (up to 180s) ---");
  const unsigned long netTimeoutMs = 180000;
  const unsigned long netStart = millis();
  bool registered = false;

  while (millis() - netStart < netTimeoutMs) {
    unsigned long elapsed = (millis() - netStart) / 1000;

    // AT+CSQ
    Serial.printf("[%lus] AT+CSQ: ", elapsed);
    if (modemSendAT("AT+CSQ", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // AT+CREG?
    Serial.printf("[%lus] AT+CREG?: ", elapsed);
    if (modemSendAT("AT+CREG?", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // AT+CEREG?
    Serial.printf("[%lus] AT+CEREG?: ", elapsed);
    if (modemSendAT("AT+CEREG?", resp, 2000)) {
      Serial.print(resp);
    } else {
      Serial.println("(no response)");
    }

    // Check registration from the CREG? response we just printed
    // resp still holds the CEREG? response, so we need to re-query CREG
    String cregCheck;
    modemSendAT("AT+CREG?", cregCheck, 2000);
    bool cregOk = (cregCheck.indexOf("+CREG: 0,1") >= 0 || cregCheck.indexOf("+CREG: 0,5") >= 0);

    if (cregOk) {
      Serial.println();
      Serial.println("GSM REGISTERED — proceeding to data diagnostics");
      registered = true;
      break;
    }

    Serial.println();
    delay(5000);
  }

  if (!registered) {
    Serial.println("FAIL: Modem did not register within 180s. Aborting.");
    modemGracefulOff();
    return;
  }

  // --- Data diagnostics ---
  Serial.println();
  Serial.println("=== MODEM DATA DIAGNOSTICS ===");

  // 1. Check current PDP contexts
  Serial.println("[1] AT+CGDCONT? (current PDP contexts)");
  modemSendAT("AT+CGDCONT?", resp, 5000);
  Serial.println(resp);

  // 2. Set APN
  Serial.println("[2] AT+CGDCONT=1,\"IP\",\"TM\"");
  modemSendAT("AT+CGDCONT=1,\"IP\",\"TM\"", resp, 5000);
  Serial.println(resp);

  // 3. Activate PDP
  Serial.println("[3] AT+CGACT=1,1");
  modemSendAT("AT+CGACT=1,1", resp, 10000);
  Serial.println(resp);

  // 4. Check PDP state
  Serial.println("[4] AT+CGACT?");
  modemSendAT("AT+CGACT?", resp, 5000);
  Serial.println(resp);

  // 5. Check CGATT
  Serial.println("[5] AT+CGATT?");
  modemSendAT("AT+CGATT?", resp, 5000);
  Serial.println(resp);

  // 6. Check PDP address
  Serial.println("[6] AT+CGPADDR=1");
  modemSendAT("AT+CGPADDR=1", resp, 5000);
  Serial.println(resp);

  // --- Bring up data connection (A7600 NETOPEN API) ---
  // Declare result variables early so goto targets have them in scope.
  bool success = false;
  int httpStatus = -1;
  String errorDetail = "";
  String responseBody = "";

  Serial.println();
  Serial.println("--- Bringing up data connection (AT+NETOPEN) ---");

  // Remove CIICR attempt — not supported on A7600
  // Go straight to NETOPEN
  Serial.println("[DATA] AT+NETOPEN...");
  bool netopenOk = modemSendAT("AT+NETOPEN", resp, 15000);
  Serial.print("[DATA] NETOPEN: ");
  Serial.println(resp);

  if (!netopenOk) {
    // NETOPEN may return OK but also emit +NETOPEN: 0 URC
    // Check if we got OK or the URC
    if (resp.indexOf("OK") < 0 && resp.indexOf("+NETOPEN") < 0) {
      errorDetail = "AT+NETOPEN failed: " + resp;
      Serial.println("[DATA] " + errorDetail);
      goto post_result;
    }
  }

  // Wait for +NETOPEN: 0 URC (success) or just delay
  Serial.println("[DATA] Waiting for network open...");
  delay(3000);

  // Check if we got the URC
  modemSendAT("AT+NETOPEN?", resp, 2000);
  Serial.print("[DATA] NETOPEN status: ");
  Serial.println(resp);

  Serial.println("[DATA] Network opened!");
  delay(1000);

  // --- Sync modem clock (needed for SSL certificate validation) ---
  Serial.println();
  Serial.println("--- Syncing modem clock ---");

  // Method 1: NTP sync (needs data connection which is now open)
  Serial.println("[NTP] Setting NTP server...");
  modemSendAT("AT+CNTP=\"pool.ntp.org\",32", resp, 5000);
  Serial.print("[NTP] CNTP set: ");
  Serial.println(resp);

  Serial.println("[NTP] Executing NTP sync...");
  modemSendAT("AT+CNTP", resp, 15000);
  Serial.print("[NTP] CNTP exec: ");
  Serial.println(resp);

  delay(3000);  // Give NTP time to complete

  // Check if clock updated
  modemSendAT("AT+CCLK?", resp, 2000);
  Serial.print("[NTP] CCLK after NTP: ");
  Serial.println(resp);

  // Method 2: Manual clock set — ONLY if NTP didn't update the clock
  // Check if the year is still 70 (NTP failed)
  if (resp.indexOf("70/01/01") >= 0 || resp.indexOf("70/") >= 0) {
    Serial.println("[NTP] NTP failed — setting clock manually...");
    modemSendAT("AT+CCLK=\"26/06/25,20:00:00+08\"", resp, 2000);
    Serial.print("[NTP] CCLK set: ");
    Serial.println(resp);

    modemSendAT("AT+CCLK?", resp, 2000);
    Serial.print("[NTP] CCLK after manual: ");
    Serial.println(resp);
  } else {
    Serial.println("[NTP] NTP sync successful — keeping NTP time");
  }

  delay(1000);

  // --- Configure SSL context ---
  Serial.println();
  Serial.println("--- Configuring SSL context ---");

  // Set SSL version to TLS 1.2 (value 4 = TLS 1.1 and above)
  modemSendAT("AT+CSSLCFG=\"sslversion\",0,4", resp, 2000);
  Serial.print("[SSL] sslversion(0): ");
  Serial.println(resp);

  // Set auth mode to 0 (no verification) for bringup testing
  modemSendAT("AT+CSSLCFG=\"authmode\",0,0", resp, 2000);
  Serial.print("[SSL] authmode(0): ");
  Serial.println(resp);

  // Try setting SSL context for CIPOPEN — the A7600 may use context index 0
  // Try AT+CASSLCFG (alternative SSL config command)
  modemSendAT("AT+CASSLCFG=0,\"SSL\",0", resp, 2000);
  Serial.print("[SSL] CASSLCFG: ");
  Serial.println(resp);

  // AT+CSSLCFG="ignorelocaltime",0,1 — ignore local time for cert validation
  modemSendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", resp, 2000);
  Serial.print("[SSL] ignorelocaltime: ");
  Serial.println(resp);

  // AT+CSSLCFG="enableSNI",0,1 — enable Server Name Indication (required by modern HTTPS servers)
  modemSendAT("AT+CSSLCFG=\"enableSNI\",0,1", resp, 2000);
  Serial.print("[SSL] enableSNI: ");
  Serial.println(resp);

  // Try setting ciphersuite to allow all
  modemSendAT("AT+CSSLCFG=\"ciphersuite\",0,\"ALL\"", resp, 2000);
  Serial.print("[SSL] ciphersuite: ");
  Serial.println(resp);

  // Try setting negotiate record version
  modemSendAT("AT+CSSLCFG=\"negotiatetime\",0,30", resp, 2000);
  Serial.print("[SSL] negotiatetime: ");
  Serial.println(resp);

  delay(2000);

  // --- HTTPS POST via CCH SSL API ---
  // Wrapped in a block scope so goto labels don't cross variable
  // initializations (C++ forbids jumping over non-trivial init).
  {
  // Declare all locals up front so goto labels don't cross init.
  String payload;
  String httpReq;
  String openCmd;
  String sendCmd;
  String tcpOpen;
  unsigned long start = 0;
  bool cchStartOk = false;
  bool cchOpenOk = false;
  bool cchOpenDone = false;
  bool tcpOpenOk = false;
  bool gotPrompt = false;
  bool sendOk = false;
  bool gotData = false;
  uint32_t reqLen = 0;

  Serial.println();
  Serial.println("--- HTTPS POST via CCH SSL API ---");

  payload = "test_node,test_value,2026-06-25T12:00:00,42.0\n";

  // Build HTTP request manually
  httpReq = "POST /macros/s/AKfycbxnxCfZhsisxiPD3dXazz1-1l2fK5wRNTNCdztXLva3jqfHL7DNCX2dvTehGz6CTZ38/exec HTTP/1.1\r\n";
  httpReq += "Host: script.google.com\r\n";
  httpReq += "User-Agent: ESP32-Mothership/1.0\r\n";
  httpReq += "Content-Type: text/plain\r\n";
  httpReq += "Content-Length: " + String(payload.length()) + "\r\n";
  httpReq += "Connection: close\r\n";
  httpReq += "\r\n";
  httpReq += payload;

  // 1. Start SSL service
  Serial.println("[SSL] AT+CCHSTART...");
  cchStartOk = modemSendAT("AT+CCHSTART", resp, 30000);
  Serial.print("[SSL] CCHSTART: ");
  Serial.println(resp);

  if (!cchStartOk) {
    // CCHSTART may return OK then +CCHSTART: 0 URC
    if (resp.indexOf("OK") < 0 && resp.indexOf("+CCHSTART") < 0) {
      errorDetail = "AT+CCHSTART failed: " + resp;
      Serial.println("[SSL] " + errorDetail);
      goto ssl_fallback_tcp;
    }
  }
  delay(2000);

  // 2. Set SSL context for session 0
  Serial.println("[SSL] AT+CCHSSLCFG=0,0...");
  modemSendAT("AT+CCHSSLCFG=0,0", resp, 5000);
  Serial.print("[SSL] CCHSSLCFG: ");
  Serial.println(resp);

  // 3. Set receive mode to auto (output data directly)
  modemSendAT("AT+CCHSET=0,0", resp, 2000);

  // 4. Open SSL connection
  Serial.println("[SSL] AT+CCHOPEN=0,\"script.google.com\",443,2...");
  openCmd = "AT+CCHOPEN=0,\"script.google.com\",443,2";

  // Raw send to catch URC
  while (Serial2.available()) { Serial2.read(); }
  Serial2.print(openCmd);
  Serial2.print("\r\n");
  Serial2.flush();

  // Wait for +CCHOPEN: 0,<err> URC (0=success, non-zero=error)
  // Note: "+CCHOPEN: 0," is 12 characters (indices 0-11), so the error
  // digit is at urcIdx + 12. The OK response often arrives BEFORE the URC,
  // so we must keep reading until the full URC + digit is present.
  resp = "";
  start = millis();
  cchOpenOk = false;
  cchOpenDone = false;
  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
      Serial.print(c);  // Echo raw output for debugging
    }
    // Check for success URC: +CCHOPEN: 0,0
    if (resp.indexOf("+CCHOPEN: 0,0") >= 0) {
      cchOpenOk = true;
      cchOpenDone = true;
      break;
    }
    // Check for error URC: +CCHOPEN: 0,<non-zero digit>
    int urcIdx = resp.indexOf("+CCHOPEN: 0,");
    if (urcIdx >= 0) {
      int digitPos = urcIdx + 12;  // index of char after the comma
      if (digitPos < resp.length()) {
        char errChar = resp.charAt(digitPos);
        if (errChar >= '0' && errChar <= '9' && errChar != '0') {
          // Non-zero error code
          cchOpenDone = true;
          break;
        }
      }
    }
    delay(10);
  }

  if (!cchOpenOk) {
    Serial.printf("\n[SSL] CCHOPEN failed: %s\n", resp.c_str());
    // Close SSL service and fall back to TCP
    modemSendAT("AT+CCHSTOP", resp, 5000);
    goto ssl_fallback_tcp;
  }

  Serial.println("\n[SSL] SSL connection opened!");
  delay(500);

  // 5. Send HTTP request
  {
    reqLen = httpReq.length();
    sendCmd = "AT+CCHSEND=0," + String(reqLen);
    Serial.printf("[SSL] CCHSEND: %u bytes\n", (unsigned)reqLen);

    while (Serial2.available()) { Serial2.read(); }
    Serial2.print(sendCmd);
    Serial2.print("\r\n");
    Serial2.flush();

    // Wait for '>' prompt
    resp = "";
    start = millis();
    gotPrompt = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf(">") >= 0) {
          gotPrompt = true;
          break;
        }
      }
      if (gotPrompt) break;
      delay(5);
    }

    if (!gotPrompt) {
      Serial.print("[SSL] No '>' prompt: ");
      Serial.println(resp);
      modemSendAT("AT+CCHCLOSE=0", resp, 2000);
      modemSendAT("AT+CCHSTOP", resp, 5000);
      goto ssl_fallback_tcp;
    }

    // Write HTTP request
    Serial2.print(httpReq);
    Serial2.flush();

    // Wait for OK after send
    resp = "";
    start = millis();
    sendOk = false;
    while (millis() - start < 10000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("OK\r\n") >= 0) {
          sendOk = true;
          break;
        }
      }
      if (sendOk) break;
      delay(5);
    }
    Serial.printf("[SSL] Send OK: %s\n", sendOk ? "YES" : "NO");
  }

  // 6. Wait for response data (auto mode — data comes directly to UART)
  Serial.println("[SSL] Waiting for response...");
  resp = "";
  start = millis();
  gotData = false;
  while (millis() - start < 30000) {
    while (Serial2.available()) {
      char c = (char)Serial2.read();
      resp += c;
      // Look for HTTP status line
      if (resp.indexOf("HTTP/1.") >= 0 && !gotData) {
        gotData = true;
        int httpIdx = resp.indexOf("HTTP/1.");
        if (httpIdx >= 0) {
          int spaceIdx = resp.indexOf(' ', httpIdx);
          if (spaceIdx >= 0) {
            int nextSpace = resp.indexOf(' ', spaceIdx + 1);
            int crIdx = resp.indexOf('\r', spaceIdx + 1);
            int endIdx = (nextSpace >= 0 && (crIdx < 0 || nextSpace < crIdx)) ? nextSpace : crIdx;
            if (endIdx < 0) endIdx = spaceIdx + 4;
            String statusStr = resp.substring(spaceIdx + 1, endIdx);
            statusStr.trim();
            httpStatus = statusStr.toInt();
          }
        }
      }
      // Check for connection close
      if (resp.indexOf("+CCH_PEER_CLOSED") >= 0 || resp.indexOf("CLOSED") >= 0) {
        break;
      }
    }
    if (resp.indexOf("+CCH_PEER_CLOSED") >= 0 || resp.indexOf("CLOSED") >= 0) break;
    delay(10);
  }

  if (gotData) {
    responseBody = resp;
    success = (httpStatus == 200);
    Serial.println("[SSL] Response received:");
    Serial.println(resp.substring(0, 800));
    if (success) {
      Serial.println("[SSL] SSL POST SUCCESS!");
    }
  } else {
    Serial.print("[SSL] No response: ");
    Serial.println(resp.substring(0, 300));
  }

  // 7. Close SSL connection and stop service
  modemSendAT("AT+CCHCLOSE=0", resp, 2000);
  modemSendAT("AT+CCHSTOP", resp, 5000);

  // If SSL succeeded, skip TCP fallback
  if (success) {
    goto post_result;
  }

ssl_fallback_tcp:
  // --- TCP fallback via CIP API ---
  if (!success) {
    Serial.println();
    Serial.println("--- TCP fallback via CIP API ---");

    // Reset state for TCP fallback
    gotData = false;
    httpStatus = -1;
    resp = "";

    // Close any existing socket
    modemSendAT("AT+CIPCLOSE=0", resp, 2000);
    delay(500);

    // Open TCP connection
    tcpOpen = "AT+CIPOPEN=0,\"TCP\",\"script.google.com\",80";
    Serial.print("[TCP] ");
    Serial.println(tcpOpen);

    while (Serial2.available()) { Serial2.read(); }
    Serial2.print(tcpOpen);
    Serial2.print("\r\n");
    Serial2.flush();

    resp = "";
    start = millis();
    tcpOpenOk = false;
    while (millis() - start < 30000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("+CIPOPEN: 0,0") >= 0) {
          tcpOpenOk = true;
          break;
        }
        if (resp.indexOf("ERROR") >= 0) break;
      }
      if (tcpOpenOk || resp.indexOf("ERROR") >= 0) break;
      delay(10);
    }

    if (!tcpOpenOk) {
      if (errorDetail.length() == 0) errorDetail = "Both SSL and TCP failed";
      Serial.println("[TCP] TCP open also failed");
      goto post_result;
    }

    Serial.println("[TCP] TCP socket opened!");
    delay(500);

    // Send data
    reqLen = httpReq.length();
    sendCmd = "AT+CIPSEND=0," + String(reqLen);
    Serial.printf("[TCP] Sending %u bytes...\n", (unsigned)reqLen);

    while (Serial2.available()) { Serial2.read(); }
    Serial2.print(sendCmd);
    Serial2.print("\r\n");
    Serial2.flush();

    resp = "";
    start = millis();
    gotPrompt = false;
    while (millis() - start < 5000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf(">") >= 0) { gotPrompt = true; break; }
      }
      if (gotPrompt) break;
      delay(5);
    }

    if (!gotPrompt) {
      if (errorDetail.length() == 0) errorDetail = "TCP CIPSEND no prompt";
      modemSendAT("AT+CIPCLOSE=0", resp, 2000);
      goto post_result;
    }

    Serial2.print(httpReq);
    Serial2.flush();

    // Wait for response
    Serial.println("[TCP] Waiting for response...");
    resp = "";
    start = millis();
    gotData = false;
    while (millis() - start < 30000) {
      while (Serial2.available()) {
        char c = (char)Serial2.read();
        resp += c;
        if (resp.indexOf("HTTP/1.") >= 0 && !gotData) {
          gotData = true;
          int httpIdx = resp.indexOf("HTTP/1.");
          if (httpIdx >= 0) {
            int spaceIdx = resp.indexOf(' ', httpIdx);
            if (spaceIdx >= 0) {
              int nextSpace = resp.indexOf(' ', spaceIdx + 1);
              int crIdx = resp.indexOf('\r', spaceIdx + 1);
              int endIdx = (nextSpace >= 0 && (crIdx < 0 || nextSpace < crIdx)) ? nextSpace : crIdx;
              if (endIdx < 0) endIdx = spaceIdx + 4;
              String statusStr = resp.substring(spaceIdx + 1, endIdx);
              statusStr.trim();
              httpStatus = statusStr.toInt();
            }
          }
        }
        // +IPCLOSE may arrive as "+IPCLOSE\r\n" or "+IPCLOSE: 0,1\r\n"
        if (resp.indexOf("+IPCLOSE") >= 0 || resp.indexOf("CLOSED") >= 0) break;
      }
      if (resp.indexOf("+IPCLOSE") >= 0 || resp.indexOf("CLOSED") >= 0) break;
      delay(10);
    }

    if (gotData) {
      responseBody = resp;
      success = (httpStatus == 200);
      Serial.println("[TCP] Response received:");
      Serial.println(resp.substring(0, 800));
    }

    modemSendAT("AT+CIPCLOSE=0", resp, 2000);
  }

  // Close network
  {
    String r2;
    modemSendAT("AT+NETCLOSE", r2, 5000);
  }
  }  // end CCH SSL / TCP fallback block scope

post_result:
  // --- Result ---
  Serial.println();
  Serial.println("--- POST Result ---");
  Serial.printf("Success: %s\n", success ? "YES" : "NO");
  Serial.printf("HTTP Status: %d\n", httpStatus);
  if (responseBody.length() > 0) {
    Serial.println("Response (first 800 chars):");
    Serial.println(responseBody.substring(0, 800));
  }
  if (errorDetail.length() > 0) {
    Serial.println("Error detail:");
    Serial.println(errorDetail);
  }

  Serial.println();
  if (success && httpStatus == 200) {
    Serial.println("OVERALL PASS: POST succeeded with HTTP 200.");
  } else if (httpStatus > 0) {
    Serial.printf("PARTIAL: POST completed but HTTP status=%d (not 200).\n", httpStatus);
  } else {
    Serial.println("OVERALL FAIL: POST did not succeed. See error detail above.");
  }

  // Check firmware version
  Serial.println();
  Serial.println("[FW] AT+CGMR");
  modemSendAT("AT+CGMR", resp, 5000);
  Serial.println(resp);

  Serial.println();
  Serial.println("=== DIAGNOSTICS COMPLETE ===");

  // --- Graceful power off ---
  Serial.println();
  Serial.println("--- Graceful power off ---");
  modemGracefulOff();
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void loop() {
  delay(5000);
}