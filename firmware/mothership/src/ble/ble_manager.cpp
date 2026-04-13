#include "ble_manager.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <vector>

namespace {

static const char* kServiceUuid = "6f880001-2d0f-4aa0-8f9e-8f8b7e5a0001";
static const char* kRequestUuid = "6f880002-2d0f-4aa0-8f9e-8f8b7e5a0001";
static const char* kResponseUuid = "6f880003-2d0f-4aa0-8f9e-8f8b7e5a0001";
static const char* kStatusUuid = "6f880004-2d0f-4aa0-8f9e-8f8b7e5a0001";

BLEServer* g_server = nullptr;
BLECharacteristic* g_requestChar = nullptr;
BLECharacteristic* g_responseChar = nullptr;
BLECharacteristic* g_statusChar = nullptr;

BleStatusJsonProvider g_statusProvider = nullptr;
BleNowUnixProvider g_nowUnixProvider = nullptr;
BleCommandRouter g_commandRouter = nullptr;

volatile bool g_bleConnected = false;
volatile bool g_hasPendingRequest = false;
String g_pendingRequest;

String g_deviceName;
uint32_t g_nextTxMessageId = 1;

static const size_t kChunkHeaderBytes = 8;
static const size_t kChunkPayloadBytes = 160;

bool g_rxChunkActive = false;
uint32_t g_rxMessageId = 0;
uint8_t g_rxChunkCount = 0;
std::vector<String> g_rxChunks;

uint32_t readU32BE(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xFF);
  p[1] = (uint8_t)((v >> 16) & 0xFF);
  p[2] = (uint8_t)((v >> 8) & 0xFF);
  p[3] = (uint8_t)(v & 0xFF);
}

uint16_t readU16BE(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

void writeU16BE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)((v >> 8) & 0xFF);
  p[1] = (uint8_t)(v & 0xFF);
}

unsigned long nowUnix() {
  return g_nowUnixProvider ? g_nowUnixProvider() : (unsigned long)(millis() / 1000UL);
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String extractJsonStringField(const String& json, const char* key) {
  if (!key || !*key) return "";

  String needle = String("\"") + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) return "";

  int startQuote = json.indexOf('"', colonPos + 1);
  if (startQuote < 0) return "";

  String value;
  bool escape = false;
  for (int i = startQuote + 1; i < (int)json.length(); ++i) {
    const char c = json[i];
    if (escape) {
      value += c;
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      return value;
    }
    value += c;
  }
  return "";
}

bool extractJsonIntField(const String& json, const char* key, long& outValue) {
  String needle = String("\"") + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return false;

  int colonPos = json.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) return false;

  int i = colonPos + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length()) return false;

  int start = i;
  if (json[i] == '-') i++;
  while (i < (int)json.length() && json[i] >= '0' && json[i] <= '9') i++;
  if (i == start || (i == start + 1 && json[start] == '-')) return false;

  outValue = json.substring(start, i).toInt();
  return true;
}

String extractJsonObjectField(const String& json, const char* key) {
  String needle = String("\"") + key + "\"";
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) return "";

  int i = colonPos + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length() || json[i] != '{') return "";

  int start = i;
  int depth = 0;
  bool inString = false;
  bool escape = false;
  for (; i < (int)json.length(); ++i) {
    const char c = json[i];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '{') depth++;
    if (c == '}') {
      depth--;
      if (depth == 0) {
        return json.substring(start, i + 1);
      }
    }
  }

  return "";
}

String buildErrorResponse(const String& correlationId, const String& code, const String& message) {
  String s;
  s.reserve(220 + message.length());
  s += "{\"type\":\"error\",\"ok\":false,\"timestampUnix\":";
  s += String(nowUnix());
  if (correlationId.length() > 0) {
    s += ",\"correlationId\":\"";
    s += jsonEscape(correlationId);
    s += "\"";
  }
  s += ",\"error\":{\"code\":\"";
  s += jsonEscape(code);
  s += "\",\"message\":\"";
  s += jsonEscape(message);
  s += "\"}}";
  return s;
}

String macToString(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

String buildStatusResponse(const String& correlationId) {
  const String dataJson = g_statusProvider ? g_statusProvider() : String("{}");

  String s;
  s.reserve(260 + dataJson.length());
  s += "{\"type\":\"mothership_status_result\",\"ok\":true,\"timestampUnix\":";
  s += String(nowUnix());
  if (correlationId.length() > 0) {
    s += ",\"correlationId\":\"";
    s += jsonEscape(correlationId);
    s += "\"";
  }
  s += ",\"data\":";
  s += dataJson;
  s += "}";
  return s;
}

String buildOkResponse(const String& correlationId, const String& type, const String& dataJson, const String& message) {
  String s;
  s.reserve(280 + dataJson.length() + message.length());
  s += "{\"type\":\"";
  s += jsonEscape(type);
  s += "\",\"ok\":true,\"timestampUnix\":";
  s += String(nowUnix());
  if (correlationId.length() > 0) {
    s += ",\"correlationId\":\"";
    s += jsonEscape(correlationId);
    s += "\"";
  }
  if (message.length() > 0) {
    s += ",\"message\":\"";
    s += jsonEscape(message);
    s += "\"";
  }
  s += ",\"data\":";
  s += dataJson.length() > 0 ? dataJson : String("{}");
  s += "}";
  return s;
}

void sendResponseNotify(const String& payload) {
  if (!g_responseChar) return;

  if (payload.length() <= 20) {
    g_responseChar->setValue((uint8_t*)payload.c_str(), payload.length());
    g_responseChar->notify();
    return;
  }

  const uint32_t messageId = g_nextTxMessageId++;
  const size_t total = payload.length();
  const uint8_t chunkCount = (uint8_t)((total + kChunkPayloadBytes - 1) / kChunkPayloadBytes);

  for (uint8_t i = 0; i < chunkCount; ++i) {
    const size_t start = (size_t)i * kChunkPayloadBytes;
    const size_t len = min(kChunkPayloadBytes, total - start);

    std::vector<uint8_t> frame;
    frame.resize(kChunkHeaderBytes + len);

    writeU32BE(frame.data(), messageId);
    frame[4] = i;
    frame[5] = chunkCount;
    writeU16BE(frame.data() + 6, (uint16_t)len);
    memcpy(frame.data() + kChunkHeaderBytes, payload.c_str() + start, len);

    g_responseChar->setValue(frame.data(), frame.size());
    g_responseChar->notify();
    delay(5);
  }
}

void sendStatusNotify(const String& payload) {
  if (!g_statusChar) return;

  if (payload.length() <= 20) {
    g_statusChar->setValue((uint8_t*)payload.c_str(), payload.length());
    g_statusChar->notify();
    return;
  }

  const uint32_t messageId = g_nextTxMessageId++;
  const size_t total = payload.length();
  const uint8_t chunkCount = (uint8_t)((total + kChunkPayloadBytes - 1) / kChunkPayloadBytes);

  for (uint8_t i = 0; i < chunkCount; ++i) {
    const size_t start = (size_t)i * kChunkPayloadBytes;
    const size_t len = min(kChunkPayloadBytes, total - start);

    std::vector<uint8_t> frame;
    frame.resize(kChunkHeaderBytes + len);

    writeU32BE(frame.data(), messageId);
    frame[4] = i;
    frame[5] = chunkCount;
    writeU16BE(frame.data() + 6, (uint16_t)len);
    memcpy(frame.data() + kChunkHeaderBytes, payload.c_str() + start, len);

    g_statusChar->setValue(frame.data(), frame.size());
    g_statusChar->notify();
    delay(5);
  }
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    g_bleConnected = true;
    Serial.println("[BLE] App connected");
  }

  void onDisconnect(BLEServer* server) override {
    g_bleConnected = false;
    Serial.println("[BLE] App disconnected");
    if (server) {
      server->getAdvertising()->start();
      Serial.println("[BLE] Advertising restarted");
    }
  }
};

class RequestCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (!c) return;

    std::string raw = c->getValue();
    if (raw.empty()) return;

    const uint8_t* p = (const uint8_t*)raw.data();
    const size_t n = raw.size();

    if (n >= kChunkHeaderBytes) {
      const uint32_t msgId = readU32BE(p);
      const uint8_t chunkIndex = p[4];
      const uint8_t chunkCount = p[5];
      const uint16_t payloadLen = readU16BE(p + 6);

      const size_t bytesAfterHeader = n - kChunkHeaderBytes;
      const bool looksChunked = (chunkCount > 0) && (chunkIndex < chunkCount) && (payloadLen == bytesAfterHeader);

      if (looksChunked) {
        if (!g_rxChunkActive || msgId != g_rxMessageId || chunkCount != g_rxChunkCount) {
          g_rxChunkActive = true;
          g_rxMessageId = msgId;
          g_rxChunkCount = chunkCount;
          g_rxChunks.clear();
          g_rxChunks.resize(chunkCount);
        }

        g_rxChunks[chunkIndex] = String((const char*)(p + kChunkHeaderBytes)).substring(0, payloadLen);

        bool complete = true;
        for (uint8_t i = 0; i < g_rxChunkCount; ++i) {
          if (g_rxChunks[i].length() == 0) {
            complete = false;
            break;
          }
        }

        if (!complete) return;

        String assembled;
        for (uint8_t i = 0; i < g_rxChunkCount; ++i) {
          assembled += g_rxChunks[i];
        }
        g_pendingRequest = assembled;
        g_hasPendingRequest = true;

        g_rxChunkActive = false;
        g_rxChunks.clear();
        return;
      }
    }

    g_pendingRequest = String(raw.c_str());
    g_hasPendingRequest = true;
  }
};

}  // namespace

void bleSetup(
  const char* deviceId,
  const char* firmwareVersion,
  BleStatusJsonProvider statusProvider,
  BleNowUnixProvider nowUnixProvider,
  BleCommandRouter commandRouter
) {
  g_statusProvider = statusProvider;
  g_nowUnixProvider = nowUnixProvider;
  g_commandRouter = commandRouter;

  g_deviceName = "Microclimate";
  if (deviceId && deviceId[0] != '\0') {
    g_deviceName += "-";
    g_deviceName += deviceId;
  }

  BLEDevice::init(g_deviceName.c_str());

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  BLEService* service = g_server->createService(kServiceUuid);

  g_requestChar = service->createCharacteristic(
    kRequestUuid,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  g_requestChar->setCallbacks(new RequestCallbacks());

  g_responseChar = service->createCharacteristic(
    kResponseUuid,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  g_responseChar->addDescriptor(new BLE2902());

  g_statusChar = service->createCharacteristic(
    kStatusUuid,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  g_statusChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);

  if (firmwareVersion && firmwareVersion[0] != '\0') {
    BLEAdvertisementData scanData;
    String mfg = String("FW:") + firmwareVersion;
    scanData.setManufacturerData(mfg.c_str());
    advertising->setScanResponseData(scanData);
  }

  BLEDevice::startAdvertising();

  Serial.printf("[BLE] Service started: %s\n", kServiceUuid);
  Serial.printf("[BLE] Device name: %s\n", g_deviceName.c_str());
}

void bleLoop() {
  if (!g_hasPendingRequest) return;

  const String req = g_pendingRequest;
  g_hasPendingRequest = false;

  const String command = extractJsonStringField(req, "command");
  String correlationId = extractJsonStringField(req, "correlationId");
  const String payloadJson = extractJsonObjectField(req, "payload");

  long timestampUnix = 0;
  const bool hasTimestamp = extractJsonIntField(req, "timestampUnix", timestampUnix);

  if (command.length() == 0) {
    sendResponseNotify(buildErrorResponse(correlationId, "INVALID_REQUEST", "Missing required field: command"));
    return;
  }

  if (!hasTimestamp) {
    sendResponseNotify(buildErrorResponse(correlationId, "INVALID_REQUEST", "Missing required field: timestampUnix"));
    return;
  }

  const unsigned long current = nowUnix();
  const long delta = (long)current - (long)timestampUnix;
  const long absDelta = delta >= 0 ? delta : -delta;
  static const long kMaxAgeSec = 5L * 60L;
  if (absDelta > kMaxAgeSec) {
    sendResponseNotify(buildErrorResponse(correlationId, "STALE_COMMAND", "Command timestamp outside allowed 5-minute window"));
    return;
  }

  if (command == "get_status") {
    sendResponseNotify(buildStatusResponse(correlationId));
    return;
  }

  if (!g_commandRouter) {
    sendResponseNotify(buildErrorResponse(correlationId, "NOT_READY", "Command router not configured"));
    return;
  }

  String responseType;
  String responseDataJson;
  String responseMessage;
  String errorCode;
  String errorMessage;

  const bool ok = g_commandRouter(
    command,
    payloadJson,
    responseType,
    responseDataJson,
    responseMessage,
    errorCode,
    errorMessage
  );

  if (ok) {
    if (responseType.length() == 0) responseType = "ack";
    sendResponseNotify(buildOkResponse(correlationId, responseType, responseDataJson, responseMessage));
    return;
  }

  if (errorCode.length() == 0) errorCode = "COMMAND_FAILED";
  if (errorMessage.length() == 0) errorMessage = "Command failed";
  sendResponseNotify(buildErrorResponse(correlationId, errorCode, errorMessage));
}

bool bleIsConnected() {
  return g_bleConnected;
}

void blePublishTelemetryEvent(const sensor_data_message_t& sample, const uint8_t mac[6]) {
  if (!g_bleConnected || !g_statusChar) return;

  String payload;
  payload.reserve(360);
  payload += "{\"type\":\"node_telemetry_event\",\"timestampUnix\":";
  payload += String(nowUnix());
  payload += ",\"data\":{";
  payload += "\"nodeId\":\"";
  payload += jsonEscape(String(sample.nodeId));
  payload += "\",";
  payload += "\"mac\":\"";
  payload += macToString(mac);
  payload += "\",";
  payload += "\"sensorId\":";
  payload += String((unsigned)sample.sensorId);
  payload += ",";
  payload += "\"sensorType\":\"";
  payload += jsonEscape(String(sample.sensorType));
  payload += "\",";
  payload += "\"sensorLabel\":\"";
  payload += jsonEscape(String(sample.sensorLabel));
  payload += "\",";
  payload += "\"value\":";
  payload += String(sample.value, 3);
  payload += ",";
  payload += "\"nodeTimestamp\":";
  payload += String((unsigned long)sample.nodeTimestamp);
  payload += ",";
  payload += "\"qualityFlags\":";
  payload += String((unsigned)sample.qualityFlags);
  payload += "}}";

  sendStatusNotify(payload);
}
