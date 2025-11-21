#pragma once
#include <Arduino.h>

// ===== Wire protocol messages (single source of truth) =====

// Sensor -> Mothership data
typedef struct sensor_data_message {
    char nodeId[16];            // e.g., "TEMP_001"
    char sensorType[16];        // e.g., "TEMPERATURE"
    float value;                // reading
    unsigned long nodeTimestamp;// node-side timestamp (unix or millis)
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
    char mothership_id[16];
} deployment_command_t;

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

#define NODE_TYPE_AIR_TEMP      "AIR_TEMP"
#define NODE_TYPE_SOIL_MOISTURE "SOIL_MOIST"
#define NODE_TYPE_HUMIDITY      "HUMIDITY"
#define NODE_TYPE_LIGHT         "LIGHT"
#define NODE_TYPE_PH            "PH"

#define ESPNOW_CHANNEL 1

// I2C pins (ESP32-C3 Mini) — used by node builds
#define RTC_SDA_PIN 8
#define RTC_SCL_PIN 9
#define RTC_INT_PIN 4
