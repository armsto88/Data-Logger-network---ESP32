# Mothership UI Redesign — Agent Handoff Prompt

> Copy everything below the line and paste it into a new agent session (e.g. Designer or Planner).

---

You are working on the **FieldMesh mothership on-device web UI** — the captive portal served by an ESP32-S3 field hub at `192.168.4.1` when a user connects to its Wi-Fi. This is the primary interface for deploying and managing sensor nodes in the field, viewed on a phone or laptop in a backyard or remote site.

Your task is to **plan a UI upgrade** that makes the portal more responsive, more beautiful, more intuitive, and smarter about showing users exactly what they need to see. You are NOT writing firmware code in this pass — you are producing a **design plan and implementation strategy** that a firmware engineer can execute.

## Constraints

- The UI runs on an **ESP32-S3** with limited flash and RAM. No external assets, no CDNs, no images, no icon fonts. Everything is embedded in firmware as PROGMEM strings.
- The web server is the synchronous `WebServer` library (not AsyncWebServer). No WebSockets, no SSE. Data flows via JSON polling (`GET /api/live` every 4s idle, 650ms during discovery).
- The portal is viewed in the field on a **phone** (captive portal) — mobile-first is non-negotiable. Desktop layout should also be good but phone is the primary target.
- The user connects to Wi-Fi `Logger001` (password `logger123`) and the portal opens automatically.
- Keep the existing design language: sage green primary (`#5b7553`), warm paper background (`#faf8f4`), terracotta warnings (`#c47a5a`), 10px radius, soft shadows. This palette is already working and matches the FieldMesh style guide.
- The portal must remain usable with **no internet connection** — it is a captive portal served entirely from the ESP32.

## Current implementation summary

The entire UI lives in **one file**: `mothership/firmware/v2/src/config/config_server.cpp` (~3540 lines). Everything — HTML, CSS (~240 lines as `COMMON_CSS`), JS (~380 lines as `COMMON_JS`), route handlers, JSON builders — is embedded as C++ `String` concatenation and `PROGMEM` strings. No templating engine, no separation of concerns.

### Current pages
| Route | Page | What it does |
|---|---|---|
| `/` | Hub Overview | KPI tiles (Active/Connected/New nodes), hub battery, storage %, last upload status, collection schedule summary, node cards with live-updating chips (status, battery, last seen), discovery panel |
| `/stations` | Stations list | Node cards with live updates, "Add New Station" guided pairing panel, "Find New Nodes" button |
| `/station?id=` | Station detail | Node status header, sensor config link, manual lat/lon entry, notes field, node ID/name (locked for deployed), recording interval (read-only global), action radio-cards (Deploy/Pause/Resume/Remove) |
| `/node-sensors?id=` | Sensor picker | Toggle-button grid to select which sensors a node should expect |
| `/settings` | Settings | Recording interval presets (1/5/10/30 min), cloud upload config (enable, API key, QR string), storage stats, manual upload (in Advanced), Save & Start button |

### Current strengths to preserve
- **Mobile-first responsive design** with `@media(max-width:480px)` (44px touch targets, sticky header, stacked cards). This already works well.
- **Live polling system** (`FM` JS object): visibility-aware (pauses when tab hidden), exponential backoff on failure, FNV version fingerprint to skip redundant DOM updates, adaptive cadence (4s idle → 650ms during discovery/post-action). This is well-engineered — keep it.
- **Async forms** with loading spinners, ok/err flash, ARIA-live status messages, 15s AbortController timeout.
- **XSS-safe DOM updates** (uses `textContent`, not `innerHTML`, for user-supplied strings).
- **Loading overlay** for page navigation.
- **CSS custom properties** design system — tokens are already defined and consistent.
- **Inline SVG icons** (no icon font, no external assets).
- **Captive portal redirect** works correctly across iOS/Android/Windows.

### Current weaknesses to address

**1. No data visualisation.** The portal shows numeric tiles and text chips only. No charts, no sparklines, no gauges, no trend indicators. A user cannot see *what their nodes are measuring* — only that they are connected and have battery. This is the single biggest gap.

**2. No "last reading" headline.** Station cards show status/battery/last-seen but not the most recent sensor values (temperature, humidity, soil moisture, etc.). The redesign doc planned this (§12.4) but it was never built. The `/api/live` endpoint doesn't currently include last-reading values.

**3. Inconsistent form feedback.** Some forms are async (JSON response, inline status), others do full-page navigation to a static confirmation page (station save, sensor save). Non-ajax fallback pages for some handlers bypass `headCommon` entirely — unstyled, no shared CSS/JS.

**4. No Data Export page.** The redesign planned a dedicated `/export` page with CSV download guidance, date range info, and file stats. Currently `/export` is just a 302 redirect to a raw CSV download with no wrapper.

**5. Terminology is mixed.** The redesign mapped Node→Station, Mothership→Hub, Deploy→Activate, etc. Routes use "station" but page text still says "Mothership time", "Find New Nodes", raw `nodeId` hex fallbacks. The pass is incomplete.

**6. Manual upload is blocking.** `handleManualUpload` holds the server for 30–60s during modem upload. The browser fetch hangs and the ESP32 can't serve other clients. Needs a non-blocking approach or at minimum a proper progress indicator.

**7. Accessibility gaps.** Form labels lack `for` associations. Sensor picker toggle buttons lack `aria-pressed`. No `<html lang>`. No skip-link. Colour-only status communication (chips/dots) has no text fallback beyond the chip label.

**8. Maintainability.** 3540 lines in one file. HTML/CSS/JS embedded in C++ strings with no syntax highlighting, no linting, quote-escaping pain. This makes any UI change slow and error-prone.

**9. Dead code.** `buildDataStatusSectionHtml()` (lines 1566–1660) is defined but never called.

**10. No offline data preview.** A user in the field can download the CSV but can't preview recent readings in the portal itself. A lightweight recent-readings table or sparkline would let them verify data quality without leaving the portal.

## What to produce

Create a **detailed design plan** covering:

### A. Information architecture
- What pages should exist and what goes on each one.
- Whether to keep the current multi-page structure or move toward a lightweight SPA (single page with view-switching via JS, given the ESP32 can't do client-side routing with real URLs).
- Navigation pattern — bottom tab bar? hamburger menu? cards on the home page?
- Progressive disclosure strategy — what's visible by default vs what's collapsed in "Advanced"/"About".

### B. Data visualisation strategy
- What to show on the Hub Overview (recent readings summary across the fleet? sparklines? a "last reading" card per station?).
- What to show on the Station detail page (last reading values with timestamps? a mini chart of recent readings? sensor health indicators?).
- How to get last-reading data to the frontend — should `/api/live` include the most recent sensor values per node? Should there be a separate `/api/readings?node=X&limit=N` endpoint? Consider RAM/flash cost on the ESP32.
- What kind of charts are feasible in embedded vanilla JS with no libraries (CSS bar charts? inline SVG sparklines? canvas-based mini-plots?). Recommend a specific approach.
- How to handle the case where a node hasn't reported yet (no data) vs has sensors but they're faulted.

### C. Responsive layout improvements
- How to make better use of desktop space (current max-width 600px / 720px wastes desktop).
- Phone layout refinements — is the current card layout optimal? Should station cards be more compact or more detailed?
- Touch target sizing, spacing, and thumb-reach considerations.
- Whether to use a bottom navigation bar on mobile (common captive portal pattern).

### D. UX flow improvements
- Form consistency — make all forms async with inline feedback. No more full-page confirmation pages.
- Manual upload — propose a non-blocking approach (background task + progress polling? or at minimum a "upload in progress" state that doesn't freeze the UI).
- Schedule changes — should there be optimistic UI updates or is the current "wait for next poll" acceptable?
- Error states — what should the user see when a form fails, when a node goes offline, when the hub is low on storage?
- Empty states — what does a first-time user see before any nodes are paired?
- Guided onboarding — can the "Add New Station" flow be more guided (step-by-step) rather than a collapsed `<details>` panel?

### E. Visual polish
- Typography hierarchy — the current CSS uses system fonts. Is that the right choice for an embedded portal? Any adjustments?
- Card design — shadows, borders, spacing. The current design is clean but could be more refined.
- Status indicators — chips and dots work, but could they be more intuitive? Colour + icon + text?
- Micro-interactions — transitions, hover states, tap feedback. Currently minimal.
- Dark mode — is it worth supporting for night-time field use? (Phone in dark mode, low-light site visit.)

### F. Accessibility
- Concrete fixes: label associations, `aria-pressed` on toggles, `<html lang>`, skip-link, text alternatives for colour-only status.
- Screen reader flow — can a user navigate the portal with VoiceOver/TalkBack?

### G. Maintainability / architecture
- Should the CSS and JS be split into separate PROGMEM strings (kept in the same file but as named constants) rather than inlined in `headCommon`?
- Should page HTML be templated (simple token replacement) rather than raw `String +=` concatenation?
- Should the file be split — e.g. `config_server_routes.cpp`, `config_server_html.cpp`, `config_server_json.cpp`?
- Recommend the lowest-risk refactor that improves maintainability without breaking the working portal.

### H. Implementation phasing
- Break the plan into phases that can be shipped incrementally without breaking the current working deployment.
- Phase 1 should be the highest-impact, lowest-risk change.
- Each phase should be independently testable against the live 3-node backyard deployment.

## Reference documents in the repo

- `mothership/firmware/v2/src/config/config_server.cpp` — the entire current UI (~3540 lines). Key sections: CSS 665–905, JS 907–1289, `headCommon` 1317–1352, `buildLiveJson` 1492–1564, `handleRoot` 1667–1900, `handleStationsPage` 2274–2391, `handleStationDetail` 2394–2580, `handleSettings` 2803–2989, route table 3468–3536.
- `mothership/firmware/v2/src/config/config_server.h` — public API (53 lines).
- `mothership/firmware/v2/src/config/node_registry.h` — `NodeInfo` struct (backing data for station cards).
- `mothership/firmware/v2/src/config/transmission_settings.h` — cloud upload config struct.
- `docs/FIELDMESH_FIELD_UI_REDESIGN.md` — the previous redesign plan (partially implemented; use as reference for terminology and page structure intentions).
- `docs/FIELDMESH_UI_STYLE_GUIDE_2026.md` — palette, typography, design principles.
- `docs/FIELDMESH_DASHBOARD_DESIGN.md` — cloud dashboard design (shared visual language reference).

## Output format

Produce a single document structured as:

1. **Executive summary** — the vision in 3–4 sentences.
2. **Information architecture** — page list, navigation pattern, progressive disclosure.
3. **Data visualisation plan** — what charts/sparklines/values to show where, and how data gets to the frontend.
4. **Responsive layout plan** — mobile and desktop layout specifics.
5. **UX flow improvements** — form consistency, upload, errors, empty states, onboarding.
6. **Visual polish** — typography, cards, status indicators, micro-interactions, dark mode.
7. **Accessibility fixes** — concrete, itemised.
8. **Maintainability refactor** — recommended file structure and templating approach.
9. **Implementation phasing** — phased plan with testable milestones.
10. **Open questions** — decisions that need the user's input before implementation.

Be specific and concrete. Reference the current code where relevant. Prefer approaches that work within the ESP32's constraints (no external assets, vanilla JS only, embedded CSS/JS) — but don't let those constraints produce a poor UX. If a constraint genuinely blocks a good UX, call it out as an open question.