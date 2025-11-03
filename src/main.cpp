#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "rtc_manager.h"

#define RTC_SSID    "Logger_Config"
#define RTC_PASS    "logger123"
WebServer server(80);

void handleRoot() {
    server.send(200, "text/html",
        "<h2>Set RTC Time</h2>"
        "<form action='/settime' method='POST'>"
        "Time (YYYY-MM-DD HH:MM:SS):<br>"
        "<input name='datetime' type='text'>"
        "<input type='submit' value='Set Time'>"
        "</form>");
}

void handleSetTime() {
    String dt = server.arg("datetime");
    int yy, mm, dd, hh, mi, ss;
    if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss) == 6) {
        // For now, we'll just acknowledge the request
        // You'll need to add a setTime function to rtc_manager if you want to set time via web
        server.send(200, "text/html", "<p>Time setting not implemented in modular design yet.</p><a href='/'>Back</a>");
    } else {
        server.send(400, "text/html", "<p>Invalid format.</p><a href='/'>Back</a>");
    }
}

void setup() {
    Serial.begin(115200);
    setupRTC(); // Use the modular RTC setup

    WiFi.softAP(RTC_SSID, RTC_PASS);
    Serial.print("SoftAP IP: "); Serial.println(WiFi.softAPIP());

    server.on("/", handleRoot);
    server.on("/settime", HTTP_POST, handleSetTime);
    server.begin();

    Serial.println("Web server started!");
}

void loop() {
    server.handleClient();
}