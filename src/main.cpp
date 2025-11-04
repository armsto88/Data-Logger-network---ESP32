#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "rtc_manager.h"
#include "sd_manager.h"
#include "espnow_manager.h"

// Device identification
const char* DEVICE_ID = "001";  // Simplified ID
const char* BASE_SSID = "Logger";
String ssid = String(BASE_SSID) + String(DEVICE_ID);  // Will be "Logger001"
const char* password = "logger123";

WebServer server(80);

void handleRoot() {
    char currentTime[24];
    getRTCTimeString(currentTime, sizeof(currentTime));
    String csvStats = getCSVStats();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5;}";
    html += ".container{max-width:500px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += "h1{color:#333;text-align:center;margin-bottom:30px;}";
    html += ".status-section{background:#e3f2fd;padding:15px;border-radius:5px;margin:20px 0;text-align:center;}";
    html += ".data-section{background:#fff3e0;padding:20px;border-radius:5px;margin:20px 0;text-align:center;}";
    html += ".device-info{background:#e8f5e8;padding:10px;border-radius:5px;margin-bottom:20px;text-align:center;}";
    html += ".download-btn{background:linear-gradient(45deg, #4CAF50, #45a049);color:white;padding:15px 30px;font-size:18px;border:none;border-radius:8px;cursor:pointer;text-decoration:none;display:inline-block;margin:10px;box-shadow:0 4px 8px rgba(0,0,0,0.2);transition:all 0.3s;}";
    html += ".download-btn:hover{transform:translateY(-2px);box-shadow:0 6px 12px rgba(0,0,0,0.3);}";
    html += ".download-icon{font-size:20px;margin-right:8px;}";
    html += ".settings-toggle{background:#2196F3;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;margin:10px 0;font-size:14px;}";
    html += ".settings-panel{display:none;background:#f8f9fa;padding:20px;border-radius:5px;margin:20px 0;border:1px solid #ddd;}";
    html += "input{padding:12px;margin:8px 0;font-size:16px;width:100%;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}";
    html += "button{padding:12px 20px;font-size:16px;border:none;border-radius:4px;cursor:pointer;margin:5px 0;width:100%;}";
    html += ".btn-primary{background:#4CAF50;color:white;} .btn-primary:hover{background:#45a049;}";
    html += ".btn-secondary{background:#2196F3;color:white;} .btn-secondary:hover{background:#1976D2;}";
    html += ".refresh-link{display:block;text-align:center;margin-top:20px;color:#4CAF50;text-decoration:none;}";
    html += "</style>";
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
    html += "function toggleSettings() {";
    html += "  var panel = document.getElementById('settings-panel');";
    html += "  var btn = document.getElementById('settings-btn');";
    html += "  if (panel.style.display === 'none' || panel.style.display === '') {";
    html += "    panel.style.display = 'block';";
    html += "    btn.textContent = 'Hide RTC Settings';";
    html += "  } else {";
    html += "    panel.style.display = 'none';";
    html += "    btn.textContent = 'Show RTC Settings';";
    html += "  }";
    html += "}";
    html += "window.onload = setCurrentTime;";
    html += "</script>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>ESP32 Data Logger</h1>";
    
    html += "<div class='device-info'>";
    html += "<strong>Device ID: " + String(DEVICE_ID) + "</strong><br>";
    html += "<small>WiFi Network: " + ssid + "</small>";
    html += "</div>";
    
    html += "<div class='status-section'>";
    html += "<strong>Current RTC Time:</strong><br>";
    html += "<span style='font-size:18px;color:#1976D2;'>" + String(currentTime) + "</span>";
    html += "</div>";
    
    html += "<div class='data-section'>";
    html += "<h3 style='margin-top:0;color:#e65100;'>📊 Data Logging</h3>";
    html += "<p style='font-size:16px;margin:10px 0;'><strong>Status:</strong> " + csvStats + "</p>";
    html += "<a href='/download-csv' class='download-btn'>";
    html += "<span class='download-icon'>⬇️</span>Download CSV Data";
    html += "</a>";
    html += "<p style='font-size:12px;color:#666;margin:10px 0;'>Click to download all logged sensor data</p>";
    html += "</div>";
    
    html += "<div class='data-section' style='background:#f3e5f5;'>";
    html += "<h3 style='margin-top:0;color:#7b1fa2;'>📡 Node Discovery & Pairing</h3>";
    html += "<p style='font-size:14px;margin:10px 0;'><strong>Mothership MAC:</strong> " + getMothershipsMAC() + "</p>";
    
    // Get node statistics
    auto allNodes = getRegisteredNodes();
    auto unpairedNodes = getUnpairedNodes();
    auto pairedNodes = getPairedNodes();
    int deployedNodes = 0;
    for (const auto& node : allNodes) {
        if (node.state == DEPLOYED && node.isActive) deployedNodes++;
    }
    
    html += "<div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin:15px 0;text-align:center;'>";
    html += "<div style='background:#e8f5e8;padding:10px;border-radius:5px;'><strong>Deployed</strong><br><span style='font-size:18px;color:#4CAF50;'>" + String(deployedNodes) + "</span></div>";
    html += "<div style='background:#fff3e0;padding:10px;border-radius:5px;'><strong>Paired</strong><br><span style='font-size:18px;color:#f57c00;'>" + String(pairedNodes.size()) + "</span></div>";
    html += "<div style='background:#ffebee;padding:10px;border-radius:5px;'><strong>Unpaired</strong><br><span style='font-size:18px;color:#f44336;'>" + String(unpairedNodes.size()) + "</span></div>";
    html += "</div>";
    
    // Discovery section
    html += "<form action='/discover-nodes' method='POST' style='margin:15px 0;'>";
    html += "<button type='submit' style='background:#2196F3;color:white;padding:12px 20px;border:none;border-radius:4px;font-size:16px;width:100%;margin:5px 0;'>🔍 Discover New Nodes</button>";
    html += "</form>";
    
    // Unpaired nodes section
    if (unpairedNodes.size() > 0) {
        html += "<div style='background:#ffebee;padding:15px;border-radius:5px;margin:15px 0;'>";
        html += "<h4 style='margin-top:0;color:#d32f2f;'>🔴 Unpaired Nodes (" + String(unpairedNodes.size()) + ")</h4>";
        html += "<form action='/pair-nodes' method='POST'>";
        
        for (const auto& node : unpairedNodes) {
            html += "<div style='border:1px solid #ddd;border-radius:4px;padding:10px;margin:8px 0;background:white;'>";
            html += "<input type='checkbox' name='selected_nodes' value='" + node.nodeId + "' style='margin-right:8px;'>";
            html += "<strong>" + node.nodeId + "</strong> (" + node.nodeType + ")<br>";
            html += "<small>MAC: ";
            for (int i = 0; i < 6; i++) {
                html += String(node.mac[i], HEX);
                if (i < 5) html += ":";
            }
            html += " | Last seen: " + String((millis() - node.lastSeen) / 1000) + "s ago</small><br>";
            html += "<label>Interval: <select name='interval_" + node.nodeId + "' style='padding:4px;margin:4px 0;'>";
            html += "<option value='1'>1 min</option>";
            html += "<option value='5' selected>5 min</option>";
            html += "<option value='10'>10 min</option>";
            html += "<option value='15'>15 min</option>";
            html += "<option value='30'>30 min</option>";
            html += "<option value='60'>60 min</option>";
            html += "</select></label>";
            html += "</div>";
        }
        
        html += "<button type='submit' style='background:#ff9800;color:white;padding:10px 20px;border:none;border-radius:4px;font-size:14px;width:100%;margin:10px 0;'>📋 Pair Selected Nodes</button>";
        html += "</form>";
        html += "</div>";
    }
    
    // Paired nodes section
    if (pairedNodes.size() > 0) {
        html += "<div style='background:#fff8e1;padding:15px;border-radius:5px;margin:15px 0;'>";
        html += "<h4 style='margin-top:0;color:#f57c00;'>� Paired Nodes (" + String(pairedNodes.size()) + ")</h4>";
        html += "<p style='font-size:14px;margin:10px 0;'>Ready for deployment with RTC sync</p>";
        html += "<form action='/deploy-nodes' method='POST'>";
        
        for (const auto& node : pairedNodes) {
            html += "<div style='border:1px solid #ddd;border-radius:4px;padding:10px;margin:8px 0;background:white;'>";
            html += "<input type='checkbox' name='deploy_nodes' value='" + node.nodeId + "' checked style='margin-right:8px;'>";
            html += "<strong>" + node.nodeId + "</strong> (" + node.nodeType + ")<br>";
            html += "<small>Interval: " + String(node.scheduleInterval) + " minutes | ";
            for (int i = 0; i < 6; i++) {
                html += String(node.mac[i], HEX);
                if (i < 5) html += ":";
            }
            html += "</small>";
            html += "</div>";
        }
        
        html += "<button type='submit' style='background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:4px;font-size:16px;width:100%;margin:10px 0;'>🚀 Deploy Selected Nodes</button>";
        html += "</form>";
        html += "</div>";
    }
    
    // Deployed nodes status
    if (deployedNodes > 0) {
        html += "<div style='background:#e8f5e8;padding:15px;border-radius:5px;margin:15px 0;'>";
        html += "<h4 style='margin-top:0;color:#4CAF50;'>🟢 Active Deployed Nodes (" + String(deployedNodes) + ")</h4>";
        html += "<p style='font-size:14px;margin:10px 0;'>Nodes are collecting data and sending to mothership</p>";
        html += "</div>";
    }
    
    html += "</div>";
    
    html += "<button id='settings-btn' class='settings-toggle' onclick='toggleSettings()'>Show RTC Settings</button>";
    
    html += "<div id='settings-panel' class='settings-panel'>";
    html += "<h4>⚙️ RTC Time Configuration</h4>";
    html += "<p style='font-size:14px;color:#666;'>Only needed for initial setup or time correction</p>";
    html += "<form action='/set-time' method='POST'>";
    html += "<label for='datetime'><strong>Set New Time:</strong></label>";
    html += "<input id='datetime' name='datetime' type='text' placeholder='YYYY-MM-DD HH:MM:SS'>";
    html += "<button type='button' class='btn-secondary' onclick='setCurrentTime()'>Auto-Detect Current Time</button>";
    html += "<button type='submit' class='btn-primary'>Set RTC Time</button>";
    html += "</form>";
    html += "</div>";
    
    html += "<a href='/' class='refresh-link'>🔄 Refresh Page</a>";
    html += "</div>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

void handleSetTime() {
    String dt = server.arg("datetime");
    int yy, mm, dd, hh, mi, ss;
    if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss) == 6) {
        if (setRTCTime(yy, mm, dd, hh, mi, ss)) {
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta charset='UTF-8'>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<style>";
            html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
            html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
            html += ".success{color:#4CAF50;font-size:24px;margin-bottom:20px;}";
            html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#4CAF50;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
            html += "</style>";
            html += "</head><body>";
            html += "<div class='container'>";
            html += "<div style='background:#e8f5e8;padding:10px;border-radius:5px;margin-bottom:20px;text-align:center;'>";
            html += "<strong>Device ID: " + String(DEVICE_ID) + "</strong>";
            html += "</div>";
            html += "<div class='success'>SUCCESS: RTC Time Updated!</div>";
            html += "<p>New time set to:<br><strong>" + dt + "</strong></p>";
            html += "<a href='/' class='button'>Back to Main Page</a>";
            html += "</div>";
            html += "</body></html>";
            server.send(200, "text/html", html);
        } else {
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta charset='UTF-8'>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<style>";
            html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
            html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
            html += ".error{color:#f44336;font-size:24px;margin-bottom:20px;}";
            html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
            html += "</style>";
            html += "</head><body>";
            html += "<div class='container'>";
            html += "<div class='error'>ERROR: Failed to Set RTC Time</div>";
            html += "<p>There was an error setting the time. Please try again.</p>";
            html += "<a href='/' class='button'>Try Again</a>";
            html += "</div>";
            html += "</body></html>";
            server.send(500, "text/html", html);
        }
    } else {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
        html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
        html += ".error{color:#ff9800;font-size:24px;margin-bottom:20px;}";
        html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
        html += "</style>";
        html += "</head><body>";
        html += "<div class='container'>";
        html += "<div class='error'>WARNING: Invalid Time Format</div>";
        html += "<p>Please use the format: YYYY-MM-DD HH:MM:SS</p>";
        html += "<p>You entered: <em>" + dt + "</em></p>";
        html += "<a href='/' class='button'>Try Again</a>";
        html += "</div>";
        html += "</body></html>";
        server.send(400, "text/html", html);
    }
}

void handleDownloadCSV() {
    File file = SD.open("/datalog.csv");
    if (!file) {
        server.send(404, "text/plain", "CSV file not found");
        return;
    }
    
    // Set headers for file download
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", "attachment; filename=datalog.csv");
    server.sendHeader("Connection", "close");
    
    // Send the file
    server.streamFile(file, "text/csv");
    file.close();
    
    Serial.println("✅ CSV file downloaded by client");
}

void handleNodeSchedule() {
    String interval = server.arg("interval");
    int intervalMinutes = interval.toInt();
    
    if (intervalMinutes > 0 && intervalMinutes <= 1440) { // Max 24 hours
        if (broadcastSchedule(intervalMinutes)) {
            String html = "<!DOCTYPE html><html><head>";
            html += "<meta charset='UTF-8'>";
            html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
            html += "<style>";
            html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
            html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
            html += ".success{color:#4CAF50;font-size:24px;margin-bottom:20px;}";
            html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
            html += "</style>";
            html += "</head><body>";
            html += "<div class='container'>";
            html += "<div class='success'>📡 Schedule Broadcast Successful!</div>";
            html += "<p>All sensor nodes have been instructed to wake every <strong>" + String(intervalMinutes) + " minutes</strong></p>";
            html += "<p>Node data will be collected automatically</p>";
            html += "<a href='/' class='button'>Back to Dashboard</a>";
            html += "</div>";
            html += "</body></html>";
            server.send(200, "text/html", html);
        } else {
            server.send(500, "text/plain", "Failed to broadcast schedule");
        }
    } else {
        String html = "<!DOCTYPE html><html><head>";
        html += "<meta charset='UTF-8'>";
        html += "<style>";
        html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
        html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
        html += ".error{color:#ff9800;font-size:24px;margin-bottom:20px;}";
        html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
        html += "</style>";
        html += "</head><body>";
        html += "<div class='container'>";
        html += "<div class='error'>⚠️ Invalid Interval</div>";
        html += "<p>Please enter a value between 1 and 1440 minutes (24 hours)</p>";
        html += "<a href='/' class='button'>Try Again</a>";
        html += "</div>";
        html += "</body></html>";
        server.send(400, "text/html", html);
    }
}

void handleDiscoverNodes() {
    Serial.println("🔍 Starting node discovery...");
    sendDiscoveryBroadcast();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='3;url=/'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
    html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += ".success{color:#2196F3;font-size:24px;margin-bottom:20px;}";
    html += ".spinner{border:4px solid #f3f3f3;border-top:4px solid #2196F3;border-radius:50%;width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto;}";
    html += "@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<div class='success'>🔍 Discovery Broadcast Sent!</div>";
    html += "<div class='spinner'></div>";
    html += "<p>Searching for new sensor nodes...</p>";
    html += "<p><small>Redirecting back to dashboard in 3 seconds...</small></p>";
    html += "</div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handlePairNodes() {
    int pairedCount = 0;
    String pairedNodes = "";
    
    // Process selected nodes
    for (int i = 0; i < server.args(); i++) {
        String argName = server.argName(i);
        String argValue = server.arg(i);
        
        if (argName == "selected_nodes") {
            // Get the interval for this node
            String intervalParam = "interval_" + argValue;
            int interval = 5; // default
            
            for (int j = 0; j < server.args(); j++) {
                if (server.argName(j) == intervalParam) {
                    interval = server.arg(j).toInt();
                    break;
                }
            }
            
            if (pairNode(argValue, interval)) {
                pairedCount++;
                pairedNodes += argValue + " (" + String(interval) + " min), ";
            }
        }
    }
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
    html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += ".success{color:#ff9800;font-size:24px;margin-bottom:20px;}";
    html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    
    if (pairedCount > 0) {
        html += "<div class='success'>📋 Nodes Paired Successfully!</div>";
        html += "<p><strong>" + String(pairedCount) + " node(s) paired:</strong></p>";
        html += "<p>" + pairedNodes.substring(0, pairedNodes.length()-2) + "</p>";
        html += "<p>Nodes are ready for deployment with RTC synchronization</p>";
    } else {
        html += "<div class='success'>⚠️ No Nodes Selected</div>";
        html += "<p>Please select at least one node to pair</p>";
    }
    
    html += "<a href='/' class='button'>Back to Dashboard</a>";
    html += "</div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleDeployNodes() {
    std::vector<String> selectedNodes;
    
    // Collect selected nodes
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) == "deploy_nodes") {
            selectedNodes.push_back(server.arg(i));
        }
    }
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:20px;text-align:center;background:#f5f5f5;}";
    html += ".container{max-width:400px;margin:50px auto;background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += ".success{color:#4CAF50;font-size:24px;margin-bottom:20px;}";
    html += ".button{padding:15px 25px;margin:10px;font-size:16px;background:#2196F3;color:white;text-decoration:none;border-radius:4px;display:inline-block;}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    
    if (selectedNodes.size() > 0) {
        char timeStr[32];
        getRTCTimeString(timeStr, sizeof(timeStr));
        
        if (deploySelectedNodes(selectedNodes)) {
            html += "<div class='success'>🚀 Deployment Successful!</div>";
            html += "<p><strong>" + String(selectedNodes.size()) + " node(s) deployed:</strong></p>";
            for (const String& nodeId : selectedNodes) {
                html += "<p>✅ " + nodeId + "</p>";
            }
            html += "<p>RTC Time synchronized: <strong>" + String(timeStr) + "</strong></p>";
            html += "<p>Nodes are now collecting data automatically</p>";
        } else {
            html += "<div class='success'>⚠️ Partial Deployment</div>";
            html += "<p>Some nodes may not have deployed successfully</p>";
            html += "<p>Check serial monitor for details</p>";
        }
    } else {
        html += "<div class='success'>⚠️ No Nodes Selected</div>";
        html += "<p>Please select at least one node to deploy</p>";
    }
    
    html += "<a href='/' class='button'>Back to Dashboard</a>";
    html += "</div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void setup() {
    Serial.begin(115200);
    
    Serial.println("Starting RTC setup...");
    setupRTC();
    
    Serial.println("Starting SD Card setup...");
    setupSD();
    
    Serial.println("Starting WiFi AP...");
    WiFi.softAP(ssid.c_str(), password, 1); // Force channel 1
    Serial.print("SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("Device ID: ");
    Serial.println(DEVICE_ID);
    Serial.print("WiFi Network: ");
    Serial.println(ssid);
    
    delay(1000); // Allow WiFi to stabilize
    Serial.println("Starting ESP-NOW setup...");
    setupESPNOW();
    
    // Check current RTC time (don't auto-overwrite it)
    char timeStr[32];
    getRTCTimeString(timeStr, sizeof(timeStr));
    Serial.print("Current RTC Time: ");
    Serial.println(timeStr);
    
    server.on("/", handleRoot);
    server.on("/set-time", HTTP_POST, handleSetTime);
    server.on("/download-csv", HTTP_GET, handleDownloadCSV);
    server.on("/set-schedule", HTTP_POST, handleNodeSchedule);
    server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
    server.on("/pair-nodes", HTTP_POST, handlePairNodes);
    server.on("/deploy-nodes", HTTP_POST, handleDeployNodes);
    server.begin();
    Serial.println("✅ Web server started!");
}

void loop() {
    server.handleClient();
    espnow_loop(); // Handle ESP-NOW node management
    
    // Print current RTC time every 10 seconds
    static unsigned long lastTimeCheck = 0;
    if (millis() - lastTimeCheck > 10000) {
        char timeBuffer[24];
        getRTCTimeString(timeBuffer, sizeof(timeBuffer));
        Serial.print("Current RTC Time: ");
        Serial.println(timeBuffer);
        lastTimeCheck = millis();
    }
    
    // Log mothership status every 60 seconds (if no nodes are sending data)
    static unsigned long lastMothershipLog = 0;
    if (millis() - lastMothershipLog > 60000) {
        char timeBuffer[24];
        getRTCTimeString(timeBuffer, sizeof(timeBuffer));
        
        // Log mothership status
        String csvRow = String(timeBuffer) + ",MOTHERSHIP," + getMothershipsMAC() + ",STATUS,ACTIVE";
        
        if (logCSVRow(csvRow)) {
            Serial.println("✅ Mothership status logged");
        }
        
        lastMothershipLog = millis();
    }
}