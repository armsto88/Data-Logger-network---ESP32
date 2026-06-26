#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
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

uint32_t gRxPackets = 0;
uint32_t gRxBytes = 0;
uint32_t gGapLoss = 0;
uint32_t gDuplicates = 0;
uint32_t gOutOfOrder = 0;
uint32_t gAckSentOk = 0;
uint32_t gAckSentFail = 0;
uint32_t gLastSeq = 0;
bool gHaveLastSeq = false;
bool gCaptureEnabled = false;
bool gWindowActive = false;
unsigned long gWindowStartMs = 0;

unsigned long gLastStatsMs = 0;

void resetCounters() {
  gRxPackets = 0;
  gRxBytes = 0;
  gGapLoss = 0;
  gDuplicates = 0;
  gOutOfOrder = 0;
  gAckSentOk = 0;
  gAckSentFail = 0;
  gLastSeq = 0;
  gHaveLastSeq = false;
}

void startTimedCapture(uint32_t windowMs) {
  resetCounters();
  gWindowStartMs = millis();
  gWindowActive = true;
  gCaptureEnabled = true;
  Serial.printf("[RX] Timed capture started for %lu ms\n", static_cast<unsigned long>(windowMs));
}

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
  if (status == ESP_NOW_SEND_SUCCESS) gAckSentOk++;
  else gAckSentFail++;
}

void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len == static_cast<int>(sizeof(ControlStartPacket))) {
    ControlStartPacket ctrl{};
    memcpy(&ctrl, data, sizeof(ctrl));
    if (ctrl.magic == kMagic && ctrl.type == kTypeCtrlStart) {
      startTimedCapture(ctrl.windowMs);
      Serial.printf("[RX] Trigger received from %s\n", macToStr(mac).c_str());
      return;
    }
  }

  if (!gCaptureEnabled) return;
  if (len != static_cast<int>(sizeof(DataPacket))) return;

  DataPacket pkt{};
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != kMagic || pkt.type != kTypeData) return;

  gRxPackets++;
  gRxBytes += static_cast<uint32_t>(len);

  if (!gHaveLastSeq) {
    gLastSeq = pkt.seq;
    gHaveLastSeq = true;
  } else {
    if (pkt.seq == gLastSeq) {
      gDuplicates++;
    } else if (pkt.seq < gLastSeq) {
      gOutOfOrder++;
    } else {
      if (pkt.seq > (gLastSeq + 1)) {
        gGapLoss += (pkt.seq - gLastSeq - 1);
      }
      gLastSeq = pkt.seq;
    }
  }

  AckPacket ack{};
  ack.magic = kMagic;
  ack.type = kTypeAck;
  ack.seq = pkt.seq;
  ack.txMs = pkt.txMs;
  ack.rxMs = millis();

  ensurePeer(mac);
  esp_err_t e = esp_now_send(mac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
  if (e != ESP_OK) {
    gAckSentFail++;
  }

  Serial.printf("RX seq=%lu from %s len=%d\n",
                static_cast<unsigned long>(pkt.seq),
                macToStr(mac).c_str(),
                len);
}

void printHelp() {
  Serial.println("Commands: G=start local 60s capture, R=reset counters, H=help");
}

void handleSerial() {
  if (Serial.available() <= 0) return;
  char c = static_cast<char>(Serial.read());
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));

  if (c == 'R') {
    resetCounters();
    gCaptureEnabled = false;
    gWindowActive = false;
    Serial.println("Counters reset");
  } else if (c == 'G') {
    startTimedCapture(TEST_WINDOW_MS);
  } else if (c == 'H') {
    printHelp();
  }
}

void printStats() {
  const uint32_t expected = gRxPackets + gGapLoss;
  const float prr = (expected > 0) ? (100.0f * static_cast<float>(gRxPackets) / static_cast<float>(expected)) : 100.0f;
  const float ackOkRate = (gRxPackets > 0) ? (100.0f * static_cast<float>(gAckSentOk) / static_cast<float>(gRxPackets)) : 0.0f;

  Serial.printf("STATS rx_pkt=%lu rx_bytes=%lu gap_loss=%lu dup=%lu ooo=%lu prr_pct=%.1f ack_ok=%lu ack_fail=%lu ack_ok_pct=%.1f\n",
                static_cast<unsigned long>(gRxPackets),
                static_cast<unsigned long>(gRxBytes),
                static_cast<unsigned long>(gGapLoss),
                static_cast<unsigned long>(gDuplicates),
                static_cast<unsigned long>(gOutOfOrder),
                prr,
                static_cast<unsigned long>(gAckSentOk),
                static_cast<unsigned long>(gAckSentFail),
                ackOkRate);
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

  Serial.println("=== ESP-NOW Range RX ===");
  Serial.printf("STA MAC=%s CH=%d\n", WiFi.macAddress().c_str(), ESPNOW_CHANNEL);
  Serial.println("RX capture starts idle. Trigger from TX with 'G' on WROOM, or local 'G' here.");
  printHelp();
}

void loop() {
  handleSerial();

  const unsigned long now = millis();
  if (gWindowActive && (now - gWindowStartMs) >= TEST_WINDOW_MS) {
    gWindowActive = false;
    gCaptureEnabled = false;
    Serial.println("[RX] Timed capture complete");
    printStats();
  }

  if ((now - gLastStatsMs) >= STATS_INTERVAL_MS) {
    gLastStatsMs = now;
    printStats();
  }
}
