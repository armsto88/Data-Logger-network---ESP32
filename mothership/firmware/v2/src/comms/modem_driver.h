// Mothership V1 — Production modem driver for SIMCom A7670G-LABE
//
// Promotes the proven bring-up helper patterns (tests/modem_at_helper.h)
// into a proper ModemDriver class with state machine, timeouts, recovery,
// and HTTPS POST support.
//
// Hardware: A7670G-LABE, firmware A110B06A7670M7 / V1.11.2
// UART2 at 115200 baud, 1.8V <-> 3.3V level shifters.
//
// Pin/timing constants come from system/pins.h — do not redefine here.

#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "system/pins.h"

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

enum class ModemState {
  OFF,
  RAIL_ENABLED,
  RAIL_STABLE,
  BOOT_REQUESTED,
  STATUS_HIGH,
  UART_READY,
  REGISTERED,
  TRANSPORT_OPEN,
  UPLOAD_ACTIVE,
  SHUTTING_DOWN,
  RECOVERY,
  ERROR
};

// ---------------------------------------------------------------------------
// HTTPS POST result
// ---------------------------------------------------------------------------

struct HttpsPostResult {
  bool success = false;
  int httpStatus = -1;  // 200, 4xx, 5xx, or -1 (no/incomplete response)
  String responseBody;  // complete, de-framed and de-chunked HTTP entity body
  String errorDetail;   // human-readable error for logging
  uint32_t responseWireBytes = 0;
  uint32_t responseHttpBytes = 0;
  bool responseComplete = false;
};

// ---------------------------------------------------------------------------
// HTTPS GET streaming download (for large bodies — e.g. an OTA firmware image)
// ---------------------------------------------------------------------------

// Called with each contiguous run of received body bytes as they arrive off the
// modem. `data`/`len` point into a driver-owned buffer valid only for the call —
// copy out anything needed beyond it. Return false to abort the transfer early
// (e.g. the OTA install rejected a chunk, or a time budget was exceeded).
using HttpsStreamChunkCallback = bool (*)(const uint8_t* data, size_t len, void* ctx);

struct HttpsGetStreamResult {
  bool     success = false;            // 2xx + body fully delivered + not aborted
  int      httpStatus = -1;
  uint32_t declaredContentLength = 0;  // from Content-Length (0 if absent)
  uint32_t bytesDelivered = 0;         // total body bytes handed to the callback
  bool     complete = false;           // declared length fully received
  bool     aborted = false;            // callback asked to stop
  String   errorDetail;
};

// ---------------------------------------------------------------------------
// Modem diagnostics — link-quality / identity snapshot for the status payload.
// Populated by getDiagnostics() while the modem is registered.  Values that
// could not be read are left at their "unknown" sentinel (0 dBm / 99 BER / "").
// ---------------------------------------------------------------------------

struct ModemDiagnostics {
  String imei;          // AT+CGSN
  String iccid;         // AT+CICCID (SIM identity)
  int    rssiDbm;       // AT+CSQ, converted to dBm (0 = unknown)
  int    ber;           // AT+CSQ bit-error-rate class (99 = unknown)
  int    rsrpDbm;       // AT+CESQ LTE RSRP in dBm (0 = unknown)
  int    rsrqDb;        // AT+CESQ LTE RSRQ in dB  (0 = unknown)
  String operatorName;  // AT+COPS? registered operator
  String accessTech;    // "LTE"/"GSM"/"WCDMA"/... from COPS/CPSI
  String cpsi;          // raw +CPSI: line (band, cell ID, TAC, freq)
};

// ---------------------------------------------------------------------------
// ModemDriver
// ---------------------------------------------------------------------------

class ModemDriver {
 public:
  ModemDriver();

  // Configure pins (4V_EN, PWRKEY OUTPUT LOW; PG, STATUS INPUT).
  // UART is NOT started here — it starts in powerOn() after boot.
  void init();

  // Rail -> PG -> PWRKEY -> boot -> UART ready.
  // Returns true if AT responds.
  bool powerOn();

  // Poll AT+CREG? and AT+CEREG? for registration (1=home, 5=roaming).
  // Returns false on timeout. MUST NOT block forever.
  bool waitForNetwork(uint32_t timeoutMs);

  // HTTP/HTTPS POST via the A7670G socket API.
  // HTTPS uses CCH* API (CCHSTART/CCHOPEN/CCHSEND) with NTP sync + SNI.
  // Explicit HTTP URLs use CIP* API (CIPOPEN/CIPSEND); HTTPS never downgrades.
  // Verified working on hardware with firmware A110B06A7670M7.
  HttpsPostResult httpsPost(const String& url,
                            const String& payload,
                            const String& contentType = "text/csv",
                            const String& authToken = "");

  // HTTPS GET that streams the response body to a callback instead of
  // buffering it, so a large (~1 MB) firmware image never lives in RAM at
  // once. HTTPS only (uses the CCH* TLS path); a non-https:// URL is rejected.
  // idleTimeoutMs: abort if no new bytes arrive for this long.
  // rangeHeader: if non-empty, sent verbatim as "Range: <rangeHeader>" (e.g.
  // "bytes=0-524287") for chunked downloads. A successful ranged response is
  // HTTP 206; the caller must check result.httpStatus, since a server that
  // ignores Range silently returns 200 with the WHOLE body instead of the
  // requested slice.
  HttpsGetStreamResult httpsGetStream(const String& url,
                                      HttpsStreamChunkCallback chunkCallback,
                                      void* callbackCtx,
                                      uint32_t idleTimeoutMs = 20000,
                                      const String& authToken = "",
                                      const String& rangeHeader = "");

  // AT+CPOF -> wait STATUS LOW -> PWRKEY 2.5s fallback -> rail off.
  void gracefulShutdown();

  // Recovery: rail off -> 3s wait -> rail on -> re-boot.
  void forcePowerCycle();

  // Accessors
  ModemState getState() const { return m_state; }
  String stateToString() const;

  // AT+CGSN (cached after first call).
  String getImei();

  // AT+CSQ -> rssi (99 = no signal).
  int getSignalQuality();

  // Populate a ModemDiagnostics snapshot (CSQ, CESQ, COPS, CICCID, CPSI, IMEI).
  // Call while registered.  Best-effort: unreadable fields keep their sentinels.
  // Returns true if the AT interface responded at all.
  bool getDiagnostics(ModemDiagnostics& out);

  // Quick AT check, returns true if OK within 2000 ms.
  bool isResponsive();

 private:
  ModemState m_state = ModemState::OFF;
  String m_imei;
  bool m_uartStarted = false;

  // Send cmd\r\n, wait for OK/ERROR. Returns true if OK.
  bool sendAT(const char* cmd, String& response, uint32_t timeoutMs);

  // Send cmd, collect response, return true if response contains expected.
  bool sendATExpect(const char* cmd, const char* expected, uint32_t timeoutMs);

  // PWM soft-start + PG wait. Returns false if PG doesn't come up.
  bool railOn();

  // 4V_EN LOW.
  void railOff();

  // PWRKEY HIGH for MODEM_PWRKEY_ON_MS, then LOW.
  void pulsePwrkey();

  // Read PIN_MODEM_STATUS.
  int readStatus();

  // Wait for STATUS HIGH, then AT handshake. Returns false on timeout.
  bool waitBoot(uint32_t timeoutMs);

  // Serial2.begin if not already started. Flush RX buffer.
  void startUart();

  // SSL/TLS POST via CCH* API (internal helpers)
  bool httpsPostSSL(const String& host, int port,
                    const String& httpReq, HttpsPostResult& result);
  bool httpsPostTCP(const String& host, int port,
                    const String& httpReq, HttpsPostResult& result);

  // SSL/TLS streaming GET via CCH* API. Opens the connection, sends the GET
  // request, then drives the incremental frame reader that forwards body
  // bytes to the callback. Fills `result`; returns result.success.
  bool httpsGetStreamSSL(const String& host, int port, const String& httpReq,
                         HttpsStreamChunkCallback chunkCallback, void* ctx,
                         uint32_t idleTimeoutMs, HttpsGetStreamResult& result);
};

// Serialise a ModemDiagnostics snapshot as the status.modem{} JSON object.
// regTimeMs = milliseconds spent in network registration this session.
String modemDiagnosticsToJson(const ModemDiagnostics& d, uint32_t regTimeMs);
