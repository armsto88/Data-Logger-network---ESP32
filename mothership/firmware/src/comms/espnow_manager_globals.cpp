#include <Arduino.h>
#include "espnow_manager.h"

// Define known sensor nodes once here (edit to your nodes)
const uint8_t KNOWN_SENSOR_NODES[][6] = {
  {0x94, 0xA9, 0x90, 0x96, 0xD9, 0xFC},  // TEMP_001
  // {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF},
};

const int NUM_KNOWN_SENSORS =
  sizeof(KNOWN_SENSOR_NODES) / sizeof(KNOWN_SENSOR_NODES[0]);
