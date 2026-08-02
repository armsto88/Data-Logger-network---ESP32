# FieldHub standalone operation

This note records the local-first contract implemented on
`feat/fieldhub-standalone-ui`. It is an operating and field-acceptance guide;
executed checks are recorded separately below so build results are not confused
with hardware evidence.

## Operating modes

Local fleet management never requires a FieldMesh account. During the existing
physically gated 30-minute configuration session an operator can discover and
pair nodes, configure sensors and recording intervals, start/end deployments,
inspect the bounded deployment history, and download raw CSV files.

The data destination is one explicit choice:

- **Local storage only** — the default. The LTE modem is not used for upload.
- **FieldMesh** — optional provisioning enables paid remote history, charts,
  alerts, collaboration, remote control, and managed backup. Local functions
  remain available. Complete deployment records still retained internally are
  backfilled through the bounded cloud outbox over successive syncs, so a hub
  may connect after being commissioned locally.
- **Custom HTTPS** — sends readings to an operator-owned ingest service. It does
  not enable FieldMesh dashboard or remote-control features.

The captive portal does not implement subscription or login checks. FieldMesh
features are gated by a valid provisioning credential at the service boundary;
local operation does not depend on that credential.

## Local retention

LittleFS is always the upload cache and internal fallback. Upload acknowledgement
advances a delivery cursor but no longer deletes a current-schema CSV. Because
the 768 KB partition is shared and bounded, retention starts at 65% total use and
rewrites the readings file to keep roughly the newest 40% of rows. When internal
storage is the active archive, the Data page reports the cumulative number of
rows removed by this limit. Downloading never deletes data.

When a compatible SD card is mounted, each accepted snapshot is also appended to
`fieldmesh_readings.csv`. Completed deployment records are appended idempotently
to `fieldmesh_deployments.csv`. These files are not purged after upload; their
limit is the usable card capacity. An incompatible existing canonical filename is
renamed with a `.legacy-N` suffix rather than overwritten. "Complete SD archive"
therefore means all writes made while a writable card was mounted; the UI exposes
write failures instead of claiming otherwise.

LittleFS retains the newest four completed deployment records per node. A real
v1-to-v2 store migration preserves known epoch/start/end/outbox information and
marks unavailable older labels, locations, or end times as unknown. It never
fabricates those values or silently reseeds epochs.

## Custom HTTPS receiver contract

The FieldHub makes HTTPS `POST` requests with `Content-Type: application/json`.
If configured, the token is sent as `Authorization: Bearer <token>` and is never
placed in the URL or status JSON. The document uses the existing FieldMesh batch
shape:

```json
{
  "readings": [{ "datetime": "...", "nodeId": "...", "seqNum": 1 }],
  "meta": { "firmwareVersion": "..." },
  "status": { "...": "present on the first data POST of a session" }
}
```

Receivers must ignore fields they do not need and should deduplicate retries by
`(nodeId, datetime, seqNum)`. Return any 2xx only after the batch is durably
stored. A non-2xx response leaves the delivery cursor unchanged. Custom mode does
not send FieldMesh deployment lifecycle events, accept remote commands, or run
cloud OTA. The full field reference remains in
[`docs/FIELDMESH_PAYLOAD_REFERENCE.md`](../../docs/FIELDMESH_PAYLOAD_REFERENCE.md).

## Required field acceptance before release

1. Upgrade a hub carrying a genuine v1 deployment store; confirm epochs,
   pending lifecycle state, and known history survive two boots.
2. Run the deployment-epoch and upload-queue test images on hardware, then use
   the documented wipe image before returning the hub to production firmware.
3. Exercise local setup with no key/SIM/backend, commission a node, end and
   redeploy it, reboot, and download both CSVs.
4. Fill LittleFS beyond its retention threshold and power-cycle during a rewrite;
   confirm one valid data file survives and removed-row reporting is truthful.
5. Insert/remove/fill an SD card and interrupt power around snapshot and End
   writes; confirm no canonical file is overwritten and replayed Ends are not
   duplicated.
6. Test a custom endpoint through success, timeout, lost response, 4xx, and 5xx;
   confirm only a durable 2xx advances delivery and retries deduplicate.
7. After at least one local-only ended deployment and one active deployment,
   provision FieldMesh and confirm retained readings resolve to the backfilled
   deployments over successive acknowledgements. Then confirm dashboard control
   convergence and OTA remain unchanged.

Build success alone does not satisfy these hardware and backend acceptance gates.

## Acceptance evidence

### 2026-08-02 — bench FieldHub on COM4

Executed from commit `91f5411` on the physical ESP32 FieldHub
(`48:9d:31:f8:16:a8`):

- `mothership-v2-test-deployment-epoch`: **193/193, OVERALL:PASS**. This
  included local archive retention, more than 16 standalone lifecycle records,
  bounded backfill fixtures, and v1 deployment-store migration.
- `mothership-v2-test-upload-queue`: **29/29, OVERALL:PASS**. The new retention
  checks confirmed that a current-schema readings file is accepted and an
  upload acknowledgement does not delete current local history.
- `mothership-v2-wipe-deploy-store`: **6/6, OVERALL:PASS**. All deployment test
  files were absent afterward and `/datalog.csv` remained present at 367 bytes.
- `mothership-v1-main` was restored successfully. Build `91f5411` booted without
  a panic and entered the expected USB-service path. A subsequent physical
  configuration wake served web UI requests, initialized the LittleFS upload
  queue, and completed the UI-requested Sync & Power Down path with the next RTC
  alarm armed.

No SD card was present on this device. SD archive, removal, capacity, canonical
filename, and interrupted-write acceptance checks were therefore **NOT RUN**,
not failed. The initial portal-start and SD-detection lines occurred before the
serial capture reconnected, so they are not claimed as evidence. Full
captive-portal workflow checks and the remaining manual/backend journeys are
still outstanding.
