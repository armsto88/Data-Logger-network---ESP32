# fieldMesh — User Onboarding & Provisioning Brief

**Date:** 2026-06-26
**Status:** Briefing document for the full-stack development manager (planning only — no code)
**Project:** fieldMesh — ESP32 environmental sensor network
**Audience:** Full-stack development manager planning backend + dashboard + auth implementation
**Companion docs:**
- `docs/FIELDMESH_SUPABASE_MIGRATION_PLAN.md` — Supabase schema, Edge Function design, migration phases
- `docs/FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` — current JSON upload protocol (`{meta, status, readings}`)
- `docs/FIELDMESH_DASHBOARD_DESIGN.md` — existing React/Vite dashboard architecture
- `docs/FIELDMESH_ABOUT_SECTION_DESIGN.md` — public-facing about content

---

## 1. Executive Summary

### 1.1 What we're building

A **multi-tenant sensor data platform** on top of the existing fieldMesh hardware network. Users sign up, create projects, register mothership hubs, receive API keys, and view their data in an isolated dashboard. Motherships upload via LTE to a single generic Supabase Edge Function endpoint, authenticated by a per-device API key that routes data to the correct project.

### 1.2 Why

fieldMesh currently runs as a prototype: one Google Sheet, one shared URL, no user accounts, no data isolation. Anyone with the Apps Script URL can read and write. This is fine for a showcase but blocks productisation. We need:

- **User accounts** — sign up, log in, own your data.
- **Project isolation** — one user's sensor data is not visible to another.
- **Scalable backend** — Supabase (Postgres + Auth + Edge Functions + Realtime) replaces Google Sheets + Apps Script.
- **Per-device credentials** — each mothership gets its own revocable API key instead of a shared token in a URL query string.
- **Self-service provisioning** — users register a mothership and get a QR code to scan in the field, no developer involvement.

### 1.3 Who this is for

This document is a **brief for the full-stack development manager** who will plan and sequence the backend (Supabase schema, Edge Functions, RLS), the dashboard (auth flows, project/mothership management UI, QR generation), and the firmware interface changes (API key in NVS, generic endpoint). It is not an internal design doc — it is a prompt that collects the constraints, the target architecture, and the open questions in one place.

### 1.4 What this document is not

- It is not implementation code.
- It does not override the confirmed decisions in `FIELDMESH_SUPABASE_MIGRATION_PLAN.md`; it extends them from single-tenant to multi-tenant and flags where the existing schema must evolve.
- It does not specify the ESP-NOW radio path, V2 snapshot wire format, or modem SSL config — those are stable and documented in the migration plan and `protocol.h`.

---

## 2. Current State

| Aspect | Current | Limitation |
|---|---|---|
| **Backend** | Google Sheet via Google Apps Script `doPost()` | No query capability, O(n) full-range scans, 20k reads/day quota |
| **Auth** | `?token=xxx` query param on the Apps Script URL | Shared single token, visible in modem AT logs, not revocable per-device |
| **User accounts** | None | One shared dashboard URL for everyone |
| **Data isolation** | None | Anyone with the URL sees all data |
| **Upload payload** | CSV text (legacy) → JSON `{meta, status, readings}` (new spec, see `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`) | JSON carries status + readings but still posts to the shared Apps Script URL |
| **Dashboard** | React + Vite, polls Apps Script `doGet` endpoints every 60s | No real-time, no auth, no per-user view |
| **Mothership identity** | `deviceId` string (e.g. "001") in JSON `meta` | Not bound to a user or project; no registration flow |

The mothership already sends a structured JSON payload with `meta`, `status` (fleet, upload queue, flash, config, node registry), and `readings` (sensor snapshots). The protocol is confirmed in `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md`. The gap is not the payload — it is the **identity, routing, and isolation** layer on the backend.

---

## 3. Target Architecture

```
User (browser)
    ↓ Supabase Auth (email/password or magic link)
fieldMesh Dashboard (React + Vite + @supabase/supabase-js)
    ↓ Supabase JS client + Realtime subscriptions
    ↓ RLS: user sees only rows where project.user_id = auth.uid()
PostgreSQL (Supabase)
    ↑ Edge Function inserts with service_role (bypasses RLS)
Supabase Edge Function: ingest-fieldmesh
    ↑ Authorization: Bearer fm_xxxxxxxx
    ↑ Content-Type: application/json  (or application/octet-stream for V2 binary)
Mothership (ESP32 + A7670G LTE)
    ↑ API key stored in NVS, entered via config UI or QR scan
Sensor Nodes (ESP-NOW, unchanged)
```

### 3.1 Key architectural shifts from the migration plan

The existing `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` is single-tenant: one `deployments` row, one `mothership_tokens` row, anon-key dashboard reads everything. This briefing extends it to multi-tenant:

| Concept | Migration plan (single-tenant) | This brief (multi-tenant) |
|---|---|---|
| **User identity** | None (anon key) | Supabase Auth (`auth.users`) |
| **Top-level container** | `deployments` (manual insert) | `projects` (user-created, owned by `auth.uid()`) |
| **Device record** | `mothership_tokens` (token → mothership_id) | `motherships` (project_id, mac, firmware, last_seen) + `api_keys` (hashed, revocable) |
| **Dashboard auth** | anon key, SELECT all | authenticated user, RLS scoped to `projects.user_id = auth.uid()` |
| **Data routing** | Token maps to a deployment_id | API key maps to a mothership → project → user |

The Edge Function remains the trust boundary and still uses the `service_role` key internally. The change is that it now resolves the API key to a `mothership_id` → `project_id` and stamps every inserted row with that `project_id`, so RLS can enforce isolation on the dashboard side.

---

## 4. User Onboarding Flow (detailed)

From a user's perspective, end to end:

### Step 1 — Sign up
- User goes to `fieldmesh.app` (or whatever the dashboard domain is).
- Signs up with email/password or magic link (auth method is an open question — see §12).
- Supabase Auth creates a row in `auth.users`.
- No project, no mothership, no data yet — empty dashboard with a "Create your first project" prompt.

### Step 2 — Create a project
- User clicks "New Project".
- Enters: name, location (free text or lat/lon), description, expected sensor types (optional, for display defaults).
- Dashboard inserts a row into `projects` with `user_id = auth.uid()`.
- Project appears in a project selector in the sidebar.

### Step 3 — Register a mothership
- User clicks "Add Mothership" within a project.
- Enters the mothership's MAC address (printed on the device label) or scans a QR code on the device packaging.
- Dashboard inserts a row into `motherships` with `project_id` and `mac_address`.
- On first upload from that device, `last_seen` and `firmware_version` populate automatically from the JSON `meta`.

### Step 4 — Get configuration (API key + endpoint)
- On the mothership detail page, the dashboard generates an API key (format `fm_xxxxxxxx`, see §8).
- The key is stored **hashed** in `api_keys`; the plaintext is shown **once** to the user.
- The dashboard displays a **QR code** encoding the endpoint URL + API key, so the user can scan it with their phone in the field.
- A "copy" button gives the URL + key as text for manual entry.

### Step 5 — Configure the mothership in the field
- User powers the mothership (battery or USB).
- Connects to the mothership's WiFi AP (`fieldmesh-xxxx`) and opens `192.168.4.1`.
- On the **Upload Settings** page, the user either:
  - **Scans the QR code** with their phone (the mothership config UI accepts a pasted QR-decoded string), or
  - **Enters the API key** manually into an "API Key" field (replaces the current "Endpoint URL" field).
- The mothership stores the API key in NVS (namespace `tx`, key `api_key`) and the generic endpoint URL in NVS.
- A "Test Upload" button sends a status-only payload to verify connectivity.

### Step 6 — Deploy nodes
- User pairs sensor nodes to the mothership via the existing Field UI Node Manager (ESP-NOW pairing flow, unchanged).
- Node metadata (nodeId, MAC, name, state) is included in the JSON `status.nodes[]` array on every upload and upserted into the `nodes` table scoped to the project.

### Step 7 — View data
- User returns to the dashboard.
- Dashboard queries `readings`, `nodes`, `sync_sessions` filtered by `project_id` (via RLS — the user only sees their own projects).
- Real-time subscription on `readings` pushes new values to sensor cards as the mothership uploads.

---

## 5. Mothership Provisioning Options

Three approaches, in recommended order:

### 5.1 QR Code (recommended)
- The dashboard generates a QR code encoding a single string: `https://<project-ref>.supabase.co/functions/v1/ingest-fieldmesh|fm_a7k2x9p3`.
- The mothership config UI has a "Scan QR" field that accepts the decoded string (pasted from a phone QR reader app) and splits on `|` into endpoint + API key.
- **Why recommended:** zero typing errors, the user does not need to understand URLs or keys, works with any phone QR app.
- **Open question:** should the mothership itself have a camera/QR module for direct scanning, or is phone-mediated paste sufficient? (See §12.)

### 5.2 Short Code
- The dashboard generates a 6–8 character short code (e.g. `A7K2-X9P3`) tied to the mothership + API key.
- The user enters the short code into the mothership config UI.
- The mothership calls a bootstrap endpoint: `GET https://<project-ref>.supabase.co/functions/v1/provision?code=A7K2X9P3`.
- The bootstrap Edge Function returns the full endpoint URL + API key.
- **Use case:** field deployment where the user has a phone but no QR scanner, or the QR is damaged.
- **Trade-off:** requires an extra HTTPS round-trip at provisioning time and a short-code → key mapping table (`provisioning_codes`).

### 5.3 Pre-provisioned (factory)
- Motherships are shipped with a bootstrap URL hardcoded and a unique device secret derived from the MAC address.
- On first boot (or when the user presses a "Register" button), the mothership calls `POST /functions/v1/provision` with its MAC address and device secret.
- The Edge Function looks up the MAC in a `pre_registered_motherships` table (populated at manufacturing/packing time), creates the `motherships` row, generates an API key, and returns it.
- The user claims the device in the dashboard by entering the MAC (or scanning the device QR), which binds the pre-created `motherships` row to their project.
- **Use case:** fleet deployments where devices are configured before the user ever opens the dashboard.
- **Trade-off:** requires a manufacturing step to populate `pre_registered_motherships`; most complex of the three.

**Recommendation:** Ship Phase 1–4 with QR Code (5.1). Add Short Code (5.2) as a fallback in a later phase. Pre-provisioned (5.3) is a future fleet-scale feature — note it in the roadmap but do not build it now. *[Proposed — confirm with full-stack manager.]*

---

## 6. Data Flow with User Isolation

### 6.1 How API keys identify the user/project

```
Mothership POSTs JSON {meta, status, readings}
  Headers: Authorization: Bearer fm_a7k2x9p3
           Content-Type: application/json
        ↓
Edge Function: ingest-fieldmesh
  1. Extract bearer token from Authorization header.
  2. Hash the token (SHA-256) and look up in api_keys.key_hash WHERE revoked_at IS NULL.
  3. Resolve api_keys.mothership_id → motherships.project_id → projects.user_id.
  4. Create a sync_sessions row stamped with mothership_id + project_id.
  5. Upsert nodes[] with project_id.
  6. Insert readings[] with project_id (denormalised for RLS efficiency).
  7. Upsert mothership_status / mothership_config with project_id.
  8. Return { success, rows, nodes }.
        ↓
PostgreSQL rows now carry project_id
        ↓
Dashboard (authenticated user)
  supabase.from("readings").select(...)
  → RLS policy: project_id IN (SELECT id FROM projects WHERE user_id = auth.uid())
  → user only sees their own data
```

### 6.2 RLS policy pattern (per-user isolation)

The existing migration plan uses `USING (true)` for anon reads. For multi-tenant, replace with user-scoped policies:

```sql
-- Users can only see projects they own
CREATE POLICY "projects_owner" ON projects
  FOR ALL TO authenticated
  USING (user_id = auth.uid()) WITH CHECK (user_id = auth.uid());

-- Users can only see motherships in their projects
CREATE POLICY "motherships_owner" ON motherships
  FOR SELECT TO authenticated
  USING (project_id IN (SELECT id FROM projects WHERE user_id = auth.uid()));

-- Users can only see readings/nodes/sessions in their projects
CREATE POLICY "readings_owner" ON readings
  FOR SELECT TO authenticated
  USING (project_id IN (SELECT id FROM projects WHERE user_id = auth.uid()));
-- (repeat for nodes, sync_sessions, mothership_status, mothership_config)
```

The Edge Function uses `service_role` and bypasses RLS, so it can insert for any project. The dashboard uses the authenticated user's JWT and RLS enforces isolation.

### 6.3 Denormalisation note

`readings.project_id` is denormalised from `motherships.project_id` at insert time by the Edge Function. This avoids a join on every dashboard query. The `nodes` and `sync_sessions` tables get the same `project_id` column. This is a schema addition over the existing migration plan DDL — see §7.

---

## 7. Database Schema Additions

The existing migration plan (`FIELDMESH_SUPABASE_MIGRATION_PLAN.md` §5) defines: `deployments`, `nodes`, `readings`, `sync_sessions`, `mothership_tokens`, `mothership_status`, `mothership_config`. For multi-tenant, add/replace as follows:

### 7.1 New tables

**`projects`** (replaces `deployments` as the user-owned top-level container):

| Column | Type | Notes |
|---|---|---|
| `id` | UUID PK | `gen_random_uuid()` |
| `user_id` | UUID | `REFERENCES auth.users(id) ON DELETE CASCADE` |
| `name` | TEXT | required |
| `location` | TEXT | free text or lat/lon string |
| `description` | TEXT | optional |
| `created_at` | TIMESTAMPTZ | `now()` |

**`motherships`** (device registry, replaces the role of `mothership_tokens`):

| Column | Type | Notes |
|---|---|---|
| `id` | UUID PK | |
| `project_id` | UUID | `REFERENCES projects(id) ON DELETE CASCADE` |
| `mac_address` | TEXT | unique, e.g. `24:6F:28:6C:0A:A0` |
| `device_id` | TEXT | optional, the firmware `meta.deviceId` |
| `firmware_version` | TEXT | populated from first upload `meta.firmwareVersion` |
| `last_seen` | TIMESTAMPTZ | updated by Edge Function on each upload |
| `registered_at` | TIMESTAMPTZ | `now()` |

**`api_keys`** (replaces `mothership_tokens` with hashed keys):

| Column | Type | Notes |
|---|---|---|
| `id` | UUID PK | |
| `mothership_id` | UUID | `REFERENCES motherships(id) ON DELETE CASCADE` |
| `key_hash` | TEXT | SHA-256 hash of the plaintext key; unique |
| `key_prefix` | TEXT | first 6 chars for display, e.g. `fm_a7k2` |
| `created_at` | TIMESTAMPTZ | `now()` |
| `revoked_at` | TIMESTAMPTZ | nullable; NULL = active |

### 7.2 Modifications to existing tables

- **`nodes`**: add `project_id UUID REFERENCES projects(id) ON DELETE CASCADE`. The Edge Function stamps this on upsert. Drop or repurpose the existing `deployment_id` (migrate to `project_id`).
- **`readings`**: add `project_id UUID REFERENCES projects(id) ON DELETE CASCADE`. Add index `idx_readings_project_ts ON readings (project_id, timestamp DESC)`.
- **`sync_sessions`**: add `project_id UUID REFERENCES projects(id) ON DELETE CASCADE`.
- **`mothership_status`**, **`mothership_config`**: add `project_id UUID REFERENCES projects(id) ON DELETE CASCADE`.
- **`mothership_tokens`**: deprecated — superseded by `api_keys`. Keep during migration, then drop in Phase 6.
- **`deployments`**: deprecated — superseded by `projects`. Migrate existing rows, then drop in Phase 6.

### 7.3 Optional provisioning tables (for Short Code / Pre-provisioned)

- **`provisioning_codes`**: `id`, `mothership_id`, `code` (unique, 6–8 chars), `expires_at`, `used_at`.
- **`pre_registered_motherships`**: `mac_address` (unique), `device_secret_hash`, `claimed_by_project_id` (nullable until user claims).

These are not needed for QR-only provisioning (Phase 1–4).

### 7.4 Relationship to existing tables

```
auth.users (Supabase Auth)
    ↓ 1:N
projects
    ↓ 1:N          ↓ 1:N          ↓ 1:N
motherships    nodes (via project_id)    readings (via project_id)
    ↓ 1:N
api_keys
```

Every data table (`readings`, `nodes`, `sync_sessions`, `mothership_status`, `mothership_config`) carries `project_id` so RLS can filter on `projects.user_id = auth.uid()` without joins.

---

## 8. API Key Design

### 8.1 Format

- Prefix `fm_` + 8 random alphanumeric chars (lowercase, no ambiguous chars like `0/O`, `1/I`).
- Example: `fm_a7k2x9p3`.
- ~41 bits of entropy in 8 chars from a 32-char alphabet — sufficient for per-device keys validated server-side, and short enough to type manually if QR fails.

### 8.2 Storage

- **Never stored plaintext** in the database. The `api_keys.key_hash` column holds a SHA-256 hash.
- The plaintext is shown to the user **once** at generation time in the dashboard, with a "copy" button and a warning that it will not be shown again.
- The `key_prefix` column stores the first 6 chars (e.g. `fm_a7k2`) so the dashboard can display "fm_a7k2••••••" in the API key management list without revealing the full key.

### 8.3 Transmission

- The mothership sends the key as: `Authorization: Bearer fm_a7k2x9p3`.
- Over HTTPS (A7670G SSL/TLS via CCH* API — already in place).
- **Not** in the URL query string (unlike the current `?token=xxx`).

### 8.4 Validation (Edge Function)

1. Extract token from `Authorization` header.
2. Compute `SHA-256(token)`.
3. `SELECT mothership_id FROM api_keys WHERE key_hash = $1 AND revoked_at IS NULL`.
4. If no row → return `403`.
5. If row → resolve `mothership_id` → `project_id` and proceed with insert.

### 8.5 Revocation

- Dashboard "Revoke" button sets `api_keys.revoked_at = now()`.
- Edge Function checks `revoked_at IS NULL` on every request → revocation is immediate (next upload is rejected with `403`).
- Mothership web UI shows "API key rejected — re-provision" and the user generates a new key from the dashboard.

### 8.6 Rotation

- To rotate: generate a new key (new `api_keys` row), update the mothership via QR/short code, then revoke the old key.
- Open question: should keys auto-expire (e.g. 1 year)? See §12.

---

## 9. Dashboard Changes Needed

The existing dashboard (`FIELDMESH_DASHBOARD_DESIGN.md`) has pages: Dashboard, Charts, Nodes, Config, About. Add auth and project/mothership management:

### 9.1 New pages

| Page | Route | Purpose |
|---|---|---|
| Login | `/login` | Email/password or magic link sign-in |
| Signup | `/signup` | Create account |
| Password reset | `/reset-password` | Supabase Auth reset flow |
| Projects | `/projects` | List, create, select, manage projects |
| Motherships | `/projects/:id/motherships` | Register, list, detail per project |
| API Keys | `/projects/:id/motherships/:mid/keys` | Generate, display (once), revoke |
| Settings | `/settings` | Account settings, project defaults, API key management overview |

### 9.2 New components

- `AuthGate.jsx` — wraps the app, redirects to `/login` if not authenticated.
- `ProjectSelector.jsx` — sidebar dropdown, switches active project context.
- `MothershipRegistrationForm.jsx` — enter MAC or scan QR.
- `ApiKeyDisplay.jsx` — one-time plaintext display + copy button + warning.
- `QrCode.jsx` — renders the endpoint+key QR (library choice is an open question, §12).
- `MothershipCard.jsx` — shows MAC, firmware, last seen, key prefix, revoke button.

### 9.3 Changes to existing pages

- **Dashboard, Charts, Nodes, Config**: all queries must be scoped to the active `project_id`. The Supabase client passes the user's JWT; RLS handles isolation, but the UI should filter by the selected project so the user does not see data from multiple projects mixed together.
- **Config page**: the "Endpoint URL" field becomes "API Key" (or is hidden entirely — the endpoint is now generic and constant). See §10.
- **About page**: unchanged (public content).

### 9.4 Supabase client changes

- Replace the anon-key-only client with an authenticated client using the user's session (Supabase Auth manages the JWT).
- Real-time subscriptions remain, but channels should be scoped to the active project (e.g. filter on `project_id` in the subscription payload).

---

## 10. Firmware Changes Needed

These are firmware-architecture changes for the `Coder` agent to implement later. This brief defines the interface; the migration plan and firmware design notes own the implementation detail.

### 10.1 Replace hardcoded URL with generic endpoint

- **Current:** `TransmissionSettings.endpointUrl` = Google Apps Script URL (per-device, entered via web UI).
- **Target:** `endpointUrl` = `https://<project-ref>.supabase.co/functions/v1/ingest-fieldmesh` — **constant across all devices**. Stored in NVS but set at flash time or via OTA, not per-deployment.
- The per-device identity is now the **API key**, not the URL.

### 10.2 API key in NVS

- New NVS key: namespace `tx`, key `api_key`, value `fm_xxxxxxxx`.
- Set via the mothership web UI (config server) on the Upload Settings page.
- Sent as `Authorization: Bearer <api_key>` by `ModemDriver::httpsPost()`.

### 10.3 Config UI changes

- The Field UI (`192.168.4.1`) Upload Settings page:
  - **Remove** the "Endpoint URL" text field (or make it read-only, showing the generic Supabase URL).
  - **Add** an "API Key" text field (accepts `fm_xxxxxxxx` or a QR-decoded `url|key` string).
  - **Add** a "Test Upload" button that sends a status-only JSON payload and reports success/failure.
- The existing `TransmissionSettings` struct gains an `apiKey` field (or reuses `authToken` with a flag indicating "bearer key" vs "query token").

### 10.4 First-boot provisioning flow (optional)

- If pre-provisioned (§5.3) is implemented: on first boot, if `api_key` is empty, the mothership calls `POST /functions/v1/provision` with its MAC address and receives an API key.
- This is a Phase 5+ feature — not required for QR-based provisioning.

### 10.5 Payload format

- The JSON `{meta, status, readings}` payload from `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` is the **primary format**. No change to the payload structure — only the endpoint and auth header change.
- The V2 binary batch format (`application/octet-stream`) from the migration plan §6.3 remains a later optimisation for reading-only payloads.

---

## 11. Implementation Priority

This is a suggested sequence for the full-stack manager. Each phase has exit criteria. Phases 1–2 come from the existing migration plan; Phases 3–6 are the multi-tenant additions from this brief.

### Phase 1 — Supabase project setup + auth + core schema
- Create Supabase project.
- Enable Supabase Auth (email/password at minimum).
- Run the schema DDL from the migration plan §5, **plus** the multi-tenant additions in §7 of this brief (`projects`, `motherships`, `api_keys`, `project_id` columns).
- Add RLS policies: user-scoped SELECT (§6.2), service_role bypass for Edge Function.
- **Exit criteria:** a user can sign up, log in, create a project, and see an empty dashboard. No data yet.

### Phase 2 — Edge Function with API key validation
- Deploy `ingest-fieldmesh` Edge Function (migration plan §7) extended to:
  - Validate bearer token against `api_keys.key_hash` (§8.4) instead of `mothership_tokens`.
  - Resolve `mothership_id` → `project_id` and stamp every inserted row.
  - Accept `application/json` (primary) and `application/octet-stream` (binary, later).
- **Exit criteria:** a `curl` POST with a valid API key inserts rows with the correct `project_id`. An invalid key returns `403`.

### Phase 3 — Dashboard auth + project management
- Implement auth pages (login, signup, reset).
- Implement project list/create/select.
- Implement mothership registration (enter MAC).
- Implement API key generation + one-time display + QR code.
- **Exit criteria:** a user can sign up, create a project, register a mothership by MAC, generate an API key, and see the QR code.

### Phase 4 — Mothership provisioning (QR code)
- Dashboard generates QR encoding `endpoint|api_key`.
- Mothership firmware accepts QR-decoded string in the config UI (§10.3).
- End-to-end: user scans QR → mothership stores key → first upload appears in dashboard.
- **Exit criteria:** a real mothership uploads data that appears in the correct user's dashboard in real time.

### Phase 5 — Firmware API key support
- `Coder` implements NVS storage for `api_key`, the config UI changes, and the `Authorization: Bearer` header in `httpsPost()`.
- Remove the `?token=xxx` query-param path.
- **Exit criteria:** no Google Apps Script URL or shared token in firmware. All uploads go to the generic Supabase endpoint with a per-device key.

### Phase 6 — Cutover from Google Sheets
- Disable the Google Sheets upload path.
- Migrate any historical data from Google Sheets to Supabase (one-time import script).
- Drop deprecated tables (`mothership_tokens`, `deployments`) after migration.
- Update `FIELDMESH_DASHBOARD_DESIGN.md` and `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` to remove single-tenant references.
- **Exit criteria:** one week of clean multi-tenant data in Supabase, no Google Sheets writes.

---

## 12. Open Questions for the Full-Stack Manager

These need decisions before or during implementation. They are not blocking Phase 1.

1. **Auth method preference.** Email/password is simplest. Magic link is lower friction but requires email deliverability. OAuth (Google/GitHub) is convenient for technical users but adds a provider dependency. *[Recommendation: email/password for Phase 1, add magic link later.]*

2. **QR code library choice for the dashboard.** `qrcode.react` is the common React choice. `qrcode` (vanilla) works if you want to render to canvas/SVG manually. Confirm so the frontend team has a single dependency.

3. **Should one mothership support multiple projects (multi-site)?** The schema in §7 binds a mothership to one `project_id`. If a user wants one hub collecting from nodes across two sites, either (a) one project spanning both sites, or (b) relax the schema to allow node-level `project_id` independent of mothership. *[Recommendation: one mothership = one project for now; revisit if a real deployment needs multi-site.]*

4. **API key rotation policy.** Manual revocation is in the design. Should keys auto-expire (e.g. 1 year, with a dashboard warning at 30 days)? Or be permanent until manually rotated? *[Recommendation: permanent until rotated; add a "last used" timestamp to `api_keys` so the dashboard can flag stale keys.]*

5. **Offline provisioning (no LTE at setup time).** QR Code (5.1) works offline — the key is in the QR, no round-trip needed. Short Code (5.2) and Pre-provisioned (5.3) both require LTE at setup. Is offline provisioning a hard requirement for field deployments? If yes, QR is the only viable option and 5.2/5.3 are deprioritised.

6. **White-label / branding considerations.** The dashboard currently has fieldMesh branding. If this becomes a white-label product (ecological consultancies running their own instance), the auth pages, project names, and about content need to be configurable. Not a Phase 1 concern, but the Supabase project structure (single shared instance vs per-customer instances) depends on it. *[Recommendation: single shared instance with `projects` isolation for now; per-customer instances only if a paying customer requires data residency guarantees.]*

7. **Data retention per project.** The migration plan recommends a 90-day retention job on the free tier. Should retention be per-project (some projects keep 1 year, others 30 days) or global? Per-project adds a `retention_days` column to `projects` and a more complex cleanup job.

8. **Rate limiting on the Edge Function.** Supabase Edge Functions do not have built-in per-key rate limiting. If a compromised key hammers the endpoint, the `api_keys.last_used_at` + a Supabase scheduled function could flag abuse. Is this needed for Phase 1?

9. **Email notifications.** Should the dashboard email users on mothership offline (>24h no upload), low battery, or API key rejection? This requires an email provider (Supabase Auth uses its own; transactional email needs Resend/Postmark). Out of scope for Phase 1 but worth scoping.

10. **Schema reconciliation with the existing migration plan.** This brief proposes `projects`/`motherships`/`api_keys` where the migration plan has `deployments`/`mothership_tokens`. The full-stack manager should decide whether to (a) update the migration plan DDL to the multi-tenant schema before Phase 1, or (b) run the single-tenant DDL first and migrate in Phase 6. *[Recommendation: (a) — adopt the multi-tenant schema now so Phase 1 auth + RLS work correctly from the start.]*

---

## Appendix A — Mapping from existing docs

| Existing doc | Section | What this brief changes |
|---|---|---|
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §4.2 schema overview | `deployments` → `projects` (user-owned); add `motherships`, `api_keys` |
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §4.4 API key auth | `mothership_tokens` (plaintext) → `api_keys` (hashed, `fm_` prefix) |
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §5 SQL DDL | Add `project_id` to `readings`, `nodes`, `sync_sessions`, `mothership_status`, `mothership_config`; add `projects`, `motherships`, `api_keys` tables |
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §5 RLS | Replace `USING (true)` anon policies with `authenticated` + `projects.user_id = auth.uid()` |
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §7 Edge Function | Token validation against `api_keys.key_hash`; resolve `project_id`; stamp rows |
| `FIELDMESH_SUPABASE_MIGRATION_PLAN.md` | §9 migration phases | Insert auth + project management as Phase 1–3; QR provisioning as Phase 4 |
| `FIELDMESH_CLOUD_UPLOAD_PROTOCOL.md` | §3.1 `meta` | `deviceId` now maps to `motherships.device_id`; no payload change |
| `FIELDMESH_DASHBOARD_DESIGN.md` | Frontend architecture | Add auth pages, project selector, mothership registration, API key + QR UI |
| `FIELDMESH_DASHBOARD_DESIGN.md` | Backend architecture | Deprecate Phase 1 (Google Sheets) and Phase 2 (Node.js) sections; Supabase is the backend |

---

## Appendix B — Glossary

| Term | Meaning |
|---|---|
| **Mothership** | ESP32 + A7670G LTE hub that collects from nodes and uploads to the cloud |
| **Node** | ESP32 sensor device, talks to mothership via ESP-NOW |
| **Project** | User-owned container for a field deployment (a site + its motherships + nodes) |
| **API key** | Per-mothership credential (`fm_xxxxxxxx`) sent as a Bearer token, hashed at rest |
| **Edge Function** | Supabase Deno function (`ingest-fieldmesh`) that validates the key and inserts data |
| **RLS** | Row-Level Security — Postgres policies that scope rows to the authenticated user |
| **NVS** | Non-Volatile Storage on the ESP32 — where the API key is persisted on the mothership |
| **Field UI** | The mothership's WiFi AP web interface at `192.168.4.1` |
| **V2 snapshot** | The binary ESP-NOW payload format (`node_snapshot_v2_t`, 48B header + 6B readings) |