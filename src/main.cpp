#include <Arduino.h>
#include <Wire.h>
#include <DS3232RTC.h>
#include <WiFi.h>
#include <WebServer.h>

#define SDA_PIN     8
#define SCL_PIN     9
#define RTC_SSID    "Logger_Config"
#define RTC_PASS    "logger123"

DS3232RTC rtc;
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
        tmElements_t tm;
        tm.Year = yy - 1970;
        tm.Month = mm;
        tm.Day = dd;
        tm.Hour = hh;
        tm.Minute = mi;
        tm.Second = ss;
        time_t tt = makeTime(tm);
        rtc.set(tt);
        server.send(200, "text/html", "<p>RTC updated!</p><a href='/'>Back</a>");
    } else {
        server.send(400, "text/html", "<p>Invalid format.</p><a href='/'>Back</a>");
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin(SDA_PIN, SCL_PIN);
    rtc.begin();

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