#include "espnow_manager.h"
#include "sd_manager.h"
#include "rtc_manager.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include "config.h"
#include <vector>
#include <Preferences.h>

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
    peerInfo.ifidx = WIFI_IF_STA;
        peerInfo.encrypt = false;
        
        // Remove peer if it already exists, then add
        esp_now_del_peer(mac);
        esp_err_t padd_local = esp_now_add_peer(&peerInfo);
        if (padd_local == ESP_OK) {
            // small settle time after adding peer to allow ESP-NOW/driver to update
            delay(30);
            Serial.print("✅ Node added as ESP-NOW peer: ");
            Serial.println(nodeId);
        } else {
            Serial.print("❌ Failed to add node as peer: ");
            Serial.println(nodeId);
            Serial.print("    esp_err: "); Serial.print((int)padd_local); Serial.print(" ("); Serial.print(esp_err_to_name(padd_local)); Serial.println(")");
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
            
            // Preserve existing state when re-registering to avoid overwriting PAIRED/DEPLOYED
            NodeState existingState = getNodeState(discovery.nodeId);
            registerNode(mac, discovery.nodeId, discovery.nodeType, existingState);
            
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
    // Handle legacy pairing_request_t (text command) from nodes
    else if (len == sizeof(pairing_request_t)) {
        pairing_request_t request;
        memcpy(&request, incomingBytes, sizeof(request));

        if (strcmp(request.command, "PAIRING_REQUEST") == 0) {
            Serial.print("📞 Pairing status request from: ");
            Serial.println(request.nodeId);

            // Preserve existing state when re-registering to avoid overwriting PAIRED/DEPLOYED
            NodeState existingState = getNodeState(request.nodeId);
            registerNode(mac, request.nodeId, "unknown", existingState);

            // Check if this node is already paired
            NodeState currentState = getNodeState(request.nodeId);

            // Send pairing response (protocol-specific)
            pairing_response_t response;
            strcpy(response.command, "PAIRING_RESPONSE");
            strcpy(response.nodeId, request.nodeId);
            response.isPaired = (currentState == PAIRED || currentState == DEPLOYED);
            response.scheduleInterval = 30; // Default for now
            strcpy(response.mothership_id, DEVICE_ID);

            uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_now_send(broadcastAddress, (uint8_t*)&response, sizeof(response));

            Serial.print("📤 Sent pairing response - isPaired: ");
            Serial.println(response.isPaired ? "true" : "false");
        }
    }
    // Handle RandomNerdTutorials compact pairing struct (rnt_pairing_t)
    else if (len == sizeof(rnt_pairing_t)) {
        rnt_pairing_t r;
        memcpy(&r, incomingBytes, sizeof(r));
        // If node is sending a pairing request (msgType == PAIR_REQUEST)
        // In RNT example PAIR_REQUEST was represented by msgType value (we'll treat non-zero as pairing)
        if (r.msgType == 0 || r.id >= 2) {
            // Build nodeId from mac for display
            char nid[20];
            snprintf(nid, sizeof(nid), "NODE_%02X%02X", r.macAddr[4], r.macAddr[5]);
            Serial.print("📞 RNT pairing packet from ");
            for (int i=0;i<6;i++){ Serial.printf("%02X", r.macAddr[i]); if (i<5) Serial.print(":"); }
            Serial.println();

            // Preserve existing state when re-registering and record channel
            NodeState existingState = getNodeState(nid);
            registerNode(r.macAddr, nid, "rnt-node", existingState);
            // find node and set channel
            for (auto &node : registeredNodes) {
                if (memcmp(node.mac, r.macAddr, 6) == 0) { node.channel = r.channel; break; }
            }
            // Update esp-now peer info to use the node's reported channel so responses
            // sent to this peer use the correct channel
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, r.macAddr, 6);
            peerInfo.channel = r.channel > 0 ? r.channel : 1;
            peerInfo.ifidx = WIFI_IF_STA;
            peerInfo.encrypt = false;
            // Remove existing peer and add with updated channel
            esp_err_t pdel = esp_now_del_peer(r.macAddr);
            // Force fixed channel 1 for pairing
            peerInfo.channel = 1;
            esp_err_t padd = esp_now_add_peer(&peerInfo);
            Serial.print("🔧 Updated peer channel for RNT node: ");
            for (int i=0;i<6;i++){ Serial.printf("%02X", r.macAddr[i]); if (i<5) Serial.print(":"); }
            Serial.print(" -> channel "); Serial.print(peerInfo.channel);
            Serial.print(" (del="); Serial.print((int)pdel); Serial.print(", add="); Serial.print((int)padd); Serial.println(")");
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
    // Fixed-channel pairing: always use channel 1 for pairing command
    const uint8_t sendChannel = 1;
    esp_wifi_set_channel(sendChannel, WIFI_SECOND_CHAN_NONE);

    // Prepare broadcast peer info
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;

    // Initialize ESP-NOW if not already initialized
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW initialization failed");
        // don't return here; some boards may still allow later init attempts
    } else {
        Serial.println("✅ ESP-NOW initialized");
        // register receive callback
        esp_now_register_recv_cb(OnDataRecv);
        // register send callback for delivery status
        esp_now_register_send_cb([](const uint8_t *mac_addr, esp_now_send_status_t status){
            Serial.print("📨 send_cb to ");
            if (mac_addr) {
                for (int i = 0; i < 6; i++) { Serial.printf("%02X", mac_addr[i]); if (i < 5) Serial.print(":"); }
            } else Serial.print("(null)");
            Serial.print(" status="); Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
        });
    }

    // Remove existing broadcast peer (if any) and add the configured broadcast peer
    esp_err_t pdel_b = esp_now_del_peer(broadcastAddress);
    esp_err_t padd_b = esp_now_add_peer(&peerInfo);
    if (padd_b != ESP_OK) {
        Serial.print("Failed to add broadcast peer: "); Serial.print((int)padd_b); Serial.print(" ("); Serial.print(esp_err_to_name(padd_b)); Serial.println(")");
    } else {
        // let the driver settle after adding broadcast peer
        delay(30);
        Serial.println("✅ Broadcast peer added for discovery");
    }
    
    // Add known sensor nodes as peers for reliable communication
    for (int i = 0; i < NUM_KNOWN_SENSORS; i++) {
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, KNOWN_SENSOR_NODES[i], 6);
    peerInfo.channel = 1;
    peerInfo.ifidx = WIFI_IF_STA;
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

    // Load persisted paired nodes from NVS and pre-add as peers
    loadPairedNodes();
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
        Serial.print("❌ Failed to send discovery broadcast: "); Serial.print((int)result);
        Serial.print(" ("); Serial.print(esp_err_to_name(result)); Serial.println(")");
        // Try re-adding broadcast peer and retry once
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;
        esp_err_t pdel = esp_now_del_peer(broadcastAddress);
        esp_err_t padd = esp_now_add_peer(&peerInfo);
        Serial.print("🔁 Retried adding broadcast peer (del="); Serial.print((int)pdel);
        Serial.print(", add="); Serial.print((int)padd); Serial.println(")");

        // retry send
        esp_err_t retry = esp_now_send(broadcastAddress, (uint8_t*)&broadcast, sizeof(broadcast));
        if (retry == ESP_OK) {
            Serial.println("📡 Discovery broadcast sent (after retry)");
            return true;
        }

        Serial.print("❌ Retry failed: "); Serial.print((int)retry);
        Serial.print(" ("); Serial.print(esp_err_to_name(retry)); Serial.println(")");
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
            
            // Ensure peer entry exists for this node on fixed pairing channel (1)
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, node.mac, 6);
            peerInfo.channel = 1;
            peerInfo.encrypt = false;

            esp_err_t pdel = esp_now_del_peer(node.mac);
            esp_err_t padd = esp_now_add_peer(&peerInfo);
            Serial.print("🔧 Ensured peer for "); Serial.print(nodeId);
            Serial.print(" (del="); Serial.print((int)pdel); Serial.print(", add="); Serial.print((int)padd); Serial.println(")");

            // small settle time after adding peer
            delay(30);

            // Try sending pairing command with a small retry loop to mitigate IF errors
            const int maxAttempts = 3;
            esp_err_t result = ESP_ERR_ESPNOW_NOT_INIT;
            for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
                // Ensure we're on the pairing channel
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
                delay(10);
                result = esp_now_send(node.mac, (uint8_t*)&pairCmd, sizeof(pairCmd));
                if (result == ESP_OK) break;
                Serial.print("⚠️ pairNode attempt "); Serial.print(attempt); Serial.print(" failed: "); Serial.print((int)result); Serial.print(" ("); Serial.print(esp_err_to_name(result)); Serial.println(")");
                delay(50);
            }

            if (result == ESP_OK) {
                Serial.print("✅ Node paired: ");
                Serial.print(nodeId);
                Serial.print(" (");
                Serial.print(scheduleInterval);
                Serial.println(" min interval)");

                // Also send RandomNerdTutorials-style compact pairing response so
                // RNT nodes can add the mothership as an ESP-NOW peer.
                rnt_pairing_t rntResp;
                memset(&rntResp, 0, sizeof(rntResp));
                rntResp.msgType = 0; // PAIRING response
                rntResp.id = 0; // mothership/server id = 0 per RNT convention

                // Fill mothership MAC into response
                uint8_t mothershipMac[6];
                WiFi.macAddress(mothershipMac);
                memcpy(rntResp.macAddr, mothershipMac, 6);

                // Use pairing channel (fixed to 1)
                uint8_t sendChannel = 1;

                // Switch WiFi channel briefly so the node receives the packet
                esp_wifi_set_channel(sendChannel, WIFI_SECOND_CHAN_NONE);
                delay(10);
                rntResp.channel = sendChannel;

                // Ensure peer is present for rnt reply as well (set channel appropriately)
                peerInfo.channel = sendChannel;
                esp_now_del_peer(node.mac);
                esp_now_add_peer(&peerInfo);
                delay(20);

                // Retry loop for RNT reply as well
                esp_err_t rres = ESP_ERR_ESPNOW_NOT_INIT;
                for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
                    esp_wifi_set_channel(sendChannel, WIFI_SECOND_CHAN_NONE);
                    delay(10);
                    rres = esp_now_send(node.mac, (uint8_t*)&rntResp, sizeof(rntResp));
                    if (rres == ESP_OK) break;
                    Serial.print("⚠️ RNT reply attempt "); Serial.print(attempt); Serial.print(" failed: "); Serial.print((int)rres); Serial.print(" ("); Serial.print(esp_err_to_name(rres)); Serial.println(")");
                    delay(50);
                }

                if (rres == ESP_OK) {
                    Serial.print("📤 Sent RNT pairing response to ");
                    for (int i = 0; i < 6; i++) { Serial.printf("%02X", node.mac[i]); if (i < 5) Serial.print(":"); }
                    Serial.println();
                    // Persist paired nodes
                    savePairedNodes();
                    return true;
                } else {
                    Serial.print("❌ Failed to send RNT pairing response to: ");
                    Serial.println(nodeId);
                    Serial.print("    esp_err_t: "); Serial.print((int)rres);
                    Serial.print(" ("); Serial.print(esp_err_to_name(rres)); Serial.println(")");
                    node.state = UNPAIRED; // revert on failure
                    return false;
                }
            } else {
                Serial.print("❌ Failed to pair node: ");
                Serial.println(nodeId);
                Serial.print("    esp_err_t: "); Serial.print((int)result);
                Serial.print(" ("); Serial.print(esp_err_to_name(result)); Serial.println(")");
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

// Persist paired nodes using Preferences (NVS)
void savePairedNodes() {
    Preferences prefs;
    if (!prefs.begin("paired_nodes", false)) {
        Serial.println("❌ Failed to open NVS for saving paired nodes");
        return;
    }

    // Store count
    int count = 0;
    for (const auto &n : registeredNodes) if (n.state == PAIRED || n.state == DEPLOYED) count++;
    prefs.putInt("count", count);

    int idx = 0;
    for (const auto &n : registeredNodes) {
        if (n.state == PAIRED || n.state == DEPLOYED) {
            char key[16];
            snprintf(key, sizeof(key), "mac%d", idx);
            // store as hex string
            char macs[18];
            snprintf(macs, sizeof(macs), "%02X%02X%02X%02X%02X%02X", n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
            prefs.putString(key, macs);

            snprintf(key, sizeof(key), "id%d", idx);
            prefs.putString(key, n.nodeId);

            snprintf(key, sizeof(key), "int%d", idx);
            prefs.putInt(key, n.scheduleInterval);

            idx++;
        }
    }

    prefs.end();
    Serial.print("✅ Saved "); Serial.print(count); Serial.println(" paired nodes to NVS");
}

void loadPairedNodes() {
    Preferences prefs;
    if (!prefs.begin("paired_nodes", true)) {
        Serial.println("❌ Failed to open NVS for loading paired nodes");
        return;
    }

    int count = prefs.getInt("count", 0);
    Serial.print("🔁 Loading "); Serial.print(count); Serial.println(" paired nodes from NVS");

    for (int i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "mac%d", i);
        String macs = prefs.getString(key, "");
        if (macs.length() != 12) continue; // invalid

        uint8_t mac[6];
        for (int j = 0; j < 6; ++j) {
            String byteStr = macs.substring(j*2, j*2+2);
            mac[j] = (uint8_t) strtoul(byteStr.c_str(), NULL, 16);
        }

        snprintf(key, sizeof(key), "id%d", i);
        String nid = prefs.getString(key, "NODE");

        snprintf(key, sizeof(key), "int%d", i);
        int interval = prefs.getInt(key, 5);

        // Add node to registry as PAIRED
        NodeInfo newNode;
        memcpy(newNode.mac, mac, 6);
        newNode.nodeId = nid;
        newNode.nodeType = String("restored");
        newNode.lastSeen = millis();
        newNode.isActive = true;
        newNode.state = PAIRED;
        newNode.scheduleInterval = interval;
        newNode.channel = 1;
        registeredNodes.push_back(newNode);

        // Add as ESP-NOW peer
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, mac, 6);
        peerInfo.channel = 1;
        peerInfo.ifidx = WIFI_IF_STA;
        peerInfo.encrypt = false;
        esp_now_del_peer(mac);
        esp_now_add_peer(&peerInfo);
    }

    prefs.end();
}

bool unpairNode(const String& nodeId) {
    for (auto it = registeredNodes.begin(); it != registeredNodes.end(); ++it) {
        if (it->nodeId == nodeId) {
            // Remove ESP-NOW peer locally
            esp_now_del_peer(it->mac);
            it->state = UNPAIRED;
            it->isActive = true;
            // Persist changes
            savePairedNodes();
            Serial.print("🗑️ Unpaired node: "); Serial.println(nodeId);
            return true;
        }
    }
    return false;
}

bool sendUnpairToNode(const String& nodeId) {
    for (const auto &n : registeredNodes) {
        if (n.nodeId == nodeId) {
            unpair_command_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            strcpy(cmd.command, "UNPAIR_NODE");
            strcpy(cmd.mothership_id, DEVICE_ID);

            esp_err_t res = esp_now_send(n.mac, (uint8_t*)&cmd, sizeof(cmd));
            Serial.print("📤 Sent UNPAIR to "); Serial.print(nodeId); Serial.print(" -> "); Serial.print((int)res); Serial.print(" ("); Serial.print(esp_err_to_name(res)); Serial.println(")");
            return (res == ESP_OK);
        }
    }
    return false;
}