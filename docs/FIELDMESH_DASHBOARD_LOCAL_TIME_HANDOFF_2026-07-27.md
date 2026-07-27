# FieldMesh — Dashboard Local-Time Display Handoff

**Date:** 2026-07-27
**From:** firmware repo (`Data-Logger-network---ESP32`)
**To:** whoever owns the cloud/dashboard frontend repo
**Type:** display-layer follow-up — no data-plane change, no action needed on the backend/DB schema

## What changed on the firmware side

The mothership's on-device config portal (the captive-portal web UI served directly from the ESP32, `mothership/firmware/v2/src/config/config_server.cpp`) now displays the fleet clock in the **operator's local time** instead of raw UTC.

Mechanism: a small JS snippet runs on every page load, reads the browser's own timezone offset (`-(new Date().getTimezoneOffset())`), and POSTs it to an existing (previously-unused) `/set-utc-offset` endpoint on the mothership. That offset is stored in NVS and applied **only** inside two string-formatting functions (`getRTCTimeString()`, `formatDateTimeDisplay()`) that build the HTML shown on the portal page. Self-corrects across DST on next page load, no manual entry required.

**Nothing else changed.** Confirmed by re-reading the code line-by-line:
- The DS3231 RTC itself is still set from browser UTC and stores UTC.
- The ESP-NOW sync schedule (`syncPhaseUnix`, `syncIntervalMin`, NODE_CONFIG/SET_SYNC_SCHED phase math) is still computed entirely in UTC.
- The JSON payload the mothership POSTs to the Supabase ingest endpoint (`datetime` fields, `serverTimeUnix`, etc. — see `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` / `FIELDMESH_PAYLOAD_REFERENCE.md`) is still built from the raw UTC RTC value, untouched.

## Why this was done

Standing product requirement: every **human-facing** time display should read in local time; every **stored/transmitted** timestamp stays UTC, converted only at render time. Rationale: a UTC-labeled clock that reads hours "behind" the viewer's wall clock gets misread as a lost-sync / wrong-clock symptom even when nothing is actually wrong — this has already caused one real diagnostic wild-goose-chase on the mothership side before this fix, and just caused a second one now that the mothership UI and the dashboard disagree with each other.

## What's needed on the dashboard side

Nothing in the database, Edge Function, or wire protocol needs to change — all stored/ingested timestamps are correctly UTC and should stay that way. The only gap is the **presentation layer**: wherever the dashboard currently renders a UTC timestamp as-is (last upload time, node last-seen, mothership last-contact, reading `datetime`, etc.), convert it to the viewer's local time at render time only — e.g. via the browser's native `Intl.DateTimeFormat`/`toLocaleString()` (simpler than the mothership's approach, since the dashboard already renders directly in the viewer's browser each time — no NVS-style persisted offset needed).

Do **not**:
- Store a converted/shifted timestamp anywhere in Postgres.
- Apply any offset correction server-side (Edge Function) to inbound `serverTimeUnix` or reading timestamps.
- Resurrect any client-side "correct the mothership's RTC" logic — that was already tried once and caused a real +2h fleet desync (see `rtc-utc-authority` history); the mothership RTC is the sole time authority and must never be written to based on a dashboard-side clock.

## Where to verify the wire format hasn't moved

- `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`
- `docs/FIELDMESH_SUPABASE_SCHEMA_CONFIRMED.md`
- `docs/FIELDMESH_PAYLOAD_REFERENCE.md`

All three still describe UTC on the wire; this handoff doesn't change any of them.
