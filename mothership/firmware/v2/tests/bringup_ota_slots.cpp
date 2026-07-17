// bringup_ota_slots.cpp — OTA app-slot + rollback proof (Phase 0 gate)
//
// Proves the two things the FieldMesh OTA plan (§7, §10.1) depends on, with
// NO SD card, NO network, and NO externally-supplied binary:
//
//   Tier 1 — slot switch: clone the currently-running app image into the
//            INACTIVE ota slot, set it bootable, reboot into it.
//   Tier 2 — rollback:    detect whether the bootloader was built with
//            CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. The stock Arduino
//            bootloader usually is NOT, in which case auto-rollback on a bad
//            boot will NOT happen and a custom bootloader is required.
//
// The clone is a byte-for-byte copy of a KNOWN-GOOD running image, so both
// slots always hold a bootable app. Worst case the device boots a working
// copy from the other slot.
//
// *** otadata trap (see memory: flash-recovery-bootloader) ***
// After this test, otadata may point at app1. A later `pio run -t upload`
// writes app0 but leaves otadata pointing at app1, so the device keeps
// running the OLD app1 image. To recover, use menu 'b' to force boot back to
// app0 BEFORE reflashing, or do a full 4-image esptool flash.
//
// Serial menu @115200:
//   i  - print partition / OTA-state info
//   c  - clone running image -> inactive slot, set boot, reboot
//   v  - mark running image VALID (cancel pending rollback)
//   r  - mark running image INVALID and reboot (manual rollback trigger)
//   b  - force boot partition back to the FIRST ota slot (recovery)

#include <Arduino.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"

static const char* stateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "??";
  }
}

static void printInfo() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot    = esp_ota_get_boot_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);

  Serial.println();
  Serial.println("===== OTA slot info =====");
  if (running) Serial.printf("running : %-8s @ 0x%06X  size 0x%06X\n",
                             running->label, running->address, running->size);
  if (boot)    Serial.printf("boot    : %-8s @ 0x%06X\n", boot->label, boot->address);
  if (next)    Serial.printf("inactive: %-8s @ 0x%06X  size 0x%06X\n",
                             next->label, next->address, next->size);

  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK) {
    Serial.printf("running OTA state: %s\n", stateName(st));
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println(">> Rollback IS enabled: this image is on probation.");
      Serial.println(">> Press 'v' to confirm it, or 'r' to force a rollback.");
    } else if (st == ESP_OTA_IMG_UNDEFINED) {
      Serial.println(">> UNDEFINED = booted via serial flash, or rollback not");
      Serial.println(">> compiled in. Do a clone ('c') then re-check this state:");
      Serial.println(">>   PENDING_VERIFY after clone -> rollback enabled.");
      Serial.println(">>   still UNDEFINED/VALID     -> rollback NOT enabled.");
    }
  } else {
    Serial.println("running OTA state: <unavailable> (no otadata entry yet)");
  }
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  Serial.println("=========================");
  Serial.println("menu: i=info  c=clone+reboot  v=valid  r=rollback  b=boot->slot0");
}

// Copy the whole inactive-slot span from the running partition. Trailing
// erased (0xFF) bytes past the real image are ignored by esp_image verify,
// which reads the true length from the image header.
static void cloneRunningToInactive() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
  if (!running || !next) { Serial.println("[clone] partition lookup failed"); return; }
  if (running->size > next->size) {
    Serial.println("[clone] inactive slot smaller than running image span — abort");
    return;
  }

  Serial.printf("[clone] %s -> %s (0x%06X bytes)...\n",
                running->label, next->label, running->size);

  esp_ota_handle_t h;
  esp_err_t err = esp_ota_begin(next, running->size, &h);
  if (err != ESP_OK) { Serial.printf("[clone] esp_ota_begin: %s\n", esp_err_to_name(err)); return; }

  static uint8_t buf[4096];
  uint32_t off = 0;
  uint32_t lastPct = 0;
  while (off < running->size) {
    size_t chunk = min((uint32_t)sizeof(buf), running->size - off);
    err = esp_partition_read(running, off, buf, chunk);
    if (err != ESP_OK) { Serial.printf("[clone] read @0x%06X: %s\n", off, esp_err_to_name(err)); esp_ota_abort(h); return; }
    err = esp_ota_write(h, buf, chunk);
    if (err != ESP_OK) { Serial.printf("[clone] write @0x%06X: %s\n", off, esp_err_to_name(err)); esp_ota_abort(h); return; }
    off += chunk;
    uint32_t pct = (off * 100) / running->size;
    if (pct >= lastPct + 10) { Serial.printf("  %u%%\n", pct); lastPct = pct; }
    yield();
  }

  err = esp_ota_end(h);   // verifies the written image
  if (err != ESP_OK) { Serial.printf("[clone] esp_ota_end (verify): %s\n", esp_err_to_name(err)); return; }

  err = esp_ota_set_boot_partition(next);
  if (err != ESP_OK) { Serial.printf("[clone] set_boot: %s\n", esp_err_to_name(err)); return; }

  Serial.printf("[clone] OK. boot set to %s. rebooting in 2s...\n", next->label);
  delay(2000);
  esp_restart();
}

static void markValid() {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("[valid] esp_ota_mark_app_valid_cancel_rollback: %s\n", esp_err_to_name(err));
}

static void markInvalidAndReboot() {
  Serial.println("[rollback] marking image INVALID and rebooting...");
  Serial.println("[rollback] if this returns an error, rollback is NOT supported.");
  delay(500);
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only reached if rollback is not supported / no valid previous app.
  Serial.printf("[rollback] did NOT reboot: %s\n", esp_err_to_name(err));
}

static void bootToSlot0() {
  const esp_partition_t* p =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (!p) { Serial.println("[recover] ota_0 not found"); return; }
  esp_err_t err = esp_ota_set_boot_partition(p);
  Serial.printf("[recover] set_boot ota_0(%s): %s. Reflash-safe now.\n",
                p->label, esp_err_to_name(err));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n### FieldMesh OTA slot bring-up test ###");
  printInfo();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  int c = Serial.read();
  switch (c) {
    case 'i': printInfo();              break;
    case 'c': cloneRunningToInactive(); break;
    case 'v': markValid();              break;
    case 'r': markInvalidAndReboot();   break;
    case 'b': bootToSlot0();            break;
    case '\n': case '\r':               break;
    default: Serial.printf("? unknown '%c' — menu: i c v r b\n", (char)c); break;
  }
}
