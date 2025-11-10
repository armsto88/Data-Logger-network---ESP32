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
#include "protocol.h"

// External reference to DEVICE_ID from main.cpp
extern const char* DEVICE_ID;

std::vector<NodeInfo> registeredNodes;

// ----------------- internal helpers -----------------
static void ensurePeerOnChannel(const uint8_t mac[6], uint8_t channel) {
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_del_peer(mac);
  esp_now_add_peer(&peer);
}

static String macToStr(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

// ----------------- registry -----------------
void registerNode(const uint8_t* mac, const char* nodeId,
                  const char* nodeType /*= "unknown"*/,
                  NodeState state /*= UNPAIRED*/) {
  bool found = false;
  for (auto& node : registeredNodes) {
    if (memcmp(node.mac, mac, 6) == 0) {
      node.lastSeen = millis();
      node.isActive = true;
      if (node.state != DEPLOYED) { // don't downgrade deployed
        node.nodeType = String(nodeType);
        node.state = state;
      }
      found = true;
      break;
    }
  }

  if (!found) {
    NodeInfo n{};
    memcpy(n.mac, mac, 6);
    n.nodeId   = String(nodeId);
    n.nodeType = String(nodeType);
    n.lastSeen = millis();
    n.isActive = true;
    n.state    = state;
    n.channel  = ESPNOW_CHANNEL;
    registeredNodes.push_back(n);

    ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
    delay(30);

    Serial.printf("✅ Node peer added: %s (%s)\n",
                  nodeId, macToStr(mac).c_str());
    Serial.printf("📡 New node discovered: %s (%s) - %s\n",
                  nodeId, nodeType, macToStr(mac).c_str());
  }
}

NodeState getNodeState(const char* nodeId) {
  for (const auto& node : registeredNodes)
    if (node.nodeId == String(nodeId)) return node.state;
  return UNPAIRED;
}

// ----------------- ESPNOW callbacks -----------------
static void OnDataRecv(const uint8_t * mac, const uint8_t *incomingBytes, int len) {
  if (len == sizeof(sensor_data_message_t)) {
    sensor_data_message_t incoming{};
    memcpy(&incoming, incomingBytes, sizeof(incoming));

    registerNode(mac, incoming.nodeId, incoming.sensorType, DEPLOYED);

    Serial.printf("📊 Data from %s (%s): %s = %.3f\n",
                  incoming.nodeId, macToStr(mac).c_str(),
                  incoming.sensorType, incoming.value);

    char ts[24];
    getRTCTimeString(ts, sizeof(ts));

    String macStr = "";
    for (int i = 0; i < 6; i++) {
      if (i) macStr += ":";
      char bb[3]; snprintf(bb, sizeof(bb), "%02x", mac[i]);
      macStr += bb;
    }

    String row = String(ts) + "," + incoming.nodeId + "," + macStr + "," +
                 incoming.sensorType + "," + String(incoming.value);
    if (logCSVRow(row)) Serial.println("✅ Node data logged");
    else                Serial.println("❌ Failed to log node data");
  }
else if (len == sizeof(discovery_message_t)) {
  discovery_message_t discovery;
  memcpy(&discovery, incomingBytes, sizeof(discovery));

  if (strcmp(discovery.command, "DISCOVER_REQUEST") == 0) {
    Serial.printf("🔍 Discovery from %s (%s)\n", discovery.nodeId, discovery.nodeType);

    // ⬇️ Force node to UNPAIRED when it announces itself
    registerNode(mac, discovery.nodeId, discovery.nodeType, UNPAIRED);

    discovery_response_t response{};
    strcpy(response.command, "DISCOVER_RESPONSE");
    strcpy(response.mothership_id, DEVICE_ID);
    response.acknowledged = true;

    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_send(bcast, (uint8_t*)&response, sizeof(response));
    Serial.println("📡 Sent discovery response");
    
    // Persist change so UI reflects immediately
    savePairedNodes();
  }
}

 else if (len == sizeof(pairing_request_t)) {
  pairing_request_t request;
  memcpy(&request, incomingBytes, sizeof(request));

  if (strcmp(request.command, "PAIRING_REQUEST") == 0) {
    Serial.printf("📞 Pairing status poll from %s\n", request.nodeId);

    // ⬇️ Treat a poll as a node that's not deployed yet
    registerNode(mac, request.nodeId, "unknown", UNPAIRED);

    NodeState current = getNodeState(request.nodeId);

    pairing_response_t response{};
    strcpy(response.command, "PAIRING_RESPONSE");
    strcpy(response.nodeId, request.nodeId);
    response.isPaired = (current == PAIRED || current == DEPLOYED);
    strcpy(response.mothership_id, DEVICE_ID);

    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_send(bcast, (uint8_t*)&response, sizeof(response));

    savePairedNodes();
  }
}

  else if (len == sizeof(rnt_pairing_t)) {
    rnt_pairing_t r{};
    memcpy(&r, incomingBytes, sizeof(r));

    if (r.msgType == 0 || r.id >= 2) {
      char nid[20]; snprintf(nid, sizeof(nid), "NODE_%02X%02X", r.macAddr[4], r.macAddr[5]);
      Serial.print("📞 RNT pairing packet from "); Serial.println(macToStr(r.macAddr));

      NodeState existing = getNodeState(nid);
      registerNode(r.macAddr, nid, "rnt-node", existing);

      for (auto &n : registeredNodes)
        if (memcmp(n.mac, r.macAddr, 6) == 0) { n.channel = r.channel ? r.channel : ESPNOW_CHANNEL; break; }

      ensurePeerOnChannel(r.macAddr, ESPNOW_CHANNEL);
      Serial.printf("🔧 Peer updated on channel %u\n", ESPNOW_CHANNEL);
    }
  }
  else if (len == sizeof(time_sync_request_t)) {
    time_sync_request_t req{};
    memcpy(&req, incomingBytes, sizeof(req));
    if (strcmp(req.command, "REQUEST_TIME") == 0) {
      Serial.printf("⏰ Time sync request from: %s\n", req.nodeId);
      sendTimeSync(mac, req.nodeId);
    }
  }
}

// ----------------- ESPNOW lifecycle -----------------
void setupESPNOW() {
  delay(100);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_now_peer_info_t peer{};
  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  memcpy(peer.peer_addr, bcast, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
  } else {
    Serial.println("✅ ESP-NOW initialized");
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb([](const uint8_t *mac_addr, esp_now_send_status_t status){
      Serial.print("📨 send_cb to ");
      if (mac_addr) Serial.println(macToStr(mac_addr));
      else          Serial.println("(null)");
      Serial.print("    status="); Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
    });
  }

  esp_now_del_peer(bcast);
  if (esp_now_add_peer(&peer) == ESP_OK) {
    delay(30);
    Serial.println("✅ Broadcast peer added");
  } else {
    Serial.println("❌ Failed to add broadcast peer");
  }

  // Preload known peers (defined in espnow_manager_globals.cpp)
  for (int i = 0; i < NUM_KNOWN_SENSORS; i++) {
    ensurePeerOnChannel(KNOWN_SENSOR_NODES[i], ESPNOW_CHANNEL);
    Serial.print("✅ Preloaded peer: ");
    Serial.println(macToStr(KNOWN_SENSOR_NODES[i]));
  }

  Serial.println("ESP-NOW ready");
  Serial.print("MAC Address: "); Serial.println(WiFi.macAddress());

  loadPairedNodes();
}

void espnow_loop() {
  static unsigned long lastDiscovery = 0;
  unsigned long now = millis();
  if (now - lastDiscovery > 30000) {
    Serial.println("🔍 Auto discovery broadcast…");
    if (sendDiscoveryBroadcast()) Serial.println("✅ Discovery broadcast sent");
    else                          Serial.println("❌ Discovery broadcast failed");
    lastDiscovery = now;
  }

  for (auto& n : registeredNodes) {
    if (now - n.lastSeen > 300000 && n.isActive) {
      n.isActive = false;
      Serial.printf("⚠️ Node %s marked inactive\n", n.nodeId.c_str());
    }
  }
}

// ----------------- commands / broadcasts -----------------
bool broadcastWakeInterval(int intervalMinutes) {
  schedule_command_message_t cmd{};
  strcpy(cmd.command, "SET_SCHEDULE");
  strncpy(cmd.mothership_id, DEVICE_ID, sizeof(cmd.mothership_id)-1);
  cmd.intervalMinutes = intervalMinutes;

  bool anySent = false;

  for (const auto& node : registeredNodes) {
    if (node.state != PAIRED && node.state != DEPLOYED) continue;

    ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    esp_err_t res = esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
    Serial.printf("📤 SET_SCHEDULE %d min -> %s : %s\n",
                  intervalMinutes, node.nodeId.c_str(),
                  (res==ESP_OK) ? "OK" : esp_err_to_name(res));
    if (res == ESP_OK) anySent = true;
  }
  return anySent;
}

bool sendTimeSync(const uint8_t* mac, const char* nodeId) {
  time_sync_response_t resp{};
  strcpy(resp.command, "TIME_SYNC");
  strncpy(resp.mothership_id, DEVICE_ID, sizeof(resp.mothership_id)-1);

  char ts[24];
  getRTCTimeString(ts, sizeof(ts));

  int y,m,d,H,M,S;
  if (sscanf(ts, "%d-%d-%d %d:%d:%d", &y,&m,&d,&H,&M,&S) == 6) {
    resp.year=y; resp.month=m; resp.day=d; resp.hour=H; resp.minute=M; resp.second=S;

    ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    esp_err_t r = esp_now_send(mac, (uint8_t*)&resp, sizeof(resp));
    if (r == ESP_OK) {
      Serial.printf("✅ Time sync sent to %s : %s\n", nodeId, ts);
      return true;
    }
    Serial.printf("❌ Time sync send fail to %s : %s\n", nodeId, esp_err_to_name(r));
  } else {
    Serial.println("❌ Failed to parse RTC time");
  }
  return false;
}

bool sendDiscoveryBroadcast() {
  uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

  discovery_response_t pkt{};
  strcpy(pkt.command, "DISCOVERY_SCAN");
  strcpy(pkt.mothership_id, DEVICE_ID);
  pkt.acknowledged = false;

  ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_err_t r = esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
  if (r == ESP_OK) return true;

  // try re-add and retry once
  ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
  esp_err_t r2 = esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
  return (r2 == ESP_OK);
}

// Pair a specific node
bool pairNode(const String& nodeId) {
    for (auto& node : registeredNodes) {
        if (node.nodeId == nodeId && node.state == UNPAIRED) {
            node.state = PAIRED;
            pairing_command_t pairCmd;
            strcpy(pairCmd.command, "PAIR_NODE");
            strcpy(pairCmd.nodeId, nodeId.c_str());
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
            delay(30);
            const int maxAttempts = 3;
            esp_err_t result = ESP_ERR_ESPNOW_NOT_INIT;
            for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
                esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
                delay(10);
                result = esp_now_send(node.mac, (uint8_t*)&pairCmd, sizeof(pairCmd));
                if (result == ESP_OK) break;
                Serial.print("⚠️ pairNode attempt "); Serial.print(attempt); Serial.print(" failed: "); Serial.print((int)result); Serial.print(" ("); Serial.print(esp_err_to_name(result)); Serial.println(")");
                delay(50);
            }
            if (result == ESP_OK) {
                Serial.print("✅ Node paired: ");
                Serial.println(nodeId);
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
                // removed: deployCmd.scheduleInterval = node.scheduleInterval;
                
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

        // Add node to registry as PAIRED
        NodeInfo newNode;
        memcpy(newNode.mac, mac, 6);
        newNode.nodeId = nid;
        newNode.nodeType = String("restored");
        newNode.lastSeen = millis();
        newNode.isActive = true;
        newNode.state = PAIRED;
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