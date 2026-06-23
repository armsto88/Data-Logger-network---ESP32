#include "comms/espnow_config.h"
#include "config/node_registry.h"
#include "time/rtc_alarm.h"
#include "storage/flash_logger.h"
#include "system/pins.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Config-mode ESP-NOW command layer for Mothership V1.
// Slim extract from production espnow_manager.cpp.

static const char* kDeviceId = "001";
static constexpr uint8_t kBroadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static String macToStr(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

static void ensurePeerOnChannel(const uint8_t mac[6], uint8_t channel) {
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_del_peer(mac);
  esp_now_add_peer(&peer);
}

static void sendSnapshotAck(const uint8_t* mac, const node_snapshot_t& snap, bool persisted) {
  if (!mac) return;

  snapshot_ack_t ack{};
  strncpy(ack.command, "SNAPSHOT_ACK", sizeof(ack.command) - 1);
  strncpy(ack.nodeId, snap.nodeId, sizeof(ack.nodeId) - 1);
  ack.seqNum = snap.seqNum;
  ack.persisted = persisted ? 1 : 0;
  ack.protocolVersion = NODE_PROTOCOL_VERSION;

  ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
  esp_err_t res = esp_now_send(mac, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
  Serial.printf("[SNAP-ACK] %.15s seq=%lu persisted=%u send=%s\n",
                ack.nodeId, static_cast<unsigned long>(ack.seqNum),
                static_cast<unsigned>(ack.persisted),
                res == ESP_OK ? "OK" : esp_err_to_name(res));
}

// RTC helpers — adapt production's getRTCTimeUnix()/getRTCTimeString() to V1.
static uint32_t getRTCTimeUnix() {
  return getRTCTime();
}

static void getRTCTimeString(char* buf, size_t bufLen) {
  uint32_t u = getRTCTime();
  if (u == 0) {
    snprintf(buf, bufLen, "RTC unset");
    return;
  }
  time_t t = (time_t)u;
  struct tm* tm = gmtime(&t);
  snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec);
}

// -----------------------------------------------------------------------------
// Receive callback
// -----------------------------------------------------------------------------

static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[ESPNOW-CFG] RX %s len=%d\n", macStr, len);

  // Debug: print struct sizes and first bytes of data
  Serial.printf("[DBG] sizeof: snap=%u hello=%u status=%u disc=%u discResp=%u pairReq=%u timeReq=%u deployAck=%u cfgAck=%u\n",
                (unsigned)sizeof(node_snapshot_t),
                (unsigned)sizeof(node_hello_message_t),
                (unsigned)sizeof(node_status_message_t),
                (unsigned)sizeof(discovery_message_t),
                (unsigned)sizeof(discovery_response_t),
                (unsigned)sizeof(pairing_request_t),
                (unsigned)sizeof(time_sync_request_t),
                (unsigned)sizeof(deployment_ack_message_t),
                (unsigned)sizeof(config_apply_ack_message_t));
  // Print first 20 bytes as string (command field area)
  char cmdBuf[21];
  memcpy(cmdBuf, data, (len < 20 ? len : 20));
  cmdBuf[20] = '\0';
  Serial.printf("[DBG] first 20 bytes: '%s'\n", cmdBuf);

  // ---- Command-based dispatch ----
  // Length-only matching collides (several structs share sizes), so we read
  // the command string first and dispatch by command, then verify length.

  // NODE_SNAPSHOT (124 bytes) — unique size, handle directly.
  if (len == (int)sizeof(node_snapshot_t)) {
    node_snapshot_t snap{};
    memcpy(&snap, data, sizeof(snap));
    if (strncmp(snap.command, "NODE_SNAPSHOT", 15) == 0) {
      registerNode(mac, snap.nodeId, "MULTI_ENV", DEPLOYED);

      for (auto& n : registeredNodes) {
        if (n.nodeId == String(snap.nodeId)) {
          if (snap.nodeTimestamp > 0 && snap.nodeTimestamp > n.lastNodeTimestamp) {
            n.lastNodeTimestamp = snap.nodeTimestamp;
          }
          if (snap.configVersion > 0 && snap.configVersion > n.configVersionApplied) {
            n.configVersionApplied = snap.configVersion;
          }
          if ((snap.sensorPresent & SNAP_PRESENT_BAT_V) && !isnan(snap.batVoltage)) {
            n.lastReportedBatV = snap.batVoltage;
          }
          break;
        }
      }

      bool persisted = false;
      if (flashIsReady()) {
        persisted = logSnapshotRow(&snap);
        if (!persisted) {
          Serial.println("[SNAP] Flash logging failed");
        }
      } else {
        Serial.println("[SNAP] Flash unavailable; snapshot not durably logged");
      }
      sendSnapshotAck(mac, snap, persisted);
      Serial.printf("[SNAP] %s seq=%lu present=0x%04X bat=%.2f\n",
                    snap.nodeId, (unsigned long)snap.seqNum,
                    (unsigned)snap.sensorPresent, snap.batVoltage);
      return;
    }
    // Fall through: size matched but command didn't — try generic dispatch.
  }

  // Extract command string. Most structs have command at offset 0.
  // discovery_message_t has command at offset 32 (after nodeId[16] + nodeType[16]).
  char cmd0[24] = {0};
  char cmd32[24] = {0};
  if (len >= (int)sizeof(cmd0) - 1) {
    memcpy(cmd0, data, sizeof(cmd0) - 1);
    cmd0[sizeof(cmd0) - 1] = '\0';
  } else if (len > 0) {
    memcpy(cmd0, data, len);
  }
  if (len >= 32 + (int)sizeof(cmd32) - 1) {
    memcpy(cmd32, data + 32, sizeof(cmd32) - 1);
    cmd32[sizeof(cmd32) - 1] = '\0';
  }

  // Helper to test a command field safely.
  auto cmdMatches = [](const char* field, const char* expected) {
    return field != nullptr && strncmp(field, expected, strlen(expected)) == 0;
  };

  // --- NODE_HELLO ---
  if (cmdMatches(cmd0, "NODE_HELLO")) {
    if (len >= (int)sizeof(node_hello_message_t)) {
      node_hello_message_t hello{};
      memcpy(&hello, data, sizeof(hello));
      handleNodeHello(mac, hello);
    } else {
      Serial.printf("[ESPNOW-CFG] NODE_HELLO too short: %d < %u\n", len, (unsigned)sizeof(node_hello_message_t));
    }
    return;
  }

  // --- NODE_STATUS ---
  if (cmdMatches(cmd0, "NODE_STATUS")) {
    if (len >= (int)sizeof(node_status_message_t)) {
      node_status_message_t st{};
      memcpy(&st, data, sizeof(st));
      NodeState reported = (st.state <= (uint8_t)DEPLOYED) ? (NodeState)st.state : UNPAIRED;
      registerNode(mac, st.nodeId, "status", reported);
      for (auto& n : registeredNodes) {
        if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(st.nodeId)) {
          n.lastSeen = millis();
          n.isActive = true;
          n.state = reported;
          savePairedNodes();
          break;
        }
      }
      Serial.printf("[STATUS] %s state=%u rtcUnix=%lu\n",
                    st.nodeId, (unsigned)st.state, (unsigned long)st.rtcUnix);
    } else {
      Serial.printf("[ESPNOW-CFG] NODE_STATUS too short: %d < %u\n", len, (unsigned)sizeof(node_status_message_t));
    }
    return;
  }

  // --- DISCOVER_REQUEST (command at offset 32) ---
  if (cmdMatches(cmd32, "DISCOVER_REQUEST")) {
    if (len >= (int)sizeof(discovery_message_t)) {
      discovery_message_t discovery{};
      memcpy(&discovery, data, sizeof(discovery));
      Serial.printf("[DISCOVERY] from %s (%s) MAC=%s\n",
                    discovery.nodeId, discovery.nodeType, macStr);
      NodeState keep = UNPAIRED;
      for (const auto& n : registeredNodes) {
        if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(discovery.nodeId)) {
          keep = n.state;
          break;
        }
      }
      registerNode(mac, discovery.nodeId, discovery.nodeType, keep);

      discovery_response_t response{};
      strcpy(response.command, "DISCOVER_RESPONSE");
      strcpy(response.mothership_id, kDeviceId);
      response.acknowledged = true;
      esp_now_send(kBroadcastAddr, (uint8_t*)&response, sizeof(response));
    } else {
      Serial.printf("[ESPNOW-CFG] DISCOVER_REQUEST too short: %d < %u\n", len, (unsigned)sizeof(discovery_message_t));
    }
    return;
  }

  // --- PAIRING_REQUEST ---
  if (cmdMatches(cmd0, "PAIRING_REQUEST")) {
    if (len >= (int)sizeof(pairing_request_t)) {
      pairing_request_t request{};
      memcpy(&request, data, sizeof(request));
      Serial.printf("[PAIR-POLL] from %s MAC=%s\n", request.nodeId, macStr);
      NodeState keep = UNPAIRED;
      for (const auto& n : registeredNodes) {
        if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(request.nodeId)) {
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
      strcpy(response.mothership_id, kDeviceId);
      esp_now_send(kBroadcastAddr, (uint8_t*)&response, sizeof(response));
    } else {
      Serial.printf("[ESPNOW-CFG] PAIRING_REQUEST too short: %d < %u\n", len, (unsigned)sizeof(pairing_request_t));
    }
    return;
  }

  // --- REQUEST_TIME (time sync request) ---
  if (cmdMatches(cmd0, "REQUEST_TIME")) {
    if (len >= (int)sizeof(time_sync_request_t)) {
      time_sync_request_t req{};
      memcpy(&req, data, sizeof(req));
      Serial.printf("[TIME-REQ] from %s MAC=%s\n", req.nodeId, macStr);
      sendTimeSync(mac, req.nodeId);
    } else {
      Serial.printf("[ESPNOW-CFG] REQUEST_TIME too short: %d < %u\n", len, (unsigned)sizeof(time_sync_request_t));
    }
    return;
  }

  // --- DEPLOY_ACK ---
  if (cmdMatches(cmd0, "DEPLOY_ACK")) {
    if (len >= (int)sizeof(deployment_ack_message_t)) {
      deployment_ack_message_t ack{};
      memcpy(&ack, data, sizeof(ack));
      if (ack.deployed == 1) {
        Serial.printf("[DEPLOY-ACK] %s rtcUnix=%lu\n", ack.nodeId, (unsigned long)ack.rtcUnix);
        registerNode(mac, ack.nodeId, "unknown", DEPLOYED);
        for (auto& n : registeredNodes) {
          if (n.nodeId == String(ack.nodeId) || memcmp(n.mac, mac, 6) == 0) {
            if (n.deployPending) n.deployPending = false;
            NodeDesiredConfig desired = getDesiredConfig(ack.nodeId);
            if (desired.configVersion > 0 && n.configVersionApplied < desired.configVersion) {
              n.configVersionApplied = desired.configVersion;
            }
            break;
          }
        }
        savePairedNodes();
      }
    } else {
      Serial.printf("[ESPNOW-CFG] DEPLOY_ACK too short: %d < %u\n", len, (unsigned)sizeof(deployment_ack_message_t));
    }
    return;
  }

  // --- CONFIG_ACK (config apply ack) ---
  if (cmdMatches(cmd0, "CONFIG_ACK")) {
    if (len >= (int)sizeof(config_apply_ack_message_t)) {
      config_apply_ack_message_t ack{};
      memcpy(&ack, data, sizeof(ack));
      Serial.printf("[CONFIG-ACK] %s v=%u ok=%u\n", ack.nodeId, (unsigned)ack.appliedVersion, (unsigned)ack.ok);
      for (auto& n : registeredNodes) {
        if (n.nodeId == String(ack.nodeId)) {
          if (ack.ok) n.configVersionApplied = ack.appliedVersion;
          break;
        }
      }
    } else {
      Serial.printf("[ESPNOW-CFG] CONFIG_ACK too short: %d < %u\n", len, (unsigned)sizeof(config_apply_ack_message_t));
    }
    return;
  }

  // --- Commands a mothership should only send, never receive — ignore. ---
  if (cmdMatches(cmd0, "DISCOVERY_SCAN") || cmdMatches(cmd32, "DISCOVERY_SCAN") ||
      cmdMatches(cmd0, "DISCOVER_RESPONSE") || cmdMatches(cmd32, "DISCOVER_RESPONSE") ||
      cmdMatches(cmd0, "PAIRING_RESPONSE") ||
      cmdMatches(cmd0, "TIME_SYNC") ||
      cmdMatches(cmd0, "DEPLOY_NODE") ||
      cmdMatches(cmd0, "PAIR_NODE") ||
      cmdMatches(cmd0, "CONFIG_SNAPSHOT") ||
      cmdMatches(cmd0, "SET_SCHEDULE") ||
      cmdMatches(cmd0, "SET_SYNC_SCHED") ||
      cmdMatches(cmd0, "UNPAIR_NODE") ||
      cmdMatches(cmd0, "SYNC_WINDOW_OPEN")) {
    // Echo from another mothership or own broadcast — ignore in config mode.
    return;
  }

  // --- Legacy sensor_data_message_t fallback (no command field) ---
  if (len >= (int)sizeof(sensor_data_message_t)) {
    const sensor_data_message_t* msg = reinterpret_cast<const sensor_data_message_t*>(data);
    if (flashIsReady()) {
      char row[256];
      snprintf(row, sizeof(row), "%lu,%.15s,,,,,%.4f,%.4f,%.4f",
               (unsigned long)millis(), msg->nodeId, msg->value, 0.0f, 0.0f);
      flashLogCSVRow(String(row));
    }
    return;
  }

  Serial.printf("[ESPNOW-CFG] unhandled packet from %s len=%d cmd0='%.20s' cmd32='%.20s'\n",
                macStr, len, cmd0, cmd32);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool initEspNowConfig(int channel) {
  // AP + STA so the web UI and ESP-NOW share one RF channel.
  WiFi.mode(WIFI_AP_STA);
  esp_wifi_set_channel(static_cast<uint8_t>(channel), WIFI_SECOND_CHAN_NONE);
  Serial.printf("[ESPNOW-CFG] AP+STA mode, channel %d\n", channel);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW-CFG] Init failed");
    return false;
  }
  Serial.println("[ESPNOW-CFG] Init OK");

  esp_now_register_recv_cb(onEspNowRecv);

  // Broadcast peer
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, kBroadcastAddr, 6);
  peer.channel = static_cast<uint8_t>(channel);
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESPNOW-CFG] Add broadcast peer failed (may already exist)");
  }

  Serial.println("[ESPNOW-CFG] Config mode ready");
  return true;
}

void espnowConfigPoll() {
  // No continuous loop — all receive handling is via the callback.
}

// -----------------------------------------------------------------------------
// Commands / broadcasts
// -----------------------------------------------------------------------------

bool sendDiscoveryBroadcast() {
  discovery_response_t pkt{};
  strcpy(pkt.command, "DISCOVERY_SCAN");
  strcpy(pkt.mothership_id, kDeviceId);
  pkt.acknowledged = false;

  ensurePeerOnChannel(kBroadcastAddr, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_err_t r = esp_now_send(kBroadcastAddr, (uint8_t*)&pkt, sizeof(pkt));
  if (r == ESP_OK) return true;
  // Retry once
  esp_err_t r2 = esp_now_send(kBroadcastAddr, (uint8_t*)&pkt, sizeof(pkt));
  return (r2 == ESP_OK);
}

bool pairNode(const String& nodeId) {
  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId) {
      ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
      esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
      delay(10);

      pairing_command_t pairCmd{};
      strcpy(pairCmd.command, "PAIR_NODE");
      strcpy(pairCmd.nodeId, nodeId.c_str());
      strncpy(pairCmd.mothership_id, kDeviceId, sizeof(pairCmd.mothership_id) - 1);

      esp_err_t sendPairCmd = esp_now_send(node.mac, (uint8_t*)&pairCmd, sizeof(pairCmd));
      esp_err_t sendPairCmdBcast = esp_now_send(kBroadcastAddr, (uint8_t*)&pairCmd, sizeof(pairCmd));

      node.state = PAIRED;

      pairing_response_t resp{};
      strcpy(resp.command, "PAIRING_RESPONSE");
      strcpy(resp.nodeId, nodeId.c_str());
      resp.isPaired = true;
      strncpy(resp.mothership_id, kDeviceId, sizeof(resp.mothership_id) - 1);

      esp_err_t sendResp = esp_now_send(node.mac, (uint8_t*)&resp, sizeof(resp));
      esp_err_t sendRespBcast = esp_now_send(kBroadcastAddr, (uint8_t*)&resp, sizeof(resp));

      Serial.printf("[PAIR] %s: PAIR_NODE=%s bcast=%s resp=%s bcast=%s\n",
                    nodeId.c_str(),
                    (sendPairCmd == ESP_OK ? "OK" : esp_err_to_name(sendPairCmd)),
                    (sendPairCmdBcast == ESP_OK ? "OK" : esp_err_to_name(sendPairCmdBcast)),
                    (sendResp == ESP_OK ? "OK" : esp_err_to_name(sendResp)),
                    (sendRespBcast == ESP_OK ? "OK" : esp_err_to_name(sendRespBcast)));

      savePairedNodes();
      return (sendPairCmd == ESP_OK) || (sendResp == ESP_OK) ||
             (sendPairCmdBcast == ESP_OK) || (sendRespBcast == ESP_OK);
    }
  }
  Serial.printf("[PAIR] %s: nodeId not found\n", nodeId.c_str());
  return false;
}

bool deploySelectedNodes(const std::vector<String>& nodeIds) {
  bool allSuccess = true;
  bool anyRequested = false;

  for (const String& nodeId : nodeIds) {
    for (auto& node : registeredNodes) {
      if (node.nodeId == nodeId &&
          (node.state == PAIRED || node.state == DEPLOYED)) {

        deployment_command_t deployCmd{};
        strcpy(deployCmd.command, "DEPLOY_NODE");
        strcpy(deployCmd.nodeId, nodeId.c_str());
        strcpy(deployCmd.mothership_id, kDeviceId);

        char timeBuffer[24];
        getRTCTimeString(timeBuffer, sizeof(timeBuffer));
        int year, month, day, hour, minute, second;
        if (sscanf(timeBuffer, "%d-%d-%d %d:%d:%d",
                   &year, &month, &day, &hour, &minute, &second) != 6) {
          Serial.println("[DEPLOY] Failed to parse RTC time");
          allSuccess = false;
          break;
        }
        if (year < 2024 || year > 2099) {
          Serial.printf("[DEPLOY] RTC invalid (%04d) — aborting deploy for %s\n", year, nodeId.c_str());
          allSuccess = false;
          break;
        }

        const uint32_t nowUnix = getRTCTimeUnix();
        NodeDesiredConfig desired = getDesiredConfig(node.nodeId.c_str());

        deployCmd.year   = year;
        deployCmd.month  = month;
        deployCmd.day    = day;
        deployCmd.hour   = hour;
        deployCmd.minute = minute;
        deployCmd.second = second;
        deployCmd.configVersion = (desired.configVersion > 0) ? desired.configVersion : 1;
        deployCmd.wakeIntervalMin = (desired.wakeIntervalMin > 0)
            ? desired.wakeIntervalMin
            : (node.wakeIntervalMin > 0 ? node.wakeIntervalMin : DEFAULT_WAKE_INTERVAL_MINUTES);
        deployCmd.syncIntervalMin = desired.syncIntervalMin;
        if (deployCmd.syncIntervalMin == 0 && desired.syncPhaseUnix == 0) {
          deployCmd.syncIntervalMin = 15;
        }
        deployCmd.syncPhaseUnix = (desired.syncPhaseUnix > 0)
            ? desired.syncPhaseUnix
            : (nowUnix - (nowUnix % 60UL));

        Serial.printf("[DEPLOY] %s: cfgV=%u wakeMin=%u syncMin=%u phase=%lu\n",
                      nodeId.c_str(),
                      (unsigned)deployCmd.configVersion,
                      (unsigned)deployCmd.wakeIntervalMin,
                      (unsigned)deployCmd.syncIntervalMin,
                      (unsigned long)deployCmd.syncPhaseUnix);

        esp_err_t result = esp_now_send(node.mac, (uint8_t*)&deployCmd, sizeof(deployCmd));
        esp_err_t resultBcast = esp_now_send(kBroadcastAddr, (uint8_t*)&deployCmd, sizeof(deployCmd));

        node.state = DEPLOYED;
        node.deployPending = true;
        anyRequested = true;

        if (result == ESP_OK || resultBcast == ESP_OK) {
          Serial.printf("[DEPLOY] %s requested at %s (direct=%s bcast=%s)\n",
                        nodeId.c_str(), timeBuffer,
                        (result == ESP_OK ? "OK" : esp_err_to_name(result)),
                        (resultBcast == ESP_OK ? "OK" : esp_err_to_name(resultBcast)));
          sendTimeSync(node.mac, nodeId.c_str());
          node.wakeIntervalMin = deployCmd.wakeIntervalMin;
        } else {
          Serial.printf("[DEPLOY] %s send missed (direct=%s bcast=%s)\n",
                        nodeId.c_str(),
                        esp_err_to_name(result), esp_err_to_name(resultBcast));
          allSuccess = false;
        }
        break;
      }
    }
  }

  if (!anyRequested) {
    Serial.println("[DEPLOY] no matching PAIRED/DEPLOYED nodes");
    allSuccess = false;
  } else {
    savePairedNodes();
  }
  return allSuccess;
}

bool sendUnpairToNode(const String& nodeId) {
  for (const auto& n : registeredNodes) {
    if (n.nodeId == nodeId) {
      unpair_command_t cmd{};
      strncpy(cmd.command, "UNPAIR_NODE", sizeof(cmd.command) - 1);
      strncpy(cmd.nodeId, nodeId.c_str(), sizeof(cmd.nodeId) - 1);
      strncpy(cmd.mothership_id, kDeviceId, sizeof(cmd.mothership_id) - 1);

      ensurePeerOnChannel(n.mac, ESPNOW_CHANNEL);
      esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

      bool anyOk = false;
      for (int i = 0; i < 3; ++i) {
        esp_err_t resDirect = esp_now_send(n.mac, (uint8_t*)&cmd, sizeof(cmd));
        esp_err_t resBcast  = esp_now_send(kBroadcastAddr, (uint8_t*)&cmd, sizeof(cmd));
        if (resDirect == ESP_OK || resBcast == ESP_OK) anyOk = true;
        delay(20);
      }
      Serial.printf("[UNPAIR] -> %s (burst=%s)\n", nodeId.c_str(), anyOk ? "OK" : "FAILED");
      return anyOk;
    }
  }
  Serial.printf("[UNPAIR] %s not found\n", nodeId.c_str());
  return false;
}

bool sendTimeSync(const uint8_t* mac, const char* nodeId) {
  time_sync_response_t resp{};
  strcpy(resp.command, "TIME_SYNC");
  strncpy(resp.mothership_id, kDeviceId, sizeof(resp.mothership_id) - 1);

  char ts[24];
  getRTCTimeString(ts, sizeof(ts));
  int y, m, d, H, M, S;
  if (sscanf(ts, "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S) != 6) {
    Serial.println("[TIME-SYNC] Failed to parse RTC time");
    return false;
  }
  resp.year = y; resp.month = m; resp.day = d;
  resp.hour = H; resp.minute = M; resp.second = S;

  ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_err_t r = esp_now_send(mac, (uint8_t*)&resp, sizeof(resp));
  if (r == ESP_OK) {
    for (auto& n : registeredNodes) {
      if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(nodeId)) {
        n.lastTimeSyncMs = millis();
        break;
      }
    }
    Serial.printf("[TIME-SYNC] -> %s (%s) OK\n", nodeId, macToStr(mac).c_str());
    return true;
  }
  Serial.printf("[TIME-SYNC] -> %s (%s) FAIL: %s\n", nodeId, macToStr(mac).c_str(), esp_err_to_name(r));
  return false;
}

bool broadcastTimeSyncAll() {
  uint16_t targeted = 0;
  uint16_t okCount = 0;
  for (auto& node : registeredNodes) {
    if (node.state != PAIRED && node.state != DEPLOYED) continue;
    targeted++;
    if (sendTimeSync(node.mac, node.nodeId.c_str())) okCount++;
  }
  if (targeted == 0) {
    Serial.println("[TIME-SYNC] no PAIRED/DEPLOYED nodes");
  } else {
    Serial.printf("[TIME-SYNC] fleet: targeted=%u success=%u\n", targeted, okCount);
  }
  return (okCount > 0);
}

bool broadcastWakeInterval(int intervalMinutes) {
  schedule_command_message_t cmd{};
  strcpy(cmd.command, "SET_SCHEDULE");
  strncpy(cmd.mothership_id, kDeviceId, sizeof(cmd.mothership_id) - 1);
  cmd.intervalMinutes = intervalMinutes;

  bool anySent = false;
  for (auto& node : registeredNodes) {
    if (node.state != PAIRED && node.state != DEPLOYED) continue;
    ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
    Serial.printf("[SCHED] SET_SCHEDULE %d -> %s: %s\n",
                  intervalMinutes, node.nodeId.c_str(),
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    if (res == ESP_OK) {
      anySent = true;
      node.wakeIntervalMin = (uint8_t)min(intervalMinutes, 255);
    }
  }
  return anySent;
}

bool broadcastSyncSchedule(int syncIntervalMinutes, unsigned long phaseUnix) {
  sync_schedule_command_message_t cmd{};
  strcpy(cmd.command, "SET_SYNC_SCHED");
  strncpy(cmd.mothership_id, kDeviceId, sizeof(cmd.mothership_id) - 1);
  cmd.syncIntervalMinutes = (unsigned long)syncIntervalMinutes;
  cmd.phaseUnix = phaseUnix;

  bool anyEligible = false;
  for (const auto& n : registeredNodes) {
    if (n.state == PAIRED || n.state == DEPLOYED) { anyEligible = true; break; }
  }
  if (!anyEligible) {
    Serial.println("[SYNC-SCHED] skipped: no PAIRED/DEPLOYED nodes");
    return false;
  }

  ensurePeerOnChannel(kBroadcastAddr, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  bool anyOk = false;
  for (uint8_t i = 0; i < 3; ++i) {
    esp_err_t res = esp_now_send(kBroadcastAddr, (uint8_t*)&cmd, sizeof(cmd));
    if (res == ESP_OK) anyOk = true;
    Serial.printf("[SYNC-SCHED] burst %u/%u %d min phase=%lu -> %s\n",
                  (unsigned)(i + 1), (unsigned)3, syncIntervalMinutes,
                  (unsigned long)phaseUnix, (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    if (i + 1 < 3) delay(200);
  }
  return anyOk;
}

bool broadcastSyncWindowOpen(unsigned long phaseUnix) {
  sync_schedule_command_message_t cmd{};
  strcpy(cmd.command, "SYNC_WINDOW_OPEN");
  strncpy(cmd.mothership_id, kDeviceId, sizeof(cmd.mothership_id) - 1);
  cmd.syncIntervalMinutes = 0;
  cmd.phaseUnix = phaseUnix;

  ensurePeerOnChannel(kBroadcastAddr, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  bool anyOk = false;
  for (uint8_t i = 0; i < 3; ++i) {
    esp_err_t res = esp_now_send(kBroadcastAddr, (uint8_t*)&cmd, sizeof(cmd));
    if (res == ESP_OK) anyOk = true;
    Serial.printf("[SYNC-WIN] burst %u/%u phase=%lu -> %s\n",
                  (unsigned)(i + 1), (unsigned)3,
                  (unsigned long)phaseUnix, (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    if (i + 1 < 3) delay(200);
  }
  return anyOk;
}

bool sendConfigSnapshot(const uint8_t* mac, const char* nodeId) {
  NodeDesiredConfig desired = getDesiredConfig(nodeId);
  if (desired.configVersion == 0) return false;

  for (auto& n : registeredNodes) {
    if (n.nodeId == String(nodeId) && n.configVersionApplied >= desired.configVersion) {
      Serial.printf("[CFG-SNAP] %s up-to-date (v%u <= v%u)\n",
                    nodeId, (unsigned)n.configVersionApplied, (unsigned)desired.configVersion);
      return false;
    }
  }

  config_snapshot_message_t snap{};
  strcpy(snap.command, "CONFIG_SNAPSHOT");
  strncpy(snap.mothership_id, kDeviceId, sizeof(snap.mothership_id) - 1);
  snap.configVersion    = desired.configVersion;
  snap.wakeIntervalMin  = desired.wakeIntervalMin;
  snap.syncIntervalMin  = desired.syncIntervalMin;
  snap.syncPhaseUnix    = desired.syncPhaseUnix;

  ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t res = esp_now_send(mac, (uint8_t*)&snap, sizeof(snap));
  Serial.printf("[CFG-SNAP] v%u -> %s: %s\n",
                (unsigned)snap.configVersion, nodeId,
                (res == ESP_OK) ? "OK" : esp_err_to_name(res));
  return (res == ESP_OK);
}

void handleNodeHello(const uint8_t* senderMac, const node_hello_message_t& hello) {
  Serial.printf("[HELLO] %s cfgV=%u wakeMin=%u qDepth=%u rtcUnix=%lu\n",
                hello.nodeId, (unsigned)hello.configVersion,
                (unsigned)hello.wakeIntervalMin, (unsigned)hello.queueDepth,
                (unsigned long)hello.rtcUnix);

  NodeState helloState = DEPLOYED;
  for (const auto& n : registeredNodes) {
    if (n.nodeId == String(hello.nodeId) || memcmp(n.mac, senderMac, 6) == 0) {
      if (n.state == UNPAIRED) helloState = n.state;
      break;
    }
  }
  registerNode(senderMac, hello.nodeId, hello.nodeType, helloState);

  for (auto& n : registeredNodes) {
    if (n.nodeId == String(hello.nodeId)) {
      n.wakeIntervalMin = hello.wakeIntervalMin;
      n.lastReportedQueueDepth = hello.queueDepth;
      if (hello.configVersion > n.configVersionApplied) {
        n.configVersionApplied = hello.configVersion;
      }
      if (n.deployPending) {
        n.deployPending = false;
        savePairedNodes();
      }
      break;
    }
  }

  sendConfigSnapshot(senderMac, hello.nodeId);
}
