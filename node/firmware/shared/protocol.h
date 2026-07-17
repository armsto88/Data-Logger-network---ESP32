#pragma once
#include <Arduino.h>

// ===== Wire protocol messages (single source of truth) =====

// Sensor -> Mothership data
typedef struct sensor_data_message {
    char nodeId[16];            // e.g., "TEMP_001"
    char sensorType[16];        // e.g., "SOIL_TEMP", "AIR_TEMP"
    char sensorLabel[24];       // e.g., "SOIL1_TEMP", "AUX1_INPUT"
    uint16_t sensorId;          // stable channel id for downstream processing
    float value;                // reading
    unsigned long nodeTimestamp;// node-side timestamp (unix or millis)
    uint16_t qualityFlags;      // reserved for sensor/time/queue quality bits
} sensor_data_message_t;

// Discovery (node -> broadcast) and response (mothership -> broadcast)
typedef struct discovery_message {
    char nodeId[16];
    char nodeType[16];          // "temperature", "humidity", etc.
    char command[20];           // "DISCOVER_REQUEST"
    unsigned long timestamp;
} discovery_message_t;

typedef struct discovery_response {
    char command[20];           // "DISCOVER_RESPONSE" or "DISCOVERY_SCAN"
    char mothership_id[16];
    bool acknowledged;
} discovery_response_t;

// Pairing (legacy request/response) + explicit “pair”
typedef struct pairing_request {
    char command[20];           // "PAIRING_REQUEST"
    char nodeId[16];
} pairing_request_t;

typedef struct pairing_response {
    char command[20];           // "PAIRING_RESPONSE"
    char nodeId[16];
    bool isPaired;
    char mothership_id[16];
} pairing_response_t;

typedef struct pairing_command {
    char command[20];           // "PAIR_NODE"
    char nodeId[16];
    char mothership_id[16];
} pairing_command_t;

// Deployment (server -> node) with time payload
typedef struct deployment_command {
    char command[20];           // "DEPLOY_NODE"
    char nodeId[16];
    unsigned long year, month, day, hour, minute, second;
    uint16_t configVersion;     // desired config snapshot version at deploy time
    uint8_t wakeIntervalMin;    // desired wake interval to apply immediately
    uint16_t syncIntervalMin;   // desired sync interval to apply immediately
    uint32_t syncPhaseUnix;     // desired sync phase anchor to apply immediately
    char mothership_id[16];
} deployment_command_t;

// Node -> mothership deployment confirmation (sent immediately after apply)
typedef struct deployment_ack_message {
    char command[20];           // "DEPLOY_ACK"
    char nodeId[16];
    uint8_t deployed;           // 1=deployed applied, 0=failed
    uint32_t rtcUnix;           // node RTC unix at ACK time
} deployment_ack_message_t;

// Time sync request/response
typedef struct time_sync_request {
    char nodeId[16];
    char command[16];           // "REQUEST_TIME"
    unsigned long requestTime;
} time_sync_request_t;

typedef struct time_sync_response {
    char command[16];           // "TIME_SYNC"
    unsigned long year, month, day, hour, minute, second;
    char mothership_id[16];
} time_sync_response_t;

// Unpair (server -> node)
typedef struct unpair_command {
    char command[16];           // "UNPAIR_NODE"
    char nodeId[16];            // target node ID; prevents broadcast unpairing every node
    char mothership_id[16];
} unpair_command_t;

// Wake-interval schedule (server -> node) -> program DS3231 alarm
typedef struct schedule_command_message {
    char command[16];           // "SET_SCHEDULE"
    int  intervalMinutes;       // 1,5,10,20,30,60
    char mothership_id[16];
} schedule_command_message_t;

// Sync schedule (server -> node): tells nodes when to open WiFi and flush backlog
typedef struct sync_schedule_command_message {
    char command[20];           // "SET_SYNC_SCHED"
    unsigned long syncIntervalMinutes;
    unsigned long phaseUnix;    // alignment anchor (unix seconds)
    char mothership_id[16];
} sync_schedule_command_message_t;

// Unified declarative node config (server -> node), broadcast every sync window.
// Supersedes SET_SCHEDULE + SET_SYNC_SCHED + UNPAIR_NODE for DEPLOYED nodes: the
// mothership holds each node's desired state and re-broadcasts it until the
// node's echoed configVersion (in snapshots) matches. Idempotent — the node
// applies only a strictly newer configVersion. targetState folds unpair/undeploy
// in (0=UNPAIRED wipe, 2=DEPLOYED apply schedule). See FIELDMESH_NODE_CONFIG_PROTOCOL.md.
typedef struct node_config_message {
    char     command[16];       // "NODE_CONFIG"
    char     nodeId[16];        // target node (broadcast-safe; node matches its own NODE_ID)
    char     mothership_id[16];
    uint16_t configVersion;     // monotonic desired version
    uint8_t  targetState;       // 0=UNPAIRED, 1=PAIRED, 2=DEPLOYED
    uint8_t  wakeIntervalMin;   // 1,5,10,20,30,60
    uint16_t syncIntervalMin;   // sync cadence (minutes)
    uint16_t sensorMask;        // expected/configured sensors — SNAP_PRESENT_* bits
                                // + NODE_SENSOR_MASK_VALID. 0 = auto (legacy: an
                                // older mothership sends 0 -> node auto-detects).
    uint32_t syncPhaseUnix;     // sync anchor (unix seconds)
} node_config_message_t;

// ===== Pull-handshake messages (Phase 2) =====

// Node -> Mothership: sent at start of each wake cycle (before data flush)
typedef struct node_hello_message {
    char command[16];       // "NODE_HELLO"
    char nodeId[16];
    char nodeType[16];
    uint16_t configVersion; // config version node currently has applied (0 = none)
    uint8_t  wakeIntervalMin;
    uint8_t  queueDepth;    // samples currently queued
    uint32_t rtcUnix;       // node RTC time at wake
} node_hello_message_t;

// Node -> Mothership: asynchronous status push (used by rescue and field recovery flows)
typedef struct node_status_message {
    char command[16];       // "NODE_STATUS"
    char nodeId[16];
    uint8_t state;          // 0=UNPAIRED, 1=PAIRED, 2=DEPLOYED
    uint8_t rtcSynced;      // 0/1
    uint8_t deployed;       // 0/1 (node local deployedFlag)
    uint8_t rescueMode;     // 0/1
    uint32_t rtcUnix;       // node RTC unix when status was generated
} node_status_message_t;

// Mothership -> Node: sent only when mothership has a newer config version
typedef struct config_snapshot_message {
    char     command[20];       // "CONFIG_SNAPSHOT"
    char     mothership_id[16];
    uint16_t configVersion;     // version being pushed
    uint8_t  wakeIntervalMin;
    uint16_t syncIntervalMin;
    uint32_t syncPhaseUnix;
    uint8_t  reserved[2];       // padding for future fields
} config_snapshot_message_t;

// Node -> Mothership: ACK after applying a CONFIG_SNAPSHOT
typedef struct config_apply_ack_message {
    char    command[20];        // "CONFIG_ACK"
    char    nodeId[16];
    uint16_t appliedVersion;
    uint8_t ok;                 // 1 = applied OK, 0 = failed
} config_apply_ack_message_t;

// Mothership -> Node: optional durable persistence acknowledgement for a
// NODE_SNAPSHOT. Legacy mothership firmware does not send this yet, so node
// firmware keeps link-layer compatibility mode enabled by default.
#ifndef NODE_PROTOCOL_VERSION
#define NODE_PROTOCOL_VERSION 2
#endif

#ifndef NODE_REQUIRE_DURABLE_SNAPSHOT_ACK
#define NODE_REQUIRE_DURABLE_SNAPSHOT_ACK 0
#endif

typedef struct snapshot_ack {
    char     command[16];       // "SNAPSHOT_ACK"
    char     nodeId[16];
    uint32_t seqNum;
    uint8_t  persisted;         // 1 = mothership durably stored this seq
    uint8_t  protocolVersion;   // NODE_PROTOCOL_VERSION
    uint16_t reserved;
} snapshot_ack_t;

// ===== Coordinated sync-session messages =====
//
// A sync wake is a bounded mothership-controlled pull session:
//   SYNC_SESSION -> jittered NODE_HELLO roster -> DUMP_GRANT chunks ->
//   DUMP_DONE -> SYNC_RELEASE -> RELEASE_ACK.
// sessionId/grantId make delayed packets from an earlier wake harmless.

typedef struct __attribute__((packed)) sync_session_open_message {
    char     command[16];       // "SYNC_SESSION"
    uint32_t sessionId;         // unique for this mothership wake
    uint16_t joinWindowMs;      // time allowed for jittered NODE_HELLO replies
    uint16_t sessionWindowSec;  // total bounded radio session
    char     mothership_id[16];
} sync_session_open_message_t;

typedef struct __attribute__((packed)) dump_grant_message {
    char     command[16];       // "DUMP_GRANT"
    char     nodeId[16];        // only this node may transmit snapshots
    uint32_t sessionId;
    uint16_t grantId;
    uint8_t  maxRecords;        // fairness quota for this round
    uint8_t  reserved;
    uint16_t grantWindowMs;     // node must stop before this expires
    uint16_t reserved2;
} dump_grant_message_t;

typedef struct __attribute__((packed)) dump_done_message {
    char     command[16];       // "DUMP_DONE"
    char     nodeId[16];
    uint32_t sessionId;
    uint16_t grantId;
    uint8_t  sentRecords;
    uint8_t  remainingRecords;
    uint8_t  status;            // 0=quota/empty, 1=send/ACK failure, 2=deadline
    uint8_t  reserved;
} dump_done_message_t;

typedef struct __attribute__((packed)) sync_release_message {
    char     command[16];       // "SYNC_RELEASE"
    char     nodeId[16];
    uint32_t sessionId;
    uint32_t mothershipUnix;    // final clock synchronization
    uint32_t syncPhaseUnix;     // active/new rendezvous phase
    uint16_t syncIntervalMin;   // active/new rendezvous interval
    uint8_t  legacyGraceCycles; // old rendezvous cycles still being serviced
    uint8_t  flags;             // bit0=schedule transition active
} sync_release_message_t;

typedef struct __attribute__((packed)) sync_release_ack_message {
    char     command[16];       // "RELEASE_ACK"
    char     nodeId[16];
    uint32_t sessionId;
    uint16_t appliedSyncIntervalMin;
    uint8_t  scheduleApplied;   // 1 only after RTC/config persistence succeeds
    uint8_t  remainingRecords;
} sync_release_ack_message_t;

// Optional: RNT-compatible pairing struct
typedef struct rnt_pairing_t {
    uint8_t msgType;            // 0 = PAIRING, 1 = DATA
    uint8_t id;                 // device/server id
    uint8_t macAddr[6];
    uint8_t channel;
} rnt_pairing_t;

// ===== Shared constants =====
#define DEFAULT_WAKE_INTERVAL_MINUTES 5
#define DEFAULT_SLEEP_TIME_SECONDS (DEFAULT_WAKE_INTERVAL_MINUTES * 60)

#define NODE_TYPE_MULTI_ENV     "MULTI_ENV"
#define NODE_PROFILE_V1         "STD_NODE_V1"

#define SENSOR_TYPE_AIR_TEMP    "AIR_TEMP"
#define SENSOR_TYPE_HUMIDITY    "HUMIDITY"
#define SENSOR_TYPE_PAR         "PAR"
#define SENSOR_TYPE_WIND        "WIND"
#define SENSOR_TYPE_SOIL_VWC    "SOIL_VWC"
#define SENSOR_TYPE_SOIL_TEMP   "SOIL_TEMP"
#define SENSOR_TYPE_AUX         "AUX"

// Stable sensor channel IDs for standard node profile.
#define SENSOR_ID_UNKNOWN       0
#define SENSOR_ID_AIR_TEMP      1001
#define SENSOR_ID_AIR_RH        1002
#define SENSOR_ID_SPECTRAL_415  1101
#define SENSOR_ID_SPECTRAL_445  1102
#define SENSOR_ID_SPECTRAL_480  1103
#define SENSOR_ID_SPECTRAL_515  1104
#define SENSOR_ID_SPECTRAL_555  1105
#define SENSOR_ID_SPECTRAL_590  1106
#define SENSOR_ID_SPECTRAL_630  1107
#define SENSOR_ID_SPECTRAL_680  1108
// Additional AS7341 outputs (same 1100 spectral group):
#define SENSOR_ID_SPECTRAL_CLEAR 1109  // wideband "Clear" photodiode, raw counts
#define SENSOR_ID_SPECTRAL_NIR   1110  // ~910 nm NIR, raw counts
#define SENSOR_ID_SPECTRAL_GAIN  1111  // applied gain multiplier (0.5..512)
#define SENSOR_ID_SPECTRAL_ATIME 1112  // integration time this read (ms)
#define SENSOR_ID_SPECTRAL_SAT   1113  // saturation/validity flag (0=ok, 1=saturated)
#define SENSOR_ID_WIND_SPEED    1201
#define SENSOR_ID_WIND_DIR      1202
// The SOIL*_VWC names are retained for wire/schema compatibility. Current
// node firmware emits moisture sensor output volts in these channels; backend
// calibration is responsible for converting volts to moisture/VWC.
#define SENSOR_ID_SOIL1_VWC     2001
#define SENSOR_ID_SOIL2_VWC     2002
#define SENSOR_ID_SOIL1_TEMP    2003
#define SENSOR_ID_SOIL2_TEMP    2004
#define SENSOR_ID_BAT_V         4001
#define SENSOR_ID_AUX1          3001
#define SENSOR_ID_AUX2          3002
#define SENSOR_ID_PAR           1301

// Reserved sensor ID ranges:
// 1000-1999: Standard environmental sensors (temp, humidity, spectral, wind, PAR)
// 2000-2999: Soil and analog sensors
// 3000-3999: Auxiliary inputs
// 4000-4999: System metrics (battery, internal temp, etc.)
// 5000-5999: Future port-based dynamic sensors (plug-and-play ports)

// ===== Snapshot packet (node -> mothership, one per wake cycle) =====
// Single packet containing all sensor values for one wake event.
// Replaces multiple sensor_data_message_t packets.
// Size: 124 bytes — well within the 250-byte ESP-NOW limit.

// Bitmask for snapshot_t::sensorPresent — set bit = sensor had a valid read this cycle.
#define SNAP_PRESENT_AIR_TEMP    (1u << 0)
#define SNAP_PRESENT_AIR_RH      (1u << 1)
#define SNAP_PRESENT_SPECTRAL    (1u << 2)  // all 8 channels as a group
#define SNAP_PRESENT_WIND        (1u << 3)  // speed + direction together
#define SNAP_PRESENT_SOIL1       (1u << 4)  // soil1 vwc + temp together
#define SNAP_PRESENT_SOIL2       (1u << 5)  // soil2 vwc + temp together
#define SNAP_PRESENT_AUX1        (1u << 6)
#define SNAP_PRESENT_AUX2        (1u << 7)
#define SNAP_PRESENT_BAT_V       (1u << 8)

// ===== Configured ("expected") sensor mask — node_config_message_t::sensorMask =====
//
// The operator selects, per node, which sensors are installed. That selection is
// broadcast in NODE_CONFIG (version-gated), persisted in node NVS, and used to:
//   * gate registration of PASSIVE (non-self-identifying) sensors on the node, and
//   * let the mothership flag a configured sensor that stops reporting as a FAULT
//     instead of silently disappearing.
// The sensor bits reuse the SNAP_PRESENT_* layout so a config mask and a snapshot's
// sensorPresent are directly comparable.
//
// NODE_SENSOR_MASK_VALID distinguishes "explicitly configured" (bit set — the mask
// is authoritative, including the case where the operator turned everything off)
// from "unset / legacy" (mask == 0 — the node auto-detects every backend, and an
// older mothership that sends 0 in this field stays fully compatible).
#define NODE_SENSOR_MASK_VALID   (1u << 15)

// Config-only selector: choose the ultrasonic wind backend instead of the reed
// cup. Both report as SNAP_PRESENT_WIND, so this bit only tells the node which
// backend to register — it is normalised back to SNAP_PRESENT_WIND for
// mothership fault detection. Reed cup is selected via SNAP_PRESENT_WIND itself.
#define NODE_SENSOR_CFG_WIND_ULTRASONIC (1u << 9)

// All operator-selectable bits: the 9 present bits (0-8) + the ultrasonic selector.
#define NODE_SENSOR_CFG_ALL_BITS (0x01FFu | NODE_SENSOR_CFG_WIND_ULTRASONIC)

// Passive capability bits: sensors the node cannot probe for on the bus, so they
// are only registered/read when the configured mask enables them. The remaining
// bits (AIR_TEMP/AIR_RH/SPECTRAL) are self-identifying I2C parts that are always
// auto-detected regardless of the mask.
#define NODE_SENSOR_PASSIVE_BITS \
    (SNAP_PRESENT_WIND | SNAP_PRESENT_SOIL1 | SNAP_PRESENT_SOIL2 | \
     SNAP_PRESENT_AUX1 | SNAP_PRESENT_AUX2)

// Map a SENSOR_ID_* channel to its SNAP_PRESENT_* capability bit (0 = unmapped).
// Shared so node registration gating and mothership fault detection agree on the
// mapping from a stable channel id to a configurable capability.
inline uint16_t snapPresentBitForSensorId(uint16_t sensorId) {
  switch (sensorId) {
    case SENSOR_ID_AIR_TEMP:     return SNAP_PRESENT_AIR_TEMP;
    case SENSOR_ID_AIR_RH:       return SNAP_PRESENT_AIR_RH;
    case SENSOR_ID_SPECTRAL_415: case SENSOR_ID_SPECTRAL_445:
    case SENSOR_ID_SPECTRAL_480: case SENSOR_ID_SPECTRAL_515:
    case SENSOR_ID_SPECTRAL_555: case SENSOR_ID_SPECTRAL_590:
    case SENSOR_ID_SPECTRAL_630: case SENSOR_ID_SPECTRAL_680:
    case SENSOR_ID_SPECTRAL_CLEAR: case SENSOR_ID_SPECTRAL_NIR:
    case SENSOR_ID_SPECTRAL_GAIN: case SENSOR_ID_SPECTRAL_ATIME:
    case SENSOR_ID_SPECTRAL_SAT:
      return SNAP_PRESENT_SPECTRAL;
    case SENSOR_ID_WIND_SPEED:   case SENSOR_ID_WIND_DIR:  return SNAP_PRESENT_WIND;
    case SENSOR_ID_SOIL1_VWC:    case SENSOR_ID_SOIL1_TEMP: return SNAP_PRESENT_SOIL1;
    case SENSOR_ID_SOIL2_VWC:    case SENSOR_ID_SOIL2_TEMP: return SNAP_PRESENT_SOIL2;
    case SENSOR_ID_AUX1:         return SNAP_PRESENT_AUX1;
    case SENSOR_ID_AUX2:         return SNAP_PRESENT_AUX2;
    default:                     return 0;
  }
}

typedef struct __attribute__((packed)) node_snapshot {
    char     command[16];       // "NODE_SNAPSHOT"              16
    char     nodeId[16];        // e.g. "ENV_94E38C"            16
    uint32_t nodeTimestamp;     // node RTC unix at capture      4
    uint32_t seqNum;            // snapshot sequence number      4
    uint16_t sensorPresent;     // bitmask of valid channels     2
    uint16_t qualityFlags;      // QF_DROPPED etc.               2
    uint16_t configVersion;     //                               2
    uint8_t  _pad[2];           //                               2
    float    batVoltage;        // V                             4
    float    airTemp;           // °C                            4
    float    airHumidity;       // % RH                          4
    float    spectral[8];       // raw counts, 415–680 nm       32
    float    windSpeed;         // m/s                           4
    float    windDir;           // degrees 0–360                 4
    float    soil1Vwc;          // m³/m³                         4
    float    soil1Temp;         // °C                            4
    float    soil2Vwc;          //                               4
    float    soil2Temp;         //                               4
    float    aux1;              //                               4
    float    aux2;              //                               4
} node_snapshot_t;
// Total: 124 bytes
static_assert(sizeof(node_snapshot_t) == 124, "node_snapshot_t size mismatch");
static_assert(sizeof(pairing_command_t) == 52, "pairing_command_t size mismatch");
static_assert(sizeof(config_snapshot_message_t) == 52, "config_snapshot_message_t size mismatch");
static_assert(sizeof(deployment_command_t) == 88, "deployment_command_t size mismatch");
static_assert(sizeof(unpair_command_t) == 48, "unpair_command_t size mismatch");
static_assert(sizeof(node_config_message_t) == 60, "node_config_message_t size mismatch");
static_assert(sizeof(time_sync_response_t) == 56, "time_sync_response_t size mismatch");
static_assert(sizeof(config_apply_ack_message_t) == 40, "config_apply_ack_message_t size mismatch");
static_assert(sizeof(snapshot_ack_t) == 40, "snapshot_ack_t size mismatch");
static_assert(sizeof(sync_session_open_message_t) == 40, "sync_session_open_message_t size mismatch");
static_assert(sizeof(dump_grant_message_t) == 44, "dump_grant_message_t size mismatch");
static_assert(sizeof(dump_done_message_t) == 42, "dump_done_message_t size mismatch");
static_assert(sizeof(sync_release_message_t) == 48, "sync_release_message_t size mismatch");
static_assert(sizeof(sync_release_ack_message_t) == 40, "sync_release_ack_message_t size mismatch");
#define SENSOR_ID_AUX2          3002

// Maximum number of sensor readings in a V2 snapshot.
// ESP-NOW limit is 250 bytes. V2 header is 48 bytes.
// (250 - 48) / 6 = 33.67, so max 33 readings.
#define MAX_READINGS_PER_SNAPSHOT 33

// V2 key-value reading entry — must be packed to ensure 6-byte wire size.
typedef struct __attribute__((packed)) v2_reading {
    uint16_t sensorId;    // SENSOR_ID_* constant
    float    value;       // sensor reading
} v2_reading_t;
static_assert(sizeof(v2_reading_t) == 6, "v2_reading_t must be 6 bytes (packed)");

// V2 snapshot header — variable-length packet.
// Wire format: header (48 bytes) + sensorCount × v2_reading_t (6 bytes each)
// Command string "NODE_SNAPSHOT2" distinguishes from V1 "NODE_SNAPSHOT".
typedef struct __attribute__((packed)) node_snapshot_v2 {
    char     command[16];       // "NODE_SNAPSHOT2"              16
    char     nodeId[16];        // e.g. "ENV_6C0AA0"             16
    uint32_t nodeTimestamp;     // node RTC unix at capture       4
    uint32_t seqNum;            // snapshot sequence number       4
    uint16_t sensorCount;       // number of v2_reading_t entries 2
    uint16_t qualityFlags;      // QF_DROPPED etc.                2
    uint16_t configVersion;     // node config version            2
    uint8_t  protocolVersion;   // 2                              1
    uint8_t  reserved;          // padding                        1
    // Body follows: v2_reading_t readings[sensorCount]
} node_snapshot_v2_t;
static_assert(sizeof(node_snapshot_v2_t) == 48, "node_snapshot_v2_t header must be 48 bytes");

#define NODE_SNAPSHOT_V2_DEFINED
#define V2_READING_DEFINED

// Calculate total wire size for a V2 snapshot with sensorCount readings.
inline size_t snapshotV2WireSize(uint16_t sensorCount) {
    return sizeof(node_snapshot_v2_t) + (size_t)sensorCount * sizeof(v2_reading_t);
}

// Detection helpers — distinguish V1 and V2 snapshots by command string.
inline bool isV1Snapshot(const uint8_t* data, int len) {
    if (!data || len != sizeof(node_snapshot_t)) return false;
    return strncmp((const char*)data, "NODE_SNAPSHOT", 16) == 0;
}

inline bool isV2Snapshot(const uint8_t* data, int len) {
    if (!data || len < (int)sizeof(node_snapshot_v2_t)) return false;
    if (strncmp((const char*)data, "NODE_SNAPSHOT2", 16) != 0) return false;
    // Validate length matches sensorCount
    const node_snapshot_v2_t* hdr = (const node_snapshot_v2_t*)data;
    if (hdr->sensorCount > MAX_READINGS_PER_SNAPSHOT) return false;
    return len == (int)snapshotV2WireSize(hdr->sensorCount);
}

#define ESPNOW_CHANNEL 11

// I2C pins (ESP32-C3 Mini) — used by node builds
#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 8
#endif

#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 9
#endif

#ifndef RTC_INT_PIN
#define RTC_INT_PIN 4
#endif


// NEW: mux configuration (PCA9548A-style)
// Change these two if you move to a different mux / address later.
#ifndef MUX_ADDR
#define MUX_ADDR      0x70
#endif

#ifndef MUX_CHANNELS
#define MUX_CHANNELS  8       // set to 4 when you move to a 4-channel mux
#endif
