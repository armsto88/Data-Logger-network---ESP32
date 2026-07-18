# FieldMesh — OTA A/B Firmware-Slot Reporting: Backend Contract

**Status:** implemented in firmware (compiles; both mothership + node builds green). Takes effect once the mothership **and** nodes are reflashed. **All new fields are additive and nullable/optional** — no existing field changed type or meaning.

**Why the backend must read this first:** the Supabase ingest rejects unknown/again-shaped payloads with a **non-retryable 400**. These additions are purely *new keys*; confirm your ingest is additive-tolerant (ignores unknown keys) or add the columns below **before** the reflash, so a firmware that now emits `slots[]` does not start 400-ing.

Complements `docs/FIELDMESH_FIRMWARE_DASHBOARD_INTEGRATION_BRIEF.md` (Appendix A).

---

## 1. What changed and why

Every FieldMesh device (mothership + each node) has two OTA app slots, `app0` / `app1`. One runs; the other holds the previous or a staged image for OTA + rollback. Until now the cloud only saw the *running* firmware. These additions surface **both slots** so the dashboard can show, per device:

- which slot is **active** (running now),
- which slot boots **next** (reveals a staged update before it takes),
- the **version** and **state** of the image in **each** slot.

The firmware already sent running-firmware identity (`status.firmware{}`, `status.nodes[].firmwareVersion` etc.) — those are unchanged. This adds the per-slot table.

---

## 2. Mothership — `status.firmware{}` (additive keys)

Existing keys (`role, version, buildId, hwTarget, protocolVersion, releaseId, runningSlot, otaState, otaReason, lastOtaReason`) are unchanged. **New keys:** `activeSlot`, `nextBootSlot`, `slots[]`.

```jsonc
"firmware": {
  "role": "mothership",
  "version": "0.1.0",
  "buildId": "5876faf",
  "hwTarget": "mothership-v1",
  "protocolVersion": 2,
  "releaseId": null,
  "runningSlot": "app0",
  "otaState": "CONFIRMED",
  "otaReason": "NONE",
  "lastOtaReason": "NONE",

  "activeSlot": "app0",        // NEW: label of the running slot
  "nextBootSlot": "app0",      // NEW: label the bootloader will boot next
                               //      (== activeSlot unless an update is staged)
  "slots": [                   // NEW: one row per OTA app slot
    { "label": "app0", "version": "0.1.0", "buildId": "5876faf",
      "state": "CONFIRMED", "active": true,  "nextBoot": true,  "present": true },
    { "label": "app1", "version": "",       "buildId": "",
      "state": "EMPTY",     "active": false, "nextBoot": false, "present": false }
  ]
}
```

### `slots[]` element fields
| field | type | notes |
|---|---|---|
| `label` | string | `"app0"` / `"app1"` |
| `version` | string | Running slot: authoritative firmware semver. Inactive slot: version from the on-flash app descriptor — **may be empty or generic** until the NVS release store lands (see §5). |
| `buildId` | string | Running slot: git short hash. Inactive slot: currently `""` (not yet tracked). |
| `state` | string enum | See §4. |
| `active` | bool | This is the running slot. |
| `nextBoot` | bool | Bootloader will boot this slot next. |
| `present` | bool | A readable app image exists in this slot. |

---

## 3. Node — `status.nodes[]` entries (additive keys)

Reported per node via ESP-NOW `FW_CAPS` and relayed by the mothership. Existing per-node firmware keys (`firmwareVersion, firmwareBuild, hardwareRevision, otaProtocolVersion, otaMaxImageSize, rollbackCapable, otaCapable`) are unchanged. **New keys:** `activeSlot`, `firmwareSlots[]` — present only when the node runs firmware new enough to report slots (older nodes omit them; treat as optional).

```jsonc
{
  // ... existing node fields ...
  "firmwareVersion": "0.1.0",
  "firmwareBuild": "5876faf",
  "hardwareRevision": "node-v3",
  "otaCapable": true,
  "rollbackCapable": true,

  "activeSlot": "app0",        // NEW
  "firmwareSlots": [           // NEW (omitted for nodes on older firmware)
    { "label": "app0", "version": "0.1.0", "buildId": "5876faf",
      "state": "CONFIRMED", "active": true  },
    { "label": "app1", "version": "",
      "state": "EMPTY",     "active": false }
  ]
}
```

Node `firmwareSlots[]` rows carry `label, version, state, active` (running row also `buildId`). They intentionally do **not** carry `nextBoot`/`present` — a node reports its running slot + the other slot's state/version only.

---

## 4. `state` enum (both mothership + node slots)

| value | meaning |
|---|---|
| `CONFIRMED` | Valid image, confirmed good. |
| `PENDING_VERIFY` | Just-installed image on first-boot probation (will roll back if it fails self-test). |
| `INVALID` | Marked invalid / rolled back. |
| `ABORTED` | Aborted install. |
| `IDLE` | No probation state / unknown. |
| `EMPTY` | No readable image in the slot. |

Store as text; do not assume the set is closed for forward-compat, but these are the only values emitted today.

---

## 5. Known limitations (so the dashboard sets expectations honestly)

- **Inactive-slot `version`** comes from the image's on-flash app descriptor. Until FieldMesh wires FW_SEMVER/FW_GIT into the app descriptor (or the NVS release store lands), it may be empty or a generic string. **Inactive-slot `state` and the `active`/`nextBoot` flags are reliable now** — prefer those for the primary display.
- **Inactive-slot `buildId`** is `""` for now.
- **`releaseId`** stays `null` until the OTA release/anti-downgrade NVS store exists.
- A **freshly flashed** device shows the other slot as `EMPTY` until its first OTA.

---

## 6. Suggested backend action

1. Confirm ingest tolerates unknown keys, **or** add nullable columns / a JSONB blob for `status.firmware.slots` and `status.nodes[].firmwareSlots` **before** the reflash.
2. Render a two-card firmware panel per device: highlight `active`; show each slot's `version` + `state`; badge the slot where `nextBoot != active` as "update staged".
3. Treat `firmwareSlots`/`activeSlot` on nodes as **optional** (absent for nodes on older firmware).

**Acceptance:** after reflashing one mothership + one node, a status upload contains `status.firmware.slots` (2 rows) and that node's `status.nodes[].firmwareSlots` (2 rows), and ingest returns success (no 400).

---

## 7. Copy-paste request for the backend repo

Paste this into the backend repo/agent. It is self-contained; §2–§5 above are the authoritative field spec.

```
REQUEST: Accept + surface OTA A/B firmware-slot fields (additive) BEFORE we reflash the FieldMesh fleet.

CONTEXT
FieldMesh firmware (mothership + nodes) will start emitting per-device OTA slot
info in the existing status upload. Each device has two app slots (app0/app1);
we now report both, so the dashboard can show which slot is active, which boots
next, and the version/state of each. All additions are NEW KEYS ONLY — no
existing field changes type or meaning.

WHY THIS IS BLOCKING THE REFLASH
The Supabase ingest returns a NON-RETRYABLE 400 on a payload it can't accept.
If ingest is strict about shape, a reflashed device emitting the new keys would
start failing permanently. So this must be handled BEFORE we flash the fleet.

NEW KEYS
1) status.firmware{}  (mothership)  adds:
   - "activeSlot":   string   e.g. "app0"
   - "nextBootSlot": string   e.g. "app0"  (== activeSlot unless an update is staged)
   - "slots": [ { "label":str, "version":str, "buildId":str,
                  "state":str-enum, "active":bool, "nextBoot":bool, "present":bool } ]
2) status.nodes[] entries add (OPTIONAL — absent for nodes on older firmware):
   - "activeSlot":     string
   - "firmwareSlots": [ { "label":str, "version":str, "buildId"?:str,
                          "state":str-enum, "active":bool } ]

state enum (string): CONFIRMED | PENDING_VERIFY | INVALID | ABORTED | IDLE | EMPTY

KNOWN-INCOMPLETE FIELDS (render defensively):
- inactive-slot "version" may be empty/generic and "buildId" may be "" for now;
  "state" + "active"/"nextBoot" are reliable — lead the UI with those.
- node "firmwareSlots"/"activeSlot" are OPTIONAL; treat absence as "not reported".

REQUIRED CHANGES
1) Ensure the ingest is additive-tolerant (ignores unknown keys) OR add storage
   for status.firmware.slots and status.nodes[].firmwareSlots (nullable columns
   or a JSONB blob). Do NOT reject a payload that carries these keys.
2) Dashboard: per-device firmware panel with two slot cards — highlight the
   active slot; show each slot's version + state; badge the slot where
   nextBoot != active as "update staged".
3) Keep node slot fields optional.

ACCEPTANCE
- A status upload from a reflashed mothership containing status.firmware.slots
  (2 rows) ingests with success (no 400) and the two slots render.
- A reflashed node's status.nodes[].firmwareSlots (2 rows) ingests + renders.
- A node WITHOUT the fields still ingests fine (optional).

Please confirm back when ingest accepts these keys (and columns exist if needed)
so we can schedule the fleet reflash without tripping 400s.
```
