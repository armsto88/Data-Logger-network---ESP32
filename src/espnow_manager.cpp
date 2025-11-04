#include "espnow_manager.h"
#include "sd_manager.h"
#include "rtc_manager.h"
#include <esp_now.h>
#include <WiFi.h>
#include "config.h"
#include <vector>

// External reference to DEVICE_ID from main.cpp
extern const char* DEVICE_ID;

// Structure to send schedule commands to nodes
typedef struct struct_schedule_command {
    char command[16];        // "SET_SCHEDULE"
    int intervalMinutes;     // Alarm interval in minutes
    char mothership_id[16];  // Mothership identifier
} struct_schedule_command;

std::vector<NodeInfo> registeredNodes;

// Add or update a node in the registry
void registerNode(const uint8_t* mac, const char* nodeId, const char* nodeType = "unknown", NodeState state = UNPAIRED) {
    bool found = false;
    for (auto& node : registeredNodes) {
        if (memcmp(node.mac, mac, 6) == 0) {
            node.lastSeen = millis();
            node.isActive = true;
            // Only update type and state if not already deployed
            if (node.state != DEPLOYED) {
                node.nodeType = String(nodeType);
                node.state = state;
            }
            found = true;
            break;
        }
    }
    
    if (!found) {
        NodeInfo newNode;
        memcpy(newNode.mac, mac, 6);
        newNode.nodeId = String(nodeId);
        newNode.nodeType = String(nodeType);
        newNode.lastSeen = millis();
        newNode.isActive = true;
        newNode.state = state;
        newNode.scheduleInterval = 5; // default 5 minutes
        registeredNodes.push_back(newNode);
        
        // Add node as ESP-NOW peer for sending pairing commands
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, mac, 6);
        peerInfo.channel = 1;
        peerInfo.encrypt = false;
        
        // Remove peer if it already exists, then add
        esp_now_del_peer(mac);
        if (esp_now_add_peer(&peerInfo) == ESP_OK) {
            Serial.print("✅ Node added as ESP-NOW peer: ");
            Serial.println(nodeId);
        } else {
            Serial.print("❌ Failed to add node as peer: ");
            Serial.println(nodeId);
        }
        
        Serial.print("📡 New node discovered: ");
        Serial.print(nodeId);
        Serial.print(" (");
        Serial.print(nodeType);
        Serial.print(") - ");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", mac[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.println();
    }
}

// Get the current state of a node by nodeId
NodeState getNodeState(const char* nodeId) {
    for (const auto& node : registeredNodes) {
        if (node.nodeId == String(nodeId)) {
            return node.state;
        }
    }
    return UNPAIRED; // Default state if node not found
}

// Callback when data is received from sensor nodes
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingBytes, int len) {
    // Check message type by length
    if (len == sizeof(sensor_data_message_t)) {
        sensor_data_message_t incomingData;
        memcpy(&incomingData, incomingBytes, sizeof(incomingData));
        
        // Register/update the node as deployed
        registerNode(mac, incomingData.nodeId, incomingData.sensorType, DEPLOYED);
        
        Serial.print("📊 Data from ");
        Serial.print(incomingData.nodeId);
        Serial.print(" (");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", mac[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.print("): ");
        Serial.print(incomingData.sensorType);
        Serial.print(" = ");
        Serial.println(incomingData.value);
        
        // Get current RTC time for logging
        char timeBuffer[24];
        getRTCTimeString(timeBuffer, sizeof(timeBuffer));
        
        // Create CSV entry with node data
        String macStr = "";
        for (int i = 0; i < 6; i++) {
            macStr += String(mac[i], HEX);
            if (i < 5) macStr += ":";
        }
        
        String csvRow = String(timeBuffer) + "," + 
                        String(incomingData.nodeId) + "," + 
                        macStr + "," +
                        String(incomingData.sensorType) + "," + 
                        String(incomingData.value);
        
        if (logCSVRow(csvRow)) {
            Serial.println("✅ Node data logged to SD card");
        } else {
            Serial.println("❌ Failed to log node data");
        }
    }
    else if (len == sizeof(discovery_message_t)) {
        discovery_message_t discovery;
        memcpy(&discovery, incomingBytes, sizeof(discovery));
        
        if (strcmp(discovery.command, "DISCOVER_REQUEST") == 0) {
            Serial.print("🔍 Discovery request from: ");
            Serial.print(discovery.nodeId);
            Serial.print(" (");
            Serial.print(discovery.nodeType);
            Serial.println(")");
            
            // Register as unpaired node
            registerNode(mac, discovery.nodeId, discovery.nodeType, UNPAIRED);
            
            // Send discovery response as broadcast so sensor node can receive it
            discovery_response_t response;
            strcpy(response.command, "DISCOVER_RESPONSE");
            strcpy(response.mothership_id, DEVICE_ID);
            response.acknowledged = true;
            
            uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcastAddress, (uint8_t*)&response, sizeof(response));
            Serial.println("📡 Sent discovery response");
        }
    }
    else if (len == sizeof(pairing_request_t)) {
        pairing_request_t request;
        memcpy(&request, incomingBytes, sizeof(request));
        
        if (strcmp(request.command, "PAIRING_REQUEST") == 0) {
            Serial.print("📞 Pairing status request from: ");
            Serial.println(request.nodeId);
            
            // Check if this node is in our paired list
            NodeState currentState = getNodeState(request.nodeId);
            
            // Send pairing response
            pairing_response_t response;
            strcpy(response.command, "PAIRING_RESPONSE");
            strcpy(response.nodeId, request.nodeId);
            response.isPaired = (currentState == PAIRED || currentState == DEPLOYED);
            response.alarmInterval = 30; // Default for now
            
            // Send back to specific node (use broadcast for now)
            uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            esp_now_send(broadcastAddress, (uint8_t*)&response, sizeof(response));
            
            Serial.print("📤 Sent pairing response - isPaired: ");
            Serial.println(response.isPaired ? "true" : "false");
        }
    }
    else if (len == sizeof(time_sync_request_t)) {
        time_sync_request_t request;
        memcpy(&request, incomingBytes, sizeof(request));
        
        if (strcmp(request.command, "REQUEST_TIME") == 0) {
            Serial.print("⏰ Time sync request from: ");
            Serial.println(request.nodeId);
            
            // Send current time back to the requesting node
            sendTimeSync(mac, request.nodeId);
        }
    }
}

void setupESPNOW() {
    delay(100); // Small delay for WiFi stability
    
    // ESP-NOW initialization (works with AP mode)
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    
    // Register for receive callback
    esp_now_register_recv_cb(OnDataRecv);
    
    // Add broadcast peer for receiving discovery requests
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1; // Force channel 1
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add broadcast peer");
    } else {
        Serial.println("✅ Broadcast peer added for discovery");
    }
    
    // Add known sensor nodes as peers for reliable communication
    for (int i = 0; i < NUM_KNOWN_SENSORS; i++) {
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, KNOWN_SENSOR_NODES[i], 6);
        peerInfo.channel = 1;
        peerInfo.encrypt = false;
        
        // Remove peer if it already exists, then add
        esp_now_del_peer(KNOWN_SENSOR_NODES[i]);
        if (esp_now_add_peer(&peerInfo) == ESP_OK) {
            Serial.print("✅ Preloaded sensor node peer: ");
            for (int j = 0; j < 6; j++) {
                Serial.printf("%02X", KNOWN_SENSOR_NODES[i][j]);
                if (j < 5) Serial.print(":");
            }
            Serial.println();
        } else {
            Serial.print("❌ Failed to add preloaded peer: ");
            for (int j = 0; j < 6; j++) {
                Serial.printf("%02X", KNOWN_SENSOR_NODES[i][j]);
                if (j < 5) Serial.print(":");
            }
            Serial.println();
        }
    }
    
    Serial.println("ESP-NOW initialized successfully");
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
}

void espnow_loop() {
    // Send discovery broadcasts every 30 seconds
    static unsigned long lastDiscoveryBroadcast = 0;
    unsigned long currentTime = millis();
    
    if (currentTime - lastDiscoveryBroadcast > 30000) { // 30 seconds
        Serial.println("🔍 Sending automatic discovery broadcast...");
        if (sendDiscoveryBroadcast()) {
            Serial.println("✅ Discovery broadcast sent successfully");
        } else {
            Serial.println("❌ Failed to send discovery broadcast");
        }
        lastDiscoveryBroadcast = currentTime;
    }
    
    // Mark nodes as inactive if not seen for 5 minutes
    for (auto& node : registeredNodes) {
        if (currentTime - node.lastSeen > 300000) { // 5 minutes
            if (node.isActive) {
                node.isActive = false;
                Serial.print("⚠️ Node ");
                Serial.print(node.nodeId);
                Serial.println(" marked as inactive");
            }
        }
    }
}

// Broadcast schedule command to all registered nodes
bool broadcastSchedule(int intervalMinutes) {
    struct_schedule_command command;
    strcpy(command.command, "SET_SCHEDULE");
    command.intervalMinutes = intervalMinutes;
    strcpy(command.mothership_id, "MOTHERSHIP001");
    
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &command, sizeof(command));
    
    if (result == ESP_OK) {
        Serial.print("📡 Broadcast schedule command: ");
        Serial.print(intervalMinutes);
        Serial.println(" minutes to all nodes");
        return true;
    } else {
        Serial.println("❌ Failed to broadcast schedule command");
        return false;
    }
}

// Send current time to a specific node
bool sendTimeSync(const uint8_t* mac, const char* nodeId) {
    time_sync_response_t response;
    strcpy(response.command, "TIME_SYNC");
    strcpy(response.mothership_id, "MOTHERSHIP001");
    
    // Get current time from mothership RTC
    char timeBuffer[24];
    getRTCTimeString(timeBuffer, sizeof(timeBuffer));
    
    // Parse the time string to extract components
    // Format: "YYYY-MM-DD HH:MM:SS"
    int year, month, day, hour, minute, second;
    if (sscanf(timeBuffer, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        response.year = year;
        response.month = month;
        response.day = day;
        response.hour = hour;
        response.minute = minute;
        response.second = second;
        
        esp_err_t result = esp_now_send(mac, (uint8_t *) &response, sizeof(response));
        
        if (result == ESP_OK) {
            Serial.print("✅ Time sync sent to ");
            Serial.print(nodeId);
            Serial.print(": ");
            Serial.println(timeBuffer);
            return true;
        } else {
            Serial.print("❌ Failed to send time sync to ");
            Serial.println(nodeId);
            return false;
        }
    } else {
        Serial.println("❌ Failed to parse mothership time for sync");
        return false;
    }
}

// Send discovery broadcast to find unpaired nodes
bool sendDiscoveryBroadcast() {
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    discovery_response_t broadcast;
    strcpy(broadcast.command, "DISCOVERY_SCAN");
    strcpy(broadcast.mothership_id, DEVICE_ID);
    broadcast.acknowledged = false;
    
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&broadcast, sizeof(broadcast));
    
    if (result == ESP_OK) {
        Serial.println("📡 Discovery broadcast sent");
        return true;
    } else {
        Serial.println("❌ Failed to send discovery broadcast");
        return false;
    }
}

// Pair a specific node
bool pairNode(const String& nodeId, int scheduleInterval) {
    for (auto& node : registeredNodes) {
        if (node.nodeId == nodeId && node.state == UNPAIRED) {
            node.state = PAIRED;
            node.scheduleInterval = scheduleInterval;
            
            pairing_command_t pairCmd;
            strcpy(pairCmd.command, "PAIR_NODE");
            strcpy(pairCmd.nodeId, nodeId.c_str());
            pairCmd.scheduleInterval = scheduleInterval;
            strcpy(pairCmd.mothership_id, DEVICE_ID);
            
            esp_err_t result = esp_now_send(node.mac, (uint8_t*)&pairCmd, sizeof(pairCmd));
            
            if (result == ESP_OK) {
                Serial.print("✅ Node paired: ");
                Serial.print(nodeId);
                Serial.print(" (");
                Serial.print(scheduleInterval);
                Serial.println(" min interval)");
                return true;
            } else {
                Serial.print("❌ Failed to pair node: ");
                Serial.println(nodeId);
                node.state = UNPAIRED; // revert on failure
                return false;
            }
        }
    }
    return false;
}

// Deploy selected paired nodes with RTC time and schedule
bool deploySelectedNodes(const std::vector<String>& nodeIds) {
    bool allSuccess = true;
    
    for (const String& nodeId : nodeIds) {
        for (auto& node : registeredNodes) {
            if (node.nodeId == nodeId && node.state == PAIRED) {
                deployment_command_t deployCmd;
                strcpy(deployCmd.command, "DEPLOY_NODE");
                strcpy(deployCmd.nodeId, nodeId.c_str());
                strcpy(deployCmd.mothership_id, DEVICE_ID);
                deployCmd.scheduleInterval = node.scheduleInterval;
                
                // Get current RTC time
                char timeBuffer[24];
                getRTCTimeString(timeBuffer, sizeof(timeBuffer));
                
                int year, month, day, hour, minute, second;
                if (sscanf(timeBuffer, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
                    deployCmd.year = year;
                    deployCmd.month = month;
                    deployCmd.day = day;
                    deployCmd.hour = hour;
                    deployCmd.minute = minute;
                    deployCmd.second = second;
                    
                    esp_err_t result = esp_now_send(node.mac, (uint8_t*)&deployCmd, sizeof(deployCmd));
                    
                    if (result == ESP_OK) {
                        node.state = DEPLOYED;
                        Serial.print("🚀 Node deployed: ");
                        Serial.print(nodeId);
                        Serial.print(" with time: ");
                        Serial.println(timeBuffer);
                    } else {
                        Serial.print("❌ Failed to deploy node: ");
                        Serial.println(nodeId);
                        allSuccess = false;
                    }
                } else {
                    Serial.println("❌ Failed to parse time for deployment");
                    allSuccess = false;
                }
                break;
            }
        }
    }
    
    return allSuccess;
}

// Get unpaired nodes
std::vector<NodeInfo> getUnpairedNodes() {
    std::vector<NodeInfo> unpaired;
    for (const auto& node : registeredNodes) {
        if (node.state == UNPAIRED && node.isActive) {
            unpaired.push_back(node);
        }
    }
    return unpaired;
}

// Get paired nodes
std::vector<NodeInfo> getPairedNodes() {
    std::vector<NodeInfo> paired;
    for (const auto& node : registeredNodes) {
        if (node.state == PAIRED && node.isActive) {
            paired.push_back(node);
        }
    }
    return paired;
}

// Get list of registered nodes
std::vector<NodeInfo> getRegisteredNodes() {
    return registeredNodes;
}

// Get mothership MAC address
String getMothershipsMAC() {
    return WiFi.macAddress();
}

// Print all registered nodes
void printRegisteredNodes() {
    Serial.println("📋 Registered Nodes:");
    if (registeredNodes.empty()) {
        Serial.println("   No nodes registered yet");
        return;
    }
    
    for (const auto& node : registeredNodes) {
        Serial.print("   ");
        Serial.print(node.nodeId);
        Serial.print(" (");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", node.mac[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.print(") - ");
        Serial.println(node.isActive ? "Active" : "Inactive");
    }
}