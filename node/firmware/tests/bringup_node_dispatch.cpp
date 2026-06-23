#include <Arduino.h>
#include "protocol.h"
#include "message_dispatch.h"

#ifndef PWR_HOLD_PIN
#define PWR_HOLD_PIN 23
#endif

#ifndef PWR_HOLD_ACTIVE_HIGH
#define PWR_HOLD_ACTIVE_HIGH 1
#endif

static bool g_pass = true;

static void report(const char* name, bool ok) {
  Serial.printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) g_pass = false;
}

template <typename T>
static void terminate(T& msg) {
  msg.command[sizeof(msg.command) - 1] = '\0';
}

void setup() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);

  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node packet dispatch production-helper test ===");

  Serial.printf("sizeof(node_snapshot_t)=%u\n", (unsigned)sizeof(node_snapshot_t));
  Serial.printf("sizeof(pairing_command_t)=%u\n", (unsigned)sizeof(pairing_command_t));
  Serial.printf("sizeof(config_snapshot_message_t)=%u\n", (unsigned)sizeof(config_snapshot_message_t));
  Serial.printf("sizeof(deployment_command_t)=%u\n", (unsigned)sizeof(deployment_command_t));
  Serial.printf("sizeof(unpair_command_t)=%u\n", (unsigned)sizeof(unpair_command_t));
  Serial.printf("sizeof(time_sync_response_t)=%u\n", (unsigned)sizeof(time_sync_response_t));
  Serial.printf("sizeof(config_apply_ack_message_t)=%u\n", (unsigned)sizeof(config_apply_ack_message_t));

  pairing_command_t pair{};
  strncpy(pair.command, "PAIR_NODE", sizeof(pair.command) - 1);
  strncpy(pair.nodeId, "ENV_TEST", sizeof(pair.nodeId) - 1);
  strncpy(pair.mothership_id, "M001", sizeof(pair.mothership_id) - 1);

  config_snapshot_message_t cfg{};
  strncpy(cfg.command, "CONFIG_SNAPSHOT", sizeof(cfg.command) - 1);
  strncpy(cfg.mothership_id, "M001", sizeof(cfg.mothership_id) - 1);
  cfg.configVersion = 7;
  cfg.wakeIntervalMin = 5;
  cfg.syncIntervalMin = 15;
  cfg.syncPhaseUnix = 1800000000UL;

  report("PAIR_NODE is 52 bytes", sizeof(pairing_command_t) == 52);
  report("CONFIG_SNAPSHOT is 52 bytes", sizeof(config_snapshot_message_t) == 52);
  report("PAIR_NODE classified command-first",
         classifyIncomingMessage(reinterpret_cast<uint8_t*>(&pair), sizeof(pair)) ==
             IncomingMessageType::PAIR_NODE);
  report("CONFIG_SNAPSHOT classified command-first",
         classifyIncomingMessage(reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg)) ==
             IncomingMessageType::CONFIG_SNAPSHOT);

  pairing_command_t badPair = pair;
  memset(badPair.command, 'X', sizeof(badPair.command));
  report("unterminated command rejected",
         classifyIncomingMessage(reinterpret_cast<uint8_t*>(&badPair), sizeof(badPair)) ==
             IncomingMessageType::INVALID);

  report("truncated CONFIG_SNAPSHOT rejected",
         classifyIncomingMessage(reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg) - 1) ==
             IncomingMessageType::INVALID);

  uint8_t oversized[sizeof(config_snapshot_message_t) + 1] = {0};
  memcpy(oversized, &cfg, sizeof(cfg));
  report("oversized CONFIG_SNAPSHOT rejected",
         classifyIncomingMessage(oversized, sizeof(oversized)) == IncomingMessageType::INVALID);

  report("target node accepted",
         incomingMessageHasValidTarget(IncomingMessageType::PAIR_NODE,
                                       reinterpret_cast<uint8_t*>(&pair),
                                       sizeof(pair),
                                       "ENV_TEST"));
  report("wrong target rejected",
         !incomingMessageHasValidTarget(IncomingMessageType::PAIR_NODE,
                                        reinterpret_cast<uint8_t*>(&pair),
                                        sizeof(pair),
                                        "ENV_OTHER"));

  Serial.printf("RESULT: %s\n", g_pass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.printf("[dispatch] idle result=%s\n", g_pass ? "PASS" : "FAIL");
  }
  delay(250);
}

