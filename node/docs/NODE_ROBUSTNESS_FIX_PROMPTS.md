# Full Node Firmware Integrity Repair and Robustness Hardening

You are working on the ESP32 environmental sensor node firmware.

Primary project:

    node/firmware/

Important files include:

    src/main.cpp
    src/protocol.h or the shared protocol header used by this project
    src/storage/local_queue.h
    src/storage/local_queue.cpp
    src/sensors.h
    src/sensors.cpp
    platformio.ini
    tests/

Read the complete node firmware, protocol definitions, queue implementation,
PlatformIO configuration, and existing tests before changing anything.

The firmware currently works in some autonomous tests. Preserve the existing
functional flow while fixing the verified integrity defects below. Do not make
large cosmetic refactors. Every behaviour change must be deliberate, logged,
tested, and compiled.

Do not merely produce recommendations. Implement the changes.

---

# Primary goals

The repaired firmware must guarantee, as far as the hardware permits, that:

1. ESP-NOW callbacks never perform unsafe I2C, NVS, filesystem, Wi-Fi
   reinitialisation, peer mutation, or blocking send operations.

2. Commands received during an autonomous wake are processed before the node
   powers off.

3. A received packet cannot be misidentified because two protocol structures
   happen to have the same byte size.

4. A queued sensor snapshot is not removed because an unrelated ESP-NOW send
   callback reported success.

5. The node does not release PWR_HOLD unless a valid future wake source has been
   programmed and verified.

6. Queue mutations and configuration updates survive interrupted or failed NVS
   writes without silently rolling back or advancing to inconsistent state.

7. Invalid, malformed, unauthorised, stale, or misdirected radio commands are
   rejected.

8. The existing interval-based sync flow continues to work while daily sync
   mode is handled consistently.

9. Tests exercise production code rather than reimplementing mock copies of the
   production logic.

10. No temporary communication failure causes queued ecological data to be
    deliberately deleted.

---

# Verified critical defects

Treat these as confirmed defects, not hypothetical possibilities.

## A. CONFIG_SNAPSHOT packet-size collision

On the ESP32 ABI:

    sizeof(pairing_command_t) == 52
    sizeof(config_snapshot_message_t) == 52

The current receive callback dispatches primarily by structure size. It checks
pairing_command_t first and returns from that branch even when the command is
not PAIR_NODE.

As a result, CONFIG_SNAPSHOT packets are swallowed and never reach their
handler.

Fix dispatch before doing any CONFIG_SNAPSHOT schedule work.

## B. Deferred work can be lost at power-off

The callback sets pending-action flags, but processPowerCut() runs at the start
of loop() before those actions are serviced.

During a sync wake:

1. TIME_SYNC, CONFIG_SNAPSHOT, schedule, or ACK-related data arrives.
2. The callback sets a pending flag.
3. handleRtcWakeEvents() finishes.
4. finalizeWakeAndSleep() schedules the power cut.
5. The next loop iteration executes processPowerCut().
6. Power can be removed before the pending command is applied.

POST_WAKE_WINDOW_MS is currently defined but not meaningfully used.

## C. ESP-NOW callbacks can be attributed to the wrong packet

The firmware uses one global completion pair:

    g_lastSendDone
    g_lastSendStatus

for all transmissions.

Functions such as sendNodeHello() start multiple sends without waiting for the
individual callbacks. A delayed callback from HELLO, broadcast fallback, status,
or another packet can satisfy the queue flush wait and cause a snapshot to be
popped even when that snapshot was not delivered.

## D. Alarm writes are not verified

Alarm 1 and Alarm 2 writes only check the I2C ACK from endTransmission(). The
actual registers and control bits are not read back.

After three failed arm attempts, finalizeWakeAndSleep() still schedules the
power cut.

The node can therefore switch itself off without a valid future wake alarm.

## E. Queue mutation is not transactional

local_queue::enqueue() and local_queue::pop() mutate the RAM queue before
persisting it.

If persistence fails, RAM and NVS represent different queue states. A hard
power cut can then lose or resurrect records.

## F. Configuration state is not atomic

Configuration values and configVersion are persisted separately.

A power interruption can leave the node reporting a new config version while
still using older schedule values.

## G. Callback safety is only partially implemented

The callback already defers many RTC and NVS operations, but it still performs
or triggers operations such as:

    esp_now_del_peer()
    esp_now_add_peer()
    sendDiscoveryRequest()
    bringupEspNow()
    WiFi mode changes
    mutation of shared configuration fields

The callback must not directly mutate operational state that the main task is
using.

## H. Existing callback-safety test does not test production code

The existing callback-safety test reimplements a simplified mock callback and
mock service routine.

A passing result does not verify src/main.cpp.

Tests must call actual production helpers or helpers extracted from production
code.

---

# General implementation rules

1. Work in stages.

2. Compile the main firmware after every stage:

       cd node/firmware
       pio run -e esp32wroom

3. Do not proceed to the next stage while the main environment fails.

4. Preserve existing sensor acquisition, local queue format migration, pairing,
   deployment, rescue mode, sync scheduling, and power-hold functionality unless
   a change is explicitly required below.

5. Do not silently discard data to unblock the queue.

6. Do not implement “force-pop after three failed syncs.” Temporary radio
   failure is not evidence that a snapshot is invalid.

7. Do not use dynamic allocation in radio callbacks.

8. Prefer fixed-size FreeRTOS queues, bounded buffers, fixed-width integer
   types, and explicit return-value checking.

9. Do not add blocking I2C, NVS, Preferences, filesystem, Wi-Fi reset, peer
   mutation, or send-and-wait calls to ESP-NOW callbacks.

10. Do not change wire protocol layouts without:
    - introducing a protocol version;
    - updating all relevant size checks;
    - documenting compatibility;
    - compiling both legacy-compatible and new-protocol modes where applicable.

11. Do not claim tests passed unless the actual commands were run.

12. If a hardware-only test cannot be executed, compile it and clearly mark it
    as requiring bench validation.

---

# Stage 0 — Establish the baseline

Before modifying code:

1. Read all production source files and protocol structures.

2. Record:

       sizeof(node_snapshot_t)
       sizeof(pairing_command_t)
       sizeof(config_snapshot_message_t)
       sizeof(deployment_command_t)
       sizeof(unpair_command_t)
       sizeof(time_sync_response_t)
       sizeof(config_ack_message_t)

3. Run:

       pio run -e esp32wroom

4. Save the baseline compiler output.

5. Identify which existing test environments actually compile production code
   and which duplicate logic.

6. Do not “fix” unrelated compiler warnings unless they affect correctness.

Deliver a short baseline report before implementing Stage 1.

---

# Stage 1 — Replace size-first packet dispatch

## Required behaviour

Dispatch must be command-first and then exact-size validated.

Do not cast incomingData to a complete packet before confirming:

- minimum command-field length;
- the command field contains a null terminator within its fixed-width array;
- the command is recognised;
- len exactly matches the expected structure size.

Create a production helper that can be unit tested, for example:

    enum class IncomingMessageType : uint8_t {
      INVALID,
      DISCOVER_RESPONSE,
      DISCOVERY_SCAN,
      PAIRING_RESPONSE,
      PAIR_NODE,
      DEPLOY_NODE,
      UNPAIR_NODE,
      SET_SCHEDULE,
      SET_SYNC_SCHED,
      SYNC_WINDOW_OPEN,
      TIME_SYNC,
      CONFIG_SNAPSHOT,
      SNAPSHOT_ACK
    };

    IncomingMessageType classifyIncomingMessage(
        const uint8_t* data,
        size_t len
    );

The helper must never read beyond len.

For each recognised command:

1. Verify exact expected size.
2. Copy into the correct destination type.
3. Explicitly ensure every fixed-width text field is terminated.
4. Validate target nodeId when the structure provides one.
5. Reject command/size mismatches.

Add a regression test proving that:

- a 52-byte PAIR_NODE is classified as PAIR_NODE;
- a 52-byte CONFIG_SNAPSHOT is classified as CONFIG_SNAPSHOT;
- neither packet is swallowed by the other;
- malformed unterminated command fields are rejected;
- truncated and oversized packets are rejected.

Do not use a test that reimplements the dispatcher. The test must call the
production classification helper.

Validation:

    pio run -e esp32wroom
    pio run -e <new-dispatch-test-env>

---

# Stage 2 — Introduce an ordered node event queue

Replace the collection of loosely related single-slot pending flags with an
ordered, fixed-capacity event queue, or use the event queue as the authoritative
path while temporarily retaining flags only for migration.

The callback runs on the Wi-Fi task, not the main Arduino loop. It must:

1. classify and validate the packet;
2. copy the validated packet and sender MAC into a fixed-size event;
3. enqueue the event without blocking;
4. update only atomic counters if the queue is full;
5. return.

Suggested form:

    enum class NodeEventType : uint8_t {
      DISCOVERY_RESPONSE,
      DISCOVERY_SCAN,
      PAIRING_RESPONSE,
      PAIR_NODE,
      DEPLOY_NODE,
      UNPAIR_NODE,
      SET_SCHEDULE,
      SET_SYNC_SCHED,
      SYNC_WINDOW_OPEN,
      TIME_SYNC,
      CONFIG_SNAPSHOT,
      SNAPSHOT_ACK
    };

    struct NodeEvent {
      NodeEventType type;
      uint8_t senderMac[6];
      uint16_t payloadLength;
      union {
        discovery_response_t discovery;
        pairing_response_t pairingResponse;
        deployment_command_t deploy;
        schedule_command_message_t schedule;
        sync_schedule_command_message_t syncSchedule;
        time_sync_response_t timeSync;
        config_snapshot_message_t configSnapshot;
        snapshot_ack_t snapshotAck;
      } payload;
    };

Use a fixed-size FreeRTOS queue or a fixed ring buffer. Do not allocate from the
heap in the callback.

The callback must no longer call or indirectly trigger:

    rtc.adjust()
    rtc.now()
    persistNodeConfig()
    local_queue::clear()
    local_queue::enqueue()
    local_queue::pop()
    ds3231DisableAlarmInterrupt()
    ds3231DisableAlarm2Interrupt()
    clearDS3231_AlarmFlags()
    esp_now_del_peer()
    esp_now_add_peer()
    espnowSendWithRecover()
    bringupEspNow()
    shutdownEspNow()
    WiFi.mode()
    sendDiscoveryRequest()
    sendTimeSyncRequest()
    sendNodeStatusUpdate()

Keep the SYNC_WINDOW_OPEN timestamp capture lightweight. Prefer enqueuing the
event as well so ordering remains visible to the main task.

Add counters for:

    callbackEventsReceived
    callbackEventsDropped
    callbackInvalidPackets

Report queue overflow prominently. Do not silently overwrite a pending event.

Validation must call the real callback or the production enqueue entry point,
not a rewritten mock implementation.

---

# Stage 3 — Centralise main-task event processing

Create:

    static void serviceNodeEvents(uint32_t maxEvents = ...);

This must be the only normal place where incoming radio commands change:

- RTC state;
- deployment state;
- schedule state;
- mothership MAC;
- peers;
- NVS;
- queue contents;
- outbound acknowledgements.

Preserve arrival order.

Call serviceNodeEvents():

1. near the top of loop();
2. while waiting in sync-listen loops;
3. while waiting for stale-sync recovery;
4. after a sync marker arrives;
5. after each outbound send completes;
6. after queue flushing;
7. during the post-wake command window;
8. before alarm rearming;
9. before scheduling a power cut;
10. immediately before executing a scheduled power cut.

Do not let processPowerCut() run while any of these are true:

- the node event queue is non-empty;
- a critical event is being applied;
- a configuration write is dirty or in progress;
- an ACK is pending;
- an ESP-NOW send is active;
- the post-wake processing window has not elapsed.

Use POST_WAKE_WINDOW_MS as an actual bounded command-processing window.

Implement:

    static bool hasCriticalPendingWork();

and make processPowerCut() defer the cut when this returns true.

Use overflow-safe millis() comparisons.

Add a test that:

1. injects TIME_SYNC or CONFIG_SNAPSHOT during a simulated sync wake;
2. schedules a power cut;
3. verifies the event is applied and persisted first;
4. verifies power-cut eligibility becomes true only afterward.

---

# Stage 4 — Create a single serialized ESP-NOW send manager

There must be at most one active send whose completion affects state.

Create one main-task send API, for example:

    struct SendResult {
      esp_err_t queueResult;
      bool callbackReceived;
      esp_now_send_status_t deliveryStatus;
    };

    SendResult sendEspNowAndWait(
        const uint8_t* destination,
        const void* payload,
        size_t payloadLength,
        uint32_t timeoutMs
    );

Requirements:

1. Only the main task may call it.

2. It must ensure the peer exists before sending.

3. It must record the expected destination MAC.

4. onDataSent() must compare the callback MAC with the currently active send.

5. A callback for an older or unrelated send must not complete the current send.

6. Do not start a second send until the first has completed or timed out.

7. Direct and broadcast HELLO sends must occur sequentially.

8. ACK, status, discovery, time request, snapshot, and fallback sends must use the
   same serialized mechanism.

9. shutdownEspNow() must not run while a send is active.

10. Recovery must not deinitialise ESP-NOW while a send callback is still
    outstanding.

11. On timeout, invalidate the active send generation before retrying.

A generation/token counter is recommended even though the ESP-NOW callback does
not provide an application token. Pair it with expected destination and strict
single-send serialization.

Replace direct raw esp_now_send() calls that are followed by assumptions about
delivery.

Add a regression test or deterministic harness showing that a delayed HELLO
callback cannot satisfy the wait for a snapshot.

Do not pop queue records based solely on a stale global send flag.

---

# Stage 5 — Verify DS3231 alarms and fail safely

Add low-level register read helpers with checked return values.

For Alarm 1 verify registers:

    0x07 through 0x0A

For Alarm 2 verify registers:

    0x0B through 0x0D

Verify the control register at:

    0x0E

Required bits:

    INTCN
    A1IE
    A2IE

Verify the status register at:

    0x0F

Required sequence:

1. calculate the intended alarm values;
2. write Alarm 1;
3. read back Alarm 1 and compare exact bytes;
4. write Alarm 2;
5. read back Alarm 2 and compare exact bytes;
6. enable alarm interrupt bits;
7. read back the control register;
8. only after successful verification, clear stale A1F/A2F flags;
9. read back status to confirm the intended result.

Return detailed status rather than one ambiguous Boolean if useful:

    struct AlarmArmResult {
      bool alarm1Written;
      bool alarm1Verified;
      bool alarm2Written;
      bool alarm2Verified;
      bool controlVerified;
      bool flagsCleared;
    };

The node must not release PWR_HOLD if no verified future wake path exists.

After bounded retries:

- retain PWR_HOLD;
- enter an explicit alarm-fault state;
- log the exact failure;
- attempt a safe recovery path;
- send a status fault when radio contact is available.

Do not invent an ESP32 deep-sleep fallback unless its wake source and GPIO hold
behaviour are verified for this board.

If the existing RTC INT deep-sleep fallback is retained:

- verify RTC_INT_PIN is RTC-capable;
- check the return code;
- do not enter deep sleep when wake configuration failed.

Add a hardware bring-up test that:

1. writes Alarm 1 and Alarm 2;
2. confirms read-back;
3. intentionally corrupts one register;
4. proves verification fails;
5. rearms and verifies success.

---

# Stage 6 — Correct sync and schedule semantics

Validate all incoming schedule values before changing state.

For data wake interval:

- reject zero unless zero has an explicitly documented meaning;
- reject values outside the supported range;
- do not cast an unrestricted integer directly to uint8_t.

For synchronization:

- preserve the documented meaning that syncIntervalMin == 0 represents daily
  synchronization;
- accept zero only when a valid daily phase is present;
- validate interval upper bounds;
- validate syncPhaseUnix;
- ensure the calculated alarm is in the future.

Fix the pre-wake rounding inconsistency.

The existing comment says “round up,” while the implementation floors to the
start of the minute. Define and test the intended behaviour.

For DS3231 Alarm 2 minute resolution:

- choose the programmed minute deterministically;
- ensure it is not already in the past;
- ensure the listening window includes the intended target and grace period.

Fix CONFIG_SNAPSHOT handling so any actual change to:

    wakeIntervalMin
    syncIntervalMin
    syncPhaseUnix

sets the rearm requirement.

Do not rearm unnecessarily when values are unchanged.

Add tests covering:

- interval mode;
- daily mode;
- phase in the future;
- phase in the past;
- pre-wake crossing a minute boundary;
- data and sync alarms occurring close together.

---

# Stage 7 — Make configuration persistence transactional

Do not continue persisting a collection of independent keys where the config
version can advance separately from the values.

Create a versioned configuration record containing all fields required to
reconstruct node state, for example:

    magic
    schemaVersion
    generation
    checksum
    mothershipMAC
    paired/deployed intent
    rtcSynced
    rtcPowerLost
    recoveryReason
    wakeIntervalMin
    syncIntervalMin
    syncPhaseUnix
    lastTimeSyncUnix
    lastSyncSlot
    appliedConfigVersion

Use fixed-width integer fields.

Store it using alternating A/B NVS records:

    node_cfg_a
    node_cfg_b

Each commit must:

1. create a complete candidate record;
2. increment generation;
3. calculate checksum;
4. write to the inactive slot;
5. read it back;
6. validate magic, schema, generation, bounds, and checksum;
7. only then treat it as committed.

At boot:

1. validate both slots;
2. select the newest valid generation using wrap-safe comparison;
3. migrate the existing legacy key/value configuration if neither slot exists;
4. never choose an older valid record when a newer valid backup exists.

The configuration version and its values must be committed together.

Do not set appliedConfigVersion before the schedule and phase are durably
committed.

Persist only when data changed. Add a dirty flag and remove unconditional
per-boot configuration writes where possible.

Keep a migration path so deployed nodes with existing NVS data are not reset.

Add fault-injection tests for:

- interrupted write to slot A;
- corrupted checksum;
- newer valid B plus older valid A;
- migration from legacy keys;
- config version not advancing when value persistence fails.

---

# Stage 8 — Make local queue persistence transactional

Review the existing QueueBlob size and stack usage before choosing a rollback
mechanism.

Do not blindly allocate a full QueueBlob backup on a small task stack.

For enqueue rollback, preserve only what is modified:

- previous head;
- previous tail if full-policy changes it;
- previous used count;
- previous next sequence;
- the record slot being overwritten;
- previous checksum/generation metadata.

For pop rollback, preserve:

- previous tail;
- previous used count;
- previous checksum/generation metadata.

If persistence fails:

- restore RAM state;
- return false;
- leave the queue logically unchanged.

Replace the single-key persistence design with generation-based A/B records or
an equivalent journalled approach.

Requirements:

1. Both records contain:
   - magic;
   - schema version;
   - generation;
   - capacity/layout identifier;
   - queue data;
   - checksum.

2. Persist to the inactive slot.

3. Verify the written record.

4. On load, choose the newest valid generation.

5. Support migration from the legacy "blob" key.

6. Do not write a “backup” and always load the primary first. That design can
   silently load an older queue after a partial write.

7. Keep the existing capacity unless there is a separately justified storage
   migration.

8. Preserve the current queue-full policy unless explicitly documented.

9. Expose:
   - dropped due to capacity;
   - persistence failures;
   - recovered-from-secondary count;
   - corrupt-record count.

Do not force-pop data because radio delivery failed.

Add tests using the real queue implementation for:

- enqueue persistence failure rollback;
- pop persistence failure rollback;
- corrupted newest slot;
- corrupted older slot;
- newest valid generation selection;
- legacy migration;
- queue full behaviour;
- hard-reset reload preserving sequence order.

---

# Stage 9 — Add durable snapshot acknowledgement support

The robust end state is that a snapshot remains in the node queue until the
mothership confirms durable persistence.

Add a versioned protocol message such as:

    struct snapshot_ack_t {
      char command[16];       // "SNAPSHOT_ACK"
      char nodeId[16];
      uint32_t seqNum;
      uint8_t persisted;
      uint8_t protocolVersion;
      uint16_t reserved;
    };

Do not remove the head record after only ESP-NOW link-layer success.

Required robust flow:

1. peek queue head;
2. send snapshot;
3. wait for radio delivery;
4. continue servicing inbound events;
5. wait for matching SNAPSHOT_ACK containing:
   - expected nodeId;
   - expected seqNum;
   - persisted == 1;
   - sender MAC == paired mothership;
6. pop only that exact head record;
7. on timeout, retain it for the next sync;
8. ignore duplicate or stale acknowledgements.

Add a queue method that explicitly acknowledges the current head sequence:

    bool acknowledgeHead(uint32_t seqNum);

It must reject a nonmatching sequence.

Because the current mothership may not yet support SNAPSHOT_ACK:

- implement this as a coordinated protocol feature;
- support a clearly named compatibility build flag;
- do not silently pretend legacy link-layer delivery is durable;
- print a prominent startup warning in legacy mode;
- do not enable ACK-required mode by default until the matching mothership
  implementation is present and verified.

Compile both modes:

    legacy compatibility mode
    durable ACK mode

Do not introduce a protocol layout change without updating protocol versioning.

---

# Stage 10 — Enforce sender and target validation

Once a node is paired, operational messages must only be accepted from the
persisted mothership MAC.

Operational messages include:

    TIME_SYNC
    SET_SCHEDULE
    SET_SYNC_SCHED
    SYNC_WINDOW_OPEN
    DEPLOY_NODE
    CONFIG_SNAPSHOT
    UNPAIR_NODE
    SNAPSHOT_ACK

Rules:

1. In UNPAIRED state:
   - accept discovery and explicit pairing workflow;
   - do not persist a mothership binding merely because DISCOVER_RESPONSE arrived.

2. In PAIRED or DEPLOYED state:
   - reject operational packets from any other MAC;
   - log a bounded diagnostic counter rather than flooding Serial.

3. Verify nodeId whenever the structure contains one.

4. Remove reliance on broadcast UNPAIR.

5. If the current unpair structure has no target node ID:
   - accept it only as a direct packet from the paired mothership;
   - do not accept broadcast unpair;
   - plan a versioned targeted unpair structure.

6. Validate all date and time components before rtc.adjust().

7. Reject implausible Unix times and clearly invalid phase values.

8. Add protocol replay/message IDs in the versioned protocol path if practical.

Do not enable ESP-NOW encryption without coordinating keys and provisioning, but
structure the code so sender validation is mandatory even before encryption is
added.

---

# Stage 11 — Repair RTC recovery and power-hold behaviour

Make RTC readiness a persistent runtime condition, not a local setup variable.

Add:

    static bool g_rtcReady;
    static bool g_rtcPowerLost;
    static RecoveryReason g_recoveryReason;

When rtc.begin() fails:

- do not continue using rtc.now() as though valid;
- do not arm alarms;
- do not release power using an unverified RTC wake path;
- enter a visible recovery state.

When rtc.lostPower() is true:

- preserve the paired mothership identity;
- preserve deployment intent separately from operational readiness;
- mark time invalid;
- disable stale alarms;
- request time recovery;
- do not sample with knowingly invalid timestamps;
- clear the diagnostic only after a valid TIME_SYNC is applied and persisted.

Do not silently turn a deployed node into an ordinary unpaired node.

Expose the recovery reason in NODE_STATUS or NODE_HELLO when protocol space or
versioning permits.

Power-hold initialization must assert the ON level immediately.

Do not intentionally write the OFF level before asserting hold.

Check the actual hardware polarity and initialise with:

    pinMode(PWR_HOLD_PIN, OUTPUT);
    digitalWrite(PWR_HOLD_PIN, kPwrHoldOnLevel);

before lengthy setup work.

---

# Stage 12 — Reduce unnecessary boot work and NVS wear

Gate the full I2C scan behind:

    #ifndef NODE_I2C_SCAN_ON_BOOT
    #define NODE_I2C_SCAN_ON_BOOT 0
    #endif

Use:

    #if NODE_I2C_SCAN_ON_BOOT
      testI2CBusesMuxAndADS();
    #endif

Do not enable it in the default production environment.

Retain a dedicated bring-up/debug environment that enables it.

Remove unconditional configuration persistence at every boot when nothing
changed.

Review queue persistence frequency and report approximate write volume, but do
not weaken durability merely to reduce writes.

Remove dead code only after confirming it is unused:

- shouldSyncAt() may be removable;
- nextSyncSlotUnix() is used and must remain unless replaced.

Use bounded string copies and validate termination.

Preferences::getString() is valid; changing it to a fixed buffer is optional
cleanup, not a critical bug.

---

# Stage 13 — Production tests

Tests must reuse production code.

Do not write another test that duplicates a simplified callback, dispatcher,
queue, or configuration store.

Extract small production modules where necessary to make real logic testable,
for example:

    message_dispatch.h/.cpp
    node_event_queue.h/.cpp
    config_store.h/.cpp
    espnow_send_manager.h/.cpp
    alarm_manager.h/.cpp

Avoid unnecessary architecture churn, but prefer tested production helpers over
unverifiable monolithic logic.

Required test coverage:

1. Packet dispatch collision.
2. Malformed and unterminated commands.
3. Wrong packet sizes.
4. Wrong sender MAC.
5. Wrong target node ID.
6. Callback returns without I2C/NVS/peer work.
7. Event ordering is preserved.
8. Event queue overflow is counted and safe.
9. Pending TIME_SYNC is applied before power cut.
10. Pending CONFIG_SNAPSHOT is applied before power cut.
11. Delayed unrelated send callback cannot acknowledge a snapshot.
12. Alarm write/read-back success.
13. Alarm corruption detection.
14. No power cut after alarm verification failure.
15. Daily sync mode.
16. Interval sync mode.
17. Config A/B recovery.
18. Queue A/B recovery.
19. Queue mutation rollback.
20. ACK sequence matching.
21. ACK timeout retains the snapshot.
22. Legacy compatibility mode build.
23. Durable ACK mode build.
24. RTC lost-power recovery state.
25. PWR_HOLD starts asserted.

Every test environment must be declared in platformio.ini and compile cleanly.

---

# Stage 14 — Final integration validation

Run all applicable builds, including:

    cd node/firmware

    pio run -e esp32wroom
    pio run -e <dispatch-test>
    pio run -e <callback-production-test>
    pio run -e <alarm-verify-test>
    pio run -e <queue-robustness-test>
    pio run -e <config-store-test>
    pio run -e <daily-sync-test>
    pio run -e <legacy-protocol-build>
    pio run -e <durable-ack-build>

Then perform or provide a bench-test checklist for:

1. Fresh unpaired boot.
2. Explicit pairing.
3. Deployment.
4. First data capture.
5. Interval sync wake.
6. Daily sync wake.
7. CONFIG_SNAPSHOT update.
8. TIME_SYNC during sync window.
9. Mothership unavailable for multiple cycles.
10. Queue retained through power cycles.
11. Queue flush after mothership returns.
12. Alarm I2C failure.
13. RTC lost power.
14. Rapid reboot rescue mode.
15. Direct unpair of only the intended node.
16. SD/mothership durable ACK integration once supported.

---

# Required implementation constraints

Do not:

- force-pop snapshots after three communication failures;
- clear the local queue during ordinary pairing unless this is explicitly
  required and confirmed;
- persist pairing merely from discovery;
- accept broadcast unpair;
- power off after alarm verification fails;
- use packet size alone to identify commands;
- perform NVS, I2C, peer mutation, Wi-Fi reset, or blocking send work in the
  receive callback;
- delete queued data on link-layer success when durable ACK mode is enabled;
- advance configVersion before configuration values are committed;
- rewrite tests as independent copies of production behaviour;
- remove rescue mode, sensor acquisition, or current logging without equivalent
  replacement;
- claim compilation success without showing the command results.

---

# Deliverables

At the end, provide:

1. A concise defect-to-fix mapping.

2. A list of every modified and added file.

3. Full contents of each modified source file, or a clean patch if the repo
   environment supports reliable patch review.

4. The exact PlatformIO commands run and their results.

5. Test results separated into:
   - compiled;
   - executed on host;
   - executed on hardware;
   - still requiring hardware.

6. Any protocol changes and compatibility implications.

7. NVS migration behaviour for existing deployed nodes.

8. Remaining risks that could not be resolved without matching mothership
   changes.

9. A clear statement of whether:
   - legacy mothership mode remains supported;
   - durable SNAPSHOT_ACK mode is implemented;
   - durable ACK mode is enabled by default.

Do not leave placeholder TODOs in release-critical paths.

Implement one stage at a time and preserve a compilable firmware after every
stage.