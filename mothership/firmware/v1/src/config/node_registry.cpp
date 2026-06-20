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

  // NVS keys are limited to 15 characters. Build the key in a fixed buffer so
  // an unexpectedly long node ID cannot create an invalid Preferences access.
  char key[16];
  int keyLen = snprintf(key, sizeof(key), "%s%s", fieldPrefix, nodeId.c_str());
  if (keyLen < 0 || keyLen >= (int)sizeof(key) || prefs.getType(key) != PT_STR) {
    prefs.end();
    return "";
  }

  // Avoid Preferences::getString(key, String), which allocates a variable-size
  // stack buffer based on the length stored in NVS in this Arduino-ESP32 core.
  // A malformed length can overflow the stack before application validation.
  char value[256] = {};
  size_t len = prefs.getString(key, value, sizeof(value));
  prefs.end();
  return (len > 0) ? String(value) : String();
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

// Maximum number of paired/deployed nodes we will persist or restore.
static const int kMaxPairedNodes = 64;

void savePairedNodes() {
  Preferences prefs;
  if (!prefs.begin("paired_nodes", false)) {
    Serial.println("[REG] Failed to open NVS for saving paired nodes");
    return;
  }

  int oldCount = prefs.getInt("count", 0);
  if (oldCount < 0 || oldCount > kMaxPairedNodes) oldCount = 0;
  int count = 0;
  for (const auto& n : registeredNodes) {
    if (n.state == PAIRED || n.state == DEPLOYED) count++;
  }
  if (count > kMaxPairedNodes) {
    Serial.printf("[REG] ERROR: refusing to save %d nodes (max %d)\n",
                  count, kMaxPairedNodes);
    prefs.end();
    return;
  }

  int idx = 0;
  bool writeOk = true;
  for (const auto& n : registeredNodes) {
    if (n.state != PAIRED && n.state != DEPLOYED) continue;

    char key[16];
    snprintf(key, sizeof(key), "mac%d", idx);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X%02X%02X%02X%02X%02X",
             n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    writeOk = prefs.putString(key, macs) > 0 && writeOk;

    snprintf(key, sizeof(key), "id%d", idx);
    writeOk = prefs.putString(key, n.nodeId) > 0 && writeOk;

    snprintf(key, sizeof(key), "typ%d", idx);
    writeOk = prefs.putString(key, n.nodeType) > 0 && writeOk;

    snprintf(key, sizeof(key), "st%d", idx);
    writeOk = prefs.putUChar(key, (uint8_t)n.state) == sizeof(uint8_t) && writeOk;

    snprintf(key, sizeof(key), "batv%d", idx);
    writeOk = prefs.putFloat(key, isnan(n.lastReportedBatV) ? 0.0f : n.lastReportedBatV) == sizeof(float) && writeOk;

    idx++;
  }

  // Publish the new count last. Each Preferences put commits immediately, so
  // this prevents an interrupted save from advertising records not yet written.
  if (writeOk) {
    writeOk = prefs.putInt("count", count) == sizeof(int32_t);
  }

  // Stale records above count are harmless, but remove them after the new count
  // is committed so a shrinking registry does not consume NVS indefinitely.
  if (writeOk && oldCount > count) {
    const char* prefixes[] = {"mac", "id", "typ", "st", "batv"};
    for (int stale = count; stale < oldCount; ++stale) {
      char key[16];
      for (const char* prefix : prefixes) {
        snprintf(key, sizeof(key), "%s%d", prefix, stale);
        prefs.remove(key);
      }
    }
  }

  prefs.end();
  if (writeOk) {
    Serial.printf("[REG] Saved %d paired/deployed nodes to NVS\n", count);
  } else {
    Serial.println("[REG] ERROR: paired-node save incomplete; previous count retained");
  }
}

// Returns true if a MAC looks valid (not all-zero and not all-0xFF).
static bool macLooksValid(const uint8_t mac[6]) {
  bool allZero = true;
  bool allFF   = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0x00) allZero = false;
    if (mac[i] != 0xFF) allFF   = false;
  }
  return !(allZero || allFF);
}

// Read a string into a caller-provided fixed buffer. This avoids the
// variable-length stack allocation used by this core's String overload.
static bool readNvsString(Preferences& prefs, const char* key,
                          char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  if (prefs.getType(key) != PT_STR) return false;
  return prefs.getString(key, out, outSize) > 0;
}

static void clearPairedNodesNamespace() {
  Preferences clear;
  if (clear.begin("paired_nodes", false)) {
    clear.clear();
    clear.end();
  }
}

void loadPairedNodes() {
  Preferences prefs;
  // Open in READ-ONLY mode — we're loading, not saving.
  // If the namespace doesn't exist, begin() returns false and we skip.
  if (!prefs.begin("paired_nodes", true)) {
    Serial.println("[REG] paired_nodes namespace not found — starting fresh");
    return;
  }

  // --- Count ---
  if (prefs.getType("count") != PT_I32) {
    Serial.println("[REG] No 'count' key in NVS — nothing to load");
    prefs.end();
    return;
  }
  int count = prefs.getInt("count", 0);
  Serial.printf("[REG] Loading %d paired/deployed nodes from NVS\n", count);

  // Sanity-check the stored count. A negative or absurdly large value
  // indicates corruption; clear the namespace and start fresh.
  if (count < 0 || count > kMaxPairedNodes) {
    Serial.printf("[REG] Paired-node count %d out of range (max %d) — clearing NVS\n",
                  count, kMaxPairedNodes);
    prefs.end();
    clearPairedNodesNamespace();
    return;
  }

  // We collect fully-validated NodeInfo records here, then close the
  // "paired_nodes" Preferences handle BEFORE calling getNodeUserId /
  // getNodeName (which open their own "node_meta" handle). This avoids
  // any interaction between two open Preferences namespaces.
  std::vector<NodeInfo> validatedNodes;
  int restored = 0;
  int skipped  = 0;

  for (int i = 0; i < count; ++i) {
    char key[16];

    // --- MAC (required) ---
    snprintf(key, sizeof(key), "mac%d", i);
    char macs[13] = {};
    if (!readNvsString(prefs, key, macs, sizeof(macs))) {
      Serial.printf("[REG] Node %d: MAC missing, wrong type, or too long — skipping\n", i);
      ++skipped;
      continue;
    }
    size_t macLen = strlen(macs);
    if (macLen != 12) {
      Serial.printf("[REG] Node %d: invalid MAC string (len=%u) — skipping\n",
                    i, (unsigned)macLen);
      ++skipped;
      continue;
    }

    uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool macParseOk = true;
    for (int j = 0; j < 6; ++j) {
      char byteStr[3] = {macs[j * 2], macs[j * 2 + 1], '\0'};
      // strtoul returns 0 on garbage; guard against non-hex chars.
      char* endp = nullptr;
      unsigned long b = strtoul(byteStr, &endp, 16);
      if (endp == byteStr || *endp != '\0') {
        macParseOk = false;
        break;
      }
      mac[j] = (uint8_t)b;
    }
    if (!macParseOk || !macLooksValid(mac)) {
      Serial.printf("[REG] Node %d: MAC parse failed or invalid (%s) — skipping\n",
                    i, macs);
      ++skipped;
      continue;
    }

    // --- Node ID (required) ---
    snprintf(key, sizeof(key), "id%d", i);
    char nidBuf[33] = {};
    if (!readNvsString(prefs, key, nidBuf, sizeof(nidBuf))) {
      Serial.printf("[REG] Node %d: nodeId missing, wrong type, or too long — skipping\n", i);
      ++skipped;
      continue;
    }
    String nid(nidBuf);
    if (nid.length() == 0 || nid.length() > 32) {
      Serial.printf("[REG] Node %d: invalid nodeId (len=%u) — skipping\n",
                    i, (unsigned)nid.length());
      ++skipped;
      continue;
    }

    // --- Node type (optional, defaults to "restored") ---
    snprintf(key, sizeof(key), "typ%d", i);
    char typeBuf[33] = {};
    String ntype = readNvsString(prefs, key, typeBuf, sizeof(typeBuf))
                     ? String(typeBuf) : String("restored");

    // --- State (required) ---
    snprintf(key, sizeof(key), "st%d", i);
    if (prefs.getType(key) != PT_U8) {
      Serial.printf("[REG] Node %d: state key missing — skipping\n", i);
      ++skipped;
      continue;
    }
    uint8_t stRaw = prefs.getUChar(key, (uint8_t)PAIRED);
    if (stRaw > (uint8_t)DEPLOYED) {
      Serial.printf("[REG] Node %d: invalid state %u (max %u) — skipping\n",
                    i, stRaw, (uint8_t)DEPLOYED);
      ++skipped;
      continue;
    }
    NodeState state = (NodeState)stRaw;

    // --- Battery voltage (optional) ---
    snprintf(key, sizeof(key), "batv%d", i);
    float savedBatV = 0.0f;
    if (prefs.getType(key) == PT_BLOB) {
      savedBatV = prefs.getFloat(key, 0.0f);
      if (isnan(savedBatV) || isinf(savedBatV)) savedBatV = 0.0f;
    }

    // All fields validated — build the NodeInfo record.
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
    // userId and name are populated after prefs.end() below.

    validatedNodes.push_back(newNode);

    Serial.printf("[REG] validated %s (%s) state=%s\n",
                  nid.c_str(), macToStr(mac).c_str(), stateToStr(state));
    ++restored;
  }

  // Close the "paired_nodes" handle BEFORE opening "node_meta" for
  // getNodeUserId / getNodeName. This eliminates any chance of two
  // Preferences handles interacting and causing a crash.
  prefs.end();

  // Now populate userId/name and add to the live registry.
  // NOTE: Do NOT call ensurePeerOnChannel() here — ESP-NOW is not yet
  // initialized (initEspNowConfig is called later in handleConfigWake).
  // Calling esp_now_add_peer() before esp_now_init() causes a crash.
  // Peers will be added when ESP-NOW is initialized or when nodes send data.
  for (auto& newNode : validatedNodes) {
    newNode.userId = getNodeUserId(newNode.nodeId);
    newNode.name   = getNodeName(newNode.nodeId);
    registeredNodes.push_back(newNode);
  }

  // If every stored node was rejected as corrupt, wipe the namespace so
  // we don't keep tripping over the same bad data on every boot.
  if (count > 0 && restored == 0) {
    Serial.printf("[REG] All %d paired nodes failed validation — clearing NVS\n", count);
    clearPairedNodesNamespace();
  } else {
    Serial.printf("[REG] Load complete: %d restored, %d skipped\n", restored, skipped);
  }
}
