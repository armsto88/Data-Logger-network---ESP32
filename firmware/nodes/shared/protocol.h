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
#define SENSOR_ID_WIND_SPEED    1201
#define SENSOR_ID_WIND_DIR      1202
#define SENSOR_ID_SOIL1_VWC     2001
#define SENSOR_ID_SOIL2_VWC     2002
#define SENSOR_ID_SOIL1_TEMP    2003
#define SENSOR_ID_SOIL2_TEMP    2004
#define SENSOR_ID_BAT_V         4001
#define SENSOR_ID_AUX1          3001
#define SENSOR_ID_AUX2          3002

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
#define SENSOR_ID_AUX2          3002

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
