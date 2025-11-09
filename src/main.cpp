// ====== ESP-NOW Web UI (refactored + UX upgrades 1–7) ======
// Routes, field names, and core logic unchanged.

// ---------- Config toggles ----------
#define ENABLE_SPIFFS_ASSETS 0  // (5) set to 1 if you upload /style.css.gz and /app.js.gz

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
const int kAllowedIntervals[] = {1,5,10,20,30,60};
const size_t kAllowedCount = sizeof(kAllowedIntervals)/sizeof(kAllowedIntervals[0]);

// ---------- Web server ----------
WebServer server(80);

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
    bool ok=false; for (size_t i=0;i<kAllowedCount;i++) if (v==kAllowedIntervals[i]) { ok=true; break; }
    gWakeIntervalMin = ok ? v : 5;
  }
}

static void saveWakeIntervalToNVS(int mins) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("wake_min", mins);
    gPrefs.end();
  }
}


// (4) Common CSS/JS in PROGMEM (saves RAM)
const char COMMON_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#f5f5f5; --panel:#ffffff; --text:#1b1f23; --sub:#5f6b7a; --border:#e5e7eb;
  --primary:#2196F3; --success:#4CAF50; --warn:#ff9800; --danger:#f44336;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(0,0,0,.08);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent} /* (6) */
html{scroll-behavior:smooth} /* (6) */
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
.item{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px}
.item-row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.meta{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}

/* Forms */
.label{display:block;margin:8px 0 6px;color:var(--sub);font-size:.95rem}
.input, input[type="text"], input[type="number"]{
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
.btn--danger{background:var(--danger);color:#fff;border-color:transparent}
.btn:disabled{opacity:.6;cursor:not-allowed}

/* Utility */
.center{text-align:center}
.badge{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;color:var(--sub);font-size:.85rem}
.card{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);padding:var(--sp-3);box-shadow:var(--shadow)}
.log{background:#111;color:#eaeaea;border-radius:8px;padding:12px;max-height:40vh;overflow:auto;font:13px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace}

/* (3) Safe-area aware footer bar */
.footer-bar{
  position:sticky;bottom:0;background:var(--panel);border:1px solid var(--border);
  padding:calc(var(--sp-2) + env(safe-area-inset-bottom)) var(--sp-3);
  border-radius:12px;box-shadow:var(--shadow);display:flex;gap:12px;justify-content:space-between
}

@media(min-width:768px){.container{max-width:720px}}
)CSS";

const char COMMON_JS[] PROGMEM = R"JS(
// (1) Prevent double submits
document.addEventListener('submit', function (e) {
  const btn = e.target.querySelector('button[type="submit"],input[type="submit"]');
  if (btn && !btn.disabled) {
    btn.disabled = true;
    btn.dataset.originalText = btn.textContent;
    btn.textContent = 'Working…';
  }
}, {capture:true});

// Existing helpers
function setCurrentTime(){
  const n=new Date();
  const z=n=>String(n).padStart(2,'0');
  const s=`${n.getFullYear()}-${z(n.getMonth()+1)}-${z(n.getDate())} ${z(n.getHours())}:${z(n.getMinutes())}:${z(n.getSeconds())}`;
  const el=document.getElementById('datetime'); if(el) el.value=s;
}
function toggleSettings(){
  const panel=document.getElementById('settings-panel');
  const btn=document.getElementById('settings-btn');
  const showing=panel.style.display==='block';
  panel.style.display= showing ? 'none' : 'block';
  if(btn) btn.textContent = showing ? 'Show RTC Settings' : 'Hide RTC Settings';
}
window.onload=setCurrentTime;

// Live 1s ticking of the displayed RTC time (DOM-ready, Safari-safe)
(function(){
  const pad = n => String(n).padStart(2,'0');
  function parseYMDHMS(str){
    // expects "YYYY-MM-DD HH:MM:SS"
    if (!str || str.length < 19) return NaN;
    const y = +str.slice(0,4), m = +str.slice(5,7), d = +str.slice(8,10);
    const H = +str.slice(11,13), M = +str.slice(14,16), S = +str.slice(17,19);
    // Build LOCAL date (avoids Safari ISO quirks)
    const dt = new Date(y, m-1, d, H, M, S);
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
    // If parsing fails, fall back to client clock
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    // Draw immediately, then tick each second
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatYMDHMS(nowMs);
    }
    draw();
    setInterval(draw, 1000);
  }
  // Wait until the DOM exists (script is in <head>)
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
  h.reserve(4500); // (7) a bit bigger reserve for fewer reallocs
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
  // (7) Version/build badge
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

// ---------- Routes / Handlers ----------

// (reused from your version) Revert node
void handleRevertNode() {
  String nodeId = server.arg("node_id");
  bool found = false;
  extern std::vector<NodeInfo> registeredNodes;
  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId && node.state == DEPLOYED) {
      node.state = PAIRED;
      savePairedNodes();
      found = true;
      Serial.print("[MOTHERSHIP] Node reverted to PAIRED state: ");
      Serial.println(nodeId);
      break;
    }
  }

  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section center'>");
  if (found) {
    html += F("<h3>Node reverted to paired state</h3><p>Node <strong>");
    html += nodeId;
    html += F("</strong> is now paired and can be redeployed.</p>");
  } else {
    html += F("<h3>Node not found or not deployed</h3><p>No action taken.</p>");
  }
  html += F("<a href='/' class='btn btn--primary'>Back to Dashboard</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleRoot() {
  String html = headCommon("ESP32 Data Logger");

  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));
    // Wake Interval control
  html += F("<div class='section'><h3>⏰ Wake Interval</h3>");
  html += F("<p class='muted'>This sets the DS3231 alarm interval on all paired/deployed nodes.</p>");
  html += F("<form action='/set-wake-interval' method='POST' class='row'>"
            "<div class='col'><label class='label'><strong>Interval (minutes)</strong></label>"
            "<select class='input' name='interval'>");

  // build <option> list with current selected
  for (size_t i=0; i<kAllowedCount; ++i) {
    int v = kAllowedIntervals[i];
    html += F("<option value='");
    html += String(v);
    html += "'";
    if (v == gWakeIntervalMin) html += F(" selected");
    html += F(">");
    html += String(v);
    html += F("</option>");
  }
  html += F("</select></div>"
            "<div class='col' style='align-self:end'><button type='submit' class='btn btn--primary'>Broadcast</button></div>"
            "</form>");

  html += F("<div class='help'>Current interval: <strong>");
  html += String(gWakeIntervalMin);
  html += F(" min</strong></div></div>");

  String csvStats = getCSVStats();

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes)
    if (node.state == DEPLOYED && node.isActive) deployedNodes++;

  // RTC section
 
    html += F(
    "<div class='section center' aria-live='polite'>"
        "<strong>Current RTC Time</strong><br>"
        "<div id='rtc-now' style='font-size:18px;color:#1976D2;margin-top:6px'>"
    );
    html += currentTime; // "YYYY-MM-DD HH:MM:SS"
    html += F("</div></div>");


  // Data logging
  html += F("<div class='section'><h3>📊 Data Logging</h3><p class='muted'><strong>Status:</strong> ");
  html += csvStats;
  html += F("</p><a href='/download-csv' class='btn btn--success'>⬇️ Download CSV Data</a>"
            "<div class='help'>Downloads all logged sensor data</div></div>");

  // Discovery + stats
  html += F("<div class='section'><h3>📡 Node Discovery &amp; Pairing</h3><p class='muted'><strong>Mothership MAC:</strong> ");
  html += getMothershipsMAC();
  html += F("</p><div class='stats' style='margin:12px 0'>"
            "<div class='stat'><strong>Deployed</strong><span class='num'>");
  html += String(deployedNodes);
  html += F("</span></div><div class='stat'><strong>Paired</strong><span class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div><div class='stat'><strong>Unpaired</strong><span class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div></div>"
            "<form action='/discover-nodes' method='POST'>"
            "<button type='submit' class='btn btn--primary'>🔍 Discover New Nodes</button>"
            "</form></div>");

  // Unpaired
  if (unpairedNodes.size() > 0) {
    html.reserve(html.length() + 500 + unpairedNodes.size()*140); // (7) reserve bump
    html += F("<div class='section'><h3>🔴 Unpaired Nodes (");
    html += String(unpairedNodes.size());
    html += F(")</h3><form action='/pair-nodes' method='POST' class='list'>");
    for (const auto& node : unpairedNodes) {
      html += F("<label class='item item-row'><div style='display:flex;align-items:center;gap:10px'>"
                "<input type='checkbox' name='selected_nodes' value='");
      html += node.nodeId;
      html += F("' aria-label='Select ");
      html += node.nodeId;
      html += F("'><div><strong>");
      html += node.nodeId;
      html += F("</strong> <span class='muted'>(");
      html += node.nodeType;
      html += F(")</span><br><small class='muted'>MAC ");
      html += formatMac(node.mac);
      html += F(" • Seen ");
      html += String((millis() - node.lastSeen) / 1000);
      html += F(" s ago</small></div></div></label>");
    }
    html += F("<button type='submit' class='btn btn--warn'>📋 Pair Selected Nodes</button></form></div>");
  }

  // Paired -> Deploy
  if (pairedNodes.size() > 0) {
    html.reserve(html.length() + 500 + pairedNodes.size()*110); // (7)
    html += F("<div class='section'><h3>🟠 Paired Nodes (");
    html += String(pairedNodes.size());
    html += F(")</h3><p class='muted'>Ready for deployment with RTC sync</p>"
              "<form action='/deploy-nodes' method='POST' class='list'>");
    for (const auto& node : pairedNodes) {
      html += F("<label class='item item-row'><div style='display:flex;align-items:center;gap:10px'>"
                "<input type='checkbox' name='deploy_nodes' value='");
      html += node.nodeId;
      html += F("' checked><div><strong>");
      html += node.nodeId;
      html += F("</strong> <span class='muted'>(");
      html += node.nodeType;
      html += F(")</span></div></div></label>");
    }
    html += F("<button type='submit' class='btn btn--success'>🚀 Deploy Selected Nodes</button></form></div>");
  }

  // Unpair
  if (pairedNodes.size() > 0) {
    html += F("<div class='section'><h3>Unpair Nodes</h3>"
              "<form action='/unpair-nodes' method='POST' class='list'>");
    for (const auto& node : pairedNodes) {
      html += F("<label class='item item-row'><div style='display:flex;align-items:center;gap:10px'>"
                "<input type='checkbox' name='unpair_nodes' value='");
      html += node.nodeId;
      html += F("'><div><strong>");
      html += node.nodeId;
      html += F("</strong> <span class='muted'>— ");
      html += node.nodeType;
      html += F("</span></div></div></label>");
    }
    html += F("<button type='submit' class='btn btn--danger'>🗑️ Unpair Selected</button></form></div>");
  }

  // Active deployed + revert
  if (deployedNodes > 0) {
    html.reserve(html.length() + 500 + deployedNodes*130); // (7)
    html += F("<div class='section'><h3>🟢 Active Deployed Nodes (");
    html += String(deployedNodes);
    html += F(")</h3><p class='muted'>Nodes are collecting data and sending to mothership</p>"
              "<div class='list'>");
    for (const auto& node : allNodes) {
      if (node.state == DEPLOYED && node.isActive) {
        html += F("<div class='item'><div class='item-row'><div><strong>");
        html += node.nodeId;
        html += F("</strong> <span class='muted'>(");
        html += node.nodeType;
        html += F(")</span><br><small class='muted'>MAC ");
        html += formatMac(node.mac);
        html += F("</small></div><form action='/revert-node' method='POST'>"
                  "<input type='hidden' name='node_id' value='");
        html += node.nodeId;
        html += F("'><button class='btn btn--primary' type='submit'>↩️ Revert to Paired</button>"
                  "</form></div></div>");
      }
    }
    html += F("</div></div>");
  }

  // Settings
  html += F("<button id='settings-btn' class='btn' onclick='toggleSettings()'>Show RTC Settings</button>"
            "<div id='settings-panel' class='section' style='display:none'>"
            "<h3>⚙️ RTC Time Configuration</h3>"
            "<p class='muted'>Only needed for initial setup or time correction</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set New Time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='YYYY-MM-DD HH:MM:SS' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Auto-Detect Current Time</button>"
            "<button type='submit' class='btn btn--success'>Set RTC Time</button>"
            "</div><div class='help'>Format: 2025-11-09 14:26:00</div>"
            "</form></div>");

  // (3) Safe-area footer bar replacing single refresh button
  html += F("<div class='footer-bar'>"
            "<a href='/' class='btn'>🔄 Refresh</a>"
            "<a href='/download-csv' class='btn btn--success'>⬇️ CSV</a>"
            "</div>");

  // (2) Auto-refresh every 15s (dashboard only)
  html += F("<script>setTimeout(()=>location.reload(),10000);</script>");

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
  if (!file) { server.send(404, "text/plain", "CSV file not found"); return; }
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

void handlePairNodes() {
  int pairedCount = 0;
  String pairedList;

  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "selected_nodes") {
      String id = server.arg(i);
      if (pairNode(id)) { pairedCount++; pairedList += id + ", "; }
    }
  }

  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section center'>");
  if (pairedCount > 0) {
    html += F("<h3>📋 Nodes Paired Successfully</h3><p><strong>");
    html += String(pairedCount);
    html += F(" node(s) paired</strong></p><p>");
    if (pairedList.length() >= 2) pairedList.remove(pairedList.length()-2);
    html += pairedList;
    html += F("</p><p>Nodes are ready for deployment with RTC synchronization</p>");
  } else {
    html += F("<h3>⚠️ No Nodes Selected</h3><p>Please select at least one node to pair.</p>");
  }
  html += F("<a href='/' class='btn btn--primary'>Back to Dashboard</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleDeployNodes() {
  std::vector<String> selectedNodes;
  for (int i = 0; i < server.args(); i++)
    if (server.argName(i) == "deploy_nodes") selectedNodes.push_back(server.arg(i));

  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section center'>");

  if (!selectedNodes.empty()) {
    char timeStr[32];
    getRTCTimeString(timeStr, sizeof(timeStr));

    if (deploySelectedNodes(selectedNodes)) {
      html += F("<h3>🚀 Deployment Successful</h3><p><strong>");
      html += String(selectedNodes.size());
      html += F(" node(s) deployed:</strong></p>");
      for (const auto& id : selectedNodes) {
        html += F("<p>✅ "); html += id; html += F("</p>");
      }
      html += F("<p>RTC Time synchronized: <strong>");
      html += timeStr;
      html += F("</strong></p><p>Nodes are now collecting data automatically</p>");
    } else {
      html += F("<h3>⚠️ Partial Deployment</h3><p>Some nodes may not have deployed successfully.</p>"
                "<p>Check serial monitor for details.</p>");
    }
  } else {
    html += F("<h3>⚠️ No Nodes Selected</h3><p>Please select at least one node to deploy.</p>");
  }

  html += F("<a href='/' class='btn btn--primary'>Back to Dashboard</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleUnpairNodes() {
  int removed = 0;
  String html = headCommon("ESP32 Data Logger");
  html += F("<div class='section'><h3>Unpair Results</h3><div class='list'>");

  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "unpair_nodes") {
      String nid = server.arg(i);
      bool sendOk = sendUnpairToNode(nid);
      bool removedLocal = unpairNode(nid);

      html += F("<div class='item'><strong>");
      html += nid;
      html += F("</strong><br>");
      html += sendOk
        ? F("<span class='chip' style='border-color:#cce5cc;color:#2e7d32'>Remote UNPAIR sent</span> ")
        : F("<span class='chip' style='border-color:#f5c6cb;color:#b71c1c'>Remote UNPAIR failed</span> ");
      if (removedLocal) { html += F("<span class='chip' style='border-color:#cce5cc;color:#2e7d32'>Locally unpaired</span>"); removed++; }
      else              { html += F("<span class='chip' style='border-color:#f5c6cb;color:#b71c1c'>Failed to locally unpair</span>"); }
      html += F("</div>");
    }
  }

  if (removed == 0) html += F("<div class='muted'>⚠️ No nodes were locally unpaired.</div>");
  html += F("</div><a href='/' class='btn btn--primary' style='margin-top:12px'>Back to Dashboard</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void handleSetWakeInterval() {
  // read & validate input
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = false;
  for (size_t i=0; i<kAllowedCount; ++i) if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 5;

  // broadcast to nodes
  bool sent = broadcastWakeInterval(interval);

  // persist & update in-memory current value
  gWakeIntervalMin = interval;
  saveWakeIntervalToNVS(interval);

  Serial.printf("[UI] Wake interval set to %d min → broadcast %s\n", interval, sent ? "SENT" : "NOT_SENT");

  // simple result page
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
// Optional: rebroadcast at boot
// broadcastWakeInterval(gWakeIntervalMin);


#if ENABLE_SPIFFS_ASSETS
  // (5) Optional: serve pre-gzipped assets
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed; falling back to inline assets.");
  } else {
    // /style.css -> serve /style.css.gz with gzip header
    server.on("/style.css", HTTP_GET, [](){
      File f = SPIFFS.open("/style.css.gz", "r");
      if (!f) { server.send(404, "text/plain", "style.css.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "text/css"); f.close();
    });
    // /app.js -> serve /app.js.gz with gzip header
    server.on("/app.js", HTTP_GET, [](){
      File f = SPIFFS.open("/app.js.gz", "r");
      if (!f) { server.send(404, "text/plain", "app.js.gz not found"); return; }
      server.sendHeader("Content-Encoding", "gzip");
      server.streamFile(f, "application/javascript"); f.close();
    });
  }
#endif

  // Routes
  server.on("/", handleRoot);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/pair-nodes", HTTP_POST, handlePairNodes);
  server.on("/deploy-nodes", HTTP_POST, handleDeployNodes);
  server.on("/revert-node", HTTP_POST, handleRevertNode);
  server.on("/unpair-nodes", HTTP_POST, handleUnpairNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);


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
    String csvRow = String(timeBuffer) + ",MOTHERSHIP," + getMothershipsMAC() + ",STATUS,ACTIVE";
    if (logCSVRow(csvRow)) Serial.println("✅ Mothership status logged");
    lastMothershipLog = millis();
  }
}
