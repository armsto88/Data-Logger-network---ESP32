// ====== ESP-NOW Web UI – main.cpp with Node Manager & Node Meta ======

// ---------- Config toggles ----------
#define ENABLE_SPIFFS_ASSETS 0  // set to 1 if you upload /style.css.gz and /app.js.gz

// ---------- Includes ----------
#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <WebServer.h>
#if ENABLE_SPIFFS_ASSETS
  #include <FS.h>
  #include <SPIFFS.h>
#endif

#include "rtc_manager.h"
#include "sd_manager.h"
#include "espnow_manager.h"
#include "protocol.h"
#include <Preferences.h>

// ---------- Device identification and WiFi ----------
const char* DEVICE_ID = "001";  // Simplified ID
const char* BASE_SSID = "Logger";
String ssid = String(BASE_SSID) + String(DEVICE_ID);  // "Logger001"
const char* password = "logger123";
#define FW_VERSION "v1.0.0"
#define FW_BUILD   __DATE__ " " __TIME__

Preferences gPrefs;
int gWakeIntervalMin = 5;          // default shown in UI & used at boot
const int kAllowedIntervals[] = {1, 5, 10, 20, 30, 60};
const size_t kAllowedCount = sizeof(kAllowedIntervals) / sizeof(kAllowedIntervals[0]);

// ---------- Web server ----------
WebServer server(80);

// NodeInfo is defined in your ESP-NOW / protocol headers
extern std::vector<NodeInfo> registeredNodes;

// ---------- UI helpers ----------
static String formatMac(const uint8_t mac[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

static void loadWakeIntervalFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int v = gPrefs.getInt("wake_min", gWakeIntervalMin);
    gPrefs.end();
    bool ok = false;
    for (size_t i = 0; i < kAllowedCount; i++)
      if (v == kAllowedIntervals[i]) { ok = true; break; }
    gWakeIntervalMin = ok ? v : 5;
  }
}

static void saveWakeIntervalToNVS(int mins) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("wake_min", mins);
    gPrefs.end();
  }
}

// ---------- Node meta helpers (numeric ID + Name in NVS) ----------
//
// Each real nodeId (from firmware) can have:
//   - userId  → numeric string like "001" (shown as "Node ID", used in CSV)
//   - name    → free-text name like "North Hedge 01"
//
// Both are stored in NVS under namespace "node_meta" as:
//   id_<nodeId>   and   name_<nodeId>

static String loadNodeMeta(const String& nodeId, const char* fieldPrefix) {
  Preferences prefs;
  // readOnly = true is fine *once the namespace exists*
  if (!prefs.begin("node_meta", /*readOnly=*/true)) {
    return "";
  }
  String key = String(fieldPrefix) + nodeId;  // e.g. "id_TEMP_001"
  String value = prefs.getString(key.c_str(), "");
  prefs.end();
  return value;
}


static void storeNodeMeta(const String& nodeId, const char* fieldPrefix, String value) {
  Preferences prefs;
  // readOnly = false → read/write, can create the namespace
  if (!prefs.begin("node_meta", /*readOnly=*/false)) {
    Serial.println("⚠️ storeNodeMeta: NVS begin failed");
    return;
  }

  String key = String(fieldPrefix) + nodeId;  // e.g. "name_TEMP_001"

  value.trim();
  if (value.length() == 0) {
    prefs.remove(key.c_str());  // empty → clear key
    Serial.printf("[NODES] Cleared %s for %s\n", fieldPrefix, nodeId.c_str());
  } else {
    prefs.putString(key.c_str(), value);
    Serial.printf("[NODES] Set %s for %s → '%s'\n",
                  fieldPrefix, nodeId.c_str(), value.c_str());
  }
  prefs.end();
}


// Numeric Node ID (user-facing, e.g. "001")
static String getNodeUserId(const String& nodeId) {
  return loadNodeMeta(nodeId, "id_");
}

// Enforce "purely numeric" up to 3 chars, e.g. "001", "012", "120"
static void setNodeUserId(const String& nodeId, String userId) {
  String cleaned;
  cleaned.reserve(4);
  userId.trim();
  for (size_t i = 0; i < userId.length(); ++i) {
    char c = userId[i];
    if (c >= '0' && c <= '9') {
      cleaned += c;
      if (cleaned.length() >= 3) break;  // max 3 chars
    }
  }

  // Left-pad to 3 digits if non-empty (optional but nice UX)
  if (cleaned.length() > 0 && cleaned.length() < 3) {
    while (cleaned.length() < 3) cleaned = "0" + cleaned;
  }

  storeNodeMeta(nodeId, "id_", cleaned);
}

// Friendly Name (free text)
static String getNodeName(const String& nodeId) {
  return loadNodeMeta(nodeId, "name_");
}

static void setNodeName(const String& nodeId, String name) {
  // You could truncate here if you want a hard max length, e.g. 32 chars:
  const size_t kMaxLen = 32;
  if (name.length() > kMaxLen) name = name.substring(0, kMaxLen);
  storeNodeMeta(nodeId, "name_", name);
}

// Helpers intended for CSV logging (non-static so espnow_manager.cpp can use them)
String getCsvNodeId(const String& nodeId) {
  String userId = getNodeUserId(nodeId);
  if (userId.length() > 0) return userId;
  return nodeId;         // fallback to firmware ID
}

String getCsvNodeName(const String& nodeId) {
  String nm = getNodeName(nodeId);
  return nm;             // may be empty; CSV can handle blank names
}


// ---------- CSS / JS ----------
const char COMMON_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#f5f5f5; --panel:#ffffff; --text:#1b1f23; --sub:#5f6b7a; --border:#e5e7eb;
  --primary:#2196F3; --success:#4CAF50; --warn:#ff9800; --danger:#f44336;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(0,0,0,.08);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{scroll-behavior:smooth}
html,body{margin:0;padding:0;background:var(--bg);color:var(--text);
  font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif}
a{color:var(--primary);text-decoration:none}
:focus-visible{outline:3px solid rgba(33,150,243,.35);outline-offset:2px}

/* Layout */
.container{max-width:600px;margin:0 auto;padding:var(--sp-3)}
.header{padding:var(--sp-3) 0;text-align:center}
.h1{font-size:22px;font-weight:700;margin:0 0 var(--sp-2)}
.section{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:var(--sp-3);box-shadow:var(--shadow);margin:var(--sp-3) 0}
.section h3{margin:0 0 var(--sp-2);font-size:18px}
.muted{color:var(--sub);font-size:.95rem}

/* Stats */
.stats{display:grid;grid-template-columns:1fr 1fr 1fr;gap:var(--sp-1);text-align:center}
.stat{background:#fafafa;border:1px solid var(--border);border-radius:8px;padding:10px}
.stat strong{display:block;font-size:13px;color:var(--sub);margin-bottom:2px}
.stat .num{font-size:18px;font-weight:700}

/* Lists/cards */
.list{display:grid;gap:var(--sp-1)}
.item{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px;display:block;color:inherit}
.item-row{display:flex;align-items:center;justify-content:space-between;gap:12px}

/* Chips */
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}
.chip--state-deployed{border-color:#c8e6c9;background:#f1f8e9;color:#256029}
.chip--state-paired{border-color:#ffe0b2;background:#fff3e0;color:#e65100}
.chip--state-unpaired{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}

/* Forms */
.label{display:block;margin:8px 0 6px;color:var(--sub);font-size:.95rem}
.input, input[type="text"], input[type="number"], select{
  width:100%;padding:12px;border:1px solid var(--border);border-radius:8px;background:#fff
}
.help{color:var(--sub);font-size:.85rem;margin-top:6px}
.row{display:flex;gap:var(--sp-1);flex-wrap:wrap}
.col{flex:1 1 220px;min-width:0}

/* Buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;
  padding:12px 16px;border-radius:8px;border:1px solid var(--border);background:#fff;color:var(--text);
  cursor:pointer;width:100%;margin-top:8px;text-decoration:none}
.btn--primary{background:var(--primary);color:#fff;border-color:transparent}
.btn--success{background:var(--success);color:#fff;border-color:transparent}
.btn--warn{background:var(--warn);color:#fff;border-color:transparent}
.btn:disabled{opacity:.6;cursor:not-allowed}

/* Utility */
.center{text-align:center}
.badge{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;color:var(--sub);font-size:.85rem}
.footer-bar{
  position:sticky;bottom:0;background:var(--panel);border:1px solid var(--border);
  padding:calc(var(--sp-2) + env(safe-area-inset-bottom)) var(--sp-3);
  border-radius:12px;box-shadow:var(--shadow);display:flex;gap:12px;justify-content:space-between
}

@media(min-width:768px){.container{max-width:720px}}
)CSS";

const char COMMON_JS[] PROGMEM = R"JS(
// Prevent double submits
document.addEventListener('submit', function (e) {
  const btn = e.target.querySelector('button[type="submit"],input[type="submit"]');
  if (btn && !btn.disabled) {
    btn.disabled = true;
    btn.dataset.originalText = btn.textContent;
    btn.textContent = 'Working…';
  }
}, {capture:true});

// Set datetime input to browser time
function setCurrentTime(){
  const n=new Date();
  const z=n=>String(n).padStart(2,'0');
  const s=`${n.getFullYear()}-${z(n.getMonth()+1)}-${z(n.getDate())} ${z(n.getHours())}:${z(n.getMinutes())}:${z(n.getSeconds())}`;
  const el=document.getElementById('datetime'); if(el) el.value=s;
}
function toggleSettings(){
  const panel=document.getElementById('settings-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
window.onload=setCurrentTime;

// Live 1s ticking of the displayed RTC time
(function(){
  const pad = n => String(n).padStart(2,'0');
  function parseYMDHMS(str){
    if (!str || str.length < 19) return NaN;
    const y = +str.slice(0,4), m = +str.slice(5,7), d = +str.slice(8,10);
    const H = +str.slice(11,13), M = +str.slice(14,16), S = +str.slice(17,19);
    const dt = new Date(y, m-1, d, H, M, S); // local
    return isNaN(dt) ? NaN : dt.getTime();
  }
  function formatYMDHMS(ms){
    const dt = new Date(ms);
    return `${dt.getFullYear()}-${pad(dt.getMonth()+1)}-${pad(dt.getDate())} ` +
           `${pad(dt.getHours())}:${pad(dt.getMinutes())}:${pad(dt.getSeconds())}`;
  }
  function startClock(){
    const el = document.getElementById('rtc-now');
    if (!el) return;
    const initial = (el.textContent || '').trim();
    const rtcMs   = parseYMDHMS(initial);
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatYMDHMS(nowMs);
    }
    draw();
    setInterval(draw, 1000);
  }
  if (document.readyState === 'loading'){
    document.addEventListener('DOMContentLoaded', startClock);
  } else {
    startClock();
  }
})();
)JS";

// ---------- Common page frame ----------
static String headCommon(const String& title){
  String h;
  h.reserve(4500);
  h += F("<!DOCTYPE html><html><head>");
  h += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");

#if ENABLE_SPIFFS_ASSETS
  h += F("<link rel='stylesheet' href='/style.css'>");
  h += F("<script defer src='/app.js'></script>");
#else
  h += F("<style>"); h += FPSTR(COMMON_CSS); h += F("</style>");
  h += F("<script>"); h += FPSTR(COMMON_JS);  h += F("</script>");
#endif

  h += F("</head><body><div class='container'>");
  h += F("<div class='header'>");
  h += F("<div class='h1'>"); h += title; h += F("</div>");
  h += F("<div class='badge'>Device ID: <strong>");
  h += DEVICE_ID;
  h += F("</strong></div>");
  h += F("<div class='muted'>Wi-Fi: ");
  h += ssid;
  h += F(" • ");
  h += FW_VERSION;
  h += F(" — ");
  h += FW_BUILD;
  h += F("</div></div>");
  return h;
}
static inline String footCommon(){ return String(F("</div></body></html>")); }

// ---------- Routes / Handlers fwd decl ----------
void handleNodeConfigForm();
void handleNodeConfigSave();
void handleNodesPage();

// ---------- Routes / Handlers ----------

void handleRevertNode() {
  String nodeId = server.arg("node_id");
  bool found   = false;
  bool sentCmd = false;

  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId && node.state == DEPLOYED) {
      node.state = PAIRED;
      savePairedNodes();
      found = true;

      sentCmd = pairNode(nodeId);  // will send PAIR_NODE + PAIRING_RESPONSE

      if (sentCmd) {
        Serial.print("[MOTHERSHIP] Node reverted to PAIRED + PAIR_NODE sent: ");
      } else {
        Serial.print("[MOTHERSHIP] Node reverted to PAIRED (local only; PAIR_NODE send failed): ");
      }
      Serial.println(nodeId);
      break;
    }
  }

  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section center'>");
  if (found) {
    html += F("<h3>Node reverted to paired state</h3><p>Node <strong>");
    html += nodeId;
    html += F("</strong> is now marked as paired.</p>");
    if (!sentCmd) {
      html += F("<p class='muted'>Warning: could not send PAIR_NODE command to the node.</p>");
    }
  } else {
    html += F("<h3>Node not found or not deployed</h3><p>No action taken.</p>");
  }
  html += F("<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleRoot() {
  String html = headCommon("ESP32 Data Logger");

  // Current RTC time
  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));

  String csvStats = getCSVStats();

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes) {
    if (node.state == DEPLOYED && node.isActive) deployedNodes++;
  }

  // --- Timing & RTC section ---
  html += F("<div class='section' aria-live='polite'>"
            "<h3>⏱ Timing &amp; RTC</h3>"
            "<p class='muted'>Live DS3231 clock plus the wake interval used by nodes.</p>"
            "<div class='row'>");

  // Left: RTC
  html += F("<div class='col'>"
              "<strong>Current RTC Time</strong><br>"
              "<div id='rtc-now' style='font-size:18px;color:#1976D2;margin-top:6px'>");
  html += currentTime;
  html += F("</div>"
            "<div class='help'>Clock is driven by the DS3231 on this mothership.</div>"
            "</div>");

  // Right: wake interval
  html += F("<div class='col'>"
              "<form action='/set-wake-interval' method='POST'>"
              "<label class='label'><strong>Wake interval (minutes)</strong></label>"
              "<select class='input' name='interval'>");

  for (size_t i = 0; i < kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<option value='");
    html += String(v);
    html += F("'");
    if (v == gWakeIntervalMin) html += F(" selected");
    html += F(">");
    html += String(v);
    html += F("</option>");
  }

  html += F("</select>"
            "<button type='submit' class='btn btn--primary' style='margin-top:8px'>"
            "Broadcast to nodes</button>"
            "<div class='help'>Current default: <strong>");
  html += String(gWakeIntervalMin);
  html += F(" min</strong></div>"
            "</form>"
            "</div>"); // end col

  html += F("</div>"); // end row

  // RTC settings toggle + panel
  html += F("<div style='margin-top:12px'>"
            "<button id='settings-btn' class='btn' type='button' onclick='toggleSettings()'>"
            "⚙️ Set RTC time…"
            "</button>"
            "</div>");

  html += F("<div id='settings-panel' class='section' style='display:none;margin-top:12px'>"
            "<h3>⚙️ RTC Time Configuration</h3>"
            "<p class='muted'>Only needed for initial setup or DS3231 correction.</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set new time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='YYYY-MM-DD HH:MM:SS' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Use browser time</button>"
            "<button type='submit' class='btn btn--success'>Set RTC</button>"
            "</div>"
            "<div class='help'>Example: 2025-11-14 21:05:00</div>"
            "</form>"
            "</div>");

  html += F("</div>"); // end Timing & RTC section

  // --- Data logging section ---
  html += F("<div class='section'>"
            "<h3>📊 Data Logging</h3>"
            "<p class='muted'><strong>Status:</strong> ");
  html += csvStats;
  html += F("</p>"
            "<a href='/download-csv' class='btn btn--success'>⬇️ Download CSV Data</a>"
            "<div class='help'>Downloads all logged sensor data from /datalog.csv.</div>"
            "</div>");

  // --- Node discovery & fleet overview ---
  html += F("<div class='section'>"
            "<h3>📡 Node Discovery &amp; Fleet Overview</h3>"
            "<p class='muted'><strong>Mothership MAC:</strong> ");
  html += getMothershipsMAC();
  html += F("</p>"
            "<div class='stats' style='margin:12px 0'>"
              "<div class='stat'><strong>Deployed</strong><span class='num'>");
  html += String(deployedNodes);
  html += F("</span></div>"
              "<div class='stat'><strong>Paired</strong><span class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div>"
              "<div class='stat'><strong>Unpaired</strong><span class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div>"
            "</div>"
            "<form action='/discover-nodes' method='POST'>"
            "<button type='submit' class='btn btn--primary'>🔍 Discover New Nodes</button>"
            "</form>"
            "<a href='/nodes' class='btn btn--success' style='margin-top:8px'>🧩 Open Node Manager</a>"
            "</div>");

  // --- Footer bar ---
  html += F("<div class='footer-bar'>"
            "<a href='/' class='btn'>🔄 Refresh</a>"
            "<a href='/download-csv' class='btn btn--success'>⬇️ CSV</a>"
            "</div>");

  // Auto-refresh every 15s (dashboard only)
  html += F("<script>setTimeout(()=>location.reload(),15000);</script>");

  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetTime() {
  String dt = server.arg("datetime");
  int yy, mm, dd, hh, mi, ss;
  if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &yy, &mm, &dd, &hh, &mi, &ss) == 6) {
    if (setRTCTime(yy, mm, dd, hh, mi, ss)) {
      String html = headCommon("ESP32 Data Logger");
      html += F("<div class='section center'><h3>SUCCESS: RTC Time Updated</h3><p>New time:<br><strong>");
      html += dt;
      html += F("</strong></p><a href='/' class='btn btn--primary'>Back to Main Page</a></div>");
      html += footCommon();
      server.send(200, "text/html", html);
    } else {
      String html = headCommon("ESP32 Data Logger");
      html += F("<div class='section center'><h3>ERROR: Failed to Set RTC Time</h3><p>Please try again.</p>"
                "<a href='/' class='btn btn--primary'>Try Again</a></div>");
      html += footCommon();
      server.send(500, "text/html", html);
    }
  } else {
    String html = headCommon("ESP32 Data Logger");
    html += F("<div class='section center'><h3>WARNING: Invalid Time Format</h3>"
              "<p>Please use the format: YYYY-MM-DD HH:MM:SS</p><p>You entered: <em>");
    html += dt;
    html += F("</em></p><a href='/' class='btn btn--primary'>Try Again</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
  }
}

void handleDownloadCSV() {
  File file = SD.open("/datalog.csv");
  if (!file) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=datalog.csv");
  server.sendHeader("Connection", "close");
  server.streamFile(file, "text/csv");
  file.close();
  Serial.println("✅ CSV file downloaded by client");
}

void handleDiscoverNodes() {
  Serial.println("🔍 Starting node discovery...");
  sendDiscoveryBroadcast();

  String html = headCommon("ESP32 Data Logger");
  html += F("<meta http-equiv='refresh' content='3;url=/'>");
  html += F("<div class='section center'><h3>🔍 Discovery Broadcast Sent</h3>"
            "<div class='muted'>Searching for new sensor nodes...</div>"
            "<div style='margin:16px auto;width:40px;height:40px;border-radius:50%;"
            "border:4px solid #eee;border-top-color:#2196F3;animation:spin 1s linear infinite'></div>"
            "<style>@keyframes spin{0%{transform:rotate(0)}100%{transform:rotate(360deg)}}</style>"
            "<p class='muted'><small>Redirecting back to dashboard in 3 seconds…</small></p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetWakeInterval() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = false;
  for (size_t i = 0; i < kAllowedCount; ++i)
    if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 5;

  bool sent = broadcastWakeInterval(interval);

  gWakeIntervalMin = interval;
  saveWakeIntervalToNVS(interval);

  Serial.printf("[UI] Wake interval set to %d min → broadcast %s\n", interval, sent ? "SENT" : "NOT_SENT");

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>⏰ Wake Interval</h3><p>Broadcasted ");
  html += String(interval);
  html += F(" min to nodes.</p><p style='color:#666'>");
  html += sent ? F("At least one node accepted the packet.") : F("No eligible nodes (PAIRED/DEPLOYED) were found.");
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}


// ---------- Configure & Start: form and handler ----------

void handleNodeConfigForm() {
  String nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  String html = headCommon("Configure Node");
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3>"
              "<p class='muted'>No node with that ID is currently registered.</p>"
              "<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  String userId  = getNodeUserId(target->nodeId);
  String name    = getNodeName(target->nodeId);

  const char* stateLabel = "Unknown";
  if (target->state == UNPAIRED) stateLabel = "Unpaired";
  else if (target->state == PAIRED) stateLabel = "Paired";
  else if (target->state == DEPLOYED) stateLabel = "Deployed";

  html += F("<h3>⚙️ Configure &amp; Start</h3>"
            "<p class='muted'>Set a numeric Node ID and a descriptive name, then start or stop the node.</p>");

  html += F("<p><strong>Firmware ID:</strong> ");
  html += target->nodeId;
  html += F("<br><strong>Current state:</strong> ");
  html += stateLabel;
  html += F("</p>");

  html += F("<form action='/node-config' method='POST'>"
            "<input type='hidden' name='node_id' value='");
  html += target->nodeId;
  html += F("'>"

            "<label class='label'>Node ID (numeric, e.g. 001)</label>"
            "<input class='input' type='text' name='user_id' maxlength='3' "
            "placeholder='001' value='");
  html += userId;
  html += F("'>"

            "<label class='label'>Name</label>"
            "<input class='input' type='text' name='name' "
            "placeholder='e.g. North Hedge 01' value='");
  html += name;
  html += F("'>"

            "<label class='label'>Interval (minutes)</label>"
            "<select class='input' name='interval'>");

  for (size_t i = 0; i < kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<option value='");
    html += String(v);
    html += F("'");
    if (v == gWakeIntervalMin) html += F(" selected");
    html += F(">");
    html += String(v);
    html += F("</option>");
  }

  html += F("</select>"

            "<label class='label'>Action</label>"
            "<div class='row'>"
              "<label style='flex:1'><input type='radio' name='action' value='start' checked> Start / deploy</label>"
              "<label style='flex:1'><input type='radio' name='action' value='stop'> Stop / keep paired</label>"
              "<label style='flex:1'><input type='radio' name='action' value='unpair'> Unpair / forget</label>"
            "</div>"
            "<button type='submit' class='btn btn--success' style='margin-top:12px'>"
            "Apply &amp; send</button>"

            "</form>"
           "<div class='help'>"
            "If the node is unpaired, <em>Start</em> will attempt to pair then deploy. "
            "If it is deployed, <em>Stop</em> will revert it to the paired state. "
            "<em>Unpair</em> will forget this node on the mothership and tell the node to reset itself."
            "</div>");

  html += F("<a href='/nodes' class='btn' style='margin-top:12px'>↩️ Back to Node Manager</a>");
  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodeConfigSave() {
  String nodeId   = server.arg("node_id");   // firmware ID
  String userId   = server.arg("user_id");   // numeric
  String name     = server.arg("name");      // friendly name
  String action   = server.arg("action");
  int interval    = server.hasArg("interval") ? server.arg("interval").toInt() : gWakeIntervalMin;

  // --- 1) Persist numeric ID + name to NVS ---
  setNodeUserId(nodeId, userId);
  setNodeName(nodeId, name);

  // --- 2) Clamp interval ---
  bool intervalOk = false;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) {
      intervalOk = true;
      break;
    }
  }
  if (!intervalOk) interval = gWakeIntervalMin;

  // --- 3) Find node entry ---
  NodeInfo* target = nullptr;
  for (auto &n : registeredNodes) {
    if (n.nodeId == nodeId) {
      target = &n;
      break;
    }
  }

  // --- 4) Apply interval (fleet-wide for now) ---
  bool scheduleSent = broadcastWakeInterval(interval);
  gWakeIntervalMin  = interval;
  saveWakeIntervalToNVS(interval);
  Serial.printf("[CONFIG] Interval via Configure & Start set to %d min, broadcast=%s\n",
                interval, scheduleSent ? "OK" : "NO_ELIGIBLE_NODES");

  bool deployOk = false;
  bool revertOk = false;
  bool pairOk   = false;
  bool unpairOk = false;

  // --- 5) Start / stop logic ---
  if (action == "start") {
    if (target && target->state == UNPAIRED) {
      pairOk = pairNode(nodeId);
      if (pairOk) {
        target->state = PAIRED;
        savePairedNodes();
        Serial.printf("[CONFIG] Node %s paired before deployment\n", nodeId.c_str());
      } else {
        Serial.printf("[CONFIG] Node %s failed to pair\n", nodeId.c_str());
      }
    }

    std::vector<String> ids;
    ids.push_back(nodeId);
    deployOk = deploySelectedNodes(ids);
    Serial.printf("[CONFIG] Start action for %s → deploySelectedNodes: %s\n",
                  nodeId.c_str(), deployOk ? "OK" : "FAIL");

  } else if (action == "stop") {
    if (target && target->state == DEPLOYED) {
      target->state = PAIRED;
      savePairedNodes();
      revertOk = pairNode(nodeId);
      Serial.printf("[CONFIG] Stop action for %s → revert to PAIRED: %s\n",
                    nodeId.c_str(), revertOk ? "OK" : "FAIL");
    }
  }  else if (action == "unpair") {
  if (target) {
    // 1) Tell the node to reset its own state (best-effort)
    bool sent  = sendUnpairToNode(nodeId);

    // 2) Locally unpair (delete peer, set UNPAIRED, persist)
    bool local = unpairNode(nodeId);

    unpairOk = sent && local;

    // 3) Clear user-facing metadata
    setNodeUserId(nodeId, "");  // removes id_<nodeId> key
    setNodeName(nodeId, "");    // removes name_<nodeId> key
    target->userId = "";
    target->name   = "";

    // 4) Log an UNPAIR event to CSV
    char timeBuffer[24];
    getRTCTimeString(timeBuffer, sizeof(timeBuffer));
    String csvRow = String(timeBuffer) + ",MOTHERSHIP," +
                    getMothershipsMAC() + ",UNPAIR," + nodeId;
    logCSVRow(csvRow);

    Serial.printf("[CONFIG] Unpair action for %s → send=%s, local=%s\n",
                  nodeId.c_str(),
                  sent  ? "OK" : "FAIL",
                  local ? "OK" : "FAIL");
  } else {
    Serial.printf("[CONFIG] Unpair action requested for %s but node not found\n",
                  nodeId.c_str());
  }
}


  // --- 6) Resolve final values for display + NodeInfo ---
  // Read back from NVS, but fall back to raw form input if empty
  String finalUserId = getNodeUserId(nodeId);
  String finalName   = getNodeName(nodeId);

  if (finalUserId.isEmpty()) finalUserId = userId;
  if (finalName.isEmpty())   finalName   = name;

  // If NodeInfo has userId / name fields, keep them in sync
  if (target) {
    target->userId = finalUserId;
    target->name   = finalName;
  }

  // --- 7) Feedback page ---
  String html = headCommon("Configure Node");
  html += F("<div class='section center'>"
            "<h3>Node configuration applied</h3>");

  if (!target) {
    html += F("<p class='muted'>Warning: this node ID is not currently in the registered list. "
              "Commands may not have reached any device.</p>");
  }

  html += F("<p><strong>Firmware ID:</strong> ");
  html += nodeId;
  html += F("<br><strong>Node ID (numeric):</strong> ");
  html += (finalUserId.length() ? finalUserId : String("-"));
  html += F("<br><strong>Name:</strong> ");
  html += (finalName.length() ? finalName : String("-"));
  html += F("<br><strong>Interval:</strong> ");
  html += String(interval);
  html += F(" min<br><strong>Action:</strong> ");
  html += action;
  html += F("</p>");

  html += F("<p class='muted'>"
            "Schedule broadcast: ");
  html += scheduleSent ? "OK" : "no eligible PAIRED/DEPLOYED nodes";
  html += F("<br>Pair (if unpaired): ");
  html += pairOk ? "OK" : "not requested / failed";
  html += F("<br>Start / deploy: ");
  html += deployOk ? "OK" : "not requested / failed";
  html += F("<br>Stop / revert: ");
  html += revertOk ? "OK" : "not requested / failed";
  html += F("<br>Unpair / forget: ");
  html += unpairOk ? "OK" : "not requested / failed";
  html += F("</p>"
            "<a href='/nodes' class='btn btn--primary'>Back to Node Manager</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}


void handleNodesPage() {
  String html = headCommon("Node Manager");

  auto allNodes = getRegisteredNodes();

  html += F("<div class='section'>"
            "<h3>🧩 Node Manager</h3>"
            "<p class='muted'>Tap a node to configure its ID, name, interval and start/stop state.</p>");

  if (allNodes.empty()) {
    html += F("<p class='muted'>No nodes registered yet. Try discovering and pairing first.</p>");
  } else {
    html += F("<div class='list'>");

    for (auto &node : allNodes) {
      const char* stateLabel = "Unknown";
      const char* stateClass = "chip";

      if (node.state == UNPAIRED) {
        stateLabel = "Unpaired";
        stateClass = "chip chip--state-unpaired";
      } else if (node.state == PAIRED) {
        stateLabel = "Paired";
        stateClass = "chip chip--state-paired";
      } else if (node.state == DEPLOYED) {
        stateLabel = "Deployed";
        stateClass = "chip chip--state-deployed";
      }

      String userId = getNodeUserId(node.nodeId);  // numeric e.g. "001"
      String name   = getNodeName(node.nodeId);    // free-text

      html += F("<a href='/node-config?node_id=");
      html += node.nodeId;
      html += F("' class='item'>"
                "<div class='item-row'>"
                  "<div>");

      // First line: Node ID (numeric if set) + firmware ID in muted text
      html += F("<strong>");
      if (userId.length()) {
        html += userId;
      } else {
        html += node.nodeId;
      }
      html += F("</strong>");

      html += F("<br><span class='muted'>FW: ");
      html += node.nodeId;
      html += F("</span>");

      // Third line: name (if any)
      if (name.length()) {
        html += F("<br><span class='muted'>");
        html += name;
        html += F("</span>");
      }

      html += F("</div><div><span class='");
      html += stateClass;
      html += F("'>");
      html += stateLabel;
      html += F("</span></div>"
                "</div>"
              "</a>");
    }

    html += F("</div>"); // .list
  }

  html += F("<a href='/' class='btn' style='margin-top:12px'>↩️ Back to Dashboard</a>");
  html += F("</div>"); // .section

  html += footCommon();
  server.send(200, "text/html", html);
}


// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);

  Serial.println("Starting RTC setup...");
  setupRTC();

  Serial.println("Starting SD Card setup...");
  setupSD();

  Serial.println("Starting WiFi AP (AP+STA mode)...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid.c_str(), password, 1); // Force channel 1
  Serial.print("SoftAP IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("Device ID: "); Serial.println(DEVICE_ID);
  Serial.print("WiFi Network: "); Serial.println(ssid);
  Serial.print("Firmware: "); Serial.print(FW_VERSION); Serial.print(" "); Serial.println(FW_BUILD);

  delay(1000);
  Serial.println("Starting ESP-NOW setup...");
  setupESPNOW();

  char timeStr[32];
  getRTCTimeString(timeStr, sizeof(timeStr));
  Serial.print("Current RTC Time: "); Serial.println(timeStr);

  loadWakeIntervalFromNVS();
  Serial.printf("Current wake interval (from NVS): %d min\n", gWakeIntervalMin);

#if ENABLE_SPIFFS_ASSETS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed; falling back to inline assets.");
  } else {
    server.on("/style.css", HTTP_GET, [](){
      File f = SPIFFS.open("/style.css.gz", "r");
      if (!f) { server.send(404, "text/plain", "style.css.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "text/css"); f.close();
    });
    server.on("/app.js", HTTP_GET, [](){
      File f = SPIFFS.open("/app.js.gz", "r");
      if (!f) { server.send(404, "text/plain", "app.js.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "application/javascript"); f.close();
    });
  }
#endif

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);

  // Node manager + config routes
  server.on("/nodes", HTTP_GET, handleNodesPage);
  server.on("/node-config", HTTP_GET, handleNodeConfigForm);
  server.on("/node-config", HTTP_POST, handleNodeConfigSave);
  server.on("/revert-node", HTTP_POST, handleRevertNode); // still available if you use it elsewhere

  server.begin();
  Serial.println("✅ Web server started!");
}

void loop() {
  server.handleClient();
  espnow_loop(); // Handle ESP-NOW node management

  // Print current RTC time every 10s
  static unsigned long lastTimeCheck = 0;
  if (millis() - lastTimeCheck > 10000) {
    char timeBuffer[24];
    getRTCTimeString(timeBuffer, sizeof(timeBuffer));
    Serial.print("Current RTC Time: ");
    Serial.println(timeBuffer);
    lastTimeCheck = millis();
  }

  // Log mothership status every 60s
  static unsigned long lastMothershipLog = 0;
  if (millis() - lastMothershipLog > 60000) {
    char timeBuffer[24];
    getRTCTimeString(timeBuffer, sizeof(timeBuffer));
    // Note: CSV schema for mothership is still the old one – fine for now.
    String csvRow = String(timeBuffer) + ",MOTHERSHIP," + getMothershipsMAC() + ",STATUS,ACTIVE";
    if (logCSVRow(csvRow)) Serial.println("✅ Mothership status logged");
    lastMothershipLog = millis();
  }
}
