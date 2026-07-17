// bringup_ota_slots.cpp — OTA app-slot + rollback proof (Phase 0 gate)
//
// Runs on a spare/bench NODE (ESP32-WROOM). Proves the two things the
// FieldMesh OTA plan (§7, §10.1) depends on, with NO SD, NO network, and NO
// externally-supplied binary. Because both roles share the same Arduino-ESP32
// bootloader, the rollback result here applies to the mothership too.
//
//   Tier 1 — slot switch: clone the currently-running app image into the
//            INACTIVE ota slot, set it bootable, reboot into it.
//   Tier 2 — rollback:    detect whether the bootloader was built with
//            CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE. The stock Arduino
//            bootloader usually is NOT, in which case auto-rollback on a bad
//            boot will NOT happen and a custom bootloader is required.
//
// Uses the pinned node layout (partitions_ota.csv): two 0x140000 slots. The
// clone is a byte-for-byte copy of a KNOWN-GOOD running image, so both slots
// always hold a bootable app.
//
// *** otadata trap (see memory: flash-recovery-bootloader) ***
// After this test, otadata may point at app1. A later `pio run -t upload` of
// the normal `esp32wroom` env may leave the device running the OLD app1 clone.
// Press 'b' to force boot back to app0 BEFORE reflashing, or full esptool flash.
//
// Serial menu @115200:
//   i  - print partition / OTA-state info
//   c  - clone running image -> inactive slot, set boot, reboot
//   v  - mark running image VALID (cancel pending rollback)
//   r  - mark running image INVALID and reboot (manual rollback trigger)
//   b  - force boot partition back to the FIRST ota slot (recovery)

#include <Arduino.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "firmware_identity.h"

// Opt OUT of the Arduino core's automatic image confirmation. By default
// initArduino() (runs before setup()) auto-calls esp_ota_mark_app_valid on a
// PENDING_VERIFY image, so a naive test always sees VALID. Returning true here
// means "I will verify myself" — the image stays on probation until this app
// explicitly calls esp_ota_mark_app_valid_cancel_rollback() (menu 'v'). If it
// reboots/crashes first (menu 'k'), the bootloader rolls back automatically.
// This is exactly the deferred-verify contract the OTA plan §7.5/§8.5 needs.
extern "C" bool verifyRollbackLater() { return true; }

// NVS marker in its own namespace so it never touches production node keys.
// Proves NVS (pairing/schedule/queue live here too) survives an OTA slot
// switch — plan §7.5/§8.5/§16 "queued readings survive upgrade".
static void printNvsMarker() {
  Preferences prefs;
  prefs.begin("otatest", true);
  bool present = prefs.isKey("marker");
  uint32_t m = prefs.getUInt("marker", 0);
  prefs.end();
  Serial.printf("NVS marker: %u %s\n", m, present ? "(present)" : "(absent — press 'n')");
}

static void writeNvsMarker() {
  Preferences prefs;
  prefs.begin("otatest", false);
  uint32_t m = prefs.getUInt("marker", 0) + 1;
  prefs.putUInt("marker", m);
  prefs.end();
  Serial.printf("[nvs] wrote marker=%u — now 'c' (clone+switch) and check it survives\n", m);
}

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
  fwIdentityPrint(fwIdentity(2 /* NODE_PROTOCOL_VERSION on this bench build */));
  if (running) Serial.printf("running : %-8s @ 0x%06X  size 0x%06X\n",
                             running->label, running->address, running->size);
  if (boot)    Serial.printf("boot    : %-8s @ 0x%06X\n", boot->label, boot->address);
  if (next)    Serial.printf("inactive: %-8s @ 0x%06X  size 0x%06X\n",
                             next->label, next->address, next->size);

  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK) {
    Serial.printf("running OTA state: %s\n", stateName(st));
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println(">> ON PROBATION. Auto-confirm is disabled (verifyRollbackLater).");
      Serial.println(">> 'v' = confirm & keep this image. 'k' = reboot without");
      Serial.println(">> confirming -> bootloader should ROLL BACK to the other slot.");
    } else if (st == ESP_OTA_IMG_UNDEFINED) {
      Serial.println(">> UNDEFINED = booted via serial flash (no OTA seq yet).");
      Serial.println(">> Clone+switch ('c') to boot an OTA image; expect PENDING_VERIFY.");
    } else if (st == ESP_OTA_IMG_VALID) {
      Serial.println(">> VALID = confirmed image (either 'v' was pressed or a prior boot");
      Serial.println(">> confirmed it).");
    }
  } else {
    Serial.println("running OTA state: <unavailable> (no otadata entry yet)");
  }
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  printNvsMarker();
  Serial.println("=========================");
  Serial.println("menu: i=info c=clone+reboot x/y=corrupt-reject n=nvs-mark v=confirm k=reboot-no-confirm r=rollback b=boot->slot0");
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

// Negative test: clone but corrupt the image magic byte. esp_ota_end must
// REJECT it and the boot partition must stay unchanged (node stays on app0).
// Proves the verification gate — §16.2/§16.3 "corrupt byte / hash failure".
static void cloneCorruptExpectReject() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot0   = esp_ota_get_boot_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
  if (!running || !next) { Serial.println("[corrupt] partition lookup failed"); return; }

  Serial.printf("[corrupt] cloning %s -> %s with a corrupted magic byte...\n",
                running->label, next->label);

  esp_ota_handle_t h;
  esp_err_t err = esp_ota_begin(next, running->size, &h);
  if (err != ESP_OK) { Serial.printf("[corrupt] esp_ota_begin: %s\n", esp_err_to_name(err)); return; }

  static uint8_t buf[4096];
  uint32_t off = 0;
  while (off < running->size) {
    size_t chunk = min((uint32_t)sizeof(buf), running->size - off);
    if (esp_partition_read(running, off, buf, chunk) != ESP_OK) { esp_ota_abort(h); Serial.println("[corrupt] read fail"); return; }
    if (off == 0) buf[0] ^= 0xFF;   // wreck the image magic byte (0xE9)
    if (esp_ota_write(h, buf, chunk) != ESP_OK) { esp_ota_abort(h); Serial.println("[corrupt] write fail"); return; }
    off += chunk;
    yield();
  }

  err = esp_ota_end(h);   // must FAIL on the corrupted image
  if (err == ESP_OK) {
    Serial.println("[corrupt] !! esp_ota_end ACCEPTED a corrupt image — verification gate is WEAK");
    Serial.println("[corrupt] !! NOT setting boot partition; app1 left with a bad image (harmless, never booted)");
  } else {
    Serial.printf("[corrupt] esp_ota_end rejected it as expected: %s\n", esp_err_to_name(err));
    Serial.println("[corrupt] PASS — bad image blocked, boot partition untouched.");
  }
  const esp_partition_t* boot1 = esp_ota_get_boot_partition();
  Serial.printf("[corrupt] boot before=%s after=%s (must be unchanged)\n",
                boot0 ? boot0->label : "?", boot1 ? boot1->label : "?");
}

// Negative test 2: valid header, but one payload byte flipped mid-image. The
// header/magic gate passes; this must be caught by the appended-SHA256 check
// at esp_ota_end — the gate that actually protects a truncated/corrupt real
// download. Boot partition must stay on app0.
static void cloneCorruptPayloadExpectReject() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot0   = esp_ota_get_boot_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
  if (!running || !next) { Serial.println("[payload] partition lookup failed"); return; }

  // Must land INSIDE the real image (past the header, before the image end +
  // its appended SHA256), NOT in the erased padding beyond it — otherwise the
  // hash check legitimately ignores it. 0x8000 is safely within any app image.
  const uint32_t corruptAt = 0x8000;
  Serial.printf("[payload] cloning %s -> %s, flipping 1 byte at 0x%06X...\n",
                running->label, next->label, corruptAt);

  esp_ota_handle_t h;
  esp_err_t err = esp_ota_begin(next, running->size, &h);
  if (err != ESP_OK) { Serial.printf("[payload] esp_ota_begin: %s\n", esp_err_to_name(err)); return; }

  static uint8_t buf[4096];
  uint32_t off = 0;
  while (off < running->size) {
    size_t chunk = min((uint32_t)sizeof(buf), running->size - off);
    if (esp_partition_read(running, off, buf, chunk) != ESP_OK) { esp_ota_abort(h); Serial.println("[payload] read fail"); return; }
    if (corruptAt >= off && corruptAt < off + chunk) buf[corruptAt - off] ^= 0xFF;
    if (esp_ota_write(h, buf, chunk) != ESP_OK) { esp_ota_abort(h); Serial.println("[payload] write fail"); return; }
    off += chunk;
    yield();
  }

  err = esp_ota_end(h);   // must FAIL via SHA256 mismatch
  if (err == ESP_OK) {
    Serial.println("[payload] !! esp_ota_end ACCEPTED a corrupt payload — hash gate WEAK");
  } else {
    Serial.printf("[payload] esp_ota_end rejected it as expected: %s\n", esp_err_to_name(err));
    Serial.println("[payload] PASS — mid-image corruption blocked by hash check.");
  }
  const esp_partition_t* boot1 = esp_ota_get_boot_partition();
  Serial.printf("[payload] boot before=%s after=%s (must be unchanged)\n",
                boot0 ? boot0->label : "?", boot1 ? boot1->label : "?");
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

// Reboot while still PENDING_VERIFY, WITHOUT confirming. If rollback is
// working, the bootloader treats this as a failed verification and boots the
// PREVIOUS slot on the next start. This is the unattended crash-recovery proof.
static void rebootWithoutConfirm() {
  Serial.println("[probation] rebooting WITHOUT marking valid...");
  Serial.println("[probation] expect to come back on the OTHER slot (auto-rollback).");
  delay(800);
  esp_restart();
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
  Serial.println("\n\n### FieldMesh NODE OTA slot bring-up test ###");
  printInfo();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  int c = Serial.read();
  switch (c) {
    case 'i': printInfo();              break;
    case 'c': cloneRunningToInactive(); break;
    case 'x': cloneCorruptExpectReject(); break;
    case 'y': cloneCorruptPayloadExpectReject(); break;
    case 'n': writeNvsMarker();         break;
    case 'v': markValid();              break;
    case 'k': rebootWithoutConfirm();   break;
    case 'r': markInvalidAndReboot();   break;
    case 'b': bootToSlot0();            break;
    case '\n': case '\r':               break;
    default: Serial.printf("? unknown '%c' — menu: i c x v r b\n", (char)c); break;
  }
}
