#pragma once
#include <Arduino.h>
#include <vector>

// Known sensor node MAC addresses for preloading peers
const uint8_t KNOWN_SENSOR_NODES[][6] = {
    {0x94, 0xA9, 0x90, 0x96, 0xD9, 0xFC},  // TEMP_001 - air temperature node
    // Add more sensor node MACs here as needed
};
const int NUM_KNOWN_SENSORS = sizeof(KNOWN_SENSOR_NODES) / sizeof(KNOWN_SENSOR_NODES[0]);

enum NodeState {
    UNPAIRED = 0,
    PAIRED = 1,
    DEPLOYED = 2
};

struct NodeInfo {
    uint8_t mac[6];
    String nodeId;
    String nodeType;  // e.g., "temperature", "humidity", "pressure"
    unsigned long lastSeen;
    bool isActive;
    NodeState state;
    int scheduleInterval;  // minutes between sensor readings
};

// Message structures (matching the node protocol)
typedef struct sensor_data_message {
    char nodeId[16];
    char sensorType[16];
    float value;
    unsigned long nodeTimestamp;
} sensor_data_message_t;

typedef struct discovery_message {
    char nodeId[16];
    char nodeType[16];  // "temperature", "humidity", etc.
    char command[20];   // "DISCOVER_REQUEST" - increased size
    unsigned long timestamp;
} discovery_message_t;

typedef struct discovery_response {
    char command[20];       // "DISCOVER_RESPONSE" - increased size
    char mothership_id[16];
    bool acknowledged;
} discovery_response_t;

typedef struct pairing_command {
    char command[20];       // "PAIR_NODE" - increased size
    char nodeId[16];
    int scheduleInterval;   // minutes
    char mothership_id[16];
} pairing_command_t;

typedef struct deployment_command {
    char command[20];       // "DEPLOY_NODE" - increased size
    char nodeId[16];
    unsigned long year;
    unsigned long month;
    unsigned long day;
    unsigned long hour;
    unsigned long minute;
    unsigned long second;
    int scheduleInterval;
    char mothership_id[16];
} deployment_command_t;

typedef struct time_sync_request {
    char nodeId[16];
    char command[16];
    unsigned long requestTime;
} time_sync_request_t;

typedef struct time_sync_response {
    char command[16];
    unsigned long year;
    unsigned long month;
    unsigned long day;
    unsigned long hour;
    unsigned long minute;
    unsigned long second;
    char mothership_id[16];
} time_sync_response_t;

typedef struct pairing_request {
    char command[20];       // "PAIRING_REQUEST"
    char nodeId[16];
} pairing_request_t;

typedef struct pairing_response {
    char command[20];       // "PAIRING_RESPONSE"
    char nodeId[16];
    bool isPaired;
    int alarmInterval;      // minutes
} pairing_response_t;

void setupESPNOW();
void espnow_loop();
bool broadcastSchedule(int intervalMinutes);
bool sendTimeSync(const uint8_t* mac, const char* nodeId);
bool sendDiscoveryBroadcast();
bool pairNode(const String& nodeId, int scheduleInterval);
bool deploySelectedNodes(const std::vector<String>& nodeIds);
std::vector<NodeInfo> getRegisteredNodes();
std::vector<NodeInfo> getUnpairedNodes();
std::vector<NodeInfo> getPairedNodes();
NodeState getNodeState(const char* nodeId);
String getMothershipsMAC();
void printRegisteredNodes();
