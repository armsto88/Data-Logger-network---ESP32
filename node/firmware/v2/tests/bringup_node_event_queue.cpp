#include <Arduino.h>
#include "protocol.h"
#include "message_dispatch.h"
#include "node_event_queue.h"

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

void setup() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, PWR_HOLD_ACTIVE_HIGH ? HIGH : LOW);

  Serial.begin(115200);
  delay(1500);
  Serial.println("=== Node production event queue test ===");

  resetNodeEventQueueForTest();
  report("queue init", initNodeEventQueue(2));

  uint8_t macA[6] = {1, 2, 3, 4, 5, 6};
  uint8_t macB[6] = {6, 5, 4, 3, 2, 1};

  time_sync_response_t ts{};
  strncpy(ts.command, "TIME_SYNC", sizeof(ts.command) - 1);
  strncpy(ts.mothership_id, "M001", sizeof(ts.mothership_id) - 1);
  ts.year = 2026;
  ts.month = 6;
  ts.day = 23;
  ts.hour = 12;
  ts.minute = 0;
  ts.second = 0;

  config_snapshot_message_t cfg{};
  strncpy(cfg.command, "CONFIG_SNAPSHOT", sizeof(cfg.command) - 1);
  strncpy(cfg.mothership_id, "M001", sizeof(cfg.mothership_id) - 1);
  cfg.configVersion = 3;
  cfg.wakeIntervalMin = 5;
  cfg.syncIntervalMin = 15;
  cfg.syncPhaseUnix = 1800000000UL;

  report("enqueue TIME_SYNC",
         enqueueValidatedNodeEvent(macA, IncomingMessageType::TIME_SYNC,
                                   reinterpret_cast<uint8_t*>(&ts), sizeof(ts), 111));
  report("enqueue CONFIG_SNAPSHOT",
         enqueueValidatedNodeEvent(macB, IncomingMessageType::CONFIG_SNAPSHOT,
                                   reinterpret_cast<uint8_t*>(&cfg), sizeof(cfg), 222));

  snapshot_ack_t ack{};
  strncpy(ack.command, "SNAPSHOT_ACK", sizeof(ack.command) - 1);
  strncpy(ack.nodeId, "ENV_TEST", sizeof(ack.nodeId) - 1);
  ack.seqNum = 9;
  ack.persisted = 1;
  ack.protocolVersion = NODE_PROTOCOL_VERSION;
  report("overflow is rejected",
         !enqueueValidatedNodeEvent(macA, IncomingMessageType::SNAPSHOT_ACK,
                                    reinterpret_cast<uint8_t*>(&ack), sizeof(ack), 333));

  NodeEvent first{};
  NodeEvent second{};
  report("pop first", popNodeEvent(first));
  report("first ordering is TIME_SYNC", first.type == NodeEventType::TIME_SYNC && first.receivedMs == 111);
  report("pop second", popNodeEvent(second));
  report("second ordering is CONFIG_SNAPSHOT",
         second.type == NodeEventType::CONFIG_SNAPSHOT && second.receivedMs == 222);
  report("queue empty", !nodeEventsPending());

  NodeEventCounters c = getNodeEventCounters();
  report("received counter", c.callbackEventsReceived == 2);
  report("drop counter", c.callbackEventsDropped == 1);

  resetNodeEventQueueForTest();
  report("session queue re-init", initNodeEventQueue(3));
  sync_session_open_message_t session{};
  strncpy(session.command, "SYNC_SESSION", sizeof(session.command) - 1);
  strncpy(session.mothership_id, "M001", sizeof(session.mothership_id) - 1);
  session.sessionId = 44;
  dump_grant_message_t grant{};
  strncpy(grant.command, "DUMP_GRANT", sizeof(grant.command) - 1);
  strncpy(grant.nodeId, "ENV_TEST", sizeof(grant.nodeId) - 1);
  grant.sessionId = 44;
  grant.grantId = 7;
  report("enqueue SYNC_SESSION",
         enqueueValidatedNodeEvent(macA, IncomingMessageType::SYNC_SESSION,
                                   reinterpret_cast<uint8_t*>(&session), sizeof(session), 444));
  report("enqueue DUMP_GRANT",
         enqueueValidatedNodeEvent(macA, IncomingMessageType::DUMP_GRANT,
                                   reinterpret_cast<uint8_t*>(&grant), sizeof(grant), 555));
  NodeEvent sessionEvent{};
  NodeEvent grantEvent{};
  report("pop SYNC_SESSION",
         popNodeEvent(sessionEvent) && sessionEvent.type == NodeEventType::SYNC_SESSION &&
         sessionEvent.payload.syncSession.sessionId == 44);
  report("pop DUMP_GRANT",
         popNodeEvent(grantEvent) && grantEvent.type == NodeEventType::DUMP_GRANT &&
         grantEvent.payload.dumpGrant.grantId == 7);

  Serial.printf("RESULT: %s\n", g_pass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.printf("[event-queue] idle result=%s\n", g_pass ? "PASS" : "FAIL");
  }
  delay(250);
}
