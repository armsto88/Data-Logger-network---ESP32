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

// Defined once in espnow_manager_globals.cpp
extern const uint8_t KNOWN_SENSOR_NODES[][6];
extern const int     NUM_KNOWN_SENSORS;

std::vector<NodeInfo> registeredNodes;

// From main.cpp – helpers that map firmware nodeId -> CSV ID + name
String getCsvNodeId(const String& nodeId);
String getCsvNodeName(const String& nodeId);

// Also import raw meta (for NodeInfo)
String getNodeUserId(const String& nodeId);
String getNodeName(const String& nodeId);

// Fleet-wide TIME_SYNC bookkeeping
static uint32_t g_lastFleetTimeSyncMs = 0;
static const uint32_t FLEET_SYNC_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL; // 24h


// ----------------- small helpers -----------------
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

static const char* stateToStr(NodeState s) {
    switch (s) {
      case UNPAIRED: return "UNPAIRED";
      case PAIRED:   return "PAIRED";
      case DEPLOYED: return "DEPLOYED";
      default:       return "UNKNOWN";
    }
}

// ----------------- registry -----------------
void registerNode(const uint8_t* mac,
                  const char* nodeId,
                  const char* nodeType /*= "unknown"*/,
                  NodeState state     /*= UNPAIRED*/)
{
    NodeInfo* existing = nullptr;

    // Find existing entry by MAC or firmware nodeId
    for (auto& node : registeredNodes) {
        if (memcmp(node.mac, mac, 6) == 0 || node.nodeId == String(nodeId)) {
            existing = &node;
            break;
        }
    }

    if (existing) {
        bool upgraded = false;

        existing->lastSeen = millis();
        existing->isActive = true;
        existing->nodeType = String(nodeType);  // keep type fresh

        // Only ever upgrade: UNPAIRED < PAIRED < DEPLOYED
        if (state > existing->state) {
            Serial.printf("📈 Node %s state upgrade: %s → %s\n",
                          existing->nodeId.c_str(),
                          stateToStr(existing->state),
                          stateToStr(state));
            existing->state = state;
            upgraded = true;
        }

        // Refresh meta for consistency with web UI / CSV
        existing->userId = getNodeUserId(existing->nodeId);
        existing->name   = getNodeName(existing->nodeId);

        if (upgraded && (existing->state == PAIRED || existing->state == DEPLOYED)) {
            savePairedNodes();  // keep NVS in sync when a node “promotes”
        }

        return;
    }

    // New node
    NodeInfo n{};
    memcpy(n.mac, mac, 6);
    n.nodeId   = String(nodeId);
    n.nodeType = String(nodeType);
    n.lastSeen = millis();
    n.isActive = true;
    n.state    = state;
    n.channel  = ESPNOW_CHANNEL;

    // NEW:
    n.lastTimeSyncMs = 0;

    // Populate user-facing meta from NVS for immediate consistency
    n.userId = getNodeUserId(n.nodeId);
    n.name   = getNodeName(n.nodeId);
  // friendly name, may be empty

    registeredNodes.push_back(n);

    ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
    Serial.printf("✅ New node: %s (%s) state=%s\n",
                  nodeId, macToStr(mac).c_str(), stateToStr(state));

    if (state == PAIRED || state == DEPLOYED) {
        savePairedNodes();
    }
}

NodeState getNodeState(const char* nodeId) {
    for (const auto& node : registeredNodes)
        if (node.nodeId == String(nodeId)) return node.state;
    return UNPAIRED;
}

// ----------------- ESPNOW callbacks -----------------
static void OnDataRecv(const uint8_t * mac,
                       const uint8_t * incomingBytes,
                       int len)
{
    // 1) Sensor data packets → mark DEPLOYED + log CSV
    if (len == sizeof(sensor_data_message_t)) {
        sensor_data_message_t incoming{};
        memcpy(&incoming, incomingBytes, sizeof(incoming));

        registerNode(mac, incoming.nodeId, incoming.sensorType, DEPLOYED);

        // MAC in lowercase colon format for CSV
        String macStr;
        for (int i = 0; i < 6; i++) {
            if (i) macStr += ":";
            char bb[3];
            snprintf(bb, sizeof(bb), "%02x", mac[i]);
            macStr += bb;
        }

        // Map firmware nodeId -> CSV node_id (numeric) + node_name (friendly)
        String fwId    = String(incoming.nodeId);  // e.g. "TEMP_001"
        String csvId   = getCsvNodeId(fwId);       // e.g. "001"
        String csvName = getCsvNodeName(fwId);     // e.g. "North Hedge 01"

        // If we have a NodeInfo with in-memory meta, prefer that
        for (auto &n : registeredNodes) {
            if (n.nodeId == fwId) {
                if (!n.userId.isEmpty()) csvId   = n.userId;
                if (!n.name.isEmpty())   csvName = n.name;
                break;
            }
        }

        // Serial log: include CSV id + friendly name so it matches the UI/CSV
        char ts[24];
        getRTCTimeString(ts, sizeof(ts));

        Serial.printf(
          "📊 Data @ %s\n"
          "   from FW=%s, MAC=%s\n"
          "   CSV node_id=%s, name='%s'\n"
          "   sensor=%s, value=%.3f, node_ts=%lu\n",
          ts,
          incoming.nodeId,
          macStr.c_str(),
          csvId.c_str(),
          csvName.c_str(),
          incoming.sensorType,
          incoming.value,
          (unsigned long)incoming.nodeTimestamp
        );

        // CSV row: timestamp, node_id, node_name, mac, sensor_type, value
        String row;
        row.reserve(160);
        row  = ts;              // timestamp (mothership RTC)
        row += ",";
        row += csvId;           // node_id (numeric, falls back to fwId if empty)
        row += ",";
        row += csvName;         // node_name (may be empty)
        row += ",";
        row += macStr;          // raw MAC
        row += ",";
        row += incoming.sensorType;
        row += ",";
        row += String(incoming.value);

        if (logCSVRow(row)) Serial.println("✅ Node data logged");
        else                Serial.println("❌ Failed to log node data");

        return;
    }

    // 2) Discovery from node
    if (len == sizeof(discovery_message_t)) {
        discovery_message_t discovery;
        memcpy(&discovery, incomingBytes, sizeof(discovery));

        if (strcmp(discovery.command, "DISCOVER_REQUEST") == 0) {
            Serial.printf("🔍 Discovery from %s (%s) MAC=%s\n",
                          discovery.nodeId,
                          discovery.nodeType,
                          macToStr(mac).c_str());

            // Preserve existing state if we already know this node
            NodeState keep = UNPAIRED;
            for (const auto& n : registeredNodes) {
                if (memcmp(n.mac, mac, 6) == 0 ||
                    n.nodeId == String(discovery.nodeId)) {
                    keep = n.state;
                    break;
                }
            }
            registerNode(mac, discovery.nodeId, discovery.nodeType, keep);

            discovery_response_t response{};
            strcpy(response.command, "DISCOVER_RESPONSE");
            strcpy(response.mothership_id, DEVICE_ID);
            response.acknowledged = true;

            uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_now_send(bcast, (uint8_t*)&response, sizeof(response));
            Serial.println("📡 Sent discovery response");
        }
        return;
    }

    if (len == sizeof(time_sync_request_t)) {
        time_sync_request_t req{};
        memcpy(&req, incomingBytes, sizeof(req));

        // command lives in req.command (second field in struct)
        if (strcmp(req.command, "REQUEST_TIME") == 0) {
            Serial.printf("⏰ Time sync request from: %s (MAC=%s)\n",
                          req.nodeId,
                          macToStr(mac).c_str());

            sendTimeSync(mac, req.nodeId);

            // Optional: update NodeInfo time health right here too
            for (auto &n : registeredNodes) {
                if (n.nodeId == String(req.nodeId) ||
                    memcmp(n.mac, mac, 6) == 0) {
                    n.lastTimeSyncMs = millis();
                    break;
                }
            }
        } else {
            Serial.printf("⏰ 36-byte packet, but command='%s' (not REQUEST_TIME)\n",
                          req.command);
        }
        return;
    }

    // 4) Pairing status poll from node
    if (len == sizeof(pairing_request_t)) {
        pairing_request_t request;
        memcpy(&request, incomingBytes, sizeof(request));

        if (strcmp(request.command, "PAIRING_REQUEST") == 0) {
            Serial.printf("📞 Pairing status poll from %s MAC=%s\n",
                          request.nodeId, macToStr(mac).c_str());

            // Preserve existing state (don’t downgrade on polls)
            NodeState keep = UNPAIRED;
            for (const auto& n : registeredNodes) {
                if (memcmp(n.mac, mac, 6) == 0 ||
                    n.nodeId == String(request.nodeId)) {
                    keep = n.state;
                    break;
                }
            }
            registerNode(mac, request.nodeId, "unknown", keep);

            NodeState current = getNodeState(request.nodeId);

            pairing_response_t response{};
            strcpy(response.command, "PAIRING_RESPONSE");
            strcpy(response.nodeId, request.nodeId);
            response.isPaired = (current == PAIRED || current == DEPLOYED);
            strcpy(response.mothership_id, DEVICE_ID);

            uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_now_send(bcast, (uint8_t*)&response, sizeof(response));

            Serial.printf("📤 PAIRING_RESPONSE to %s → isPaired=%d (state=%s)\n",
                          request.nodeId,
                          response.isPaired,
                          stateToStr(current));
        } else {
            Serial.printf("📞 36-byte packet, but command='%s' (not PAIRING_REQUEST)\n",
                          request.command);
        }
        return;
    }

    // (any other packet types can be handled here if needed)
}

// ----------------- ESPNOW lifecycle -----------------
void setupESPNOW() {
    delay(100);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    // Init ESPNOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW init failed");
    } else {
        Serial.println("✅ ESP-NOW initialized");
        esp_now_register_recv_cb(OnDataRecv);
        esp_now_register_send_cb([](const uint8_t *mac_addr,
                                    esp_now_send_status_t status){
            Serial.print("📨 send_cb to ");
            if (mac_addr) Serial.print(macToStr(mac_addr));
            else          Serial.print("(null)");
            Serial.print("\n    status=");
            Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
        });
    }

    // Broadcast peer
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
    Serial.println("✅ Broadcast peer added");

    // Preload known peers
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
    unsigned long now = millis();

    // Mark nodes inactive if not seen for 5 min
    for (auto& n : registeredNodes) {
        if (n.isActive && (now - n.lastSeen > 300000UL)) {
            n.isActive = false;
            Serial.printf("⚠️ Node %s (%s) marked inactive (state=%s)\n",
                          n.nodeId.c_str(),
                          macToStr(n.mac).c_str(),
                          stateToStr(n.state));
        }
    }

    // NEW: periodic fleet-wide TIME_SYNC (every ~24 h)
    broadcastTimeSyncIfDue(false);
}



// ----------------- Persistence (paired/deployed list) -----------------
void savePairedNodes() {
    Preferences prefs;
    if (!prefs.begin("paired_nodes", false)) {
        Serial.println("❌ Failed to open NVS for saving paired nodes");
        return;
    }

    // Only store PAIRED or DEPLOYED nodes
    int count = 0;
    for (const auto &n : registeredNodes) {
        if (n.state == PAIRED || n.state == DEPLOYED) count++;
    }
    prefs.putInt("count", count);

    int idx = 0;
    for (const auto &n : registeredNodes) {
        if (n.state != PAIRED && n.state != DEPLOYED) continue;

        char key[16];

        // MAC as 12-char hex string
        snprintf(key, sizeof(key), "mac%d", idx);
        char macs[18];
        snprintf(macs, sizeof(macs), "%02X%02X%02X%02X%02X%02X",
                 n.mac[0], n.mac[1], n.mac[2],
                 n.mac[3], n.mac[4], n.mac[5]);
        prefs.putString(key, macs);

        // Node ID
        snprintf(key, sizeof(key), "id%d", idx);
        prefs.putString(key, n.nodeId);

        // Node type
        snprintf(key, sizeof(key), "typ%d", idx);
        prefs.putString(key, n.nodeType);

        // Node state
        snprintf(key, sizeof(key), "st%d", idx);
        prefs.putUChar(key, (uint8_t)n.state);

        idx++;
    }

    prefs.end();
    Serial.printf("✅ Saved %d paired/deployed nodes to NVS\n", count);
}

void loadPairedNodes() {
    Preferences prefs;
    if (!prefs.begin("paired_nodes", true)) {
        Serial.println("❌ Failed to open NVS for loading paired nodes");
        return;
    }

    int count = prefs.getInt("count", 0);
    Serial.printf("🔁 Loading %d paired/deployed nodes from NVS\n", count);

    for (int i = 0; i < count; ++i) {
        char key[16];

        // MAC
        snprintf(key, sizeof(key), "mac%d", i);
        String macs = prefs.getString(key, "");
        if (macs.length() != 12) {
            Serial.printf("⚠️ Skipping entry %d: invalid MAC string '%s'\n",
                          i, macs.c_str());
            continue;
        }

        uint8_t mac[6];
        for (int j = 0; j < 6; ++j) {
            String byteStr = macs.substring(j * 2, j * 2 + 2);
            mac[j] = (uint8_t)strtoul(byteStr.c_str(), nullptr, 16);
        }

        // Node ID
        snprintf(key, sizeof(key), "id%d", i);
        String nid = prefs.getString(key, "NODE");

        // Node type
        snprintf(key, sizeof(key), "typ%d", i);
        String ntype = prefs.getString(key, "restored");

        // Node state
        snprintf(key, sizeof(key), "st%d", i);
        uint8_t stRaw = prefs.getUChar(key, (uint8_t)PAIRED);
        NodeState state = (stRaw <= DEPLOYED) ? (NodeState)stRaw : PAIRED;

      NodeInfo newNode{};
        memcpy(newNode.mac, mac, 6);
        newNode.nodeId   = nid;
        newNode.nodeType = ntype;
        newNode.lastSeen = millis();
        newNode.isActive = true;
        newNode.state    = state;
        newNode.channel  = ESPNOW_CHANNEL;

        // NEW:
        newNode.lastTimeSyncMs = 0;


        // Hydrate user-facing meta from node_meta
        newNode.userId = getNodeUserId(newNode.nodeId);
        newNode.name   = getNodeName(newNode.nodeId);

        registeredNodes.push_back(newNode);

        ensurePeerOnChannel(mac, ESPNOW_CHANNEL);

        Serial.printf("   ↪ restored %s (%s), state=%s, userId=%s, name='%s'\n",
                      nid.c_str(),
                      macToStr(mac).c_str(),
                      stateToStr(state),
                      newNode.userId.c_str(),
                      newNode.name.c_str());
    }

    prefs.end();
}

// ----------------- Commands / broadcasts -----------------
bool broadcastWakeInterval(int intervalMinutes) {
    schedule_command_message_t cmd{};
    strcpy(cmd.command, "SET_SCHEDULE");
    strncpy(cmd.mothership_id, DEVICE_ID,
            sizeof(cmd.mothership_id) - 1);
    cmd.intervalMinutes = intervalMinutes;

    bool anySent = false;

    for (const auto& node : registeredNodes) {
        if (node.state != PAIRED && node.state != DEPLOYED) continue;

        ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

        esp_err_t res = esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
        Serial.printf("📤 SET_SCHEDULE %d min -> %s (%s) : %s\n",
                      intervalMinutes,
                      node.nodeId.c_str(),
                      stateToStr(node.state),
                      (res==ESP_OK) ? "OK" : esp_err_to_name(res));
        if (res == ESP_OK) anySent = true;
    }
    return anySent;
}

bool sendTimeSync(const uint8_t* mac, const char* nodeId) {
    time_sync_response_t resp{};
    strcpy(resp.command, "TIME_SYNC");
    strncpy(resp.mothership_id, DEVICE_ID,
            sizeof(resp.mothership_id)-1);

    char ts[24];
    getRTCTimeString(ts, sizeof(ts));

    int y,m,d,H,M,S;
    if (sscanf(ts, "%d-%d-%d %d:%d:%d", &y,&m,&d,&H,&M,&S) == 6) {
        resp.year   = y;
        resp.month  = m;
        resp.day    = d;
        resp.hour   = H;
        resp.minute = M;
        resp.second = S;

        ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

        esp_err_t r = esp_now_send(mac, (uint8_t*)&resp, sizeof(resp));
        if (r == ESP_OK) {
            // NEW: update the NodeInfo record
            for (auto &n : registeredNodes) {
                if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(nodeId)) {
                    n.lastTimeSyncMs = millis();
                    break;
                }
            }

            Serial.printf("✅ TIME_SYNC → %s (%s) @ %s\n",
                          nodeId,
                          macToStr(mac).c_str(),
                          ts);
            return true;
        }
        Serial.printf("❌ Time sync send fail to %s (%s) : %s\n",
                      nodeId,
                      macToStr(mac).c_str(),
                      esp_err_to_name(r));
    } else {
        Serial.println("❌ Failed to parse RTC time for TIME_SYNC");
    }
    return false;
}

bool broadcastTimeSyncAll() {
    uint32_t nowMs   = millis();
    uint16_t targeted = 0;
    uint16_t okCount  = 0;

    for (auto &node : registeredNodes) {
        if (!node.isActive) continue;
        if (node.state != PAIRED && node.state != DEPLOYED) continue;

        targeted++;
        bool ok = sendTimeSync(node.mac, node.nodeId.c_str());
        if (ok) {
            node.lastTimeSyncMs = nowMs;
            okCount++;
        }
    }

    if (targeted == 0) {
        Serial.println("⚠️ Fleet TIME_SYNC: no eligible PAIRED/DEPLOYED nodes");
    } else {
        Serial.printf("⏰ Fleet TIME_SYNC broadcast: targeted=%u, success=%u\n",
                      targeted, okCount);
    }
    return (okCount > 0);
}

bool broadcastTimeSyncIfDue(bool force) {
    uint32_t nowMs = millis();

    if (!force) {
        if (g_lastFleetTimeSyncMs != 0 &&
            (uint32_t)(nowMs - g_lastFleetTimeSyncMs) < FLEET_SYNC_INTERVAL_MS) {
            return false;  // not due yet
        }
    }

    bool any = broadcastTimeSyncAll();
    if (any) {
        g_lastFleetTimeSyncMs = nowMs;

        char ts[24];
        getRTCTimeString(ts, sizeof(ts));

        Serial.printf("⏰ Fleet TIME_SYNC triggered (force=%d) at %s\n",
                      force ? 1 : 0, ts);

        // Optional: log to CSV as a mothership event
        String row = String(ts) + ",MOTHERSHIP," +
                     getMothershipsMAC() + ",TIME_SYNC_FLEET,OK";
        logCSVRow(row);
    }
    return any;
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

    // Retry once
    ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
    esp_err_t r2 = esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
    return (r2 == ESP_OK);
}

// Pair a specific node (deterministic + immediate ack)
bool pairNode(const String& nodeId) {
    for (auto& node : registeredNodes) {
        if (node.nodeId == nodeId) {

            // Ensure peer is present and on the right channel
            ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
            esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
            delay(10);

            // 1) Explicit PAIR_NODE command
            pairing_command_t pairCmd{};
            strcpy(pairCmd.command, "PAIR_NODE");
            strcpy(pairCmd.nodeId, nodeId.c_str());
            strncpy(pairCmd.mothership_id, DEVICE_ID, sizeof(pairCmd.mothership_id) - 1);

            esp_err_t sendPairCmd = esp_now_send(node.mac, (uint8_t*)&pairCmd, sizeof(pairCmd));

            // 2) Flip local state and persist
            node.state = PAIRED;

            // 3) Immediate PAIRING_RESPONSE
            pairing_response_t resp{};
            strcpy(resp.command, "PAIRING_RESPONSE");
            strcpy(resp.nodeId, nodeId.c_str());
            resp.isPaired = true;
            strncpy(resp.mothership_id, DEVICE_ID, sizeof(resp.mothership_id) - 1);

            esp_err_t sendResp = esp_now_send(node.mac, (uint8_t*)&resp, sizeof(resp));

            Serial.printf("📤 pairNode(%s): PAIR_NODE=%s, PAIRING_RESPONSE=%s\n",
                          nodeId.c_str(),
                          (sendPairCmd == ESP_OK ? "OK" : esp_err_to_name(sendPairCmd)),
                          (sendResp   == ESP_OK ? "OK" : esp_err_to_name(sendResp)));

            savePairedNodes();

            // Success if either packet went out
            return (sendPairCmd == ESP_OK) || (sendResp == ESP_OK);
        }
    }
    Serial.printf("⚠️ pairNode(%s): nodeId not found\n", nodeId.c_str());
    return false;
}

// Deploy selected paired nodes with RTC time
bool deploySelectedNodes(const std::vector<String>& nodeIds) {
    bool allSuccess  = true;
    bool anyDeployed = false;

    for (const String& nodeId : nodeIds) {
        for (auto& node : registeredNodes) {
            if (node.nodeId == nodeId &&
                (node.state == PAIRED || node.state == DEPLOYED)) {

                deployment_command_t deployCmd{};
                strcpy(deployCmd.command, "DEPLOY_NODE");
                strcpy(deployCmd.nodeId,  nodeId.c_str());
                strcpy(deployCmd.mothership_id, DEVICE_ID);

                char timeBuffer[24];
                getRTCTimeString(timeBuffer, sizeof(timeBuffer));

                int year, month, day, hour, minute, second;
                if (sscanf(timeBuffer, "%d-%d-%d %d:%d:%d",
                           &year, &month, &day,
                           &hour, &minute, &second) == 6)
                {
                    deployCmd.year   = year;
                    deployCmd.month  = month;
                    deployCmd.day    = day;
                    deployCmd.hour   = hour;
                    deployCmd.minute = minute;
                    deployCmd.second = second;

                    esp_err_t result =
                        esp_now_send(node.mac,
                                     (uint8_t*)&deployCmd,
                                     sizeof(deployCmd));

                    if (result == ESP_OK) {
                        node.state = DEPLOYED;
                        anyDeployed = true;
                        Serial.printf("🚀 Node deployed: %s at %s\n",
                                      nodeId.c_str(), timeBuffer);
                    } else {
                        Serial.printf("❌ Failed to deploy node: %s (%s)\n",
                                      nodeId.c_str(), esp_err_to_name(result));
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

    if (!anyDeployed) {
        Serial.println("⚠️ deploySelectedNodes: no matching nodes in PAIRED/DEPLOYED state");
    } else {
        savePairedNodes();
    }
    return allSuccess;
}


// ----------------- Queries & unpair -----------------
std::vector<NodeInfo> getUnpairedNodes() {
    std::vector<NodeInfo> unpaired;
    for (const auto& node : registeredNodes) {
        if (node.state == UNPAIRED && node.isActive) {
            unpaired.push_back(node);
        }
    }
    return unpaired;
}

std::vector<NodeInfo> getPairedNodes() {
    std::vector<NodeInfo> paired;
    for (const auto& node : registeredNodes) {
        if (node.state == PAIRED && node.isActive) {
            paired.push_back(node);
        }
    }
    return paired;
}

std::vector<NodeInfo> getRegisteredNodes() {
    return registeredNodes;
}

String getMothershipsMAC() {
    return WiFi.macAddress();
}

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
        Serial.print(macToStr(node.mac));
        Serial.print(") - ");
        Serial.print(node.isActive ? "Active" : "Inactive");
        Serial.print(" state=");
        Serial.print(stateToStr(node.state));
        Serial.print(" userId=");
        Serial.print(node.userId);
        Serial.print(" name='");
        Serial.print(node.name);
        Serial.println("'");
    }
}

bool unpairNode(const String& nodeId) {
    for (auto &n : registeredNodes) {
        if (n.nodeId == nodeId) {
            esp_now_del_peer(n.mac);   // remove peer
            n.state    = UNPAIRED;
            n.isActive = true;
            savePairedNodes();
            Serial.print("🗑️ Unpaired node: ");
            Serial.println(nodeId);
            return true;
        }
    }
    return false;
}

bool sendUnpairToNode(const String& nodeId) {
    for (const auto &n : registeredNodes) {
        if (n.nodeId == nodeId) {
            unpair_command_t cmd{};
            strncpy(cmd.command, "UNPAIR_NODE", sizeof(cmd.command) - 1);
            strncpy(cmd.mothership_id, DEVICE_ID, sizeof(cmd.mothership_id) - 1);

            ensurePeerOnChannel(n.mac, ESPNOW_CHANNEL);
            esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

            esp_err_t res = esp_now_send(n.mac, (uint8_t*)&cmd, sizeof(cmd));
            Serial.printf("📤 UNPAIR_NODE -> %s (%s)\n",
                          nodeId.c_str(), esp_err_to_name(res));
            return (res == ESP_OK);
        }
    }
    Serial.printf("⚠️ sendUnpairToNode: node %s not found\n", nodeId.c_str());
    return false;
}
