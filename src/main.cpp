#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "rtc_manager.h"

const char* ssid = "Logger_Config";
const char* password = "logger123";

WebServer server(80);

void handleRoot() {
    char currentTime[24];
    getRTCTimeString(currentTime, sizeof(currentTime));
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;margin:20px;} input{padding:10px;margin:5px;font-size:16px;} button{padding:15px;font-size:16px;background:#4CAF50;color:white;border:none;border-radius:4px;}</style>";
    html += "<script>";
    html += "function setCurrentTime() {";
    html += "  var now = new Date();";
    html += "  var year = now.getFullYear();";
    html += "  var month = String(now.getMonth() + 1).padStart(2, '0');";
    html += "  var day = String(now.getDate()).padStart(2, '0');";
    html += "  var hour = String(now.getHours()).padStart(2, '0');";
    html += "  var minute = String(now.getMinutes()).padStart(2, '0');";
    html += "  var second = String(now.getSeconds()).padStart(2, '0');";
    html += "  var timeString = year + '-' + month + '-' + day + ' ' + hour + ':' + minute + ':' + second;";
    html += "  document.getElementById('datetime').value = timeString;";
    html += "}";
    html += "window.onload = setCurrentTime;";
    html += "</script>";
    html += "</head><body>";
    html += "<h2>🕐 ESP32 Data Logger - Set RTC Time</h2>";
    html += "<p><strong>Current RTC Time:</strong> <span style='color:#2196F3;'>" + String(currentTime) + "</span></p>";
    html += "<form action='/settime' method='POST'>";
    html += "<p><strong>New Time (auto-detected from your device):</strong></p>";
    html += "<input id='datetime' name='datetime' type='text' style='width:250px;' placeholder='YYYY-MM-DD HH:MM:SS'><br><br>";
    html += "<button type='button' onclick='setCurrentTime()'>🔄 Refresh Current Time</button> ";
    html += "<button type='submit'>✅ Set RTC Time</button>";
    html += "</form>";
    html += "<br><a href='/' style='text-decoration:none; color:#4CAF50;'>🔄 Refresh Page</a>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

void handleSetTime() {
    String dt = server.arg("datetime");
    int yy, mm, dd, hh, mi, ss;
    if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss) == 6) {
        if (setRTCTime(yy, mm, dd, hh, mi, ss)) {
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<style>body{font-family:Arial;margin:20px;text-align:center;} .success{color:#4CAF50;font-size:24px;} .button{padding:15px;margin:10px;font-size:16px;background:#4CAF50;color:white;text-decoration:none;border-radius:4px;display:inline-block;}</style>";
            html += "</head><body>";
            html += "<div class='success'>✅ RTC Time Updated Successfully!</div>";
            html += "<p>New time set to: <strong>" + dt + "</strong></p>";
            html += "<a href='/' class='button'>🏠 Back to Main Page</a>";
            html += "</body></html>";
            server.send(200, "text/html", html);
        } else {
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<style>body{font-family:Arial;margin:20px;text-align:center;} .error{color:#f44336;font-size:24px;} .button{padding:15px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}</style>";
            html += "</head><body>";
            html += "<div class='error'>❌ Failed to Set RTC Time</div>";
            html += "<p>There was an error setting the time. Please try again.</p>";
            html += "<a href='/' class='button'>🔙 Try Again</a>";
            html += "</body></html>";
            server.send(400, "text/html", html);
        }
    } else {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:Arial;margin:20px;text-align:center;} .error{color:#f44336;font-size:24px;} .button{padding:15px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}</style>";
        html += "</head><body>";
        html += "<div class='error'>⚠️ Invalid Time Format</div>";
        html += "<p>Please use format: YYYY-MM-DD HH:MM:SS</p>";
        html += "<p>You entered: <em>" + dt + "</em></p>";
        html += "<a href='/' class='button'>🔙 Try Again</a>";
        html += "</body></html>";
        server.send(400, "text/html", html);
    }
}

void setup() {
    Serial.begin(115200);
    
    Serial.println("Starting RTC setup...");
    setupRTC();
    
    // Auto-set RTC time to current date/time (November 3, 2025)
    Serial.println("Auto-setting RTC time...");
    bool timeSet = setRTCTime(2025, 11, 3, 14, 30, 0);  // Nov 3, 2025 2:30 PM
    if (timeSet) {
        Serial.println("✅ RTC time auto-set successfully!");
    } else {
        Serial.println("❌ Failed to auto-set RTC time");
    }
    
    // Verify RTC is working
    delay(1000);
    char timeStr[32];
    getRTCTimeString(timeStr, sizeof(timeStr));
    Serial.print("Current RTC Time: ");
    Serial.println(timeStr);
    
    Serial.println("Starting WiFi AP...");
    WiFi.softAP(ssid, password);
    Serial.print("SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
    
    server.on("/", handleRoot);
    server.on("/set-time", HTTP_POST, handleSetTime);
    server.begin();
    Serial.println("✅ Web server started!");
}

void loop() {
    server.handleClient();
    
    // Print current RTC time every 10 seconds
    static unsigned long lastTimeCheck = 0;
    if (millis() - lastTimeCheck > 10000) {
        char timeBuffer[24];
        getRTCTimeString(timeBuffer, sizeof(timeBuffer));
        Serial.print("Current RTC Time: ");
        Serial.println(timeBuffer);
        lastTimeCheck = millis();
    }
}