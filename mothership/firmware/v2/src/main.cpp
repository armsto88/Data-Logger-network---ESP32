// Mothership V1 main firmware
// Wake-reason branching architecture with power gating, RTC alarm scheduling,
// ESP-NOW sync, SD logging, and config/service modes.
//
// Boot sequence:
//   1. Assert PWR_HOLD (GPIO26) — CRITICAL, must be first
//   2. Detect wake reason (config latch, RTC alarm, or USB)
//   3. Branch to appropriate handler
//   4. Re-arm RTC alarm before power-down
//   5. Release PWR_HOLD to power off

#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_system.h>  // esp_reset_reason()
#include <Preferences.h>
#include <WiFi.h>

#include "system/pins.h"
#include "system/power.h"
#include "system/wake_reason.h"
#include "time/rtc_alarm.h"
#include "comms/espnow_sync.h"
#include "storage/sd_logger.h"
#include "storage/flash_logger.h"
#include "config/node_registry.h"
#include "comms/espnow_config.h"
#include "config/config_server.h"
#include "config/transmission_settings.h"
#include "storage/upload_queue.h"
#include "storage/json_payload.h"
#include "comms/modem_driver.h"
#include "protocol.h"
#include "firmware_identity.h"  // role/version/build/hw identity (FW_GIT injected)
#include "ota/mothership_selfupdate.h"
#include "command_dispatcher.h"
#include "control/backend_command_ingest.h"
#include "control/node_config_control.h"

// Defer OTA image confirmation to our own first-boot self-test (see the call to
// mothershipOtaFirstBootCheck() below). Without this the Arduino core would
// auto-confirm a new image before we know it is healthy, defeating rollback.
extern "C" bool verifyRollbackLater() { return true; }

// Enlarge the Arduino loop task stack (default 8 KB). The sync-wake upload
// builds the full status+JSON payload (nodes[], modem diagnostics, transmission,
// etc.) while handleSyncWake's large frame — snapshot buffers, node-config
// vector, ACK array — is still live beneath it, which overflowed the 8 KB
// stack and reset the board mid-upload. 16 KB gives comfortable headroom
// (RAM usage is ~14%). Must be at global scope.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#ifndef DEFAULT_SYNC_INTERVAL_MIN
#define DEFAULT_SYNC_INTERVAL_MIN 60
#endif
// Total sync-wake budget (ESP-NOW listen window + modem upload). Raised from
// 180 s so that when a node is missing and the ESP-NOW window runs its full
// 120 s, the modem still has ~180 s to power on, register and upload.
static constexpr uint32_t kSyncSessionLimitMs = 300000UL;  // 5 min

// ---------------------------------------------------------------------------
// Upload subsystem globals
// ---------------------------------------------------------------------------
UploadQueue uploadQueue;

// Project started — first-ever boot timestamp (set once in NVS, never
// overwritten).  Populated in setup() for dashboard/status reporting.
uint32_t g_projectStartedUnix = 0;

// Boot diagnostics — captured once in setup(), reported in status.diagnostics.
// g_resetReasonStr is the human-readable esp_reset_reason(); g_bootCount is a
// monotonic power-on counter persisted in NVS namespace "diag".
String   g_resetReasonStr = "UNKNOWN";
uint32_t g_bootCount = 0;

// gSyncIntervalMin is now owned by config_server.cpp (loaded from NVS in
// config mode).  In sync-wake mode it falls back to the compile-time default
// when config mode has not run yet (e.g. first boot).

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void handleSyncWake();
void handleConfigWake();
void handleServiceWake();
void performModemUpload(const TransmissionSettings& txSettings, uint32_t sessionStartMs);

static BackendCommandApplyResult executeBackendNodeConfig(const Command& command) {
  const NodeConfigApplyResult applied = controlApplyNodeConfig(command);
  BackendCommandApplyResult result{};
  result.command = applied.command;
  result.durable = applied.durable;
  result.applied = applied.registryApplied;
  result.wireConfigVersion = applied.wireConfigVersion;
  return result;
}

static bool resolveBackendNodeConfig(const Command& requested,
                                     Command& resolved,
                                     CmdOutcome& rejection) {
  return controlResolveBackendNodeConfig(requested, resolved, rejection);
}

static BackendCommandApplyResult executeBackendRecordingInterval(
    const Command& command) {
  return configApplyBackendRecordingInterval(command);
}

static bool markControlConverged(const char* nodeId,
                                 uint16_t configVersion) {
  const bool nodeChanged =
      controlMarkNodeConfigConverged(nodeId, configVersion);
  const bool intervalChanged =
      configMarkRecordingIntervalNodeConverged(nodeId, configVersion);
  return nodeChanged || intervalChanged;
}

static BackendIngestResult ingestBackendResponse(const String& responseBody) {
  const uint32_t rtcBefore = getRTCTime();
  const bool rtcTrusted = rtcBefore >= 1704067200UL;
  Serial.printf("[CONTROL] HTTP response body bytes=%u\n",
                static_cast<unsigned>(responseBody.length()));
  const BackendIngestResult result = backendIngestUploadResponse(
      responseBody, rtcBefore, rtcTrusted, resolveBackendNodeConfig,
      executeBackendNodeConfig, executeBackendRecordingInterval);
  const uint32_t diagnosticUnix = result.serverTimeUnix >= 1704067200UL
      ? result.serverTimeUnix : rtcBefore;
  const bool diagnosticsDurable = backendControlRecordDiagnostics(
      result, responseBody.length(), diagnosticUnix);
  Serial.printf("[CONTROL] response=%s rejection=%s bytes=%u commands=%u processed=%u rejected=%u replayed=%u nextCursor=%lu initialRevision=%lu cursor=%lu diagnostics=%s\n",
                backendIngestStatusStr(result.status),
                backendIngestRejectionStr(result.rejection),
                static_cast<unsigned>(responseBody.length()),
                static_cast<unsigned>(result.commandCount),
                static_cast<unsigned>(result.processedCount),
                static_cast<unsigned>(result.rejectedCount),
                static_cast<unsigned>(result.replayedCount),
                static_cast<unsigned long>(result.responseNextCursor),
                static_cast<unsigned long>(result.initialStateRevision),
                static_cast<unsigned long>(result.persistedCursor),
                diagnosticsDurable ? "durable" : "FAILED");

  // The battery-backed DS3231 (set from the browser-UTC field in config mode)
  // is the sole UTC authority for the fleet's sync schedule. We deliberately DO
  // NOT let the backend response clock override it: the backend's serverTimeUnix
  // was observed carrying LOCAL wall time (not UTC), and adopting it dragged the
  // RTC +2h off true UTC and desynced every deployed node. serverTimeUnix is
  // still surfaced above for diagnostics only. Re-enable an RTC correction here
  // ONLY once the backend guarantees serverTimeUnix is true UTC epoch seconds.
  return result;
}

static void boundedRetryAndShutdown(const char* context) {
  constexpr int kMaxAttempts = 3;

  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    Serial.printf("[RETRY] %s: attempt %d/%d\n", context, attempt, kMaxAttempts);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    delay(100);

    if (initRTC() != RTC_ABSENT && armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN)) {
      Serial.printf("[RETRY] %s: rescue alarm armed on attempt %d\n", context, attempt);
      Serial.println("[RETRY] Releasing PWR_HOLD - board will wake on rescue alarm");
      releasePwrHold();
      return;
    }

    delay(500);
  }

  Serial.printf("[RETRY] %s: all attempts failed - trying one final best-effort rescue alarm\n",
                context);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(100);
  const RtcInitStatus rtcStatus = initRTC();
  const bool rescueArmed = armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN);
  Serial.printf("[RETRY] %s: final RTC init=%s rescue alarm=%s\n",
                context, rtcStatus == RTC_OK ? "ok" :
                         (rtcStatus == RTC_PRESENT_TIME_INVALID ? "TIME_INVALID" : "ABSENT"),
                rescueArmed ? "armed" : "FAILED");
  Serial.printf("[RETRY] %s: releasing PWR_HOLD as last resort\n", context);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// ESP-NOW snapshot processing (main task only)
// ---------------------------------------------------------------------------
static void sendSnapshotAck(const uint8_t* mac, const DecodedSnapshot& decoded, bool persisted) {
  if (!mac) return;

  snapshot_ack_t ack{};
  strncpy(ack.command, "SNAPSHOT_ACK", sizeof(ack.command) - 1);
  strncpy(ack.nodeId, decoded.nodeId, sizeof(ack.nodeId) - 1);
  ack.seqNum = decoded.seqNum;
  ack.persisted = persisted ? 1 : 0;
  ack.protocolVersion = decoded.protocolVersion;

  const bool sendResult = sendSnapshotAckNow(mac, ack);
  Serial.printf("[SNAP-ACK] %.15s seq=%lu persisted=%u proto=%u send=%s\n",
                ack.nodeId, static_cast<unsigned long>(ack.seqNum),
                static_cast<unsigned>(ack.persisted),
                (unsigned)ack.protocolVersion,
                sendResult ? "OK" : "FAIL");
}

void processSnapshot(const DecodedSnapshot& decoded, const uint8_t* mac) {
  if (!mac) return;

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  const float* batV = decoded.find(SENSOR_ID_BAT_V);
  const float* airT = decoded.find(SENSOR_ID_AIR_TEMP);
  const float* airH = decoded.find(SENSOR_ID_AIR_RH);
  Serial.printf("[SNAP] RX=%s nodeId=%.15s seq=%lu present=0x%04X proto=%u bat=%.2fV airT=%.1f airH=%.1f\n",
                macStr, decoded.nodeId, static_cast<unsigned long>(decoded.seqNum),
                (unsigned)decoded.sensorPresent, (unsigned)decoded.protocolVersion,
                batV ? *batV : 0.0f, airT ? *airT : 0.0f, airH ? *airH : 0.0f);

  bool persisted = false;
  if (flashIsReady()) {
    persisted = logDecodedSnapshot(decoded);
    if (!persisted) {
      Serial.println("[SNAP] Flash logging failed");
    }
  } else {
    Serial.println("[SNAP] Flash unavailable; snapshot not durably logged");
  }
  sendSnapshotAck(mac, decoded, persisted);

  for (auto& n : registeredNodes) {
    if (strncmp(n.nodeId.c_str(), decoded.nodeId, 16) == 0 ||
        memcmp(n.mac, mac, 6) == 0) {
      n.lastSeen = millis();
      n.isActive = true;
      if (batV && !isnan(*batV)) {
        n.lastReportedBatV = *batV;
      }
      if (decoded.nodeTimestamp > 0 && decoded.nodeTimestamp > n.lastNodeTimestamp) {
        n.lastNodeTimestamp = decoded.nodeTimestamp;
      }
      // Configured-sensor fault detection. A configured sensor whose channel is
      // absent from this snapshot (node emits a reading only on a successful
      // read) faults after two consecutive misses, so a single transient read
      // doesn't flap the dashboard. expectedSensorMask holds capability bits only.
      {
        const uint16_t present  = decoded.sensorPresent;
        // Normalise the ultrasonic-wind selector to the WIND present bit — a
        // snapshot only ever reports SNAP_PRESENT_WIND regardless of backend —
        // and keep only the 9 present bits for the comparison.
        uint16_t expected = n.expectedSensorMask;  // capability bits, no VALID
        if (expected & NODE_SENSOR_CFG_WIND_ULTRASONIC)
          expected = (uint16_t)((expected & ~NODE_SENSOR_CFG_WIND_ULTRASONIC) | SNAP_PRESENT_WIND);
        expected &= 0x01FF;
        const uint16_t missNow  = (uint16_t)(expected & ~present);
        n.sensorFaultMask   = (uint16_t)(missNow & n.sensorMissPrev);
        n.sensorMissPrev    = missNow;
        n.lastSensorPresent = present;
        if (n.sensorFaultMask) {
          Serial.printf("[SNAP] %.15s sensor fault mask=0x%04X (expected=0x%04X present=0x%04X)\n",
                        decoded.nodeId, (unsigned)n.sensorFaultMask,
                        (unsigned)expected, (unsigned)present);
        }
      }
      break;
    }
  }

  // A snapshot's configVersion is an existing-protocol convergence signal when
  // CONFIG_ACK was lost. Map it back to the retained dashboard/local command
  // without adding another radio message.
  if (decoded.configVersion > 0) {
    markControlConverged(decoded.nodeId, decoded.configVersion);
  }

  // Legacy sensor_data_message_t packets are intentionally ignored. All
  // deployed nodes send node_snapshot_t (V1) or node_snapshot_v2_t (V2).
}

struct ActiveSyncNode {
  uint8_t mac[6] = {0};
  char nodeId[16] = {0};
  uint8_t queueDepth = 0;
  uint8_t failedGrants = 0;
  bool released = false;
  bool releaseConfirmed = false;
};

struct LegacyRendezvousState {
  bool active = false;
  uint16_t intervalMin = 0;
  uint32_t phaseUnix = 0;
  uint8_t remainingCycles = 0;
};

static LegacyRendezvousState loadLegacyRendezvousState() {
  LegacyRendezvousState state{};
  Preferences prefs;
  if (!prefs.begin("sync_grace", true)) return state;
  state.active = prefs.getBool("active", false);
  state.intervalMin = prefs.getUShort("old_min", 0);
  state.phaseUnix = prefs.getULong("old_phase", 0);
  state.remainingCycles = prefs.getUChar("remaining", 0);
  prefs.end();
  if (state.intervalMin == 0 || state.phaseUnix < 1704067200UL ||
      state.remainingCycles == 0) {
    state = {};
  }
  return state;
}

static void saveLegacyRendezvousState(const LegacyRendezvousState& state) {
  Preferences prefs;
  if (!prefs.begin("sync_grace", false)) return;
  prefs.putBool("active", state.active && state.remainingCycles > 0);
  prefs.putUShort("old_min", state.intervalMin);
  prefs.putULong("old_phase", state.phaseUnix);
  prefs.putUChar("remaining", state.remainingCycles);
  prefs.end();
}

static bool wakeMatchesRendezvous(uint32_t nowUnix, uint16_t intervalMin,
                                  uint32_t phaseUnix) {
  if (intervalMin == 0 || phaseUnix == 0 || nowUnix < phaseUnix) return false;
  const uint32_t period = (uint32_t)intervalMin * 60UL;
  const uint32_t remainder = (nowUnix - phaseUnix) % period;
  return remainder <= 120UL || (period - remainder) <= 120UL;
}

static uint32_t nextRendezvousUnix(uint32_t nowUnix, uint16_t intervalMin,
                                   uint32_t phaseUnix) {
  if (intervalMin == 0) return UINT32_MAX;
  const uint32_t period = (uint32_t)intervalMin * 60UL;
  if (phaseUnix == 0) return nowUnix + period;
  if (nowUnix < phaseUnix) return phaseUnix;
  const uint32_t slots = (nowUnix - phaseUnix) / period;
  return phaseUnix + (slots + 1UL) * period;
}

static void drainAndPersistSnapshots() {
  EspNowSnapSlot slots[8];
  int drained = 0;
  do {
    drained = drainSnapQueue(slots, 8);
    for (int i = 0; i < drained; ++i) {
      processSnapshot(slots[i].snap, slots[i].mac);
    }
  } while (drained > 0);
}

static int findActiveSyncNode(const std::vector<ActiveSyncNode>& nodes,
                              const uint8_t* mac, const char* nodeId) {
  for (size_t i = 0; i < nodes.size(); ++i) {
    if ((mac && memcmp(nodes[i].mac, mac, 6) == 0) ||
        (nodeId && strncmp(nodes[i].nodeId, nodeId, sizeof(nodes[i].nodeId)) == 0)) {
      return (int)i;
    }
  }
  return -1;
}

// Re-establish a known node whose own NVS record is unavailable. Config version
// is intentionally zero: the following normal NODE_CONFIG burst must reapply
// the FieldHub's authoritative active/standby state and sensor mask instead of
// treating the recovery deploy as convergence.
static bool buildRecoveryDeploy(const NodeInfo& node, uint16_t activeSyncMin,
                                uint32_t activeSyncPhase,
                                deployment_command_t& deploy) {
  const uint32_t nowUnix = getRTCTime();
  if (nowUnix < 1704067200UL || activeSyncPhase < 1704067200UL) return false;

  const DateTime now(nowUnix);
  const NodeDesiredConfig desired = getDesiredConfig(node.nodeId.c_str());
  memset(&deploy, 0, sizeof(deploy));
  strncpy(deploy.command, "DEPLOY_NODE", sizeof(deploy.command) - 1);
  strncpy(deploy.nodeId, node.nodeId.c_str(), sizeof(deploy.nodeId) - 1);
  strncpy(deploy.mothership_id, "M001", sizeof(deploy.mothership_id) - 1);
  deploy.year = now.year();
  deploy.month = now.month();
  deploy.day = now.day();
  deploy.hour = now.hour();
  deploy.minute = now.minute();
  deploy.second = now.second();
  deploy.configVersion = 0;
  deploy.wakeIntervalMin = desired.wakeIntervalMin ? desired.wakeIntervalMin
      : (node.wakeIntervalMin ? node.wakeIntervalMin : DEFAULT_WAKE_INTERVAL_MINUTES);
  deploy.syncIntervalMin = activeSyncMin;
  deploy.syncPhaseUnix = activeSyncPhase;
  return true;
}

static void runCoordinatedSyncWindow(
    uint32_t sessionStartMs, bool& sessionTimedOut,
    const std::vector<node_config_message_t>& nodeCfgs,
    uint16_t activeSyncMin, uint32_t activeSyncPhase,
    uint8_t legacyGraceCycles) {
  // The rendezvous window is ANCHORED TO THE SYNC SLOT, not to our (pre-rolled)
  // wake — otherwise the mothership's ~10 s pre-roll consumes it before any node
  // is even awake. A node's DS3231 Alarm 2 is minute-resolution (it wakes on the
  // slot) and it cold-boots ~2.5 s before it can send NODE_HELLO, so we keep
  // collecting HELLOs until slot + kJoinPostSlotSec. Sized for ~20 nodes booting
  // together (HELLO jitter + a couple of retries + ESP-NOW contention). The old
  // fixed 12 s window closed ~2 s after the slot — just before nodes could answer
  // — which is why responders was always 0.
  static constexpr uint32_t kJoinPostSlotSec = 15;      // hold rendezvous this long past the slot
  static constexpr uint32_t kJoinFloorMs     = 15000UL; // min (if we wake at/after the slot)
  static constexpr uint32_t kJoinCapMs       = 45000UL; // hard ceiling
  static constexpr uint32_t kCoordinatedWindowMs = 105000UL;
  static constexpr uint16_t kGrantWindowMs = 9000U;
  static constexpr uint8_t kGrantQuota = 4;

  int deployedCount = 0;
  for (const auto& node : registeredNodes) {
    if (node.state == DEPLOYED) deployedCount++;
  }

  const uint32_t syncStartMs = millis();
  const uint32_t syncBudgetMs = (uint32_t)SYNC_WINDOW_MS < kCoordinatedWindowMs
      ? (uint32_t)SYNC_WINDOW_MS : kCoordinatedWindowMs;
  const uint32_t syncDeadlineMs = syncStartMs + syncBudgetMs;
  uint32_t sessionId = getRTCTime() ^ esp_random();
  if (sessionId == 0) sessionId = 1;

  // Anchor the join window to the slot: round the current RTC time to the nearest
  // phase-aligned boundary, then hold the rendezvous open until slot +
  // kJoinPostSlotSec. Falls back to the floor when no anchor is known.
  uint32_t joinWindowMs = kJoinFloorMs;
  {
    const uint32_t nowUnix = getRTCTime();
    if (activeSyncMin > 0 && activeSyncPhase > 0 && nowUnix >= activeSyncPhase) {
      const uint32_t period = (uint32_t)activeSyncMin * 60UL;
      const uint32_t rem = (nowUnix - activeSyncPhase) % period;
      const int32_t toSlotSec = (rem <= period - rem)
          ? -(int32_t)rem                 // nearest slot is behind us (woke late)
          : (int32_t)(period - rem);      // nearest slot is ahead (normal pre-roll)
      int32_t ms = (toSlotSec + (int32_t)kJoinPostSlotSec) * 1000;
      if (ms < (int32_t)kJoinFloorMs) ms = (int32_t)kJoinFloorMs;
      if (ms > (int32_t)kJoinCapMs)   ms = (int32_t)kJoinCapMs;
      joinWindowMs = (uint32_t)ms;
    }
  }
  Serial.printf("[SYNC] join window=%lu ms (anchored to slot+%lus, deployed=%d)\n",
                (unsigned long)joinWindowMs, (unsigned long)kJoinPostSlotSec, deployedCount);

  sync_session_open_message_t sessionOpen{};
  strncpy(sessionOpen.command, "SYNC_SESSION", sizeof(sessionOpen.command) - 1);
  sessionOpen.sessionId = sessionId;
  sessionOpen.joinWindowMs = (uint16_t)joinWindowMs;
  // Nodes start their local timer when they hear a beacon, possibly near the
  // end of rendezvous. Leave a 15 s release margin beyond our grant deadline.
  sessionOpen.sessionWindowSec = (uint16_t)((syncBudgetMs + 15000UL) / 1000UL);
  strncpy(sessionOpen.mothership_id, "M001", sizeof(sessionOpen.mothership_id) - 1);

  std::vector<ActiveSyncNode> responders;
  std::vector<String> recoveryAttempts;
  auto collectHellos = [&]() {
    SyncHelloSlot hellos[8];
    int count = 0;
    do {
      count = drainSyncHellos(hellos, 8);
      for (int i = 0; i < count; ++i) {
        hellos[i].hello.nodeId[sizeof(hellos[i].hello.nodeId) - 1] = '\0';
        NodeInfo* authorizedNode = nullptr;
        for (auto& registered : registeredNodes) {
          if (registered.state == DEPLOYED &&
              memcmp(registered.mac, hellos[i].mac, 6) == 0 &&
              registered.nodeId == String(hellos[i].hello.nodeId)) {
            authorizedNode = &registered;
            break;
          }
        }
        if (!authorizedNode) {
          Serial.printf("[SYNC] ignored HELLO from unregistered/mismatched node %.15s\n",
                        hellos[i].hello.nodeId);
          continue;
        }
        // NODE_HELLO carries the node's persisted applied config version. Treat
        // it as convergence evidence just like CONFIG_ACK/snapshot echo so a
        // lost ACK cannot leave pause/resume reporting stale for another cycle.
        const NodeDesiredConfig desired =
            getDesiredConfig(hellos[i].hello.nodeId);
        if (desired.configVersion > 0 &&
            hellos[i].hello.configVersion >= desired.configVersion) {
          const bool wasPending = authorizedNode->stateChangePending ||
              authorizedNode->configVersionApplied < desired.configVersion;
          markControlConverged(hellos[i].hello.nodeId,
                               hellos[i].hello.configVersion);
          if (wasPending) {
            Serial.printf("[SYNC] HELLO converged %.15s v%u\n",
                          hellos[i].hello.nodeId,
                          (unsigned)hellos[i].hello.configVersion);
          }
        }
        int existing = findActiveSyncNode(responders, hellos[i].mac,
                                          hellos[i].hello.nodeId);
        if (existing < 0) {
          ActiveSyncNode responder{};
          memcpy(responder.mac, hellos[i].mac, 6);
          strncpy(responder.nodeId, hellos[i].hello.nodeId,
                  sizeof(responder.nodeId) - 1);
          responder.queueDepth = hellos[i].hello.queueDepth;
          responders.push_back(responder);
          Serial.printf("[SYNC] roster +%.15s queue=%u\n", responder.nodeId,
                        (unsigned)responder.queueDepth);
        } else {
          responders[(size_t)existing].queueDepth = hellos[i].hello.queueDepth;
        }
      }

      // Fold any FW_CAPS reports into the registry (additive; older nodes send
      // none). Keeps the loop alive if caps arrive without a fresh hello.
      SyncCapsSlot caps[8];
      int capsCount = drainSyncCaps(caps, 8);
      for (int i = 0; i < capsCount; ++i) {
        caps[i].caps.nodeId[sizeof(caps[i].caps.nodeId) - 1] = '\0';
        setNodeFirmwareCaps(caps[i].caps);
        Serial.printf("[SYNC] FW_CAPS %.15s v%.11s hw=%.15s proto=%u\n",
                      caps[i].caps.nodeId, caps[i].caps.fwVersion,
                      caps[i].caps.hwTarget, (unsigned)caps[i].caps.protocolVersion);
      }

      // An unpaired node cannot join the normal HELLO/session protocol, but
      // reports NODE_STATUS while it remains awake. Only a MAC + nodeId match
      // to an existing DEPLOYED record is eligible for automatic recovery;
      // a dashboard-requested unpair (desired target 0) is never resurrected.
      SyncStatusSlot statuses[8];
      const int statusCount = drainSyncStatuses(statuses, 8);
      for (int i = 0; i < statusCount; ++i) {
        statuses[i].status.nodeId[sizeof(statuses[i].status.nodeId) - 1] = '\0';
        NodeInfo* known = nullptr;
        for (auto& node : registeredNodes) {
          if (node.state == DEPLOYED &&
              memcmp(node.mac, statuses[i].mac, 6) == 0 &&
              node.nodeId == String(statuses[i].status.nodeId)) {
            known = &node;
            break;
          }
        }
        const bool reportsUndeployed =
            statuses[i].status.state != (uint8_t)DEPLOYED ||
            statuses[i].status.deployed == 0;
        if (!known || !reportsUndeployed) continue;

        const NodeDesiredConfig desired = getDesiredConfig(known->nodeId.c_str());
        if (desired.targetState == 0) {
          Serial.printf("[RECOVERY] %.15s reports unpaired, but desired state is UNPAIRED\n",
                        known->nodeId.c_str());
          continue;
        }

        bool attempted = false;
        for (const String& id : recoveryAttempts) {
          if (id == known->nodeId) { attempted = true; break; }
        }
        if (attempted) continue;

        deployment_command_t deploy{};
        if (!buildRecoveryDeploy(*known, activeSyncMin, activeSyncPhase, deploy)) {
          Serial.printf("[RECOVERY] %.15s not dispatched: invalid FieldHub clock/schedule\n",
                        known->nodeId.c_str());
          continue;
        }

        const bool sent = sendDeploymentNow(statuses[i].mac, deploy);
        known->lastSeen = millis();
        known->isActive = true;
        known->deployPending = true;
        Serial.printf("[RECOVERY] %s -> %.15s after unpaired status (direct=%s)\n",
                      sent ? "DEPLOY_NODE sent" : "DEPLOY_NODE failed",
                      known->nodeId.c_str(), sent ? "OK" : "FAIL");
        if (sent) recoveryAttempts.push_back(known->nodeId);
      }

      // Explicit DEPLOY_ACK is the first proof that the node has accepted its
      // recovered identity. NODE_CONFIG/HELLO will independently prove the
      // authoritative desired configuration in the same or next sync window.
      SyncDeployAckSlot deployAcks[8];
      const int deployAckCount = drainDeployAcks(deployAcks, 8);
      for (int i = 0; i < deployAckCount; ++i) {
        deployAcks[i].ack.nodeId[sizeof(deployAcks[i].ack.nodeId) - 1] = '\0';
        if (deployAcks[i].ack.deployed != 1) continue;
        for (auto& node : registeredNodes) {
          if (node.state == DEPLOYED &&
              memcmp(node.mac, deployAcks[i].mac, 6) == 0 &&
              node.nodeId == String(deployAcks[i].ack.nodeId)) {
            node.deployPending = false;
            node.lastSeen = millis();
            node.isActive = true;
            Serial.printf("[RECOVERY] DEPLOY_ACK confirmed %.15s\n", node.nodeId.c_str());
            break;
          }
        }
      }
      count += capsCount + statusCount + deployAckCount;
    } while (count > 0);
  };

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session timeout before ESP-NOW rendezvous");
    sessionTimedOut = true;
    return;
  }

  // Bounded rendezvous. Control/config frames are paced by the ESP-NOW send
  // callback; no node receives permission to dump during this collection phase.
  uint32_t lastBeaconMs = 0;
  uint32_t lastConfigBurstMs = 0;
  while ((uint32_t)(millis() - syncStartMs) < joinWindowMs) {
    const uint32_t nowMs = millis();
    if (lastBeaconMs == 0 || (uint32_t)(nowMs - lastBeaconMs) >= 1000UL) {
      broadcastSyncWindowOpen();  // rolling-upgrade compatibility
      broadcastSyncSessionOpen(sessionOpen);
      lastBeaconMs = millis();
    }
    if (lastConfigBurstMs == 0 || (uint32_t)(nowMs - lastConfigBurstMs) >= 6000UL) {
      for (const auto& cfg : nodeCfgs) broadcastNodeConfigNow(cfg);
      // Repeat the active schedule at every rendezvous. A node waking on an old
      // grace slot therefore gets another migration opportunity.
      broadcastSyncScheduleNow(activeSyncMin, activeSyncPhase);
      lastConfigBurstMs = millis();
    }
    collectHellos();
    drainAndPersistSnapshots();
    delay(5);
  }

  collectHellos();
  Serial.printf("[SYNC] rendezvous closed: responders=%u deployed=%d\n",
                (unsigned)responders.size(), deployedCount);

  auto releaseNode = [&](ActiveSyncNode& responder) {
    if (responder.released) return;
    sync_release_message_t release{};
    strncpy(release.command, "SYNC_RELEASE", sizeof(release.command) - 1);
    strncpy(release.nodeId, responder.nodeId, sizeof(release.nodeId) - 1);
    release.sessionId = sessionId;
    release.mothershipUnix = getRTCTime();
    release.syncPhaseUnix = activeSyncPhase;
    release.syncIntervalMin = activeSyncMin;
    release.legacyGraceCycles = legacyGraceCycles;
    release.flags = legacyGraceCycles > 0 ? 0x01 : 0x00;
    responder.released = sendSyncRelease(responder.mac, release);

    const uint32_t ackDeadlineMs = millis() + 1200UL;
    while (responder.released && !responder.releaseConfirmed &&
           (int32_t)(ackDeadlineMs - millis()) > 0) {
      drainAndPersistSnapshots();
      SyncReleaseAckSlot ackSlots[8];
      const int ackCount = drainReleaseAcks(ackSlots, 8);
      for (int i = 0; i < ackCount; ++i) {
        if (ackSlots[i].ack.sessionId == sessionId &&
            memcmp(ackSlots[i].mac, responder.mac, 6) == 0) {
          responder.releaseConfirmed = ackSlots[i].ack.scheduleApplied == 1;
          responder.queueDepth = ackSlots[i].ack.remainingRecords;
        }
      }
      delay(5);
    }
    Serial.printf("[SYNC] release node=%.15s sent=%u confirmed=%u remaining=%u\n",
                  responder.nodeId, responder.released ? 1 : 0,
                  responder.releaseConfirmed ? 1 : 0,
                  (unsigned)responder.queueDepth);
    // The ESP-NOW peer table is limited. Peers are re-added on demand for a
    // later grace/session wake, so release the slot immediately.
    esp_now_del_peer(responder.mac);
  };

  // Empty nodes can be clock/schedule synchronized and released immediately.
  for (auto& responder : responders) {
    if (responder.queueDepth == 0) releaseNode(responder);
  }

  uint32_t releaseReserveMs = 3000UL + (uint32_t)responders.size() * 1600UL;
  if (releaseReserveMs > 30000UL) releaseReserveMs = 30000UL;
  const uint32_t grantStopMs = syncDeadlineMs - releaseReserveMs;

  uint16_t nextGrantId = 1;
  bool madeProgress = true;
  while (madeProgress && (int32_t)(grantStopMs - millis()) > 0) {
    madeProgress = false;
    for (auto& responder : responders) {
      if (responder.released || responder.queueDepth == 0 ||
          responder.failedGrants >= 2 ||
          (int32_t)(grantStopMs - millis()) <= (int32_t)kGrantWindowMs) {
        continue;
      }

      dump_grant_message_t grant{};
      strncpy(grant.command, "DUMP_GRANT", sizeof(grant.command) - 1);
      strncpy(grant.nodeId, responder.nodeId, sizeof(grant.nodeId) - 1);
      grant.sessionId = sessionId;
      grant.grantId = nextGrantId++;
      grant.maxRecords = kGrantQuota;
      grant.grantWindowMs = kGrantWindowMs;
      if (!sendDumpGrant(responder.mac, grant)) {
        responder.failedGrants++;
        Serial.printf("[SYNC] grant send failed node=%.15s failures=%u\n",
                      responder.nodeId, (unsigned)responder.failedGrants);
        continue;
      }

      Serial.printf("[SYNC] grant node=%.15s id=%u quota=%u reportedQueue=%u\n",
                    responder.nodeId, (unsigned)grant.grantId,
                    (unsigned)grant.maxRecords, (unsigned)responder.queueDepth);
      const uint32_t grantDeadlineMs = millis() + kGrantWindowMs + 1200UL;
      bool doneMatched = false;
      while (!doneMatched && (int32_t)(grantDeadlineMs - millis()) > 0 &&
             (int32_t)(grantStopMs - millis()) > 0) {
        drainAndPersistSnapshots();
        SyncDoneSlot doneSlots[8];
        const int doneCount = drainDumpDone(doneSlots, 8);
        for (int i = 0; i < doneCount; ++i) {
          if (doneSlots[i].done.sessionId == sessionId &&
              doneSlots[i].done.grantId == grant.grantId &&
              memcmp(doneSlots[i].mac, responder.mac, 6) == 0) {
            responder.queueDepth = doneSlots[i].done.remainingRecords;
            doneMatched = true;
            madeProgress = madeProgress || doneSlots[i].done.sentRecords > 0;
            Serial.printf("[SYNC] done node=%.15s sent=%u remaining=%u status=%u\n",
                          responder.nodeId,
                          (unsigned)doneSlots[i].done.sentRecords,
                          (unsigned)doneSlots[i].done.remainingRecords,
                          (unsigned)doneSlots[i].done.status);
          }
        }
        delay(5);
      }
      drainAndPersistSnapshots();
      if (!doneMatched) {
        responder.failedGrants++;
        Serial.printf("[SYNC] grant timeout node=%.15s failures=%u\n",
                      responder.nodeId, (unsigned)responder.failedGrants);
      } else if (responder.queueDepth == 0) {
        releaseNode(responder);
      }
      if (!responder.released) esp_now_del_peer(responder.mac);
    }
  }

  // Release every responder individually, even with backlog remaining. The
  // node keeps unsent records but still receives time and the active schedule.
  for (auto& responder : responders) {
    releaseNode(responder);
  }

  drainAndPersistSnapshots();
  Serial.printf("[SYNC] coordinated window complete: responders=%u drops=%lu\n",
                (unsigned)responders.size(), (unsigned long)getSnapDropCount());
}

// ---------------------------------------------------------------------------
// Modem upload sequence (called from handleSyncWake when txSettings.enabled)
// ---------------------------------------------------------------------------
void performModemUpload(const TransmissionSettings& txSettings, uint32_t sessionStartMs) {
  Serial.println("[UPLOAD] === Starting modem upload sequence ===");

  const uint32_t retryNowUnix = getRTCTime();
  const uint32_t retryIntervalMin = txSettings.uploadIntervalMin > 0 ?
      txSettings.uploadIntervalMin :
      static_cast<uint32_t>(gSyncIntervalMin > 0 ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN);
  const uint32_t retryCooldownSec = retryIntervalMin * 60UL;

  // Sample the battery at REST, before the modem rail comes up.  The A7670G
  // draws heavy (amp-level) current during TX, sagging the rail, so a reading
  // taken mid-upload badly understates the true state of charge.  status.batV
  // uses this; the loaded reading is captured later as diagnostics.batLoadedV.
  const float restingBatV = readBatteryVoltage();

  ModemDriver modem;
  modem.init();

  auto sessionExpired = [&]() -> bool {
    return millis() - sessionStartMs > kSyncSessionLimitMs;
  };

  // 1. Power on modem
  if (!modem.powerOn()) {
    Serial.println("[UPLOAD] FAIL: Modem power-on failed");
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
    return;
  }
  Serial.println("[UPLOAD] Modem powered on");
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout after modem power-on - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // 2. Wait for network registration (60s timeout — will fail without antenna)
  Serial.println("[UPLOAD] Waiting for network registration (60s timeout)...");
  const uint32_t regStartMs = millis();
  if (!modem.waitForNetwork(60000)) {
    Serial.println("[UPLOAD] Network registration failed/timeout — skipping upload");
    modem.gracefulShutdown();
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
    return;
  }
  const uint32_t regTimeMs = millis() - regStartMs;
  Serial.println("[UPLOAD] Network registered");
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout after network registration - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // -----------------------------------------------------------------------
  // JSON upload path (multi-POST loop) — used when txSettings.useJsonUpload
  // is true.  Falls back to the single-POST CSV path below on build failure.
  // -----------------------------------------------------------------------
  if (txSettings.useJsonUpload) {
    constexpr uint16_t kMaxReadingsPerPost = 100;
    constexpr uint32_t kJsonChunkBytes     = 16384;  // ~100-130 rows of CSV

    // Supabase: header-only Bearer auth, JSON array body, no query params.
    // The legacy Google Apps Script path (apiKey empty) still appends action.
    const bool isSupabase = txSettings.apiKey.length() > 0;
    const String authHeader = txSettings.apiKey.length() > 0
        ? txSettings.apiKey : txSettings.authToken;

    const uint32_t totalPendingRows = uploadQueue.getPendingRows();
    Serial.printf("[UPLOAD] JSON path: %u pending rows, %u per POST\n",
                  (unsigned)totalPendingRows, (unsigned)kMaxReadingsPerPost);

    bool anyJsonSuccess = false;
    bool firstChunk = true;
    bool controlReportDirty = false;
    uint32_t nowUnix = getRTCTime();

    // Build the status context once per upload session — the backend stores
    // it in mothership_status on every POST, so the dashboard sees fresh
    // battery/flash/fleet/schedule data after each collection.
    const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
    const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
    const uint32_t flashPct = (fsTotal > 0)
      ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;
    const UploadCursor statusCursor = uploadQueue.getCursor();
    const auto allNodes = getRegisteredNodes();
    uint16_t fTotal = 0, fDeployed = 0, fPaired = 0, fUnpaired = 0, fPending = 0, fPaused = 0;
    for (const auto& n : allNodes) {
      fTotal++;
      if (n.state == DEPLOYED) fDeployed++;
      else if (n.state == PAIRED) fPaired++;
      else fUnpaired++;
      if (n.stateChangePending || n.deployPending) fPending++;
      if (n.state == DEPLOYED && n.recordingPaused) fPaused++;
    }
    const char* statusLastResult =
        (statusCursor.lastUploadUnix > 0 && statusCursor.retryCount == 0) ? "success"
        : (statusCursor.retryCount > 0) ? "failed" : "pending";

    // Radio link quality + modem identity (queried live while registered).
    ModemDiagnostics mdiag;
    modem.getDiagnostics(mdiag);
    const String modemJson = modemDiagnosticsToJson(mdiag, regTimeMs);
    // Mothership system health. batLoadedV is sampled NOW (modem on) — the
    // sag vs status.batVoltage (resting) is a battery/regulator health signal.
    const float loadedBatV = readBatteryVoltage();
    const String diagJson =
        String("{\"resetReason\":\"") + g_resetReasonStr +
        "\",\"bootCount\":" + String(g_bootCount) +
        ",\"freeHeap\":" + String((unsigned)ESP.getFreeHeap()) +
        ",\"minFreeHeap\":" + String((unsigned)ESP.getMinFreeHeap()) +
        ",\"snapQueueDropped\":" + String((unsigned)getSnapDropCount()) +
        ",\"batLoadedV\":" +
        (isnan(loadedBatV) ? String("null") : String(loadedBatV, 2)) +
        ",\"sessionMs\":" + String((unsigned)(millis() - sessionStartMs)) + "}";

    // Firmware identity + OTA state, and the dispatcher control revision — both
    // pre-built here and emitted as status.firmware{} / status.control{}.
    const String firmwareStatusJson = mothershipFirmwareStatusJson();
    const String controlStatusJson  = backendControlStatusJson();

    StatusContext statusCtx = {
      restingBatV, flashPct, fsTotal, fsUsed,
      "scheduled", computeNextSyncIsoLocal(),
      gWakeIntervalMin, gSyncIntervalMin,
      (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily",
      fTotal, fDeployed, fPaired, fUnpaired,
      uploadQueue.getPendingRows(), statusCursor.rowsUploaded,
      statusCursor.retryCount, statusCursor.lastUploadUnix,
      FW_SEMVER, FW_BUILD, getRTCTime(),
      WiFi.macAddress(),
      fPending, txSettings.enabled,
      uploadQueue.getPendingRows(), (uint64_t)getCSVFileSize(), String(""),
      buildNodesStatusJson(nowUnix),
      buildTransmissionStatusJson(txSettings),
      (gSyncMode == SYNC_MODE_DAILY)
          ? formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute) : String(""),
      uploadQueue.getPendingBytes(),
      g_projectStartedUnix,
      statusLastResult,
      modemJson,
      diagJson,
      fPaused,
      firmwareStatusJson,
      controlStatusJson
    };

    while (uploadQueue.getPendingRows() > 0 && !sessionExpired()) {
      UploadPayload payload = uploadQueue.getNewData(kJsonChunkBytes);
      if (payload.byteLength == 0) {
        Serial.println("[UPLOAD] JSON: no data returned from queue");
        break;
      }
      Serial.printf("[UPLOAD] JSON chunk: %u bytes, ~%u rows\n",
                    payload.byteLength, payload.rowEstimate);

      // Send the mothership status object only on the FIRST POST of the
      // session — it doesn't change between chunks, so repeating it on every
      // chunk just bloats the body and writes mothership_status N times.
      JsonPayload json = buildJsonUpload(payload.csvData, kMaxReadingsPerPost,
                                         FW_SEMVER, firstChunk ? &statusCtx : nullptr,
                                         getRTCTime());
      if (json.ok && json.rowCount == 0 && json.csvBytesConsumed > 0) {
        // The row(s) in this chunk were malformed and skipped by the builder.
        // Advance past them (and purge) so the cursor doesn't stall — do NOT
        // POST or treat as an error.
        Serial.printf("[UPLOAD] JSON: skipped malformed row(s), advancing %u bytes\n",
                      (unsigned)json.csvBytesConsumed);
        nowUnix = getRTCTime();
        uploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, nowUnix);
        uploadQueue.purgeUploaded();
        continue;
      }
      if (!json.ok || json.rowCount == 0) {
        // Build failed (heap) — fall back to a CSV POST for this chunk.
        Serial.println("[UPLOAD] JSON build failed/empty — falling back to CSV POST");
        String url = buildUploadUrl(txSettings);
        if (!isSupabase) {
          url += (url.indexOf('?') >= 0) ? "&action=uploadSync" : "?action=uploadSync";
        }
        HttpsPostResult result = modem.httpsPost(url, payload.csvData,
                                                 "text/csv", authHeader);
        if (result.httpStatus == 200) {
          Serial.printf("[UPLOAD] CSV fallback SUCCESS: HTTP 200, %u bytes\n",
                        payload.byteLength);
          nowUnix = getRTCTime();
          uploadQueue.advanceCursor(payload.startOffset + payload.byteLength, nowUnix,
                                    payload.rowEstimate);
          uploadQueue.purgeUploaded();
          uploadQueue.resetRetryCount();
          anyJsonSuccess = true;
          firstChunk = false;
          continue;
        } else if (result.httpStatus == 400 || result.httpStatus == 401) {
          Serial.printf("[UPLOAD] CSV fallback non-retryable HTTP %d (%s) — stopping\n",
                        result.httpStatus,
                        result.httpStatus == 401 ? "credentials" : "bad payload");
          break;  // not transient — do not increment retry counter
        } else {
          Serial.printf("[UPLOAD] CSV fallback retryable HTTP %d, %s\n",
                        result.httpStatus, result.errorDetail.c_str());
          uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
          break;
        }
      }

      Serial.printf("[UPLOAD] JSON built: %u bytes, %u readings, consumed %u CSV bytes\n",
                    json.byteLength, (unsigned)json.rowCount, (unsigned)json.csvBytesConsumed);

      String url = buildUploadUrl(txSettings);
      if (!isSupabase) {
        url += (url.indexOf('?') >= 0)
            ? (firstChunk ? "&action=uploadSync" : "&action=uploadData")
            : (firstChunk ? "?action=uploadSync" : "?action=uploadData");
      }

      if (sessionExpired()) {
        Serial.println("[WATCHDOG] Session timeout before JSON POST - forcing shutdown");
        modem.gracefulShutdown();
        return;
      }

      Serial.printf("[UPLOAD] POSTing JSON to %s (%u bytes)\n",
                    url.c_str(), json.byteLength);
      HttpsPostResult result = modem.httpsPost(url, json.body,
                                               "application/json", authHeader);

      if (result.httpStatus == 200) {
        Serial.printf("[UPLOAD] JSON SUCCESS: HTTP 200, %u readings\n",
                      (unsigned)json.rowCount);
        nowUnix = getRTCTime();
        uploadQueue.advanceCursor(payload.startOffset + json.csvBytesConsumed, nowUnix,
                                  json.rowCount);
        uploadQueue.purgeUploaded();
        uploadQueue.resetRetryCount();
        anyJsonSuccess = true;
        firstChunk = false;
        // Commit readings first: control parsing never invalidates a successful
        // data POST or rewinds the existing upload cursor.
        const BackendIngestResult ingest =
            ingestBackendResponse(result.responseBody);
        controlReportDirty = controlReportDirty || ingest.commandCount > 0;
        // Continue loop for next chunk if more rows remain.
      } else if (result.httpStatus == 400 || result.httpStatus == 401) {
        // Not transient: bad payload or bad credentials.  Do NOT advance the
        // cursor and do NOT increment the retry counter — retrying won't help.
        Serial.printf("[UPLOAD] JSON non-retryable HTTP %d (%s) — not advancing cursor, not retrying\n",
                      result.httpStatus,
                      result.httpStatus == 401 ? "auth/credential issue" : "bad payload");
        break;
      } else {
        // 429, 5xx, or transport error (-1): retry with backoff next window.
        Serial.printf("[UPLOAD] JSON retryable HTTP %d, %s\n",
                      result.httpStatus, result.errorDetail.c_str());
        uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
        break;
      }
    }

    // A fully paused fleet legitimately has no reading rows, but the cloud
    // still needs proof that the mothership woke, completed the sync window,
    // and remains healthy. Supabase accepts the canonical batch shape with an
    // empty readings[] array and records a zero-reading sync_session while
    // refreshing mothership_status and nodes.
    if (totalPendingRows == 0 && isSupabase && !sessionExpired()) {
      JsonPayload heartbeat = buildJsonUpload(String(), 1, FW_SEMVER,
                                               &statusCtx, getRTCTime());
      if (!heartbeat.ok) {
        Serial.println("[UPLOAD] Status heartbeat JSON build failed");
      } else {
        String url = buildUploadUrl(txSettings);
        Serial.printf("[UPLOAD] POSTing status heartbeat (%u bytes, 0 readings)\n",
                      heartbeat.byteLength);
        HttpsPostResult result = modem.httpsPost(
            url, heartbeat.body, "application/json", authHeader);
        if (result.httpStatus == 200) {
          Serial.println("[UPLOAD] Status heartbeat SUCCESS: HTTP 200");
          uploadQueue.resetRetryCount();
          anyJsonSuccess = true;
          firstChunk = false;
          const BackendIngestResult ingest =
              ingestBackendResponse(result.responseBody);
          controlReportDirty = controlReportDirty || ingest.commandCount > 0;
        } else if (result.httpStatus == 400 || result.httpStatus == 401) {
          Serial.printf("[UPLOAD] Status heartbeat non-retryable HTTP %d (%s)\n",
                        result.httpStatus,
                        result.httpStatus == 401 ? "auth/credential issue" : "bad payload");
        } else {
          Serial.printf("[UPLOAD] Status heartbeat retryable HTTP %d, %s\n",
                        result.httpStatus, result.errorDetail.c_str());
          uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
        }
      }
    } else if (firstChunk) {
      Serial.println("[UPLOAD] JSON: no reading payload sent");
    }

    // A command is accepted from the HTTP response after the status object in
    // that request has already been built. Report the new durable cursor,
    // result and desired node state immediately instead of leaving the
    // dashboard at "offered, not acknowledged" until the next sync wake.
    if (controlReportDirty && isSupabase && !sessionExpired()) {
      fPending = 0;
      fPaused = 0;
      for (const auto& node : getRegisteredNodes()) {
        if (node.stateChangePending || node.deployPending) fPending++;
        if (node.state == DEPLOYED && node.recordingPaused) fPaused++;
      }
      statusCtx.rtcUnix = getRTCTime();
      statusCtx.fleetPending = fPending;
      statusCtx.fleetPaused = fPaused;
      statusCtx.pendingRows = uploadQueue.getPendingRows();
      statusCtx.pendingBytes = uploadQueue.getPendingBytes();
      statusCtx.nodesJson = buildNodesStatusJson(statusCtx.rtcUnix);
      statusCtx.controlJson = backendControlStatusJson();

      JsonPayload controlHeartbeat = buildJsonUpload(
          String(), 1, FW_SEMVER, &statusCtx, statusCtx.rtcUnix);
      if (!controlHeartbeat.ok) {
        Serial.println("[CONTROL] acknowledgement heartbeat JSON build failed");
      } else {
        const String url = buildUploadUrl(txSettings);
        Serial.printf("[CONTROL] POSTing acknowledgement heartbeat (%u bytes)\n",
                      controlHeartbeat.byteLength);
        HttpsPostResult result = modem.httpsPost(
            url, controlHeartbeat.body, "application/json", authHeader);
        if (result.httpStatus == 200) {
          Serial.println("[CONTROL] acknowledgement heartbeat SUCCESS: HTTP 200");
          uploadQueue.resetRetryCount();
          anyJsonSuccess = true;
          ingestBackendResponse(result.responseBody);
        } else {
          Serial.printf("[CONTROL] acknowledgement heartbeat HTTP %d, %s\n",
                        result.httpStatus, result.errorDetail.c_str());
        }
      }
    }

    if (anyJsonSuccess) {
      uploadQueue.resetRetryCount();
    }

    // Emergency purge + graceful shutdown.
    uploadQueue.emergencyPurgeIfFull(80);
    modem.gracefulShutdown();
    Serial.println("[UPLOAD] Modem upload sequence complete (JSON path)");
    return;
  }

  // -----------------------------------------------------------------------
  // CSV fallback path (single POST) — existing behaviour, unchanged.
  // -----------------------------------------------------------------------
  // 3. Get new data from cursor
  UploadPayload payload = uploadQueue.getNewData(txSettings.maxBytesPerSession);
  if (payload.byteLength == 0) {
    Serial.println("[UPLOAD] No new data to upload");
    modem.gracefulShutdown();
    return;
  }
  Serial.printf("[UPLOAD] Payload: %u bytes, ~%u rows\n", payload.byteLength, payload.rowEstimate);

  // 4. Build URL with auth token
  String url = buildUploadUrl(txSettings);
  if (sessionExpired()) {
    Serial.println("[WATCHDOG] Session timeout before HTTPS upload - forcing shutdown");
    modem.gracefulShutdown();
    return;
  }

  // 5. HTTPS POST
  Serial.printf("[UPLOAD] POSTing to %s\n", url.c_str());
  HttpsPostResult result = modem.httpsPost(url, payload.csvData, "text/plain", txSettings.authToken);

  // 6. Handle result (302 is a valid success for Google Apps Script redirects)
  if (result.success) {
    Serial.printf("[UPLOAD] SUCCESS: HTTP %d, %u bytes uploaded\n", result.httpStatus, payload.byteLength);
    uint32_t nowUnix = getRTCTime();
    uploadQueue.advanceCursor(payload.startOffset + payload.byteLength, nowUnix,
                              payload.rowEstimate);
    uploadQueue.purgeUploaded();
    uploadQueue.resetRetryCount();
  } else {
    Serial.printf("[UPLOAD] FAIL: HTTP %d, %s\n", result.httpStatus, result.errorDetail.c_str());
    uploadQueue.incrementRetryCount(retryNowUnix, retryCooldownSec);
  }

  // 7. Emergency purge check (regardless of upload success)
  uploadQueue.emergencyPurgeIfFull(80);

  // 8. Graceful shutdown
  modem.gracefulShutdown();
  Serial.println("[UPLOAD] Modem upload sequence complete");
}

// ---------------------------------------------------------------------------
// Sync wake handler
// ---------------------------------------------------------------------------
void handleSyncWake() {
  Serial.println("=== SYNC WAKE ===");
  const uint32_t sessionStartMs = millis();
  bool sessionTimedOut = false;
  setLed(true);

  // Load sync interval from NVS (gSyncIntervalMin is only set during config mode)
  loadWakeIntervalFromNVS();
  loadSyncModeFromNVS();
  loadDailySyncTimeFromNVS();
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  Serial.printf("[SYNC] Loaded from NVS: wake=%d min sync=%d min mode=%s daily=%02d:%02d\n",
                gWakeIntervalMin, gSyncIntervalMin,
                gSyncMode == SYNC_MODE_DAILY ? "daily" : "interval",
                gSyncDailyHour, gSyncDailyMinute);

  // --- Schedule transition detection (interval mode) -----------------------
  // If the wake/sync interval changed during config mode, the persisted anchor
  // still holds the OLD sync interval+phase that the sleeping fleet's A2
  // alarms are aligned to, and config shutdown armed THIS mothership to that
  // OLD schedule so it meets the nodes now. The nodes are only awake during
  // this sync window, so we must hand them the NEW schedule here and then both
  // sides re-anchor to it (commit happens at the re-arm below). We detect the
  // transition BEFORE the window by comparing the desired sync interval (from
  // the NVS wake setting) against the interval stored in the anchor.
  const int newSyncIntervalMin = gSyncIntervalMin;  // desired (NEW) from NVS wake
  loadSyncRuntimeGuardsFromNVS();                   // loads OLD anchor into globals
  const int anchorSyncIntervalMin = gSyncIntervalMin;
  const uint32_t anchorSyncPhaseUnix = (uint32_t)gLastSyncBroadcastUnix;
  gSyncIntervalMin = newSyncIntervalMin;            // restore NEW for upload policy
  const bool scheduleTransitionPending =
      (gSyncMode == SYNC_MODE_INTERVAL) &&
      (newSyncIntervalMin > 0) &&
      (anchorSyncIntervalMin > 0) &&
      (newSyncIntervalMin != anchorSyncIntervalMin);
  uint32_t transitionNewPhase = 0;
  if (scheduleTransitionPending) {
    // Minute-align the current meeting slot. The SAME value is announced to
    // the fleet during the window and used for the mothership re-anchor, so
    // both sides arm to identical slot boundaries regardless of small drift.
    const uint32_t nowUnix = getRTCTime();
    transitionNewPhase = (nowUnix / 60UL) * 60UL;
    // Point the phase anchor at the new slot NOW so any status object built
    // during this session's upload reports the correct next-sync time. Without
    // this, nextSyncLocal is computed from the NEW interval but the OLD phase
    // (the anchor isn't committed until the re-arm below), giving a wrong
    // displayed time. The re-arm reloads + commits the same transitionNewPhase,
    // so this early assignment doesn't affect scheduling.
    gLastSyncBroadcastUnix = transitionNewPhase;
    Serial.printf("[SYNC] Transition pending: old sync=%d min -> new sync=%d min, new phase=%lu (announcing this window)\n",
                  anchorSyncIntervalMin, newSyncIntervalMin,
                  (unsigned long)transitionNewPhase);
  }

  // Init subsystems
  const RtcInitStatus rtcStatus = initRTC();
  if (rtcStatus != RTC_OK) {
    Serial.printf("[FATAL] RTC %s - sync scheduling is unsafe\n",
                  rtcStatus == RTC_PRESENT_TIME_INVALID ? "time invalid" : "absent");
    boundedRetryAndShutdown(rtcStatus == RTC_PRESENT_TIME_INVALID ?
                            "RTC time invalid" : "RTC init failed");
    return;
  }

  LegacyRendezvousState legacyRendezvous = loadLegacyRendezvousState();
  if (legacyRendezvous.active &&
      wakeMatchesRendezvous(getRTCTime(), legacyRendezvous.intervalMin,
                            legacyRendezvous.phaseUnix)) {
    if (legacyRendezvous.remainingCycles > 0) legacyRendezvous.remainingCycles--;
    legacyRendezvous.active = legacyRendezvous.remainingCycles > 0;
    saveLegacyRendezvousState(legacyRendezvous);
    Serial.printf("[SYNC] legacy rendezvous serviced; remaining old cycles=%u\n",
                  (unsigned)legacyRendezvous.remainingCycles);
  }

  if (scheduleTransitionPending) {
    // The old schedule is now an explicit persisted recovery path, so it is
    // safe to commit the new active anchor BEFORE radio handover. If power is
    // lost mid-session, the mothership still wakes on both schedules.
    legacyRendezvous.active = anchorSyncIntervalMin > 0 &&
                              anchorSyncPhaseUnix >= 1704067200UL;
    legacyRendezvous.intervalMin = (uint16_t)anchorSyncIntervalMin;
    legacyRendezvous.phaseUnix = anchorSyncPhaseUnix;
    legacyRendezvous.remainingCycles = legacyRendezvous.active ? 3U : 0U;
    saveLegacyRendezvousState(legacyRendezvous);

    gSyncIntervalMin = newSyncIntervalMin;
    gLastSyncBroadcastUnix = transitionNewPhase;
    saveSyncRuntimeGuardsToNVS();
    Serial.printf("[SYNC] transition persisted before handover: active=%dmin phase=%lu old=%umin grace=%u\n",
                  newSyncIntervalMin, (unsigned long)transitionNewPhase,
                  (unsigned)legacyRendezvous.intervalMin,
                  (unsigned)legacyRendezvous.remainingCycles);
  }

  if (!initSD()) {
    Serial.println("[WARN] SD card init failed — continuing with flash if available");
  } else {
    Serial.println("[STORAGE] SD card mounted");
  }
  if (!initFlash()) {
    Serial.println("[WARN] Flash init failed — continuing without snapshot logging/upload queue");
  } else {
    Serial.println("[STORAGE] Active snapshot/upload storage: FLASH (LittleFS)");
  }

  // Init upload queue (after flash is ready) and emergency-purge before
  // logging new data so there is always space for incoming node snapshots.
  if (flashIsReady()) {
    uploadQueue.init();
    uploadQueue.emergencyPurgeIfFull(80);
  }

  // Load paired/deployed nodes from NVS so fleet counts and node metadata
  // are available for the JSON upload payload.
  loadPairedNodes();
  configInitRecordingIntervalControl();

  // Init ESP-NOW in sync-only mode
  if (!initEspNowSyncOnly(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW init failed — sync window will be empty");
  }
  initSnapQueue(32);

  // Listen for node data for SYNC_WINDOW_MS
  // Broadcast SYNC_WINDOW_OPEN repeatedly every 5 seconds so nodes that wake
  // later (e.g., 10 seconds after the mothership) can still catch the marker.
  // Intelligent early shutdown: exit once all deployed nodes have reported,
  // with a minimum listen time of 15 seconds.

  // Count deployed nodes for early shutdown tracking
  int deployedCount = 0;
  for (const auto& n : registeredNodes) {
    if (n.state == DEPLOYED) deployedCount++;
  }

  Serial.printf("[SYNC] Listening for %d ms (deployed nodes: %d)...\n", SYNC_WINDOW_MS, deployedCount);

  // Build the per-node NODE_CONFIG broadcasts once for this window. Each carries
  // the node's durable desired state (recording interval + targetState +
  // monotonic version). Nodes apply only a strictly newer version and ACK via
  // CONFIG_ACK, so re-broadcasting every cadence tick is idempotent. This is the
  // unified declarative delivery that replaces the old SET_SCHEDULE broadcast;
  // the SYNC schedule still rides the SET_SYNC_SCHED transition handover below.
  std::vector<node_config_message_t> nodeCfgs;
  for (const auto& n : registeredNodes) {
    NodeDesiredConfig dc = getDesiredConfig(n.nodeId.c_str());
    // Broadcast to deployed nodes and to any node pending unpair.
    if (n.state != DEPLOYED && dc.targetState != 0 /*UNPAIRED*/) continue;
    node_config_message_t cfg{};
    strncpy(cfg.command, "NODE_CONFIG", sizeof(cfg.command) - 1);
    strncpy(cfg.nodeId, n.nodeId.c_str(), sizeof(cfg.nodeId) - 1);
    strncpy(cfg.mothership_id, "M001", sizeof(cfg.mothership_id) - 1);
    cfg.configVersion   = dc.configVersion;
    cfg.targetState     = dc.targetState;   // 2=DEPLOYED, 0=UNPAIRED
    cfg.wakeIntervalMin = dc.wakeIntervalMin ? dc.wakeIntervalMin
                                             : (uint8_t)gWakeIntervalMin;
    cfg.syncIntervalMin = (uint16_t)(gSyncIntervalMin > 0 ? gSyncIntervalMin : 15);
    cfg.syncPhaseUnix   = 0;   // sync (A2) is governed by SET_SYNC_SCHED, not this
    cfg.sensorMask      = dc.sensorMask;  // 0 = auto; else SNAP_PRESENT_* + VALID bit
    // Refresh the RAM cache used by snapshot fault detection (strip the VALID bit
    // to leave just the capability bits) so faults reflect the current selection.
    setNodeExpectedSensorMask(n.nodeId.c_str(),
        (dc.sensorMask & NODE_SENSOR_MASK_VALID)
            ? (uint16_t)(dc.sensorMask & ~NODE_SENSOR_MASK_VALID) : 0);
    nodeCfgs.push_back(cfg);
  }
  Serial.printf("[SYNC] NODE_CONFIG broadcasts prepared: %u node(s)\n",
                (unsigned)nodeCfgs.size());

  const uint16_t activeSyncMin = (gSyncMode == SYNC_MODE_DAILY)
      ? 0U
      : (uint16_t)(newSyncIntervalMin > 0
          ? newSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN);
  uint32_t activeSyncPhase = scheduleTransitionPending
      ? transitionNewPhase : (uint32_t)gLastSyncBroadcastUnix;
  if (gSyncMode == SYNC_MODE_DAILY && activeSyncPhase < 1704067200UL) {
    DateTime now(getRTCTime());
    activeSyncPhase = DateTime(now.year(), now.month(), now.day(),
                               gSyncDailyHour, gSyncDailyMinute, 0).unixtime();
  }
  const uint8_t releaseGraceCycles = scheduleTransitionPending
      ? 3U : legacyRendezvous.remainingCycles;
  runCoordinatedSyncWindow(sessionStartMs, sessionTimedOut, nodeCfgs,
                           activeSyncMin, activeSyncPhase,
                           releaseGraceCycles);

  Serial.println("[SYNC] Sync window closed");

  // Persist paired-node state (including freshly reported battery voltages)
  // to NVS so it survives power-off between sync cycles.
  savePairedNodes();

  // Drain packets already accepted before unregistering the producer.
  EspNowSnapSlot finalSlots[4];
  int finalDrained = 0;
  do {
    finalDrained = drainSnapQueue(finalSlots, 4);
    for (int i = 0; i < finalDrained; ++i) {
      processSnapshot(finalSlots[i].snap, finalSlots[i].mac);
    }
  } while (finalDrained > 0);

  // --- NODE_CONFIG reconcile: process CONFIG_ACKs collected this window ---
  // A node ACKs after applying a NODE_CONFIG. An UNPAIRED ack (version matched)
  // is positive confirmation the node has wiped, so we remove it now — never on
  // absence, so a node on a flaky link is never orphaned. A DEPLOYED ack just
  // confirms convergence (the snapshot's configVersion echo also tracks this).
  {
    config_apply_ack_message_t acks[8];
    const int nAcks = drainConfigAcks(acks, 8);
    for (int i = 0; i < nAcks; ++i) {
      const String ackNodeId = String(acks[i].nodeId);
      NodeDesiredConfig dc = getDesiredConfig(ackNodeId.c_str());
      if (dc.targetState == 0 /*UNPAIRED*/ && acks[i].ok == 1 &&
          acks[i].appliedVersion >= dc.configVersion) {
        markControlConverged(ackNodeId.c_str(), acks[i].appliedVersion);
        Serial.printf("[SYNC] CONFIG_ACK unpair confirmed: %s v%u — removing node\n",
                      ackNodeId.c_str(), (unsigned)acks[i].appliedVersion);
        for (auto it = registeredNodes.begin(); it != registeredNodes.end(); ++it) {
          if (it->nodeId == ackNodeId) {
            esp_now_del_peer(it->mac);
            registeredNodes.erase(it);
            break;
          }
        }
        // Reset desired config so a future re-pair of this ID is not auto-unpaired.
        NodeDesiredConfig reset{};
        reset.configVersion = 0; reset.wakeIntervalMin = 0;
        reset.syncIntervalMin = 15; reset.syncPhaseUnix = 0; reset.targetState = 2;
        setDesiredConfig(ackNodeId.c_str(), reset);
        setNodeUserId(ackNodeId, ""); setNodeName(ackNodeId, ""); setNodeNotes(ackNodeId, "");
        savePairedNodes();
      } else if (acks[i].ok == 1 && acks[i].appliedVersion >= dc.configVersion) {
        markControlConverged(ackNodeId.c_str(), acks[i].appliedVersion);
        // Deployed/standby converged through the shared ACK/HELLO/snapshot path.
        const bool nowPaused = (dc.targetState == 3 /*STANDBY*/);
        Serial.printf("[SYNC] CONFIG_ACK converged: %s v%u (%s)\n",
                      ackNodeId.c_str(), (unsigned)acks[i].appliedVersion,
                      nowPaused ? "STANDBY" : "ACTIVE");
      }
    }
  }

  // Stop the WiFi-task producer before upload or purge code touches files.
  deinitEspNowSync();

  // --- LTE upload phase ---
  // Entirely conditional on txSettings.enabled — a complete no-op when
  // disabled, with no serial spam.  Upload failure never blocks the sync
  // wake from completing; local logging is always primary.
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session timeout before upload - skipping upload");
    sessionTimedOut = true;
  }

  if (!sessionTimedOut && txSettings.enabled && flashIsReady()) {
    uploadQueue.incrementWakeCounter();

    // Determine upload policy: uploadIntervalMin=0 means every wake.
    // Otherwise compute how many sync wakes to skip.
    uint8_t policyWakes = 1;  // default: every wake
    if (txSettings.uploadIntervalMin > 0 && gSyncIntervalMin > 0) {
      policyWakes = (uint8_t)(txSettings.uploadIntervalMin / gSyncIntervalMin);
      if (policyWakes < 1) policyWakes = 1;
    }

    if (uploadQueue.shouldUploadThisWake(policyWakes)) {
      float batV = readBatteryVoltage();
      uint16_t batMv = (uint16_t)(batV * 1000);

      if (batMv >= txSettings.minBatteryMv) {
        if (!uploadQueue.maxRetriesExceeded(txSettings.maxRetriesPerWindow, getRTCTime())) {
          const bool needsStatusHeartbeat =
              txSettings.useJsonUpload && txSettings.apiKey.length() > 0;
          if (uploadQueue.getPendingBytes() > 0 || needsStatusHeartbeat) {
            performModemUpload(txSettings, sessionStartMs);
            if (millis() - sessionStartMs > kSyncSessionLimitMs) {
              Serial.println("[WATCHDOG] Session timeout during upload - proceeding to alarm re-arm");
              sessionTimedOut = true;
            }
          } else {
            Serial.println("[UPLOAD] No new data to upload");
          }
        } else {
          Serial.printf("[UPLOAD] Max retries (%u) exceeded — skipping\n", txSettings.maxRetriesPerWindow);
        }
      } else {
        Serial.printf("[UPLOAD] Battery %u mV < min %u mV — skipping\n", batMv, txSettings.minBatteryMv);
      }
    } else {
      Serial.println("[UPLOAD] Not scheduled this wake — skipping");
    }
  }

  if (millis() - sessionStartMs > kSyncSessionLimitMs) {
    Serial.println("[WATCHDOG] Session limit reached - forcing alarm re-arm and shutdown");
    sessionTimedOut = true;
  }

  // Re-arm according to the configured schedule mode.
  loadSyncRuntimeGuardsFromNVS();

  // Detect a schedule transition: if the wake interval was changed in
  // config mode, the anchor still holds the OLD sync interval (used to
  // wake the mothership at the time the nodes expected).  Now that the
  // sync window has run and nodes have applied the new desired config,
  // re-anchor to the NEW schedule so both mothership and nodes converge.
  const int desiredSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  if (gSyncMode == SYNC_MODE_INTERVAL &&
      desiredSyncIntervalMin > 0 &&
      gSyncIntervalMin > 0 &&
      desiredSyncIntervalMin != gSyncIntervalMin) {
    const uint16_t oldIntervalMin = (uint16_t)gSyncIntervalMin;
    const uint32_t oldPhaseUnix = anchorSyncPhaseUnix;
    // Commit the transition. Re-anchor to the SAME minute-aligned phase that
    // was announced to the fleet (broadcastSyncScheduleNow above) so the
    // mothership and nodes arm to identical slot boundaries. The nodes applied
    // this phase via SET_SYNC_SCHED and re-armed A2 to the same boundary.
    uint32_t newPhase = transitionNewPhase;
    if (newPhase == 0) {
      // Defensive fallback if the transition wasn't detected pre-window
      // (e.g. anchor was missing then). Use the current minute slot.
      const uint32_t nowUnix = getRTCTime();
      newPhase = (nowUnix / 60UL) * 60UL;
    }
    Serial.printf("[SYNC] Schedule transition commit: %d->%d min, re-anchored phase=%lu\n",
                  gSyncIntervalMin, desiredSyncIntervalMin,
                  (unsigned long)newPhase);
    gLastSyncBroadcastUnix = newPhase;
    gSyncIntervalMin = desiredSyncIntervalMin;
    saveSyncRuntimeGuardsToNVS();

    // Keep three future appointments on the old cadence. This state is
    // persisted independently of the active anchor so a reboot cannot strand
    // a node that missed the first schedule handover.
    legacyRendezvous.active = oldIntervalMin > 0 && oldPhaseUnix >= 1704067200UL;
    legacyRendezvous.intervalMin = oldIntervalMin;
    legacyRendezvous.phaseUnix = oldPhaseUnix;
    legacyRendezvous.remainingCycles = legacyRendezvous.active ? 3U : 0U;
    saveLegacyRendezvousState(legacyRendezvous);
    Serial.printf("[SYNC] legacy rendezvous grace started: old=%umin phase=%lu cycles=%u\n",
                  (unsigned)legacyRendezvous.intervalMin,
                  (unsigned long)legacyRendezvous.phaseUnix,
                  (unsigned)legacyRendezvous.remainingCycles);
  }

  if (gSyncMode == SYNC_MODE_DAILY) {
    if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
      Serial.println("[FATAL] Failed to arm daily alarm - starting bounded recovery");
      boundedRetryAndShutdown("Daily alarm arm failed");
      return;
    }
  } else {
    int syncInterval = (gSyncIntervalMin > 0) ?
                       gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN;
    uint32_t phaseUnix = static_cast<uint32_t>(gLastSyncBroadcastUnix);
    if (legacyRendezvous.active && legacyRendezvous.remainingCycles > 0) {
      const uint32_t nowUnix = getRTCTime();
      const uint32_t nextActive = nextRendezvousUnix(nowUnix,
          (uint16_t)syncInterval, phaseUnix);
      const uint32_t nextLegacy = nextRendezvousUnix(nowUnix,
          legacyRendezvous.intervalMin, legacyRendezvous.phaseUnix);
      if (nextLegacy < nextActive) {
        syncInterval = legacyRendezvous.intervalMin;
        phaseUnix = legacyRendezvous.phaseUnix;
        Serial.printf("[SYNC] next alarm uses OLD rendezvous (%umin); %u grace cycles remain\n",
                      (unsigned)legacyRendezvous.intervalMin,
                      (unsigned)legacyRendezvous.remainingCycles);
      } else {
        Serial.printf("[SYNC] next alarm uses active rendezvous (%dmin); old grace still armed logically\n",
                      syncInterval);
      }
    }
    if (!armNextSyncAlarmPhase(syncInterval, phaseUnix)) {
      Serial.println("[FATAL] Failed to arm next sync alarm - starting bounded recovery");
      boundedRetryAndShutdown("Sync alarm arm failed");
      return;
    }
  }

  // Verify alarm is properly set before power-down
  if (!verifyAlarmSet()) {
    Serial.println("[FATAL] Alarm verification failed - starting bounded recovery");
    boundedRetryAndShutdown("Alarm verification failed");
    return;
  }

  Serial.println("[SYNC] Alarm armed and verified. Powering down.");
  setLed(false);
  delay(100);
  releasePwrHold();  // Board powers off here
}

// ---------------------------------------------------------------------------
// Config wake handler
// ---------------------------------------------------------------------------
void handleConfigWake() {
  Serial.println("=== CONFIG WAKE ===");

  // Keep the config request latched for the whole session. It is cleared only
  // during the final shutdown sequence, after the RTC alarm is armed.
  setLed(true);

  // Step 1: Init RTC for time display and alarm scheduling
  Serial.println("[CFG-DBG] Step 1: initRTC...");
  Serial.flush();
  const RtcInitStatus configRtcStatus = initRTC();
  if (configRtcStatus == RTC_ABSENT) {
    Serial.println("[WARN] RTC init failed in config mode — continuing without RTC");
  }
  if (configRtcStatus == RTC_PRESENT_TIME_INVALID) {
    Serial.println("[WARN] RTC time invalid in config mode - set the clock before shutdown");
  }
  Serial.println("[CFG-DBG] Step 1 done: initRTC");
  Serial.flush();

  // Step 2: Init storage: try SD first, fall back to flash (LittleFS)
  Serial.println("[CFG-DBG] Step 2: initSD / initFlash...");
  Serial.flush();
  if (!initSD()) {
    Serial.println("[WARN] SD card init failed — falling back to flash (LittleFS)");
    if (!initFlash()) {
      Serial.println("[WARN] Flash init also failed — continuing without logging");
    } else {
      Serial.println("[STORAGE] Active storage: FLASH (LittleFS)");
    }
  } else {
    Serial.println("[STORAGE] Active storage: SD card");
    // Also init flash as a secondary store for the config server's CSV view.
    initFlash();
  }
  Serial.println("[CFG-DBG] Step 2 done: initSD / initFlash");
  Serial.flush();

  // Step 3: Load paired nodes
  Serial.println("[CFG-DBG] Step 3: loading paired nodes...");
  Serial.flush();
  loadPairedNodes();
  configInitRecordingIntervalControl();
  Serial.println("[CFG-DBG] Step 3 done: loadPairedNodes OK");
  Serial.flush();

  // Step 4: Load wake interval from NVS
  Serial.println("[CFG-DBG] Step 4: loadWakeIntervalFromNVS...");
  Serial.flush();
  loadWakeIntervalFromNVS();
  Serial.println("[CFG-DBG] Step 4 done: loadWakeIntervalFromNVS");
  Serial.flush();

  // Step 5: Compute auto sync min
  Serial.println("[CFG-DBG] Step 5: computeAutoSyncMin...");
  Serial.flush();
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  Serial.println("[CFG-DBG] Step 5 done: computeAutoSyncMin");
  Serial.flush();

  // Step 6: Load sync mode from NVS
  Serial.println("[CFG-DBG] Step 6: loadSyncModeFromNVS...");
  Serial.flush();
  loadSyncModeFromNVS();
  Serial.println("[CFG-DBG] Step 6 done: loadSyncModeFromNVS");
  Serial.flush();

  // Step 7: Load daily sync time from NVS
  Serial.println("[CFG-DBG] Step 7: loadDailySyncTimeFromNVS...");
  Serial.flush();
  loadDailySyncTimeFromNVS();
  Serial.println("[CFG-DBG] Step 7 done: loadDailySyncTimeFromNVS");
  Serial.flush();

  // Step 8: Load sync runtime guards from NVS
  Serial.println("[CFG-DBG] Step 8: loadSyncRuntimeGuardsFromNVS...");
  Serial.flush();
  loadSyncRuntimeGuardsFromNVS();
  Serial.println("[CFG-DBG] Step 8 done: loadSyncRuntimeGuardsFromNVS");
  Serial.flush();

  Serial.printf("[CONFIG] wake=%d min sync=%d min mode=%s daily=%02d:%02d\n",
                gWakeIntervalMin, gSyncIntervalMin,
                (gSyncMode == 1) ? "interval" : "daily",
                gSyncDailyHour, gSyncDailyMinute);

  // Step 9: Init ESP-NOW in config mode (AP+STA on ESPNOW_CHANNEL)
  Serial.println("[CFG-DBG] Step 9: initEspNowConfig...");
  Serial.flush();
  if (!initEspNowConfig(ESPNOW_CHANNEL)) {
    Serial.println("[WARN] ESP-NOW config init failed — continuing without ESP-NOW");
  }
  Serial.println("[CFG-DBG] Step 9 done: initEspNowConfig");
  Serial.flush();

  // Step 10: Start WiFi AP + web server
  Serial.println("[CFG-DBG] Step 10: startConfigServer...");
  Serial.flush();
  startConfigServer();
  Serial.println("[CFG-DBG] Step 10 done: startConfigServer");
  Serial.flush();

  // Config server loop — exit from the web UI or on the config timeout.
  unsigned long configStartMs = millis();
  const unsigned long kConfigTimeoutMs = 30UL * 60UL * 1000UL;  // 30 min

  // Config button is for WAKING only — not for exiting.
  // The SN74LVC2G74 latch bounces unpredictably, so we don't poll it for exit.
  // Exit is via web UI /shutdown route or 10-minute timeout.
  Serial.println("[CONFIG] Config mode active. Use web UI Resume Sync button or wait 30 min.");

  while (true) {
    configServerLoop();

    // Exit condition 1: web UI shutdown button
    if (gShutdownRequested) {
      Serial.println("[CONFIG] Sync & Power Down requested via web UI — exiting config mode");
      gShutdownRequested = false;
      break;
    }

    // Exit condition 2: config timeout
    if (millis() - configStartMs > kConfigTimeoutMs) {
      Serial.println("[CONFIG] config timeout reached — exiting config mode");
      break;
    }

    delay(10);
  }

  // 7. Save NVS + re-arm alarm before power-down
  // Load the anchor BEFORE saving so we can detect a schedule change.
  // If the wake interval was changed during config mode, the anchor still
  // holds the OLD sync interval that the nodes' A2 alarms are armed to.
  // We must wake at the OLD schedule to meet the nodes, push the new
  // desired config, then switch to the new schedule at the next sync wake.
  loadSyncRuntimeGuardsFromNVS();
  const int anchorSyncIntervalMin = gSyncIntervalMin;  // interval from the loaded anchor
  const uint32_t anchorPhaseUnix = (uint32_t)gLastSyncBroadcastUnix;
  const int desiredSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  const bool scheduleChangedDuringConfig =
      (gSyncMode == SYNC_MODE_INTERVAL) &&
      (desiredSyncIntervalMin != anchorSyncIntervalMin) &&
      (anchorSyncIntervalMin > 0);

  // Save the OLD interval+phase to the anchor so that:
  //  (a) this shutdown arms the alarm to the OLD schedule (meet nodes), and
  //  (b) handleSyncWake at the next wake detects the mismatch between
  //      the anchor's OLD interval and the NVS wake interval's NEW sync,
  //      triggering the schedule transition re-anchor.
  // Do NOT save the new interval here — the transition is completed in
  // handleSyncWake after the nodes have been contacted.
  if (scheduleChangedDuringConfig) {
    Serial.printf("[CONFIG] Schedule changed during config: arming OLD sync %d min phase=%lu to meet nodes (new sync=%d min pending)\n",
                  anchorSyncIntervalMin, (unsigned long)anchorPhaseUnix,
                  desiredSyncIntervalMin);
    // gSyncIntervalMin and gLastSyncBroadcastUnix already hold the OLD
    // values from loadSyncRuntimeGuardsFromNVS() — save them as-is.
    saveSyncRuntimeGuardsToNVS();
  } else {
    // No schedule change — save normally with the current values.
    gSyncIntervalMin = desiredSyncIntervalMin;
    saveSyncRuntimeGuardsToNVS();
  }

  const RtcInitStatus shutdownRtcStatus = initRTC();
  if (shutdownRtcStatus != RTC_ABSENT) {
    if (shutdownRtcStatus == RTC_PRESENT_TIME_INVALID) {
      Serial.println("[WARN] RTC time remains invalid - arming best-effort alarm");
    }
    if (gSyncMode == 1) {
      // Interval mode — phase-aligned with nodes
      uint32_t phaseUnix = (uint32_t)gLastSyncBroadcastUnix;
      if (!armNextSyncAlarmPhase(gSyncIntervalMin > 0 ? gSyncIntervalMin : DEFAULT_SYNC_INTERVAL_MIN, phaseUnix)) {
        Serial.println("[FATAL] Failed to arm next sync alarm - starting bounded recovery");
        boundedRetryAndShutdown("Config wake sync alarm arm failed");
        return;
      }
    } else {
      // Daily mode
      if (!armDailyAlarm(gSyncDailyHour, gSyncDailyMinute)) {
        Serial.println("[FATAL] Failed to arm daily alarm - starting bounded recovery");
        boundedRetryAndShutdown("Config wake daily alarm arm failed");
        return;
      }
    }
    if (!verifyAlarmSet()) {
      Serial.println("[FATAL] Alarm verification failed - starting bounded recovery");
      boundedRetryAndShutdown("Config wake alarm verification failed");
      return;
    }
  } else {
    Serial.println("[FATAL] RTC not available - cannot arm alarm; starting bounded recovery");
    boundedRetryAndShutdown("Config wake RTC unavailable");
    return;
  }

  Serial.println("[CONFIG] Powering down.");
  setLed(false);
  clearConfigLatch();
  delay(100);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// Service wake handler (USB connected)
// ---------------------------------------------------------------------------
void handleServiceWake() {
  Serial.println("=== SERVICE WAKE (USB) ===");
  Serial.println("[SERVICE] No config button, no RTC alarm.");
  Serial.println("[SERVICE] Powering off. Press config button to enter config mode.");

  setLed(false);
  delay(100);
  releasePwrHold();
}

// ---------------------------------------------------------------------------
// Arduino setup and loop
// ---------------------------------------------------------------------------
void setup() {
  // CRITICAL: Assert PWR_HOLD as the very first action
  powerInit();
  assertPwrHold();

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Firmware ===");
  fwIdentityPrint(fwIdentity(NODE_PROTOCOL_VERSION));
  Serial.println("[FW] V2 snapshot decode; CSV schema=30; spectral metadata IDs=1109-1113");

  // Available in every wake mode. A command received after this wake's node
  // window is delivered through the existing NODE_CONFIG path at the next
  // sync; there is no direct dashboard-to-device transport.
  dispatcherInit();
  backendControlInit();

  // Arm a conservative fallback before wake classification or long-running
  // sync/upload work. Preserve the flag that caused an RTC wake because the
  // rescue alarm's verified commit clears A1F by design.
  bool rtcAlarmPendingAtBoot = false;
  const RtcInitStatus bootRtcStatus = initRTC();
  if (bootRtcStatus != RTC_ABSENT) {
    rtcAlarmPendingAtBoot = readAlarmFlag();
    if (armRescueAlarm(DEFAULT_SYNC_INTERVAL_MIN)) {
      Serial.printf("[BOOT] Rescue alarm armed for %d minutes\n", DEFAULT_SYNC_INTERVAL_MIN);
    } else {
      Serial.println("[BOOT] Warning: failed to arm rescue alarm");
    }
  } else {
    Serial.println("[BOOT] Warning: RTC unavailable; rescue alarm not armed");
  }

  // Project started — first-ever boot timestamp.  Stored once in NVS
  // namespace "tx" under "first_boot" and never overwritten, so it
  // survives reboots and firmware updates.  Only written when the RTC
  // has a valid time (after 2020-01-01) to avoid storing a garbage
  // timestamp from an unsynced RTC.
  {
    Preferences prefs;
    if (prefs.begin("tx", false)) {
      uint32_t firstBoot = prefs.getUInt("first_boot", 0);
      if (firstBoot == 0) {
        // RTC not yet synced — defer writing until we have a valid time.
        // Check again on every boot; once the RTC is set (via config mode
        // "Set time" or NTP sync), the next boot will capture it.
        uint32_t nowUnix = getRTCTime();
        if (nowUnix > 1577836800UL) {  // after 2020-01-01 00:00:00 UTC
          firstBoot = nowUnix;
          prefs.putUInt("first_boot", firstBoot);
          Serial.printf("[BOOT] Project started at unix=%u\n", (unsigned)firstBoot);
        } else {
          Serial.println("[BOOT] RTC not synced — deferring projectStarted write");
        }
      }
      prefs.end();
      g_projectStartedUnix = firstBoot;
    } else {
      Serial.println("[BOOT] Warning: could not open NVS \"tx\" for project-start");
    }
  }

  // Boot diagnostics — reset reason + monotonic boot counter.  Cheap early-
  // warning signal: repeated BROWNOUT/WDT resets point at power/regulator or
  // firmware-hang problems that are otherwise invisible from the dashboard.
  {
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:   g_resetReasonStr = "POWERON";   break;
      case ESP_RST_EXT:       g_resetReasonStr = "EXT";       break;
      case ESP_RST_SW:        g_resetReasonStr = "SW";        break;
      case ESP_RST_PANIC:     g_resetReasonStr = "PANIC";     break;
      case ESP_RST_INT_WDT:   g_resetReasonStr = "INT_WDT";   break;
      case ESP_RST_TASK_WDT:  g_resetReasonStr = "TASK_WDT";  break;
      case ESP_RST_WDT:       g_resetReasonStr = "WDT";       break;
      case ESP_RST_DEEPSLEEP: g_resetReasonStr = "DEEPSLEEP"; break;
      case ESP_RST_BROWNOUT:  g_resetReasonStr = "BROWNOUT";  break;
      case ESP_RST_SDIO:      g_resetReasonStr = "SDIO";      break;
      default:                g_resetReasonStr = "UNKNOWN";   break;
    }
    Preferences dprefs;
    if (dprefs.begin("diag", false)) {
      g_bootCount = dprefs.getUInt("boot_count", 0) + 1;
      dprefs.putUInt("boot_count", g_bootCount);
      dprefs.end();
    }
    Serial.printf("[BOOT] Reset reason=%s bootCount=%u\n",
                  g_resetReasonStr.c_str(), (unsigned)g_bootCount);
  }

  // Print battery voltage at boot
  float vBat = readBatteryVoltage();
  Serial.printf("[PWR] Battery: %.3f V\n", vBat);

  // Capture wake inputs independently. Config wins if the button latch and
  // RTC alarm are active at the same time.
  WakeSources sources = detectWakeSources();
  sources.rtcAlarm = sources.rtcAlarm || rtcAlarmPendingAtBoot;
  WakeReason reason = selectWakeReason(sources);
  printWakeSources(sources);
  printWakeReason(reason);

  // PWR_HOLD is already secure. Clear a pending RTC flag only after both wake
  // inputs have been captured, including when config mode takes priority.
  if (sources.rtcAlarm && !clearAlarmFlag()) {
    Serial.println("[WAKE] Warning: failed to clear RTC alarm flag");
  }

  // First-boot OTA self-test: reaching here means PWR_HOLD, NVS/registry, RTC,
  // and wake classification all succeeded on this boot. If the running image is
  // on probation, confirm it now; a new image that hung before this point would
  // have been rolled back by the bootloader instead.
  mothershipOtaFirstBootCheck();

  // Branch based on wake reason
  switch (reason) {
    case WAKE_RTC_ALARM:
      handleSyncWake();
      break;
    case WAKE_CONFIG_BUTTON:
      handleConfigWake();
      break;
    case WAKE_USB_SERVICE:
      handleServiceWake();
      break;
    case WAKE_UNKNOWN:
      Serial.println("[WAKE] Unknown wake reason (RTC read failed) - arming rescue and shutting down");
      boundedRetryAndShutdown("WAKE_UNKNOWN: RTC status read failed");
      break;
    default:
      Serial.println("[WAKE] Unknown wake reason — defaulting to sync");
      handleSyncWake();
      break;
  }
}

void loop() {
  // If we reach here, releasePwrHold() didn't cut power.
  // This happens when USB/SW10 is still holding VSYS after flashing.
  // Just keep trying to release PWR_HOLD — the board will die when
  // the user flicks SW10 to remove USB power.
  static unsigned long lastRetry = 0;
  if (millis() - lastRetry > 2000) {
    Serial.println("[WAIT] Board still alive — flick SW10 to cut USB power.");
    digitalWrite(PIN_PWR_HOLD, LOW);
    lastRetry = millis();
  }
  delay(100);
}
