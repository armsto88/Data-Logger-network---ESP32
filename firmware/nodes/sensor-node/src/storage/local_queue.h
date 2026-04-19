#pragma once

#include <Arduino.h>

namespace local_queue {

static constexpr uint16_t QF_DROPPED = 0x0001;

struct QueuedSample {
  uint32_t sampleSeq;
  uint32_t sampleUnix;
  uint16_t sensorId;
  char sensorType[16];
  char sensorLabel[24];
  float value;
  uint16_t qualityFlags;
};

bool begin();
bool enqueue(uint32_t sampleUnix,
             uint16_t sensorId,
             const char* sensorType,
             const char* sensorLabel,
             float value,
             uint16_t qualityFlags = 0);
bool peek(QueuedSample& out);
bool pop();
uint16_t count();
uint32_t nextSeq();
void clear();

}  // namespace local_queue
