#pragma once

// Message structures for ESP-NOW communication between nodes and mothership

// Structure for sensor data sent from nodes to mothership
typedef struct sensor_data_message {
    char nodeId[16];         // e.g., "TEMP_001", "SOIL_001"
    char sensorType[16];     // e.g., "TEMPERATURE", "HUMIDITY", "SOIL_MOISTURE"
    float value;             // Sensor reading value
    unsigned long nodeTimestamp; // Node's internal timestamp
} sensor_data_message_t;

// Structure for schedule commands sent from mothership to nodes
typedef struct schedule_command_message {
    char command[16];        // "SET_SCHEDULE"
    int intervalMinutes;     // Wake interval in minutes
    char mothership_id[16];  // "MOTHERSHIP001"
} schedule_command_message_t;

// Structure for time sync requests from nodes to mothership
typedef struct time_sync_request {
    char nodeId[16];         // Node requesting time sync
    char command[16];        // "REQUEST_TIME"
    unsigned long requestTime; // Node's current time for offset calculation
} time_sync_request_t;

// Structure for time sync response from mothership to nodes
typedef struct time_sync_response {
    char command[16];        // "TIME_SYNC"
    unsigned long year;
    unsigned long month;
    unsigned long day;
    unsigned long hour;
    unsigned long minute;
    unsigned long second;
    char mothership_id[16];  // "MOTHERSHIP001"
} time_sync_response_t;

// Default configuration values
#define DEFAULT_WAKE_INTERVAL_MINUTES 5
#define DEFAULT_SLEEP_TIME_SECONDS (DEFAULT_WAKE_INTERVAL_MINUTES * 60)

// Node types for identification
#define NODE_TYPE_AIR_TEMP "AIR_TEMP"
#define NODE_TYPE_SOIL_MOISTURE "SOIL_MOIST" 
#define NODE_TYPE_HUMIDITY "HUMIDITY"
#define NODE_TYPE_LIGHT "LIGHT"
#define NODE_TYPE_PH "PH"

// Common ESP-NOW channel
#define ESPNOW_CHANNEL 1

// I2C pins for ESP32-C3 Mini
#define RTC_SDA_PIN 8
#define RTC_SCL_PIN 9
#define RTC_INT_PIN 3