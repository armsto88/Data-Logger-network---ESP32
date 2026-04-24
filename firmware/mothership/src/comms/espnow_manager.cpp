#include "espnow_manager.h"
#include "../storage/sd_manager.h"
#include "../time/rtc_manager.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include "../system/config.h"
#include <vector>
#include <Preferences.h>
#include "protocol.h"

#ifndef ENABLE_VERBOSE_SENSOR_PACKET_LOG
#define ENABLE_VERBOSE_SENSOR_PACKET_LOG 0
#endif

#ifndef ENABLE_SEND_CALLBACK_LOG
#define ENABLE_SEND_CALLBACK_LOG 0
#endif

#ifndef ENABLE_CYCLE_MONITOR_LOG
#define ENABLE_CYCLE_MONITOR_LOG 0
#endif

// External reference to DEVICE_ID from main.cpp
extern const char* DEVICE_ID;
extern int gSyncMode;
extern int gSyncDailyHour;
extern int gSyncDailyMinute;
extern unsigned long gLastSyncBroadcastUnix;
extern long long     gLastSyncIntervalSlot;
void saveSyncRuntimeGuardsToNVS();

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
static const uint32_t FLEET_SYNC_NO_ELIGIBLE_RETRY_MS = 60UL * 1000UL;       // 1 min
static uint32_t g_lastNoEligibleLogMs = 0;
static const uint32_t NO_ELIGIBLE_LOG_INTERVAL_MS = 30UL * 1000UL;           // 30 s
static SensorDataEventCallback g_sensorDataEventCb = nullptr;
static uint32_t g_lastCycleMonitorMs = 0;
static const uint32_t CYCLE_MONITOR_INTERVAL_MS = 30UL * 1000UL;             // 30 s
static void ensurePeerOnChannel(const uint8_t mac[6], uint8_t channel);
static const uint32_t PENDING_RETRY_INTERVAL_MS = 5000UL;
static constexpr int kSyncModeDaily = 0;
static const uint8_t STALE_MISS_THRESHOLD = 3;
static const uint32_t STALE_MIN_AGE_MS = 24UL * 60UL * 60UL * 1000UL;
static const uint32_t STALE_ASSIST_LEAD_MS = 20000UL;
static const uint32_t STALE_ASSIST_LAG_MS = 45000UL;
static const uint32_t STALE_ASSIST_RETRY_MS = 10000UL;

struct SyncRxTracker {
    String nodeId;
    uint32_t helloMs;
    uint8_t expectedQueue;
    uint16_t receivedInWindow;
    uint32_t lastLogMs;
    bool completionLogged;
};
static std::vector<SyncRxTracker> g_syncRxTrackers;

static SyncRxTracker* getSyncRxTracker(const String& nodeId) {
    for (auto& t : g_syncRxTrackers) {
        if (t.nodeId == nodeId) return &t;
    }
    SyncRxTracker t{};
    t.nodeId = nodeId;
    t.helloMs = 0;
    t.expectedQueue = 0;
    t.receivedInWindow = 0;
    t.lastLogMs = 0;
    t.completionLogged = false;
    g_syncRxTrackers.push_back(t);
    return &g_syncRxTrackers.back();
}

static void noteNodeHelloForSyncRx(const char* nodeId, uint8_t queueDepth) {
    if (!nodeId || !nodeId[0]) return;
    SyncRxTracker* t = getSyncRxTracker(String(nodeId));
    if (!t) return;
    t->helloMs = millis();
    t->expectedQueue = queueDepth;
    t->receivedInWindow = 0;
    t->lastLogMs = 0;
    t->completionLogged = false;
    Serial.printf("[SYNC_RX] %s hello: queued=%u\n", nodeId, (unsigned)queueDepth);
}

static void noteNodeSampleForSyncRx(const char* nodeId) {
    if (!nodeId || !nodeId[0]) return;
    SyncRxTracker* t = getSyncRxTracker(String(nodeId));
    if (!t) return;

    t->receivedInWindow++;
    const uint32_t nowMs = millis();
    const bool periodicLog = (t->lastLogMs == 0) || ((nowMs - t->lastLogMs) >= 5000UL);
    if (periodicLog) {
        Serial.printf("[SYNC_RX] %s received=%u", nodeId, (unsigned)t->receivedInWindow);
        if (t->expectedQueue > 0) {
            Serial.printf("/%u", (unsigned)t->expectedQueue);
        }
        Serial.println();
        t->lastLogMs = nowMs;
    }

    if (t->expectedQueue > 0 && !t->completionLogged && t->receivedInWindow >= t->expectedQueue) {
        t->completionLogged = true;
        Serial.printf("[SYNC_RX] %s flush complete approx (%u/%u)\n",
                      nodeId,
                      (unsigned)t->receivedInWindow,
                      (unsigned)t->expectedQueue);
    }
}

// CSV writes are buffered from ESP-NOW callback context and drained from the main loop.
static constexpr uint8_t kCsvQueueCapacity = 32;
static constexpr size_t kCsvRowMaxLen = 320;
static portMUX_TYPE g_csvQueueMux = portMUX_INITIALIZER_UNLOCKED;
static char g_csvRowQueue[kCsvQueueCapacity][kCsvRowMaxLen];
static uint8_t g_csvQueueHead = 0;
static uint8_t g_csvQueueTail = 0;
static uint8_t g_csvQueueCount = 0;
static uint32_t g_csvQueueDropped = 0;
static uint32_t g_csvQueueDroppedReported = 0;

static bool enqueueCsvRowFromCallback(const String& row) {
    bool queued = false;
    portENTER_CRITICAL(&g_csvQueueMux);
    if (g_csvQueueCount < kCsvQueueCapacity) {
        const uint8_t idx = g_csvQueueHead;
        strncpy(g_csvRowQueue[idx], row.c_str(), kCsvRowMaxLen - 1);
        g_csvRowQueue[idx][kCsvRowMaxLen - 1] = '\0';
        g_csvQueueHead = (uint8_t)((g_csvQueueHead + 1) % kCsvQueueCapacity);
        g_csvQueueCount++;
        queued = true;
    } else {
        g_csvQueueDropped++;
    }
    portEXIT_CRITICAL(&g_csvQueueMux);
    return queued;
}

static bool dequeueCsvRowForSdWrite(char* outRow, size_t outLen) {
    if (!outRow || outLen == 0) return false;

    bool hadRow = false;
    portENTER_CRITICAL(&g_csvQueueMux);
    if (g_csvQueueCount > 0) {
        const uint8_t idx = g_csvQueueTail;
        strncpy(outRow, g_csvRowQueue[idx], outLen - 1);
        outRow[outLen - 1] = '\0';
        g_csvQueueTail = (uint8_t)((g_csvQueueTail + 1) % kCsvQueueCapacity);
        g_csvQueueCount--;
        hadRow = true;
    }
    portEXIT_CRITICAL(&g_csvQueueMux);
    return hadRow;
}

static void drainCsvQueueToSd(uint8_t maxRowsPerLoop = 8) {
    char rowBuf[kCsvRowMaxLen];
    uint8_t written = 0;
    while (written < maxRowsPerLoop && dequeueCsvRowForSdWrite(rowBuf, sizeof(rowBuf))) {
        if (!logCSVRow(String(rowBuf))) {
            Serial.println("❌ Failed to log node data");
        }
        written++;
    }

    if (g_csvQueueDropped != g_csvQueueDroppedReported) {
        const uint32_t droppedNow = g_csvQueueDropped;
        const uint32_t delta = droppedNow - g_csvQueueDroppedReported;
        g_csvQueueDroppedReported = droppedNow;
        Serial.printf("⚠️ CSV callback queue overflow: dropped %lu row(s) (total=%lu)\n",
                      (unsigned long)delta,
                      (unsigned long)droppedNow);
    }
}

static const char* pendingStateToStr(NodePendingState s) {
    switch (s) {
      case PENDING_TO_UNPAIRED: return "UNPAIRED";
      case PENDING_TO_PAIRED: return "PAIRED";
      case PENDING_TO_DEPLOYED: return "DEPLOYED";
      default: return "NONE";
    }
}

static void clearPendingState(NodeInfo& node, const char* reason) {
    if (!node.stateChangePending) return;
    Serial.printf("✅ Pending state cleared for %s (%s)\n",
                  node.nodeId.c_str(),
                  reason ? reason : "no reason");
    node.lastStateAppliedMs = millis();
    node.lastAppliedTargetState = node.pendingTargetState;
    node.stateChangePending = false;
    node.pendingTargetState = PENDING_NONE;
    node.pendingSinceMs = 0;
    node.pendingLastAttemptMs = 0;
}

static void queuePendingState(NodeInfo& node, NodePendingState target) {
    const uint32_t nowMs = millis();
    const bool changed = (!node.stateChangePending) || (node.pendingTargetState != target);
    node.stateChangePending = true;
    node.pendingTargetState = target;
    // Force an immediate retry on next contact when target changed.
    node.pendingLastAttemptMs = changed ? 0 : nowMs;
    if (changed || node.pendingSinceMs == 0) {
        node.pendingSinceMs = nowMs;
    }
    Serial.printf("📝 Pending state queued for %s -> %s\n",
                  node.nodeId.c_str(), pendingStateToStr(target));
}

static void processPendingStateCommand(NodeInfo& node, const char* reason) {
    if (!node.stateChangePending) return;
    const uint32_t nowMs = millis();
    if (node.pendingLastAttemptMs > 0 &&
        (nowMs - node.pendingLastAttemptMs) < PENDING_RETRY_INTERVAL_MS) {
        return;
    }

    bool sent = false;
    if (node.pendingTargetState == PENDING_TO_UNPAIRED) {
        sent = sendUnpairToNode(node.nodeId);
    } else if (node.pendingTargetState == PENDING_TO_PAIRED) {
        sent = pairNode(node.nodeId);
    } else if (node.pendingTargetState == PENDING_TO_DEPLOYED) {
        std::vector<String> ids;
        ids.push_back(node.nodeId);
        sent = deploySelectedNodes(ids);
    }

    node.pendingLastAttemptMs = nowMs;
    Serial.printf("🔁 Pending state replay for %s -> %s on %s: %s\n",
                  node.nodeId.c_str(),
                  pendingStateToStr(node.pendingTargetState),
                  reason ? reason : "contact",
                  sent ? "SENT" : "FAILED");
}

static bool isReasonableWakeInterval(uint8_t mins) {
    return mins >= 1 && mins <= 60;
}

static uint32_t computeNextDailySyncPhaseUnix(uint32_t nowUnix) {
    const uint32_t dayStartUnix = (nowUnix / 86400UL) * 86400UL;
    const uint32_t targetOffset = (uint32_t)max(0, min(23, gSyncDailyHour)) * 3600UL
                                + (uint32_t)max(0, min(59, gSyncDailyMinute)) * 60UL;
    uint32_t targetUnix = dayStartUnix + targetOffset;
    if (targetUnix <= nowUnix) targetUnix += 86400UL;
    return targetUnix;
}

static bool pushDesiredConfigSnapshot(const uint8_t* senderMac,
                                      const char* nodeId,
                                      const NodeDesiredConfig& desired) {
    if (!senderMac || !nodeId || desired.configVersion == 0) return false;

    config_snapshot_message_t snap{};
    strcpy(snap.command, "CONFIG_SNAPSHOT");
    strncpy(snap.mothership_id, DEVICE_ID, sizeof(snap.mothership_id) - 1);
    snap.configVersion    = desired.configVersion;
    snap.wakeIntervalMin  = desired.wakeIntervalMin;
    snap.syncIntervalMin  = desired.syncIntervalMin;
    snap.syncPhaseUnix    = desired.syncPhaseUnix;

    ensurePeerOnChannel(senderMac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(senderMac, (uint8_t*)&snap, sizeof(snap));
    Serial.printf("↪ CONFIG_SNAPSHOT v%u push to %s on sensor contact: %s\n",
                  (unsigned)snap.configVersion,
                  nodeId,
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    return (res == ESP_OK);
}

static bool pushSyncScheduleToNode(const uint8_t* senderMac,
                                   const char* nodeId,
                                   const NodeDesiredConfig& desired) {
    if (!senderMac || !nodeId) return false;

    uint16_t syncMin = (desired.syncIntervalMin > 0) ? desired.syncIntervalMin : 0;
    uint32_t phaseUnix = desired.syncPhaseUnix;

    // Daily mode is a global fleet slot: nodes sync once/day at mothership time.
    if (gSyncMode == kSyncModeDaily) {
        syncMin = 0;  // 0 = daily mode sentinel; node extracts HH:MM from phaseUnix
        phaseUnix = computeNextDailySyncPhaseUnix(getRTCTimeUnix());
    } else if (phaseUnix == 0) {
        phaseUnix = getRTCTimeUnix();
    }

    phaseUnix -= (phaseUnix % 60UL);
    // Allow syncMin==0 (daily sentinel); only reject when there is no valid phase anchor
    if (phaseUnix == 0) return false;

    sync_schedule_command_message_t sched{};
    strcpy(sched.command, "SET_SYNC_SCHED");
    strncpy(sched.mothership_id, DEVICE_ID, sizeof(sched.mothership_id) - 1);
    sched.syncIntervalMinutes = (unsigned long)syncMin;
    sched.phaseUnix = phaseUnix;

    ensurePeerOnChannel(senderMac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(senderMac, (uint8_t*)&sched, sizeof(sched));
    Serial.printf("↪ SET_SYNC_SCHED to %s (syncMin=%u phase=%lu): %s\n",
                  nodeId,
                  (unsigned)syncMin,
                  (unsigned long)phaseUnix,
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    return (res == ESP_OK);
}

static bool pushWakeScheduleToNode(const uint8_t* senderMac,
                                   const char* nodeId,
                                   uint8_t wakeMin) {
    if (!senderMac || !nodeId || !isReasonableWakeInterval(wakeMin)) return false;

    schedule_command_message_t cmd{};
    strcpy(cmd.command, "SET_SCHEDULE");
    strncpy(cmd.mothership_id, DEVICE_ID, sizeof(cmd.mothership_id) - 1);
    cmd.intervalMinutes = wakeMin;

    ensurePeerOnChannel(senderMac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(senderMac, (uint8_t*)&cmd, sizeof(cmd));
    Serial.printf("↪ Deploy-align SET_SCHEDULE to %s (%u min): %s\n",
                  nodeId,
                  (unsigned)wakeMin,
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    return (res == ESP_OK);
}

static bool pushSyncWindowOpenToNode(const uint8_t* senderMac,
                                     const char* nodeId,
                                     uint32_t phaseUnix) {
    if (!senderMac || !nodeId || phaseUnix == 0) return false;

    sync_schedule_command_message_t cmd{};
    strcpy(cmd.command, "SYNC_WINDOW_OPEN");
    strncpy(cmd.mothership_id, DEVICE_ID, sizeof(cmd.mothership_id) - 1);
    cmd.syncIntervalMinutes = 0;
    cmd.phaseUnix = phaseUnix;

    ensurePeerOnChannel(senderMac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(senderMac, (uint8_t*)&cmd, sizeof(cmd));
    Serial.printf("↪ Deploy-align SYNC_WINDOW_OPEN to %s (phase=%lu): %s\n",
                  nodeId,
                  (unsigned long)phaseUnix,
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
    return (res == ESP_OK);
}


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

static void logCycleMonitorSnapshot(unsigned long nowMs) {
    bool anyTracked = false;
    for (const auto& n : registeredNodes) {
        if (n.state != PAIRED && n.state != DEPLOYED) continue;
        anyTracked = true;

        const unsigned long ageMs = (nowMs >= n.lastSeen) ? (nowMs - n.lastSeen) : 0;
        Serial.printf("🛰️ Cycle monitor: %s state=%s link=%s lastSeen=%lus wakeMin=%u pending=%u\n",
                      n.nodeId.c_str(),
                      stateToStr(n.state),
                      n.isActive ? "AWAKE" : "ASLEEP",
                      ageMs / 1000UL,
                      (unsigned)n.wakeIntervalMin,
                      n.deployPending ? 1 : 0);
    }

    if (!anyTracked) {
        Serial.println("🛰️ Cycle monitor: no paired/deployed nodes tracked yet");
    }
}

// ----------------- registry -----------------
void registerNode(const uint8_t* mac,
                  const char* nodeId,
                  const char* nodeType /*= "unknown"*/,
                  NodeState state     /*= UNPAIRED*/)
{
    NodeInfo* existing = nullptr;

    // Find existing entry by immutable hardware identity (MAC).
    // Do not merge by nodeId; labels can collide and must not alias devices.
    for (auto& node : registeredNodes) {
        if (memcmp(node.mac, mac, 6) == 0) {
            existing = &node;
            break;
        }
    }

    if (existing) {
        bool upgraded = false;
        bool macChanged = false;
        bool idChanged = false;

        if (memcmp(existing->mac, mac, 6) != 0) {
            Serial.printf("🔄 Node %s MAC updated: %s -> %s\n",
                          existing->nodeId.c_str(),
                          macToStr(existing->mac).c_str(),
                          macToStr(mac).c_str());
            memcpy(existing->mac, mac, 6);
            macChanged = true;
            ensurePeerOnChannel(existing->mac, ESPNOW_CHANNEL);
        }

        existing->lastSeen = millis();
        existing->isActive = true;
        existing->syncStale = false;
        existing->staleMissCount = 0;
        if (nodeId && nodeId[0] != '\0' && existing->nodeId != String(nodeId)) {
            Serial.printf("🔄 Node MAC %s runtime ID updated: %s -> %s\n",
                          macToStr(existing->mac).c_str(),
                          existing->nodeId.c_str(),
                          nodeId);
            existing->nodeId = String(nodeId);
            idChanged = true;
        }
        existing->nodeType = String(nodeType);  // keep type fresh

        // Only ever upgrade: UNPAIRED < PAIRED < DEPLOYED
        if (state > existing->state) {
            Serial.printf("📈 Node %s state upgrade: %s → %s\n",
                          existing->nodeId.c_str(),
                          stateToStr(existing->state),
                          stateToStr(state));
            existing->state = state;
            if (state == DEPLOYED) {
                existing->deployPending = false;
                if (existing->stateChangePending &&
                    existing->pendingTargetState == PENDING_TO_DEPLOYED) {
                    clearPendingState(*existing, "node confirmed DEPLOYED");
                }
            }
            upgraded = true;
        }

        // Refresh meta for consistency with web UI / CSV
        existing->userId = getNodeUserId(existing->nodeId);
        existing->name   = getNodeName(existing->nodeId);

        if ((upgraded || macChanged || idChanged) &&
            (existing->state == PAIRED || existing->state == DEPLOYED)) {
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
    n.lastTimeSyncMs       = 0;
    n.wakeIntervalMin      = 0;
    n.lastReportedQueueDepth = 0;
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

void setSensorDataEventCallback(SensorDataEventCallback cb) {
    g_sensorDataEventCb = cb;
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
    // 0) Node status push (authoritative state update from node runtime)
    if (len == sizeof(node_status_message_t)) {
        node_status_message_t st{};
        memcpy(&st, incomingBytes, sizeof(st));

        if (strcmp(st.command, "NODE_STATUS") == 0) {
            NodeState reported = (st.state <= (uint8_t)DEPLOYED)
                ? (NodeState)st.state
                : UNPAIRED;

            NodeInfo* target = nullptr;
            for (auto& n : registeredNodes) {
                if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(st.nodeId)) {
                    target = &n;
                    break;
                }
            }

            if (!target) {
                registerNode(mac, st.nodeId, "status", reported);
                for (auto& n : registeredNodes) {
                    if (memcmp(n.mac, mac, 6) == 0 || n.nodeId == String(st.nodeId)) {
                        target = &n;
                        break;
                    }
                }
            }

            if (target) {
                NodeState previous = target->state;
                target->lastSeen = millis();
                target->isActive = true;
                const bool lockUnpaired = (target->state == UNPAIRED) ||
                                          (target->stateChangePending && target->pendingTargetState == PENDING_TO_UNPAIRED);
                if (lockUnpaired && reported != UNPAIRED) {
                    Serial.printf("🛡️ NODE_STATUS promotion blocked for %s while UI state is UNPAIRED/pending-unpair\n",
                                  st.nodeId);
                    target->state = UNPAIRED;
                } else {
                    target->state = reported;
                }
                target->deployPending = (reported == DEPLOYED) ? target->deployPending : false;

                if (reported == UNPAIRED && target->stateChangePending) {
                    clearPendingState(*target, "NODE_STATUS reports UNPAIRED");
                }

                Serial.printf("📣 NODE_STATUS from %s (%s): %s -> %s rtcSynced=%u deployed=%u rescue=%u rtcUnix=%lu\n",
                              st.nodeId,
                              macToStr(mac).c_str(),
                              stateToStr(previous),
                              stateToStr(target->state),
                              (unsigned)st.rtcSynced,
                              (unsigned)st.deployed,
                              (unsigned)st.rescueMode,
                              (unsigned long)st.rtcUnix);

                savePairedNodes();
            }
        }
        return;
    }

    // 1) Sensor data packets → mark DEPLOYED + log CSV
    if (len == sizeof(sensor_data_message_t)) {
        sensor_data_message_t incoming{};
        memcpy(&incoming, incomingBytes, sizeof(incoming));

        noteNodeSampleForSyncRx(incoming.nodeId);

        if (g_sensorDataEventCb) {
            g_sensorDataEventCb(incoming, mac);
        }

        // Honor local unpair authority: if this MAC is currently UNPAIRED in the registry,
        // do not auto-promote to DEPLOYED from passive sensor contact.
        NodeState observedState = UNPAIRED;
        bool pendingBlocksAutoDeploy = false;
        bool knownMac = false;
        for (const auto& n : registeredNodes) {
            if (memcmp(n.mac, mac, 6) == 0) {
                observedState = n.state;
                pendingBlocksAutoDeploy = n.stateChangePending &&
                                          n.pendingTargetState != PENDING_TO_DEPLOYED;
                knownMac = true;
                break;
            }
        }

        if (knownMac && (observedState == UNPAIRED || pendingBlocksAutoDeploy)) {
            registerNode(mac, incoming.nodeId, incoming.sensorType, UNPAIRED);
            Serial.printf("🛡️ Ignoring auto-deploy promotion from sensor data for %s (state=%s, pending=%d)\n",
                          incoming.nodeId,
                          stateToStr(observedState),
                          pendingBlocksAutoDeploy ? 1 : 0);
        } else {
            registerNode(mac, incoming.nodeId, incoming.sensorType, DEPLOYED);
        }
        NodeInfo* nodeInfo = nullptr;

        for (auto& n : registeredNodes) {
            if (n.nodeId == String(incoming.nodeId)) {
                nodeInfo = &n;
                if (incoming.nodeTimestamp > 0) {
                    if (n.lastNodeTimestamp > 0 && incoming.nodeTimestamp > n.lastNodeTimestamp) {
                        const uint32_t deltaSec = incoming.nodeTimestamp - n.lastNodeTimestamp;
                        // Infer cadence from node timestamps; tolerate small jitter.
                        const uint8_t mins = (uint8_t)((deltaSec + 30UL) / 60UL);
                        const uint32_t expectedSec = (uint32_t)mins * 60UL;
                        const uint32_t absErr = (deltaSec > expectedSec)
                            ? (deltaSec - expectedSec)
                            : (expectedSec - deltaSec);
                        if (isReasonableWakeInterval(mins) && absErr <= 10UL) {
                            if (n.inferredWakeIntervalMin != mins) {
                                Serial.printf("🧭 Inferred wake cadence for %s: ~%u min (delta=%lus)\n",
                                              incoming.nodeId,
                                              (unsigned)mins,
                                              (unsigned long)deltaSec);
                            }
                            n.inferredWakeIntervalMin = mins;
                            if (n.wakeIntervalMin == 0) {
                                n.wakeIntervalMin = mins;
                            }
                        }
                    }
                    if (incoming.nodeTimestamp > n.lastNodeTimestamp) {
                        n.lastNodeTimestamp = incoming.nodeTimestamp;
                    }
                }
                break;
            }
        }

        if (nodeInfo) {
            processPendingStateCommand(*nodeInfo, "sensor contact");
        }

        // Fallback path: if HELLO is missed, still push pending config when sensor data proves contact.
        if (nodeInfo) {
            NodeDesiredConfig desired = getDesiredConfig(incoming.nodeId);
            const bool cfgPending = (desired.configVersion > 0) &&
                                    (nodeInfo->configVersionApplied < desired.configVersion);
            const uint32_t nowMs = millis();
            const bool canRetry = (nodeInfo->lastConfigPushMs == 0) ||
                                  (nowMs - nodeInfo->lastConfigPushMs >= 5000UL);
            if (cfgPending && canRetry) {
                if (pushDesiredConfigSnapshot(mac, incoming.nodeId, desired)) {
                    nodeInfo->lastConfigPushMs = nowMs;
                }
            }
        }

        // If a deployed node is producing data, treat it as runtime deploy confirmation.
        for (auto& n : registeredNodes) {
            if (n.nodeId == String(incoming.nodeId)) {
                if (n.stateChangePending && n.pendingTargetState == PENDING_TO_DEPLOYED) {
                    clearPendingState(n, "SENSOR data confirms deployed runtime");
                }
                if (n.deployPending) {
                    n.deployPending = false;
                    Serial.printf("✅ Deploy pending cleared by SENSOR data from %s\n", incoming.nodeId);
                    savePairedNodes();
                }
                break;
            }
        }

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

        // Keep packet logging concise in operator mode.
    #if ENABLE_VERBOSE_SENSOR_PACKET_LOG
        Serial.printf(
          "📊 Data @ %s\n"
          "   from FW=%s, MAC=%s\n"
          "   CSV node_id=%s, name='%s'\n"
          "   sensor_id=%u, sensor_type=%s, sensor_label=%s, value=%.3f, node_ts=%lu, qf=0x%04X\n",
          ts,
          incoming.nodeId,
          macStr.c_str(),
          csvId.c_str(),
          csvName.c_str(),
          (unsigned)incoming.sensorId,
          incoming.sensorType,
                    incoming.sensorLabel,
          incoming.value,
                    (unsigned long)incoming.nodeTimestamp,
                    (unsigned)incoming.qualityFlags
        );
    #else
        Serial.printf("📊 SENSOR fw=%s id=%u label=%s val=%.3f node_ts=%lu\n",
                  incoming.nodeId,
                  (unsigned)incoming.sensorId,
                  incoming.sensorLabel,
                  incoming.value,
                  (unsigned long)incoming.nodeTimestamp);
    #endif

    // CSV row: timestamp,node_id,node_name,mac,event_type,sensor_type,value,meta
        String row;
        row.reserve(240);
        row  = ts;               // timestamp (mothership RTC)
        row += ",";
        row += csvId;            // node_id (numeric, falls back to fwId if empty)
        row += ",";
        row += csvName;          // node_name (may be empty)
        row += ",";
        row += macStr;           // raw MAC
        row += ",";
        row += "SENSOR";         // event_type (more explicit than "DATA")
        row += ",";
        row += incoming.sensorLabel[0] ? incoming.sensorLabel : incoming.sensorType;
        row += ",";
        row += String(incoming.value, 3);   // value with 3 decimals
        row += ",";

        // --- meta: start using this for future stuff ---
        // For now: firmware ID + node-side timestamp
        String meta;
        meta.reserve(80);
        meta  = "FW_ID=";
        meta += fwId; // e.g. "TEMP_001"
        meta += ";SENSOR_ID=";
        meta += String((unsigned)incoming.sensorId);
        meta += ";SENSOR_TYPE=";
        meta += incoming.sensorType;
        meta += ";QF=0x";
        char qfHex[5];
        snprintf(qfHex, sizeof(qfHex), "%04X", (unsigned)incoming.qualityFlags);
        meta += qfHex;
        meta += ";NODE_TS=";
        meta += String(incoming.nodeTimestamp);  // as sent by node (millis or unix)

        row += meta;

        if (!enqueueCsvRowFromCallback(row)) {
            Serial.println("⚠️ Node data queued for SD write failed: callback queue full");
        }

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
            for (auto& n : registeredNodes) {
                if (n.nodeId == String(discovery.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                    if (n.stateChangePending && n.pendingTargetState == PENDING_TO_UNPAIRED) {
                        clearPendingState(n, "discovery from unpaired node");
                    }
                    processPendingStateCommand(n, "discovery contact");
                    break;
                }
            }

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
                    if (n.stateChangePending && n.pendingTargetState == PENDING_TO_PAIRED) {
                        clearPendingState(n, "REQUEST_TIME indicates paired/unsynced runtime");
                        savePairedNodes();
                    }
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

            for (auto& n : registeredNodes) {
                if (n.nodeId == String(request.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                    processPendingStateCommand(n, "pairing poll");
                    break;
                }
            }

            NodeState current = getNodeState(request.nodeId);

            // Node pairing poll is evidence the node is in active pairing flow.
            // If we were waiting for a PAIRED transition, clear pending once state is PAIRED.
            if (current == PAIRED) {
                for (auto& n : registeredNodes) {
                    if (n.nodeId == String(request.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                        if (n.stateChangePending && n.pendingTargetState == PENDING_TO_PAIRED) {
                            clearPendingState(n, "PAIRING_REQUEST confirms paired state");
                            savePairedNodes();
                        }
                        break;
                    }
                }
            }

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

    // 5) NODE_HELLO pull handshake
    if (len == sizeof(node_hello_message_t)) {
        node_hello_message_t hello{};
        memcpy(&hello, incomingBytes, sizeof(hello));
        if (strcmp(hello.command, "NODE_HELLO") == 0) {
            handleNodeHello(mac, hello);
            for (auto& n : registeredNodes) {
                if (n.nodeId == String(hello.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                    processPendingStateCommand(n, "NODE_HELLO");
                    break;
                }
            }
        }
        return;
    }

    // 6) DEPLOY_ACK from node after applying DEPLOY_NODE
    if (len == sizeof(deployment_ack_message_t)) {
        deployment_ack_message_t ack{};
        memcpy(&ack, incomingBytes, sizeof(ack));
        if (strcmp(ack.command, "DEPLOY_ACK") == 0 && ack.deployed == 1) {
            Serial.printf("✅ DEPLOY_ACK from %s (rtcUnix=%lu)\n",
                          ack.nodeId,
                          (unsigned long)ack.rtcUnix);
            bool blockDeployPromotion = false;
            for (const auto& n : registeredNodes) {
                if (n.nodeId == String(ack.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                    blockDeployPromotion = n.stateChangePending &&
                                           n.pendingTargetState != PENDING_TO_DEPLOYED;
                    if (!blockDeployPromotion && n.state == UNPAIRED) {
                        blockDeployPromotion = true;
                    }
                    if (blockDeployPromotion) {
                        Serial.printf("🛡️ DEPLOY_ACK promotion blocked for %s while pending target=%s\n",
                                      ack.nodeId,
                                      pendingStateToStr(n.pendingTargetState));
                    }
                    break;
                }
            }

            if (!blockDeployPromotion) {
                registerNode(mac, ack.nodeId, "unknown", DEPLOYED);
            }
            savePairedNodes();
            for (auto& n : registeredNodes) {
                if (n.nodeId == String(ack.nodeId) || memcmp(n.mac, mac, 6) == 0) {
                    // Clear deploy-pending so the UI removes the "pending" indicator.
                    if (n.deployPending) {
                        n.deployPending = false;
                        Serial.printf("✅ deployPending cleared by DEPLOY_ACK from %s\n", ack.nodeId);
                    }
                    // Infer config version applied from ACK (avoids waiting for CONFIG_ACK).
                    NodeDesiredConfig desired = getDesiredConfig(ack.nodeId);
                    if (desired.configVersion > 0 && n.configVersionApplied < desired.configVersion) {
                        n.configVersionApplied = desired.configVersion;
                        Serial.printf("✅ configVersionApplied inferred from DEPLOY_ACK for %s: v%u\n",
                                      ack.nodeId, (unsigned)desired.configVersion);
                    }
                    if (n.stateChangePending && n.pendingTargetState == PENDING_TO_DEPLOYED) {
                        clearPendingState(n, "DEPLOY_ACK received");
                    }
                    break;
                }
            }
        }
        return;
    }

    // 7) CONFIG_APPLY_ACK from node after applying CONFIG_SNAPSHOT
    if (len == sizeof(config_apply_ack_message_t)) {
        config_apply_ack_message_t ack{};
        memcpy(&ack, incomingBytes, sizeof(ack));
        if (strcmp(ack.command, "CONFIG_ACK") == 0) {
            Serial.printf("✅ CONFIG_ACK from %s: appliedV=%u ok=%d\n",
                          ack.nodeId, ack.appliedVersion, ack.ok);
            for (auto& n : registeredNodes) {
                if (n.nodeId == String(ack.nodeId)) {
                    if (ack.ok) n.configVersionApplied = ack.appliedVersion;
                    break;
                }
            }
        }
        return;
    }

    // Fallback debug: surface unknown packet shapes during field testing.
    char preview[17] = {0};
    int previewLen = (len < 16) ? len : 16;
    for (int i = 0; i < previewLen; ++i) {
        const uint8_t c = incomingBytes[i];
        preview[i] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    Serial.printf("⚠️ Unhandled ESP-NOW packet from %s len=%d preview='%s'\n",
                  macToStr(mac).c_str(), len, preview);
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
#if ENABLE_SEND_CALLBACK_LOG
            Serial.print("📨 send_cb to ");
            if (mac_addr) Serial.print(macToStr(mac_addr));
            else          Serial.print("(null)");
            Serial.print("\n    status=");
            Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
#else
            if (status != ESP_NOW_SEND_SUCCESS) {
                Serial.print("📨 send_cb FAIL to ");
                if (mac_addr) Serial.println(macToStr(mac_addr));
                else          Serial.println("(null)");
            }
#endif
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

    // Flush buffered SD writes outside ESP-NOW callback context.
    drainCsvQueueToSd();

    // Mark nodes not-awake shortly after contact so UI reflects real wake windows.
    for (auto& n : registeredNodes) {
        // Keep this intentionally short: AWAKE means "recently heard from", not "online".
        uint32_t inactiveThreshMs = 30000UL;
        if (n.wakeIntervalMin > 0) {
            // Scale a little with configured wake interval, but cap to avoid stale AWAKE chips.
            uint32_t scaled = (uint32_t)n.wakeIntervalMin * 15000UL;
            if (scaled < 12000UL) scaled = 12000UL;
            if (scaled > 45000UL) scaled = 45000UL;
            inactiveThreshMs = scaled;
        }
        if (n.isActive && (now - n.lastSeen > inactiveThreshMs)) {
            n.isActive = false;
            Serial.printf("⚠️ Node %s (%s) marked asleep for UI (state=%s)\n",
                          n.nodeId.c_str(),
                          macToStr(n.mac).c_str(),
                          stateToStr(n.state));
        }
    }

    // Mothership-side stale inference: no node-side stale flag required.
    for (auto& n : registeredNodes) {
        if (!(n.state == PAIRED || n.state == DEPLOYED)) continue;

        NodeDesiredConfig desired = getDesiredConfig(n.nodeId.c_str());
        uint16_t cadenceMin = 0;
        if (n.state == DEPLOYED && desired.syncIntervalMin > 0) {
            // Deployed nodes are expected to sync on fleet cadence, not every wake.
            cadenceMin = desired.syncIntervalMin;
        } else if (desired.wakeIntervalMin > 0) {
            cadenceMin = desired.wakeIntervalMin;
        } else if (n.wakeIntervalMin > 0) {
            cadenceMin = n.wakeIntervalMin;
        } else if (n.inferredWakeIntervalMin > 0) {
            cadenceMin = n.inferredWakeIntervalMin;
        }
        if (!isReasonableWakeInterval((uint8_t)cadenceMin)) continue;

        const uint32_t periodMs = (uint32_t)cadenceMin * 60000UL;
        const uint32_t ageMs = (now >= n.lastSeen) ? (now - n.lastSeen) : 0;
        const uint32_t missed = ageMs / periodMs;
        n.staleMissCount = (uint8_t)((missed > 255UL) ? 255UL : missed);
        n.syncStale = (missed >= (uint32_t)STALE_MISS_THRESHOLD) || (ageMs >= STALE_MIN_AGE_MS);

        if (!n.syncStale) continue;

        const uint32_t prevExpectedMs = n.lastSeen + (missed * periodMs);
        const uint32_t assistStartMs = (prevExpectedMs > STALE_ASSIST_LEAD_MS)
            ? (prevExpectedMs - STALE_ASSIST_LEAD_MS)
            : 0;
        const uint32_t assistEndMs = prevExpectedMs + STALE_ASSIST_LAG_MS;
        const bool inAssistWindow = (now >= assistStartMs) && (now <= assistEndMs);
        const bool retryDue = (n.lastStaleAssistMs == 0) || ((now - n.lastStaleAssistMs) >= STALE_ASSIST_RETRY_MS);

        if (inAssistWindow && retryDue) {
            bool schedOk = pushSyncScheduleToNode(n.mac, n.nodeId.c_str(), desired);
            bool timeOk = sendTimeSync(n.mac, n.nodeId.c_str());
            n.lastStaleAssistMs = now;
            Serial.printf("🛟 Stale-assist for %s: missed=%lu age=%lus wakeMin=%u sched=%s time=%s\n",
                          n.nodeId.c_str(),
                          (unsigned long)missed,
                          (unsigned long)(ageMs / 1000UL),
                          (unsigned)cadenceMin,
                          schedOk ? "OK" : "FAIL",
                          timeOk ? "OK" : "FAIL");
        }
    }

    // NEW: periodic fleet-wide TIME_SYNC (every ~24 h)
    broadcastTimeSyncIfDue(false);

#if ENABLE_CYCLE_MONITOR_LOG
    // TEMP debug telemetry: helps validate node wake/sleep cycles without node serial.
    if (now - g_lastCycleMonitorMs >= CYCLE_MONITOR_INTERVAL_MS) {
        g_lastCycleMonitorMs = now;
        logCycleMonitorSnapshot(now);
    }
#endif
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
        newNode.lastTimeSyncMs       = 0;
        newNode.wakeIntervalMin      = 0;
        newNode.lastReportedQueueDepth = 0;
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

    for (auto& node : registeredNodes) {
        if (node.state != PAIRED && node.state != DEPLOYED) continue;

        ensurePeerOnChannel(node.mac, ESPNOW_CHANNEL);
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

        esp_err_t res = esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
        Serial.printf("📤 SET_SCHEDULE %d min -> %s (%s) : %s\n",
                      intervalMinutes,
                      node.nodeId.c_str(),
                      stateToStr(node.state),
                      (res==ESP_OK) ? "OK" : esp_err_to_name(res));
        if (res == ESP_OK) {
            anySent = true;
            node.wakeIntervalMin = (uint8_t)min(intervalMinutes, 255);
        }
    }
    return anySent;
}

// ===== Pull-handshake: desired config store + NODE_HELLO handler =====

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

static String legacyDesiredConfigKeyPrefix(const char* nodeId) {
    String key = String("dc_") + (nodeId ? nodeId : "");
    if (key.length() > 14) key = key.substring(0, 14);
    return key;
}

NodeDesiredConfig getDesiredConfig(const char* nodeId) {
    Preferences prefs;
    NodeDesiredConfig cfg{};
    const String key = desiredConfigKeyPrefix(nodeId);
    const String legacyKey = legacyDesiredConfigKeyPrefix(nodeId);

    if (!prefs.begin("node_dcfg", true)) return cfg;

    const String keyV = key + "v";
    const bool hasNew = prefs.isKey(keyV.c_str());
    const String activeKey = hasNew ? key : legacyKey;

    cfg.configVersion   = prefs.getUShort((activeKey + "v").c_str(), 0);
    cfg.wakeIntervalMin = prefs.getUChar((activeKey + "w").c_str(), 0);
    cfg.syncIntervalMin = prefs.getUShort((activeKey + "s").c_str(), 15);
    cfg.syncPhaseUnix   = prefs.getULong((activeKey + "p").c_str(), 0);
    prefs.end();
    return cfg;
}

void setDesiredConfig(const char* nodeId, const NodeDesiredConfig& cfg) {
    Preferences prefs;
    const String key = desiredConfigKeyPrefix(nodeId);
    if (!prefs.begin("node_dcfg", false)) {
        Serial.println("❌ setDesiredConfig: NVS open failed");
        return;
    }
    prefs.putUShort((key + "v").c_str(), cfg.configVersion);
    prefs.putUChar((key + "w").c_str(), cfg.wakeIntervalMin);
    prefs.putUShort((key + "s").c_str(), cfg.syncIntervalMin);
    prefs.putULong((key + "p").c_str(), cfg.syncPhaseUnix);
    prefs.end();
    Serial.printf("💾 setDesiredConfig: %s v=%u wakeMin=%u syncMin=%u\n",
                  nodeId, cfg.configVersion, cfg.wakeIntervalMin, cfg.syncIntervalMin);
}

void handleNodeHello(const uint8_t* senderMac, const node_hello_message_t& hello) {
    Serial.printf("👋 NODE_HELLO from %s: cfgV=%u wakeMin=%u qDepth=%u rtcUnix=%lu\n",
                  hello.nodeId, hello.configVersion,
                  hello.wakeIntervalMin, hello.queueDepth,
                  (unsigned long)hello.rtcUnix);
    Serial.printf("🔁 Cycle check-in from %s (node wake window open)\n", hello.nodeId);
    noteNodeHelloForSyncRx(hello.nodeId, hello.queueDepth);

    // Register/refresh the node as active (HELLO counts as contact).
    // If UI has a pending non-deploy target, do not let HELLO re-promote to DEPLOYED.
    NodeState helloState = DEPLOYED;
    for (const auto& n : registeredNodes) {
        if (n.nodeId == String(hello.nodeId) || memcmp(n.mac, senderMac, 6) == 0) {
            if ((n.stateChangePending && n.pendingTargetState != PENDING_TO_DEPLOYED) ||
                n.state == UNPAIRED) {
                helloState = n.state;
                Serial.printf("🛡️ HELLO promotion blocked for %s while pending target=%s\n",
                              hello.nodeId,
                              pendingStateToStr(n.pendingTargetState));
            }
            break;
        }
    }
    registerNode(senderMac, hello.nodeId, hello.nodeType, helloState);

    // Update the per-node wake interval from what the node reports
    for (auto& n : registeredNodes) {
        if (n.nodeId == String(hello.nodeId)) {
            n.wakeIntervalMin = hello.wakeIntervalMin;
            n.lastReportedQueueDepth = hello.queueDepth;
            if (hello.configVersion > n.configVersionApplied) {
                n.configVersionApplied = hello.configVersion;
                Serial.printf("✅ Config version inferred from HELLO for %s: appliedV=%u\n",
                              hello.nodeId,
                              (unsigned)n.configVersionApplied);
            }
            if (n.stateChangePending && n.pendingTargetState == PENDING_TO_DEPLOYED) {
                clearPendingState(n, "HELLO confirms deployed runtime");
            }
            if (n.deployPending) {
                n.deployPending = false;
                Serial.printf("✅ Deploy pending cleared by HELLO from %s\n", hello.nodeId);
                savePairedNodes();
            }
            break;
        }
    }

    // Check if we have a newer config to push
    NodeDesiredConfig desired = getDesiredConfig(hello.nodeId);
    if (desired.configVersion == 0) {
        Serial.printf("   ↪ No desired config stored for %s; skipping snapshot\n", hello.nodeId);
        return;
    }
    if (desired.configVersion <= hello.configVersion) {
        Serial.printf("   ↪ Node config up-to-date (v%u <= v%u)\n",
                      desired.configVersion, hello.configVersion);
        return;
    }

    // Push CONFIG_SNAPSHOT
    config_snapshot_message_t snap{};
    strcpy(snap.command, "CONFIG_SNAPSHOT");
    strncpy(snap.mothership_id, DEVICE_ID, sizeof(snap.mothership_id) - 1);
    snap.configVersion    = desired.configVersion;
    snap.wakeIntervalMin  = desired.wakeIntervalMin;
    snap.syncIntervalMin  = desired.syncIntervalMin;
    snap.syncPhaseUnix    = desired.syncPhaseUnix;

    ensurePeerOnChannel(senderMac, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_err_t res = esp_now_send(senderMac, (uint8_t*)&snap, sizeof(snap));
    Serial.printf("   ↪ Sent CONFIG_SNAPSHOT v%u to %s: %s\n",
                  snap.configVersion, hello.nodeId,
                  (res == ESP_OK) ? "OK" : esp_err_to_name(res));
}

bool broadcastSyncSchedule(int syncIntervalMinutes, unsigned long phaseUnix) {
    sync_schedule_command_message_t cmd{};
    strcpy(cmd.command, "SET_SYNC_SCHED");
    strncpy(cmd.mothership_id, DEVICE_ID,
            sizeof(cmd.mothership_id) - 1);
    cmd.syncIntervalMinutes = (unsigned long)syncIntervalMinutes;
    cmd.phaseUnix = phaseUnix;

    // Check at least one eligible node exists before sending.
    bool anyEligible = false;
    for (const auto& n : registeredNodes) {
        if (n.state == PAIRED || n.state == DEPLOYED) { anyEligible = true; break; }
    }
    if (!anyEligible) {
        Serial.println("⚠️ SET_SYNC_SCHED skipped: no PAIRED/DEPLOYED nodes eligible");
        return false;
    }

    // True ESP-NOW broadcast: one transmission reaches all nodes simultaneously.
    // Repeat 3 times for reliability.  O(1) regardless of fleet size.
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    bool anyOk = false;
    const uint8_t burstCount = 3;
    for (uint8_t i = 0; i < burstCount; ++i) {
        char nowTs[24];
        getRTCTimeString(nowTs, sizeof(nowTs));
        esp_err_t res = esp_now_send(bcast, (uint8_t*)&cmd, sizeof(cmd));
        if (res == ESP_OK) anyOk = true;
        Serial.printf("📤 [%s] SET_SYNC_SCHED BCAST burst %u/%u %d min phase=%lu -> %s\n",
                      nowTs,
                      (unsigned)(i + 1),
                      (unsigned)burstCount,
                      syncIntervalMinutes,
                      phaseUnix,
                      (res == ESP_OK) ? "OK" : esp_err_to_name(res));
        if (i + 1 < burstCount) delay(200);
    }
    return anyOk;
}

bool broadcastSyncWindowOpen(unsigned long phaseUnix) {
    sync_schedule_command_message_t cmd{};
    strcpy(cmd.command, "SYNC_WINDOW_OPEN");
    strncpy(cmd.mothership_id, DEVICE_ID,
            sizeof(cmd.mothership_id) - 1);
    cmd.syncIntervalMinutes = 0;
    cmd.phaseUnix = phaseUnix;

    bool anyEligible = false;
    for (const auto& n : registeredNodes) {
        if (n.state == PAIRED || n.state == DEPLOYED) { anyEligible = true; break; }
    }

    // True broadcast: all nodes hear one transmission simultaneously.
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ensurePeerOnChannel(bcast, ESPNOW_CHANNEL);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    bool anyOk = false;
    const uint8_t burstCount = 3;
    for (uint8_t i = 0; i < burstCount; ++i) {
        char nowTs[24];
        getRTCTimeString(nowTs, sizeof(nowTs));
        esp_err_t res = esp_now_send(bcast, (uint8_t*)&cmd, sizeof(cmd));
        if (res == ESP_OK) anyOk = true;
        Serial.printf("📤 [%s] SYNC_WINDOW_OPEN BCAST burst %u/%u phase=%lu -> %s\n",
                      nowTs,
                      (unsigned)(i + 1),
                      (unsigned)burstCount,
                      phaseUnix,
                      (res == ESP_OK) ? "OK" : esp_err_to_name(res));
        if (i + 1 < burstCount) delay(200);
    }
    if (!anyEligible) {
        Serial.println("⚠️ SYNC_WINDOW_OPEN skipped: no PAIRED/DEPLOYED nodes eligible");
    }
    return anyOk;
}

bool sendTimeSync(const uint8_t* mac, const char* nodeId) {
    time_sync_response_t resp{};
    strcpy(resp.command, "TIME_SYNC");
    strncpy(resp.mothership_id, DEVICE_ID,
            sizeof(resp.mothership_id)-1);

    char ts[24];
    getRTCTimeString(ts, sizeof(ts));

    int y,m,d,H,M,S;
    if (sscanf(ts, "%d:%d:%d %d-%d-%d", &H,&M,&S,&d,&m,&y) == 6) {
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
        if (node.state != PAIRED && node.state != DEPLOYED) continue;

        targeted++;
        bool ok = sendTimeSync(node.mac, node.nodeId.c_str());
        if (ok) {
            node.lastTimeSyncMs = nowMs;
            okCount++;
        }
    }

    if (targeted == 0) {
        uint32_t nowLogMs = millis();
        if (g_lastNoEligibleLogMs == 0 ||
            (uint32_t)(nowLogMs - g_lastNoEligibleLogMs) >= NO_ELIGIBLE_LOG_INTERVAL_MS) {
            Serial.println("⚠️ Fleet TIME_SYNC: no PAIRED/DEPLOYED nodes registered");
            g_lastNoEligibleLogMs = nowLogMs;
        }
    } else {
        Serial.printf("⏰ Fleet TIME_SYNC broadcast: targeted=%u, success=%u\n",
                      targeted, okCount);
    }
    return (okCount > 0);
}

bool broadcastTimeSyncIfDue(bool force) {
    uint32_t nowMs = millis();

    if (!force) {
        bool hasEligible = false;
        for (const auto &node : registeredNodes) {
            if (node.state == PAIRED || node.state == DEPLOYED) {
                hasEligible = true;
                break;
            }
        }

        uint32_t intervalMs = hasEligible
            ? FLEET_SYNC_INTERVAL_MS
            : FLEET_SYNC_NO_ELIGIBLE_RETRY_MS;

        if (g_lastFleetTimeSyncMs != 0 &&
            (uint32_t)(nowMs - g_lastFleetTimeSyncMs) < intervalMs) {
            return false;  // not due yet
        }
    }

    bool any = broadcastTimeSyncAll();
    g_lastFleetTimeSyncMs = nowMs;
    if (any) {
        char ts[24];
        getRTCTimeString(ts, sizeof(ts));

        Serial.printf("⏰ Fleet TIME_SYNC triggered (force=%d) at %s\n",
                      force ? 1 : 0, ts);
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
            uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            esp_err_t sendPairCmdBcast = esp_now_send(bcast, (uint8_t*)&pairCmd, sizeof(pairCmd));

            // 2) Flip local state and persist
            node.state = PAIRED;
            queuePendingState(node, PENDING_TO_PAIRED);

            // 3) Immediate PAIRING_RESPONSE
            pairing_response_t resp{};
            strcpy(resp.command, "PAIRING_RESPONSE");
            strcpy(resp.nodeId, nodeId.c_str());
            resp.isPaired = true;
            strncpy(resp.mothership_id, DEVICE_ID, sizeof(resp.mothership_id) - 1);

            esp_err_t sendResp = esp_now_send(node.mac, (uint8_t*)&resp, sizeof(resp));
            esp_err_t sendRespBcast = esp_now_send(bcast, (uint8_t*)&resp, sizeof(resp));

            Serial.printf("📤 pairNode(%s): PAIR_NODE=%s (bcast=%s), PAIRING_RESPONSE=%s (bcast=%s)\n",
                          nodeId.c_str(),
                          (sendPairCmd == ESP_OK ? "OK" : esp_err_to_name(sendPairCmd)),
                          (sendPairCmdBcast == ESP_OK ? "OK" : esp_err_to_name(sendPairCmdBcast)),
                          (sendResp   == ESP_OK ? "OK" : esp_err_to_name(sendResp)),
                          (sendRespBcast == ESP_OK ? "OK" : esp_err_to_name(sendRespBcast)));

            savePairedNodes();

                 // Success if any direct or broadcast packet was queued
                 return (sendPairCmd == ESP_OK) || (sendResp == ESP_OK) ||
                     (sendPairCmdBcast == ESP_OK) || (sendRespBcast == ESP_OK);
        }
    }
    Serial.printf("⚠️ pairNode(%s): nodeId not found\n", nodeId.c_str());
    return false;
}

// Deploy selected paired nodes with RTC time
bool deploySelectedNodes(const std::vector<String>& nodeIds) {
    bool allSuccess   = true;
    bool anyRequested = false;

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
                if (sscanf(timeBuffer, "%d:%d:%d %d-%d-%d",
                           &hour, &minute, &second,
                           &day, &month, &year) == 6)
                {
                    if (year < 2024 || year > 2099) {
                        Serial.printf("⚠️ RTC invalid (%04d-%02d-%02d %02d:%02d:%02d): aborting deploy for %s\n",
                                      year, month, day, hour, minute, second,
                                      nodeId.c_str());
                        allSuccess = false;
                        continue;
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
                    if (gSyncMode == kSyncModeDaily) {
                        deployCmd.syncIntervalMin = 0;  // 0 = daily sentinel; node extracts HH:MM from phaseUnix
                        deployCmd.syncPhaseUnix = computeNextDailySyncPhaseUnix(nowUnix);
                    } else {
                        deployCmd.syncIntervalMin = desired.syncIntervalMin;
                        if (deployCmd.syncIntervalMin == 0 && desired.syncPhaseUnix == 0) {
                            // Only fall back to interval mode when no explicit sync mechanism exists.
                            deployCmd.syncIntervalMin = 15;
                        }

                        // Phase = actual next fleet sync slot so the node's first A2 alarm
                        // fires at the same minute boundary as the mothership's next broadcast.
                        // After that sync completes the node receives a fresh SET_SYNC_SCHED
                        // broadcast and rolls forward from the stable fleet anchor thereafter.
                        const uint32_t deployPeriodSec = (uint32_t)max((int)deployCmd.syncIntervalMin, 1) * 60UL;
                        const uint32_t deployAnchor = (gLastSyncBroadcastUnix > 0)
                            ? (uint32_t)gLastSyncBroadcastUnix
                            : (nowUnix - (nowUnix % 60UL));
                        if (deployAnchor > nowUnix) {
                            deployCmd.syncPhaseUnix = deployAnchor; // anchor is itself still future
                        } else {
                            const uint32_t elapsed = nowUnix - deployAnchor;
                            deployCmd.syncPhaseUnix = deployAnchor + (elapsed / deployPeriodSec + 1UL) * deployPeriodSec;
                        }
                    }

                    Serial.printf("📤 DEPLOY payload for %s: cfgV=%u wakeMin=%u syncMin=%u phase=%lu\n",
                                  nodeId.c_str(),
                                  (unsigned)deployCmd.configVersion,
                                  (unsigned)deployCmd.wakeIntervalMin,
                                  (unsigned)deployCmd.syncIntervalMin,
                                  (unsigned long)deployCmd.syncPhaseUnix);

                    // Seed the global fleet anchor so subsequent interval broadcasts
                    // use the same phase that was sent to the node, not a stale NVS value.
                    // Also reset the slot guard so the trigger fires at slot 0 of the
                    // new anchor instead of seeing it as "already fired" from NVS.
                    if (gSyncMode != kSyncModeDaily && deployCmd.syncPhaseUnix > 0) {
                        gLastSyncBroadcastUnix = deployCmd.syncPhaseUnix;
                        gLastSyncIntervalSlot  = -1LL;
                        saveSyncRuntimeGuardsToNVS();
                    }

                    esp_err_t result =
                        esp_now_send(node.mac,
                                     (uint8_t*)&deployCmd,
                                     sizeof(deployCmd));
                    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                    esp_err_t resultBcast = esp_now_send(bcast,
                                                         (uint8_t*)&deployCmd,
                                                         sizeof(deployCmd));

                    // Reflect requested UI intent immediately; node confirms on next contact/wake.
                    node.state = DEPLOYED;
                    node.deployPending = true;
                    queuePendingState(node, PENDING_TO_DEPLOYED);
                    anyRequested = true;

                    if (result == ESP_OK || resultBcast == ESP_OK) {
                        Serial.printf("🚀 Deploy requested: %s at %s (direct=%s, bcast=%s, awaiting node runtime confirmation)\n",
                                      nodeId.c_str(),
                                      timeBuffer,
                                      (result == ESP_OK ? "OK" : esp_err_to_name(result)),
                                      (resultBcast == ESP_OK ? "OK" : esp_err_to_name(resultBcast)));

                        // Immediate post-deploy alignment while radios are already awake.
                        NodeDesiredConfig align = desired;
                        align.configVersion = deployCmd.configVersion;
                        align.wakeIntervalMin = deployCmd.wakeIntervalMin;
                        align.syncIntervalMin = deployCmd.syncIntervalMin;
                        align.syncPhaseUnix = deployCmd.syncPhaseUnix;

                        bool wakeOk = pushWakeScheduleToNode(node.mac, nodeId.c_str(), deployCmd.wakeIntervalMin);
                        bool snapOk = pushDesiredConfigSnapshot(node.mac, nodeId.c_str(), align);
                        bool schedOk = pushSyncScheduleToNode(node.mac, nodeId.c_str(), align);
                        bool markerOk = pushSyncWindowOpenToNode(node.mac, nodeId.c_str(), deployCmd.syncPhaseUnix);
                        bool timeOk = sendTimeSync(node.mac, nodeId.c_str());
                        node.wakeIntervalMin = deployCmd.wakeIntervalMin;

                        Serial.printf("✅ Deploy-align bundle for %s: wake=%s snapshot=%s syncSched=%s marker=%s time=%s\n",
                                      nodeId.c_str(),
                                      wakeOk ? "OK" : "FAIL",
                                      snapOk ? "OK" : "FAIL",
                                      schedOk ? "OK" : "FAIL",
                                      markerOk ? "OK" : "FAIL",
                                      timeOk ? "OK" : "FAIL");
                    } else {
                        Serial.printf("⚠️ Deploy send missed for %s (direct=%s, bcast=%s) — pending replay queued\n",
                                      nodeId.c_str(),
                                      esp_err_to_name(result),
                                      esp_err_to_name(resultBcast));
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

    if (!anyRequested) {
        Serial.println("⚠️ deploySelectedNodes: no matching nodes in PAIRED/DEPLOYED state");
        allSuccess = false;
    } else {
        savePairedNodes();
    }
    return allSuccess;
}


// ----------------- Queries & unpair -----------------
std::vector<NodeInfo> getUnpairedNodes() {
    std::vector<NodeInfo> unpaired;
    for (const auto& node : registeredNodes) {
        if (node.state == UNPAIRED) {
            unpaired.push_back(node);
        }
    }
    return unpaired;
}

std::vector<NodeInfo> getPairedNodes() {
    std::vector<NodeInfo> paired;
    for (const auto& node : registeredNodes) {
        if (node.state == PAIRED) {
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
            queuePendingState(n, PENDING_TO_UNPAIRED);
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

            // Burst send improves delivery when node wake windows are short.
            uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            bool anyOk = false;
            for (int i = 0; i < 3; ++i) {
                esp_err_t resDirect = esp_now_send(n.mac, (uint8_t*)&cmd, sizeof(cmd));
                esp_err_t resBcast  = esp_now_send(bcast, (uint8_t*)&cmd, sizeof(cmd));
                if (resDirect == ESP_OK || resBcast == ESP_OK) anyOk = true;
                delay(20);
            }

            Serial.printf("📤 UNPAIR_NODE -> %s (burst=%s)\n",
                          nodeId.c_str(), anyOk ? "OK" : "FAILED");
            return anyOk;
        }
    }
    Serial.printf("⚠️ sendUnpairToNode: node %s not found\n", nodeId.c_str());
    return false;
}
