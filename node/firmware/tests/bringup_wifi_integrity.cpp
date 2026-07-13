#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#ifndef WIFI_DIAG_AP_SSID
#define WIFI_DIAG_AP_SSID "ESP32-WIFI-DIAG"
#endif

#ifndef WIFI_DIAG_AP_PASS
#define WIFI_DIAG_AP_PASS "diag1234"
#endif

#ifndef WIFI_DIAG_AP_CHANNEL
#define WIFI_DIAG_AP_CHANNEL 1
#endif

#ifndef WIFI_DIAG_UDP_PORT
#define WIFI_DIAG_UDP_PORT 3333
#endif

#ifndef WIFI_DIAG_BEACON_PORT
#define WIFI_DIAG_BEACON_PORT 3334
#endif

#ifndef WIFI_DIAG_STATS_MS
#define WIFI_DIAG_STATS_MS 5000
#endif

#ifndef WIFI_DIAG_SCAN_MS
#define WIFI_DIAG_SCAN_MS 10000
#endif

#ifndef WIFI_DIAG_BEACON_MS
#define WIFI_DIAG_BEACON_MS 2000
#endif

#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID ""
#endif

#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS ""
#endif

namespace {

WiFiUDP gUdp;

uint32_t gRxPackets = 0;
uint32_t gTxPackets = 0;
uint32_t gRxBytes = 0;
uint32_t gTxBytes = 0;
uint32_t gScanCount = 0;

unsigned long gLastStatsMs = 0;
unsigned long gLastScanMs = 0;
unsigned long gLastBeaconMs = 0;

void printNetworkInfo() {
  Serial.println("=== WiFi Integrity Bring-up ===");
  Serial.printf("AP SSID: %s\n", WIFI_DIAG_AP_SSID);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("UDP echo port: %d\n", WIFI_DIAG_UDP_PORT);
  Serial.printf("Beacon port: %d\n", WIFI_DIAG_BEACON_PORT);
  Serial.println("Send any UDP payload to port 3333; board echoes it back.");
  Serial.println("Commands: S=immediate scan, R=reset counters, H=help");
}

void printStats() {
  Serial.printf("[WIFI] RX_PKT=%lu TX_PKT=%lu RX_BYTES=%lu TX_BYTES=%lu SCANS=%lu STA=%s RSSI=%d\n",
                static_cast<unsigned long>(gRxPackets),
                static_cast<unsigned long>(gTxPackets),
                static_cast<unsigned long>(gRxBytes),
                static_cast<unsigned long>(gTxBytes),
                static_cast<unsigned long>(gScanCount),
                WiFi.status() == WL_CONNECTED ? "CONNECTED" : "NOT_CONNECTED",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

void runScan() {
  int count = WiFi.scanNetworks(false, true);
  gScanCount++;
  Serial.printf("[WIFI] Scan #%lu found %d network(s)\n",
                static_cast<unsigned long>(gScanCount), count);
  const int maxPrint = (count < 5) ? count : 5;
  for (int i = 0; i < maxPrint; ++i) {
    Serial.printf("  %d) SSID=%s RSSI=%d CH=%d\n",
                  i + 1,
                  WiFi.SSID(i).c_str(),
                  WiFi.RSSI(i),
                  WiFi.channel(i));
  }
  WiFi.scanDelete();
}

void maybeHandleSerialCommand() {
  if (Serial.available() <= 0) {
    return;
  }

  char c = static_cast<char>(Serial.read());
  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - ('a' - 'A'));
  }

  if (c == 'S') {
    runScan();
  } else if (c == 'R') {
    gRxPackets = 0;
    gTxPackets = 0;
    gRxBytes = 0;
    gTxBytes = 0;
    Serial.println("[WIFI] Counters reset");
  } else if (c == 'H') {
    Serial.println("Commands: S=immediate scan, R=reset counters, H=help");
  }
}

void maybeSendBeacon() {
  const unsigned long now = millis();
  if ((now - gLastBeaconMs) < WIFI_DIAG_BEACON_MS) {
    return;
  }
  gLastBeaconMs = now;

  const IPAddress bcast(192, 168, 4, 255);
  const char* msg = "wifi_diag_beacon";
  if (gUdp.beginPacket(bcast, WIFI_DIAG_BEACON_PORT)) {
    const size_t n = gUdp.write(reinterpret_cast<const uint8_t*>(msg), strlen(msg));
    if (n > 0 && gUdp.endPacket() == 1) {
      gTxPackets++;
      gTxBytes += static_cast<uint32_t>(n);
    }
  }
}

void maybeProcessUdpEcho() {
  const int packetLen = gUdp.parsePacket();
  if (packetLen <= 0) {
    return;
  }

  static uint8_t buf[512];
  const int toRead = (packetLen > static_cast<int>(sizeof(buf))) ? static_cast<int>(sizeof(buf)) : packetLen;
  const int n = gUdp.read(buf, toRead);
  if (n <= 0) {
    return;
  }

  gRxPackets++;
  gRxBytes += static_cast<uint32_t>(n);

  IPAddress remoteIp = gUdp.remoteIP();
  uint16_t remotePort = gUdp.remotePort();

  if (gUdp.beginPacket(remoteIp, remotePort)) {
    const size_t wn = gUdp.write(buf, n);
    if (wn == static_cast<size_t>(n) && gUdp.endPacket() == 1) {
      gTxPackets++;
      gTxBytes += static_cast<uint32_t>(wn);
    }
  }

  Serial.printf("[WIFI] UDP RX %d bytes from %s:%u -> echoed\n",
                n,
                remoteIp.toString().c_str(),
                static_cast<unsigned>(remotePort));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(WIFI_DIAG_AP_SSID, WIFI_DIAG_AP_PASS, WIFI_DIAG_AP_CHANNEL);
  if (!apOk) {
    Serial.println("[WIFI] ERROR: softAP start failed");
  }

  if (strlen(WIFI_STA_SSID) > 0) {
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    Serial.printf("[WIFI] Trying STA connect to SSID '%s'...\n", WIFI_STA_SSID);
  } else {
    Serial.println("[WIFI] STA connect skipped (WIFI_STA_SSID empty)");
  }

  if (!gUdp.begin(WIFI_DIAG_UDP_PORT)) {
    Serial.println("[WIFI] ERROR: UDP begin failed");
  }

  printNetworkInfo();
  runScan();
}

void loop() {
  maybeHandleSerialCommand();
  maybeProcessUdpEcho();
  maybeSendBeacon();

  const unsigned long now = millis();
  if ((now - gLastStatsMs) >= WIFI_DIAG_STATS_MS) {
    gLastStatsMs = now;
    printStats();
  }
  if ((now - gLastScanMs) >= WIFI_DIAG_SCAN_MS) {
    gLastScanMs = now;
    runScan();
  }
}
