#include <Arduino.h>
#include <WiFi.h>

static const char* kSsid = "Logger001_TEST";
static const char* kPass = "logger123";

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("[AP-ONLY] boot");
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  bool ok = WiFi.softAP(kSsid, kPass, 1, false, 4);
  if (!ok) {
    Serial.println("[AP-ONLY] channel 1 failed, trying channel 11");
    ok = WiFi.softAP(kSsid, kPass, 11, false, 4);
  }

  Serial.printf("[AP-ONLY] softAP start: %s\n", ok ? "OK" : "FAIL");
  Serial.print("[AP-ONLY] SSID: ");
  Serial.println(WiFi.softAPSSID());
  Serial.print("[AP-ONLY] IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("[AP-ONLY] MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("[AP-ONLY] channel: ");
  Serial.println(WiFi.channel());
}

void loop() {
  static unsigned long last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.print("[AP-ONLY] alive, station count=");
    Serial.println(WiFi.softAPgetStationNum());
  }
}
