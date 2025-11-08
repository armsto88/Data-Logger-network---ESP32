#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_wifi.h>

// Node States
enum NodeState {
    STATE_UNPAIRED = 0,
    STATE_PAIRED = 1,
    STATE_DEPLOYED = 2
};

// Protocol definitions (must match mothership)
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
    char command[20];       // "DISCOVER_RESPONSE" or "DISCOVERY_SCAN" - increased size
    char mothership_id[16];
    bool acknowledged;
} discovery_response_t;

typedef struct pairing_request {
    char command[20];       // "PAIRING_REQUEST"
    char nodeId[16];
} pairing_request_t;

typedef struct pairing_response {
    char command[20];       // "PAIRING_RESPONSE"
    char nodeId[16];
    bool isPaired;
    char mothership_id[16];
} pairing_response_t;

// Remote unpair command struct (must match mothership)
typedef struct unpair_command {
    char command[16]; // "UNPAIR_NODE"
    char mothership_id[16];
} unpair_command_t;

typedef struct deployment_command {
    char command[20];       // "DEPLOY_NODE" - increased size
    char nodeId[16];
    unsigned long year;
    unsigned long month;
    unsigned long day;
    unsigned long hour;
    unsigned long minute;
    unsigned long second;
    char mothership_id[16];
} deployment_command_t;

#define RTC_SDA_PIN 8
#define RTC_SCL_PIN 9

// Node Configuration
#define NODE_ID "TEMP_001"
#define NODE_TYPE "temperature"

// Node state persistence
RTC_DATA_ATTR NodeState nodeState = STATE_UNPAIRED;
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool rtcSynced = false;
RTC_DATA_ATTR uint8_t mothershipMAC[6] = {0x30, 0xED, 0xA0, 0xAA, 0x67, 0x84}; // Preloaded mothership MAC

// RTC setup
RTC_DS3231 rtc;

// ESP-NOW peer info
esp_now_peer_info_t peerInfo;
bool waitingForTimeSync = false;

// Function prototypes
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len);
void sendDiscoveryRequest();
void sendSensorData();

// Callback when data is sent
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.print("Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

// Callback when data is received
void onDataReceived(const uint8_t *mac, const uint8_t *incomingData, int len) {
    Serial.print("📨 ESP-NOW message received, length: ");
    Serial.println(len);
    Serial.print("Expected discovery_response size: ");
    Serial.println(sizeof(discovery_response_t));
    Serial.print("Expected pairing_response size: ");
    Serial.println(sizeof(pairing_response_t));
    
    // Handle discovery response
    if (len == sizeof(discovery_response_t)) {
        discovery_response_t response;
        memcpy(&response, incomingData, sizeof(response));
        
        if (strcmp(response.command, "DISCOVER_RESPONSE") == 0) {
            Serial.print("📡 Discovered by mothership: ");
            Serial.println(response.mothership_id);
            
            // Store mothership MAC
            memcpy(mothershipMAC, mac, 6);
            
            // Add mothership as peer for receiving pairing commands
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, mac, 6);
            peerInfo.channel = 1;
            peerInfo.ifidx = WIFI_IF_STA;
            peerInfo.encrypt = false;
            
            // Remove peer if it already exists, then add
            esp_now_del_peer(mac);
            if (esp_now_add_peer(&peerInfo) == ESP_OK) {
                Serial.println("✅ Mothership added as peer for pairing");
            } else {
                Serial.println("❌ Failed to add mothership as peer");
            }
            
        } else if (strcmp(response.command, "DISCOVERY_SCAN") == 0) {
            // Mothership is scanning for nodes - respond if unpaired
            if (nodeState == STATE_UNPAIRED) {
                Serial.println("🔍 Responding to discovery scan...");
                sendDiscoveryRequest();
            }
        }
    }
    // Handle pairing response
    else if (len == sizeof(pairing_response_t)) {
        pairing_response_t pairResp;
        memcpy(&pairResp, incomingData, sizeof(pairResp));
        
        if (strcmp(pairResp.command, "PAIRING_RESPONSE") == 0 && 
            strcmp(pairResp.nodeId, NODE_ID) == 0) {
            if (pairResp.isPaired) {
                Serial.println("📋 Pairing confirmed!");
                nodeState = STATE_PAIRED;
                memcpy(mothershipMAC, mac, 6);
            } else {
                Serial.println("📋 Still unpaired, continuing discovery...");
            }
        }
    }
    // Handle deployment command
    else if (len == sizeof(deployment_command_t)) {
        deployment_command_t deployCmd;
        memcpy(&deployCmd, incomingData, sizeof(deployCmd));
        
        if (strcmp(deployCmd.command, "DEPLOY_NODE") == 0 && 
            strcmp(deployCmd.nodeId, NODE_ID) == 0) {
            Serial.println("🚀 Deployment command received!");
            // Set RTC time
            rtc.adjust(DateTime(deployCmd.year, deployCmd.month, deployCmd.day, 
                              deployCmd.hour, deployCmd.minute, deployCmd.second));
            rtcSynced = true;
            nodeState = STATE_DEPLOYED;
            memcpy(mothershipMAC, mac, 6);
            Serial.print("RTC synchronized to: ");
            Serial.println(rtc.now().timestamp());
            Serial.println("✅ Node deployed! Starting data collection...");
        }
    }
    // Handle remote unpair command
    else if (len == sizeof(unpair_command_t)) {
        // lightweight struct for command
        struct unpair_command_t_local { char command[16]; char mothership_id[16]; } uc;
        memcpy(&uc, incomingData, sizeof(uc));
        if (strcmp(uc.command, "UNPAIR_NODE") == 0) {
            Serial.println("🗑️ Received UNPAIR from mothership");
            // Clear stored mothership MAC and mark unpaired
            memset(mothershipMAC, 0, sizeof(mothershipMAC));
            nodeState = STATE_UNPAIRED;
            // Optionally, trigger discovery immediately
            sendDiscoveryRequest();
        }
    }
}

// Send discovery request to find motherships
void sendDiscoveryRequest() {
    // Broadcast discovery message
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    discovery_message_t discovery;
    strcpy(discovery.nodeId, NODE_ID);
    strcpy(discovery.nodeType, NODE_TYPE);
    strcpy(discovery.command, "DISCOVER_REQUEST");
    discovery.timestamp = millis();
    
    // Ensure we are on the fixed pairing channel
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&discovery, sizeof(discovery));
    
    if (result == ESP_OK) {
        Serial.println("📡 Discovery request sent");
    } else {
        Serial.println("❌ Discovery request failed");
    }
}

// Send pairing status request to mothership
void sendPairingRequest() {
    // Use broadcast like discovery for testing
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    pairing_request_t pairingReq;
    strcpy(pairingReq.command, "PAIRING_REQUEST");
    strcpy(pairingReq.nodeId, NODE_ID);
    
    // Ensure we are on the fixed pairing channel
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&pairingReq, sizeof(pairingReq));
    
    if (result == ESP_OK) {
        Serial.println("📋 Pairing status request sent");
    } else {
        Serial.println("❌ Pairing request failed");
    }
}

// Send sensor data to mothership
void sendSensorData() {
    if (nodeState != STATE_DEPLOYED) {
        Serial.println("⚠️ Node not deployed - skipping data collection");
        return;
    }
    
    // Simulate temperature reading (replace with actual sensor)
    float temperature = 20.0 + (random(0, 200) / 10.0); // 20.0 to 40.0°C
    
    sensor_data_message_t sensorMsg;
    strcpy(sensorMsg.nodeId, NODE_ID);
    strcpy(sensorMsg.sensorType, NODE_TYPE);
    sensorMsg.value = temperature;
    sensorMsg.nodeTimestamp = rtc.now().unixtime();
    
    // Ensure sending on pairing/comm channel
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_err_t result = esp_now_send(mothershipMAC, (uint8_t*)&sensorMsg, sizeof(sensorMsg));
    
    if (result == ESP_OK) {
        Serial.print("📊 Sensor data sent: ");
        Serial.print(temperature);
        Serial.println("°C");
    } else {
        Serial.println("❌ Failed to send sensor data");
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000); // Important delay for ESP32-C3 Hardware CDC to initialize
    bootCount++;
    
    Serial.println("====================================");
    Serial.print("🌡️ Air Temperature Node: ");
    Serial.println(NODE_ID);
    Serial.print("Boot #");
    Serial.println(bootCount);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.print("State: ");
    if (nodeState == STATE_UNPAIRED) Serial.println("UNPAIRED");
    else if (nodeState == STATE_PAIRED) Serial.println("PAIRED");
    else if (nodeState == STATE_DEPLOYED) Serial.println("DEPLOYED");
    Serial.println("====================================");
    
    // Initialize I2C for RTC
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    
    // Initialize RTC
    if (!rtc.begin()) {
        Serial.println("❌ RTC not found!");
    } else {
        Serial.println("✅ RTC initialized");
        if (rtcSynced) {
            DateTime now = rtc.now();
            Serial.print("RTC Time: ");
            Serial.println(now.timestamp());
        } else {
            Serial.println("RTC not synchronized yet");
        }
    }
    
    // Initialize WiFi
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(1000); // Increased delay for WiFi stability
    
    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW initialization failed");
        return;
    }
    
    Serial.println("✅ ESP-NOW initialized");
    
    // Register callbacks
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);
    
    // Add broadcast peer for discovery
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            memcpy(peerInfo.peer_addr, broadcastAddress, 6);
            peerInfo.channel = 1; // Force channel 1
            peerInfo.ifidx = WIFI_IF_STA;
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
    
    // Add preloaded mothership as peer for reliable communication
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, mothershipMAC, 6);
    peerInfo.channel = 1; // Force channel 1
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;
    
    // Remove peer if it already exists, then add
    esp_now_del_peer(mothershipMAC);
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
        Serial.print("✅ Preloaded mothership peer: ");
        for (int i = 0; i < 6; i++) {
            if (i > 0) Serial.print(":");
            Serial.printf("%02X", mothershipMAC[i]);
        }
        Serial.println();
    } else {
        Serial.println("❌ Failed to add mothership as peer");
    }
}

void loop() {
    static unsigned long lastAction = 0;
    unsigned long currentTime = millis();
    
    // Handle different states
    if (nodeState == STATE_UNPAIRED) {
        // Send discovery request every 10 seconds when unpaired
        if (currentTime - lastAction > 10000) {
            Serial.println("🔍 Searching for motherships...");
            sendDiscoveryRequest();
            
            // Also poll for pairing status to check if we've been paired
            delay(1000);  // Small delay after discovery
            Serial.println("📞 Polling pairing status...");
            sendPairingRequest();
            
            lastAction = currentTime;
        }
    }
    else if (nodeState == STATE_PAIRED) {
        // Wait for deployment - just listen for commands
        if (currentTime - lastAction > 5000) {
            Serial.println("🟡 Paired - not deployed. Not transmitting data. Waiting for deployment command from mothership...");
            lastAction = currentTime;
        }
    }
    else if (nodeState == STATE_DEPLOYED) {
        // Send sensor data every 30 seconds (fixed, no schedule/interval logic)
        if (currentTime - lastAction > 30000) {
            Serial.println("🟢 Deployed - transmitting sensor data to mothership...");
            sendSensorData();
            lastAction = currentTime;
            // In production, would enter deep sleep here:
            // Serial.println("😴 Entering deep sleep...");
            // esp_deep_sleep(30 * 1000000ULL); // 30 seconds
        }
    }
    
    delay(100); // Small delay to prevent excessive CPU usage
}