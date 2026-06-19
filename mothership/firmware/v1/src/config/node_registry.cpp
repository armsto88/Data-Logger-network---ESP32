#include "config/node_registry.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>

// Node registry + NVS persistence for Mothership V1 config mode.
// Slim extract from production espnow_manager.cpp.

std::vector<NodeInfo> registeredNodes;

// Device ID used for mothership_id fields in wire messages.
// Defined here so the config layer is self-contained.
static const char* kDeviceId = "001";

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

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

static void ensurePeerOnChannel(const uint8_t mac[6], uint8_t channel) {
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = channel;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  esp_now_del_peer(mac);
  esp_now_add_peer(&peer);
}

// -----------------------------------------------------------------------------
// Node meta helpers (NVS namespace "node_meta")
// -----------------------------------------------------------------------------

static String loadNodeMeta(const String& nodeId, const char* fieldPrefix) {
  Preferences prefs;
  if (!prefs.begin("node_meta", true)) return "";
  String key = String(fieldPrefix) + nodeId;
  String value = prefs.isKey(key.c_str()) ? prefs.getString(key.c_str(), "") : "";
  prefs.end();
  return value;
}

static void storeNodeMeta(const String& nodeId, const char* fieldPrefix, String value) {
  Preferences prefs;
  if (!prefs.begin("node_meta", false)) {
    Serial.println("[REG] storeNodeMeta: NVS begin failed");
    return;
  }
  String key = String(fieldPrefix) + nodeId;
  value.trim();
  if (value.length() == 0) {
    prefs.remove(key.c_str());
  } else {
    prefs.putString(key.c_str(), value);
  }
  prefs.end();
}

String getNodeUserId(const String& nodeId) {
  return loadNodeMeta(nodeId, "id_");
}

void setNodeUserId(const String& nodeId, String userId) {
  String cleaned;
  userId.trim();
  for (size_t i = 0; i < userId.length(); ++i) {
    char c = userId[i];
    if (c >= '0' && c <= '9') {
      cleaned += c;
      if (cleaned.length() >= 3) break;
    }
  }
  if (cleaned.length() > 0 && cleaned.length() < 3) {
    while (cleaned.length() < 3) cleaned = "0" + cleaned;
  }
  storeNodeMeta(nodeId, "id_", cleaned);
}

String getNodeName(const String& nodeId) {
  return loadNodeMeta(nodeId, "name_");
}

void setNodeName(const String& nodeId, String name) {
  const size_t kMaxLen = 32;
  if (name.length() > kMaxLen) name = name.substring(0, kMaxLen);
  storeNodeMeta(nodeId, "name_", name);
}

String getNodeNotes(const String& nodeId) {
  return loadNodeMeta(nodeId, "note_");
}

void setNodeNotes(const String& nodeId, String notes) {
  const size_t kMaxLen = 180;
  if (notes.length() > kMaxLen) notes = notes.substring(0, kMaxLen);
  storeNodeMeta(nodeId, "note_", notes);
}

String getCsvNodeId(const String& nodeId) {
  String userId = getNodeUserId(nodeId);
  if (userId.length() > 0) return userId;
  return nodeId;
}

String getCsvNodeName(const String& nodeId) {
  return getNodeName(nodeId);
}

// -----------------------------------------------------------------------------
// Desired config (NVS namespace "node_dcfg")
// -----------------------------------------------------------------------------

static uint32_t fnv1a32NodeId(const char* s) {
  if (!s) return 0;
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619u;
  }
  return h;
}

static String desiredConfigKeyPrefix(const char* nodeId) {
  const uint32_t h = fnv1a32NodeId(nodeId);
  char b[10];
  snprintf(b, sizeof(b), "d%08X", (unsigned)h);
  return String(b);
}

NodeDesiredConfig getDesiredConfig(const char* nodeId) {
  Preferences prefs;
  NodeDesiredConfig cfg{};
  const String key = desiredConfigKeyPrefix(nodeId);
  if (!prefs.begin("node_dcfg", true)) return cfg;
  cfg.configVersion   = prefs.getUShort((key + "v").c_str(), 0);
  cfg.wakeIntervalMin = prefs.getUChar((key + "w").c_str(), 0);
  cfg.syncIntervalMin = prefs.getUShort((key + "s").c_str(), 15);
  cfg.syncPhaseUnix   = prefs.getULong((key + "p").c_str(), 0);
  prefs.end();
  return cfg;
}

void setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg) {
  Preferences prefs;
  const String key = desiredConfigKeyPrefix(nodeId);
  if (!prefs.begin("node_dcfg", false)) {
    Serial.println("[REG] setDesiredConfig: NVS open failed");
    return;
  }
  prefs.putUShort((key + "v").c_str(), cfg.configVersion);
  prefs.putUChar((key + "w").c_str(), cfg.wakeIntervalMin);
  prefs.putUShort((key + "s").c_str(), cfg.syncIntervalMin);
  prefs.putULong((key + "p").c_str(), cfg.syncPhaseUnix);
  prefs.end();
}

// -----------------------------------------------------------------------------
// Registry queries
// -----------------------------------------------------------------------------

std::vector<NodeInfo> getRegisteredNodes() {
  return registeredNodes;
}

std::vector<NodeInfo> getUnpairedNodes() {
  std::vector<NodeInfo> unpaired;
  for (const auto& node : registeredNodes) {
    if (node.state == UNPAIRED) unpaired.push_back(node);
  }
  return unpaired;
}

std::vector<NodeInfo> getPairedNodes() {
  std::vector<NodeInfo> paired;
  for (const auto& node : registeredNodes) {
    if (node.state == PAIRED) paired.push_back(node);
  }
  return paired;
}

NodeState getNodeState(const char* nodeId) {
  for (const auto& node : registeredNodes)
    if (node.nodeId == String(nodeId)) return node.state;
  return UNPAIRED;
}

String getMothershipsMAC() {
  return WiFi.macAddress();
}

// -----------------------------------------------------------------------------
// Registry mutation
// -----------------------------------------------------------------------------

void registerNode(const uint8_t* mac,
                   const char* nodeId,
                   const char* nodeType,
                   NodeState state)
{
  NodeInfo* existing = nullptr;
  for (auto& node : registeredNodes) {
    if (memcmp(node.mac, mac, 6) == 0) {
      existing = &node;
      break;
    }
  }

  if (existing) {
    if (memcmp(existing->mac, mac, 6) != 0) {
      memcpy(existing->mac, mac, 6);
      ensurePeerOnChannel(existing->mac, ESPNOW_CHANNEL);
    }
    existing->lastSeen = millis();
    existing->isActive = true;
    existing->syncStale = false;
    existing->staleMissCount = 0;
    if (nodeId && nodeId[0] != '\0' && existing->nodeId != String(nodeId)) {
      existing->nodeId = String(nodeId);
    }
    existing->nodeType = String(nodeType);
    if (state > existing->state) {
      existing->state = state;
      if (state == DEPLOYED) {
        existing->deployPending = false;
      }
    }
    existing->userId = getNodeUserId(existing->nodeId);
    existing->name   = getNodeName(existing->nodeId);
    if (existing->state == PAIRED || existing->state == DEPLOYED) {
      savePairedNodes();
    }
    return;
  }

  NodeInfo n{};
  memcpy(n.mac, mac, 6);
  n.nodeId   = String(nodeId);
  n.nodeType = String(nodeType);
  n.lastSeen = millis();
  n.isActive = true;
  n.state    = state;
  n.channel  = ESPNOW_CHANNEL;
  n.lastTimeSyncMs       = 0;
  n.wakeIntervalMin      = 0;
  n.lastReportedQueueDepth = 0;
  n.lastReportedBatV     = NAN;
  n.inferredWakeIntervalMin = 0;
  n.lastNodeTimestamp    = 0;
  n.configVersionApplied = 0;
  n.lastConfigPushMs     = 0;
  n.deployPending        = false;
  n.stateChangePending   = false;
  n.pendingTargetState   = PENDING_NONE;
  n.pendingSinceMs       = 0;
  n.pendingLastAttemptMs = 0;
  n.lastStateAppliedMs   = 0;
  n.lastAppliedTargetState = PENDING_NONE;
  n.syncStale = false;
  n.staleMissCount = 0;
  n.lastStaleAssistMs = 0;
  n.userId = getNodeUserId(n.nodeId);
  n.name   = getNodeName(n.nodeId);

  registeredNodes.push_back(n);
  ensurePeerOnChannel(mac, ESPNOW_CHANNEL);
  Serial.printf("[REG] New node: %s (%s) state=%s\n",
                nodeId, macToStr(mac).c_str(), stateToStr(state));

  if (state == PAIRED || state == DEPLOYED) {
    savePairedNodes();
  }
}

bool unpairNode(const String& nodeId) {
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) {
      esp_now_del_peer(n.mac);
      n.state    = UNPAIRED;
      n.isActive = true;
      savePairedNodes();
      Serial.printf("[REG] Unpaired node: %s\n", nodeId.c_str());
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Persistence (paired/deployed list — NVS namespace "paired_nodes")
// -----------------------------------------------------------------------------

void savePairedNodes() {
  Preferences prefs;
  if (!prefs.begin("paired_nodes", false)) {
    Serial.println("[REG] Failed to open NVS for saving paired nodes");
    return;
  }

  int count = 0;
  for (const auto& n : registeredNodes) {
    if (n.state == PAIRED || n.state == DEPLOYED) count++;
  }
  prefs.putInt("count", count);

  int idx = 0;
  for (const auto& n : registeredNodes) {
    if (n.state != PAIRED && n.state != DEPLOYED) continue;

    char key[16];
    snprintf(key, sizeof(key), "mac%d", idx);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X%02X%02X%02X%02X%02X",
             n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    prefs.putString(key, macs);

    snprintf(key, sizeof(key), "id%d", idx);
    prefs.putString(key, n.nodeId);

    snprintf(key, sizeof(key), "typ%d", idx);
    prefs.putString(key, n.nodeType);

    snprintf(key, sizeof(key), "st%d", idx);
    prefs.putUChar(key, (uint8_t)n.state);

    snprintf(key, sizeof(key), "batv%d", idx);
    prefs.putFloat(key, isnan(n.lastReportedBatV) ? 0.0f : n.lastReportedBatV);

    idx++;
  }

  prefs.end();
  Serial.printf("[REG] Saved %d paired/deployed nodes to NVS\n", count);
}

void loadPairedNodes() {
  Preferences prefs;
  if (!prefs.begin("paired_nodes", false)) {
    Serial.println("[REG] Failed to open NVS for loading paired nodes");
    return;
  }

  int count = prefs.getInt("count", 0);
  Serial.printf("[REG] Loading %d paired/deployed nodes from NVS\n", count);

  for (int i = 0; i < count; ++i) {
    char key[16];

    snprintf(key, sizeof(key), "mac%d", i);
    String macs = prefs.getString(key, "");
    if (macs.length() != 12) continue;

    uint8_t mac[6];
    for (int j = 0; j < 6; ++j) {
      String byteStr = macs.substring(j * 2, j * 2 + 2);
      mac[j] = (uint8_t)strtoul(byteStr.c_str(), nullptr, 16);
    }

    snprintf(key, sizeof(key), "id%d", i);
    String nid = prefs.getString(key, "NODE");

    snprintf(key, sizeof(key), "typ%d", i);
    String ntype = prefs.getString(key, "restored");

    snprintf(key, sizeof(key), "st%d", i);
    uint8_t stRaw = prefs.getUChar(key, (uint8_t)PAIRED);
    NodeState state = (stRaw <= DEPLOYED) ? (NodeState)stRaw : PAIRED;

    snprintf(key, sizeof(key), "batv%d", i);
    float savedBatV = prefs.getFloat(key, 0.0f);

    NodeInfo newNode{};
    memcpy(newNode.mac, mac, 6);
    newNode.nodeId   = nid;
    newNode.nodeType = ntype;
    newNode.lastSeen = millis();
    newNode.isActive = true;
    newNode.state    = state;
    newNode.channel  = ESPNOW_CHANNEL;
    newNode.lastTimeSyncMs       = 0;
    newNode.wakeIntervalMin      = 0;
    newNode.lastReportedQueueDepth = 0;
    newNode.lastReportedBatV     = (savedBatV > 0.0f) ? savedBatV : NAN;
    newNode.inferredWakeIntervalMin = 0;
    newNode.lastNodeTimestamp    = 0;
    newNode.configVersionApplied = 0;
    newNode.lastConfigPushMs     = 0;
    newNode.deployPending        = false;
    newNode.stateChangePending   = false;
    newNode.pendingTargetState   = PENDING_NONE;
    newNode.pendingSinceMs       = 0;
    newNode.pendingLastAttemptMs = 0;
    newNode.lastStateAppliedMs   = 0;
    newNode.lastAppliedTargetState = PENDING_NONE;
    newNode.syncStale            = false;
    newNode.staleMissCount       = 0;
    newNode.lastStaleAssistMs    = 0;
    newNode.userId = getNodeUserId(newNode.nodeId);
    newNode.name   = getNodeName(newNode.nodeId);

    registeredNodes.push_back(newNode);
    ensurePeerOnChannel(mac, ESPNOW_CHANNEL);

    Serial.printf("[REG] restored %s (%s) state=%s\n",
                  nid.c_str(), macToStr(mac).c_str(), stateToStr(state));
  }

  prefs.end();
}