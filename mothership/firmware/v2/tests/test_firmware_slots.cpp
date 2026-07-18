// On-device assertion test for the A/B firmware-slot JSON shaping.
//
// Exercises mothershipFwSlotsJson() with fixture OtaSlotInfo rows (no esp_ota
// dependency), so the exact JSON contract the backend consumes is verified
// deterministically. The live esp_ota partition reads (readOtaSlots) are
// validated separately on the real device via the status upload.
//
//   pio run -e mothership-v2-test-firmware-slots -t upload && pio device monitor
//
#include <Arduino.h>
#include "ota/mothership_selfupdate.h"

static int failures = 0;

static void expectContains(const String& hay, const char* needle, const char* label) {
  if (hay.indexOf(needle) < 0) {
    Serial.printf("FAIL %-22s : missing %s\n", label, needle);
    failures++;
  } else {
    Serial.printf("ok   %-22s\n", label);
  }
}

static void expectEquals(const String& got, const char* want, const char* label) {
  if (got != want) {
    Serial.printf("FAIL %-22s : got '%s' want '%s'\n", label, got.c_str(), want);
    failures++;
  } else {
    Serial.printf("ok   %-22s\n", label);
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n[TEST] firmware A/B slots JSON");

  // Fixture: app0 running+confirmed+nextBoot, app1 holds a staged image.
  OtaSlotInfo slots[2] = {};
  strncpy(slots[0].label,   "app0",    sizeof(slots[0].label)   - 1);
  strncpy(slots[0].version, "0.1.0",   sizeof(slots[0].version) - 1);
  strncpy(slots[0].buildId, "5876faf", sizeof(slots[0].buildId) - 1);
  slots[0].state = "CONFIRMED"; slots[0].active = true;  slots[0].nextBoot = true;  slots[0].present = true;

  strncpy(slots[1].label,   "app1",  sizeof(slots[1].label)   - 1);
  strncpy(slots[1].version, "0.2.0", sizeof(slots[1].version) - 1);
  slots[1].buildId[0] = '\0';  // inactive-slot buildId not yet known
  slots[1].state = "IDLE"; slots[1].active = false; slots[1].nextBoot = false; slots[1].present = true;

  const String j = mothershipFwSlotsJson(slots, 2);
  Serial.println(j);

  expectContains(j, "\"label\":\"app0\"",     "app0 label");
  expectContains(j, "\"version\":\"0.1.0\"",  "app0 version");
  expectContains(j, "\"buildId\":\"5876faf\"","app0 buildId");
  expectContains(j, "\"state\":\"CONFIRMED\"","app0 state");
  expectContains(j, "\"active\":true",        "app0 active");
  expectContains(j, "\"nextBoot\":true",      "app0 nextBoot");
  expectContains(j, "\"present\":true",       "app0 present");
  expectContains(j, "\"label\":\"app1\"",     "app1 label");
  expectContains(j, "\"version\":\"0.2.0\"",  "app1 version");
  expectContains(j, "\"buildId\":\"\"",       "app1 empty buildId");
  expectContains(j, "\"active\":false",       "app1 inactive");

  // Empty fleet / no slots -> valid empty array.
  expectEquals(mothershipFwSlotsJson(nullptr, 0), "[]", "empty array");

  // Single EMPTY slot (freshly-flashed device, nothing staged).
  OtaSlotInfo one[1] = {};
  strncpy(one[0].label, "app1", sizeof(one[0].label) - 1);
  one[0].state = "EMPTY"; one[0].active = false; one[0].present = false;
  const String je = mothershipFwSlotsJson(one, 1);
  expectContains(je, "\"state\":\"EMPTY\"",  "empty slot state");
  expectContains(je, "\"present\":false",    "empty slot present");

  Serial.printf("\n[TEST] %s (failures=%d)\n", failures == 0 ? "PASS" : "FAIL", failures);
}

void loop() {}
