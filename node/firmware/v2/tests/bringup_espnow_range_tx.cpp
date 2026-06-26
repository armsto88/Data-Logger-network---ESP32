#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif
#ifndef SEND_INTERVAL_MS
#define SEND_INTERVAL_MS 100
#endif
#ifndef STATS_INTERVAL_MS
#define STATS_INTERVAL_MS 5000
#endif
#ifndef PAYLOAD_BYTES
#define PAYLOAD_BYTES 64
#endif
#ifndef TEST_WINDOW_MS
#define TEST_WINDOW_MS 60000
#endif
#ifndef START_LEAD_MS
#define START_LEAD_MS 500
#endif

namespace {

constexpr uint32_t kMagic = 0x45534E57; // "ESNW"
constexpr uint8_t kTypeData = 1;
constexpr uint8_t kTypeAck = 2;
constexpr uint8_t kTypeCtrlStart = 3;

struct __attribute__((packed)) DataPacket {
  uint32_t magic;
  uint8_t type;
  uint32_t seq;
  uint32_t txMs;
  uint16_t payloadLen;
  uint8_t payload[PAYLOAD_BYTES];
};

struct __attribute__((packed)) AckPacket {
  uint32_t magic;
  uint8_t type;
  uint32_t seq;
  uint32_t txMs;
  uint32_t rxMs;
};

struct __attribute__((packed)) ControlStartPacket {
  uint32_t magic;
  uint8_t type;
  uint32_t windowMs;
};

const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint32_t gSeq = 0;
uint32_t gSent = 0;
uint32_t gSendCbOk = 0;
uint32_t gSendCbFail = 0;
uint32_t gCtrlSendPending = 0;
uint32_t gAckRx = 0;
uint32_t gAckUnexpected = 0;
uint32_t gRttSamples = 0;
uint64_t gRttSumMs = 0;
uint32_t gRttMinMs = 0xFFFFFFFF;
uint32_t gRttMaxMs = 0;

bool gPaused = false;
bool gWindowActive = false;
unsigned long gWindowStartMs = 0;
unsigned long gDataStartMs = 0;
unsigned long gLastSendMs = 0;
unsigned long gLastStatsMs = 0;

String macToStr(const uint8_t* mac) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

void ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p{};
  memcpy(p.peer_addr, mac, 6);
  p.channel = ESPNOW_CHANNEL;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  (void)mac_addr;
  if (gCtrlSendPending > 0) {
    gCtrlSendPending--;
    return;
  }
  if (status == ESP_NOW_SEND_SUCCESS) gSendCbOk++;
  else gSendCbFail++;
}

void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != static_cast<int>(sizeof(AckPacket))) return;
  AckPacket ack{};
  memcpy(&ack, data, sizeof(ack));
  if (ack.magic != kMagic || ack.type != kTypeAck) return;

  const uint32_t now = millis();
  uint32_t rtt = 0;
  if (now >= ack.txMs) rtt = now - ack.txMs;

  gAckRx++;
  if (ack.seq > gSeq) gAckUnexpected++;

  gRttSamples++;
  gRttSumMs += rtt;
  if (rtt < gRttMinMs) gRttMinMs = rtt;
  if (rtt > gRttMaxMs) gRttMaxMs = rtt;

  Serial.printf("ACK seq=%lu from %s rtt_ms=%lu\n",
                static_cast<unsigned long>(ack.seq),
                macToStr(mac).c_str(),
                static_cast<unsigned long>(rtt));
}

void printHelp() {
  Serial.println("Commands: G=start 60s run, P=pause/resume, R=reset counters, H=help");
}

void resetCounters() {
  gSent = 0;
  gSendCbOk = 0;
  gSendCbFail = 0;
  gCtrlSendPending = 0;
  gAckRx = 0;
  gAckUnexpected = 0;
  gRttSamples = 0;
  gRttSumMs = 0;
  gRttMinMs = 0xFFFFFFFF;
  gRttMaxMs = 0;
}

void sendStartControl() {
  ControlStartPacket pkt{};
  pkt.magic = kMagic;
  pkt.type = kTypeCtrlStart;
  pkt.windowMs = TEST_WINDOW_MS;

  // Send a few times to make trigger robust before timed run starts.
  gCtrlSendPending = 5;
  for (int i = 0; i < 5; ++i) {
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    delay(20);
  }
}

void handleSerial() {
  if (Serial.available() <= 0) return;
  char c = static_cast<char>(Serial.read());
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));

  if (c == 'P') {
    gPaused = !gPaused;
    if (gPaused) gWindowActive = false;
    Serial.printf("TX %s\n", gPaused ? "PAUSED" : "RUNNING");
  } else if (c == 'G') {
    resetCounters();
    sendStartControl();
    gWindowStartMs = millis();
    gDataStartMs = gWindowStartMs + START_LEAD_MS;
    gWindowActive = true;
    gPaused = false;
    gLastSendMs = 0;
    Serial.printf("[TX] Timed run started for %lu ms (data starts after %lu ms lead)\n",
                  static_cast<unsigned long>(TEST_WINDOW_MS),
                  static_cast<unsigned long>(START_LEAD_MS));
  } else if (c == 'R') {
    resetCounters();
    Serial.println("Counters reset");
  } else if (c == 'H') {
    printHelp();
  }
}

void printStats() {
  const float ackRate = (gSent > 0) ? (100.0f * static_cast<float>(gAckRx) / static_cast<float>(gSent)) : 0.0f;
  const float sendOkRate = (gSent > 0) ? (100.0f * static_cast<float>(gSendCbOk) / static_cast<float>(gSent)) : 0.0f;
  const float rttAvg = (gRttSamples > 0) ? (static_cast<float>(gRttSumMs) / static_cast<float>(gRttSamples)) : 0.0f;
  const uint32_t rttMin = (gRttSamples > 0) ? gRttMinMs : 0;
  const uint32_t rttMax = (gRttSamples > 0) ? gRttMaxMs : 0;

  Serial.printf("STATS sent=%lu send_ok=%lu send_fail=%lu send_ok_pct=%.1f ack_rx=%lu ack_pct=%.1f rtt_ms_avg=%.1f rtt_ms_min=%lu rtt_ms_max=%lu unexpected_ack=%lu\n",
                static_cast<unsigned long>(gSent),
                static_cast<unsigned long>(gSendCbOk),
                static_cast<unsigned long>(gSendCbFail),
                sendOkRate,
                static_cast<unsigned long>(gAckRx),
                ackRate,
                rttAvg,
                static_cast<unsigned long>(rttMin),
                static_cast<unsigned long>(rttMax),
                static_cast<unsigned long>(gAckUnexpected));
}

void sendOne() {
  DataPacket pkt{};
  pkt.magic = kMagic;
  pkt.type = kTypeData;
  pkt.seq = ++gSeq;
  pkt.txMs = millis();
  pkt.payloadLen = PAYLOAD_BYTES;
  for (size_t i = 0; i < PAYLOAD_BYTES; ++i) {
    pkt.payload[i] = static_cast<uint8_t>((pkt.seq + i) & 0xFF);
  }

  esp_err_t e = esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  if (e == ESP_OK) {
    gSent++;
  } else {
    gSendCbFail++;
    Serial.printf("esp_now_send error=%d on seq=%lu\n", static_cast<int>(e), static_cast<unsigned long>(pkt.seq));
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  ensurePeer(kBroadcastMac);

  Serial.println("=== ESP-NOW Range TX ===");
  Serial.printf("STA MAC=%s CH=%d interval_ms=%d payload=%d\n",
                WiFi.macAddress().c_str(), ESPNOW_CHANNEL, SEND_INTERVAL_MS, PAYLOAD_BYTES);
  Serial.println("TX starts paused. Use 'G' to start a timed 60s run.");
  gPaused = true;
  printHelp();
}

void loop() {
  handleSerial();

  const unsigned long now = millis();

  if (gWindowActive && (now - gWindowStartMs) >= TEST_WINDOW_MS) {
    gWindowActive = false;
    gPaused = true;
    Serial.println("[TX] Timed run complete");
    printStats();
  }

  if (!gPaused && now >= gDataStartMs && (now - gLastSendMs) >= SEND_INTERVAL_MS) {
    gLastSendMs = now;
    sendOne();
  }

  if ((now - gLastStatsMs) >= STATS_INTERVAL_MS) {
    gLastStatsMs = now;
    printStats();
  }
}
