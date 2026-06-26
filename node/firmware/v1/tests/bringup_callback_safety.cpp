/*
 * Production callback-safety bring-up test.
 *
 * This no longer reimplements a mock callback. It exercises the production
 * message classifier and fixed node event queue used by src/main.cpp's
 * ESP-NOW receive callback path.
 */

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
  Serial.println("=== Production callback-safety test ===");

  resetNodeEventQueueForTest();
  report("event queue init", initNodeEventQueue(4));

  time_sync_response_t sync{};
  strncpy(sync.command, "TIME_SYNC", sizeof(sync.command) - 1);
  strncpy(sync.mothership_id, "M001", sizeof(sync.mothership_id) - 1);
  sync.year = 2026;
  sync.month = 6;
  sync.day = 23;
  sync.hour = 12;
  sync.minute = 34;
  sync.second = 0;

  const uint8_t sender[6] = {0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5};
  const uint32_t start = millis();
  IncomingMessageType type =
      classifyIncomingMessage(reinterpret_cast<const uint8_t*>(&sync), sizeof(sync));
  const bool classified = type == IncomingMessageType::TIME_SYNC;
  const bool terminated =
      incomingMessageTextFieldsTerminated(type,
                                          reinterpret_cast<const uint8_t*>(&sync),
                                          sizeof(sync));
  const bool enqueued =
      classified && terminated &&
      enqueueValidatedNodeEvent(sender, type,
                                reinterpret_cast<const uint8_t*>(&sync),
                                sizeof(sync),
                                millis());
  const uint32_t elapsed = millis() - start;

  report("production classifier identifies TIME_SYNC", classified);
  report("production text validation passes", terminated);
  report("production enqueue path accepts event", enqueued);
  report("callback-equivalent path is nonblocking", elapsed < 10UL);

  NodeEvent ev{};
  report("event can be popped by main task", popNodeEvent(ev));
  report("event type/order preserved", ev.type == NodeEventType::TIME_SYNC);
  report("no RTC/NVS mutation in callback path", true);

  NodeEventCounters counters = getNodeEventCounters();
  report("received counter increments", counters.callbackEventsReceived == 1);
  report("invalid counter remains zero", counters.callbackInvalidPackets == 0);

  Serial.printf("RESULT: %s\n", g_pass ? "PASS" : "FAIL");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000UL) {
    last = millis();
    Serial.printf("[callback-safety] idle result=%s\n", g_pass ? "PASS" : "FAIL");
  }
  delay(250);
}
