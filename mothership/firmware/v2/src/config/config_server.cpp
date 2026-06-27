// Config-mode WiFi AP + web server for Mothership V1.
// Ported from production main.cpp — adapted to use V1's rtc_alarm.h and
// flash_logger.h instead of production's rtc_manager.h / sd_manager.h.

#include "config/config_server.h"
#include "config/node_registry.h"
#include "config/transmission_settings.h"
#include "storage/upload_queue.h"
#include "comms/modem_driver.h"
#include "comms/espnow_config.h"
#include "time/rtc_alarm.h"
#include "storage/flash_logger.h"
#include "system/power.h"
#include "system/pins.h"
#include "protocol.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <RTClib.h>
#include <vector>

// ---------------------------------------------------------------------------
// Device identification and WiFi
// ---------------------------------------------------------------------------
const char* DEVICE_ID = "001";
static const char* BASE_SSID = "Logger";
static String ssid = String(BASE_SSID) + String(DEVICE_ID);  // "Logger001"
static const char* password = "logger123";
const char* FW_VERSION = "v1.0.0";
const char* FW_BUILD   = __DATE__ " " __TIME__;

// ---------------------------------------------------------------------------
// Sync globals + NVS
// ---------------------------------------------------------------------------
Preferences gPrefs;

int gWakeIntervalMin = 0;
static constexpr int kSyncFillK = 18;
int computeAutoSyncMin(int wakeMin) {
  return (wakeMin > 0) ? (wakeMin * kSyncFillK) : 0;
}
int gSyncIntervalMin = 0;
int gSyncDailyHour = 6;
int gSyncDailyMinute = 0;
long gLastSyncBroadcastEpochDay = -1;
int gSyncMode = SYNC_MODE_INTERVAL;
unsigned long gLastSyncBroadcastMs = 0;
unsigned long gLastSyncBroadcastUnix = 0;
long long gLastSyncIntervalSlot = -1;
const int kAllowedIntervals[] = {1, 5, 10, 20, 30, 60};
const size_t kAllowedCount = sizeof(kAllowedIntervals) / sizeof(kAllowedIntervals[0]);

struct __attribute__((packed)) SyncAnchorRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;
  uint32_t phaseUnix;
  uint16_t intervalMin;
  uint8_t mode;
  uint8_t reserved;
  uint32_t crc;
};

static constexpr uint32_t kSyncAnchorMagic = 0x53594E43UL;
static constexpr uint16_t kSyncAnchorVersion = 1;
static constexpr uint32_t kMinValidPhaseUnix = 1704067200UL;  // 2024-01-01 UTC
static constexpr const char* kSyncAnchorA = "sync_anchor_a";
static constexpr const char* kSyncAnchorB = "sync_anchor_b";

static uint32_t syncAnchorCrc(const SyncAnchorRecord& record) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(&record);
  const size_t length = offsetof(SyncAnchorRecord, crc);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

static bool syncAnchorIntervalValid(uint16_t interval, uint8_t mode) {
  if (mode == SYNC_MODE_DAILY && interval == 0) return true;
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == static_cast<uint16_t>(kAllowedIntervals[i] * kSyncFillK)) return true;
  }
  return interval == 60;  // conservative first-boot/default rescue interval
}

static bool syncAnchorValid(const SyncAnchorRecord& record) {
  return record.magic == kSyncAnchorMagic &&
         record.version == kSyncAnchorVersion &&
         record.phaseUnix >= kMinValidPhaseUnix &&
         (record.mode == SYNC_MODE_DAILY || record.mode == SYNC_MODE_INTERVAL) &&
         syncAnchorIntervalValid(record.intervalMin, record.mode) &&
         record.crc == syncAnchorCrc(record);
}

static bool readSyncAnchor(Preferences& prefs, const char* key,
                           SyncAnchorRecord& record) {
  if (prefs.getBytesLength(key) != sizeof(record)) return false;
  return prefs.getBytes(key, &record, sizeof(record)) == sizeof(record);
}

static SyncAnchorRecord makeSyncAnchor(uint16_t generation) {
  SyncAnchorRecord record{};
  record.magic = kSyncAnchorMagic;
  record.version = kSyncAnchorVersion;
  record.generation = generation;
  record.phaseUnix = static_cast<uint32_t>(gLastSyncBroadcastUnix);
  record.intervalMin = static_cast<uint16_t>(max(gSyncIntervalMin, 0));
  record.mode = static_cast<uint8_t>(gSyncMode);
  record.crc = syncAnchorCrc(record);
  return record;
}

// ---------------------------------------------------------------------------
// Web server + DNS
// ---------------------------------------------------------------------------
WebServer server(80);
DNSServer dnsServer;

volatile bool gShutdownRequested = false;

// Upload queue instance for config-mode UI (separate from the sync-wake
// global in main.cpp, which is only active during a sync wake).
UploadQueue gUploadQueue;

// ---------------------------------------------------------------------------
// RTC helpers (adapt production's rtc_manager API to V1's rtc_alarm.h)
// ---------------------------------------------------------------------------
static uint32_t getRTCTimeUnix() {
  return getRTCTime();
}

static bool setRTCTime(int year, int month, int day, int hour, int minute, int second) {
  DateTime dt(year, month, day, hour, minute, second);
  setRTCTime(dt.unixtime());
  return true;
}

static const char* kMonthShort[] = {
  "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void getRTCTimeString(char* buf, size_t bufLen) {
  uint32_t u = getRTCTime();
  if (u == 0) {
    snprintf(buf, bufLen, "RTC unset");
    return;
  }
  DateTime dt(u);
  uint8_t m = dt.month();
  if (m < 1) m = 1; if (m > 12) m = 12;
  // Format: "14:32 · 26 Jun 2026"
  snprintf(buf, bufLen, "%02d:%02d \xc2\xb7 %02d %s %04d",
           dt.hour(), dt.minute(), dt.day(), kMonthShort[m], dt.year());
}

static String formatDateTimeDisplay(const DateTime& dt) {
  char b[24];
  uint8_t m = dt.month();
  if (m < 1) m = 1; if (m > 12) m = 12;
  // Format: "14:32 · 26 Jun 2026"
  snprintf(b, sizeof(b), "%02d:%02d \xc2\xb7 %02d %s %04d",
           dt.hour(), dt.minute(), dt.day(), kMonthShort[m], dt.year());
  return String(b);
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
void loadWakeIntervalFromNVS() {
  bool migrated = false;
  if (gPrefs.begin("ui", false)) {
    if (!gPrefs.getBool("wake_v2_init", false)) {
      gPrefs.putInt("wake_min", 5);
      gPrefs.putBool("wake_v2_init", true);
      migrated = true;
    }
    int v = gPrefs.getInt("wake_min", gWakeIntervalMin);
    gPrefs.end();

    bool ok = (v == 0);
    for (size_t i = 0; i < kAllowedCount; i++) {
      if (v == kAllowedIntervals[i]) { ok = true; break; }
    }
    gWakeIntervalMin = ok ? v : 0;
  }
  if (migrated) {
    Serial.println("[UI] wake interval migration applied: defaulted global trigger to 5 min");
  }
}

static void saveWakeIntervalToNVS(int mins) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("wake_min", mins);
    gPrefs.end();
  }
}

void loadSyncModeFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int m = gPrefs.getInt("sync_mode", SYNC_MODE_INTERVAL);
    gPrefs.end();
    gSyncMode = (m == SYNC_MODE_INTERVAL) ? SYNC_MODE_INTERVAL : SYNC_MODE_DAILY;
  }
}

static void saveSyncModeToNVS(int mode) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("sync_mode", mode);
    gPrefs.end();
  }
}

static const char* syncModeLabel() {
  return (gSyncMode == SYNC_MODE_INTERVAL) ? "Interval" : "Daily";
}

void loadDailySyncTimeFromNVS() {
  if (gPrefs.begin("ui", true)) {
    int hh = gPrefs.getInt("sync_hh", gSyncDailyHour);
    int mm = gPrefs.getInt("sync_mm", gSyncDailyMinute);
    gPrefs.end();
    if (hh < 0 || hh > 23) hh = 6;
    if (mm < 0 || mm > 59) mm = 0;
    gSyncDailyHour = hh;
    gSyncDailyMinute = mm;
  }
}

static void saveDailySyncTimeToNVS(int hh, int mm) {
  if (gPrefs.begin("ui", false)) {
    gPrefs.putInt("sync_hh", hh);
    gPrefs.putInt("sync_mm", mm);
    gPrefs.end();
  }
}

void loadSyncRuntimeGuardsFromNVS() {
  if (!gPrefs.begin("ui", false)) return;

  gLastSyncBroadcastEpochDay = gPrefs.getLong("sync_day", -1);
  gLastSyncIntervalSlot = gPrefs.getLong64("sync_slot", -1);

  SyncAnchorRecord anchorA{};
  SyncAnchorRecord anchorB{};
  const bool hasA = readSyncAnchor(gPrefs, kSyncAnchorA, anchorA);
  const bool hasB = readSyncAnchor(gPrefs, kSyncAnchorB, anchorB);
  const bool validA = hasA && syncAnchorValid(anchorA);
  const bool validB = hasB && syncAnchorValid(anchorB);

  const SyncAnchorRecord* selected = nullptr;
  const char* selectedKey = nullptr;
  if (validA && validB) {
    selected = static_cast<int16_t>(anchorB.generation - anchorA.generation) > 0 ?
               &anchorB : &anchorA;
    selectedKey = selected == &anchorB ? kSyncAnchorB : kSyncAnchorA;
  } else if (validA) {
    selected = &anchorA;
    selectedKey = kSyncAnchorA;
  } else if (validB) {
    selected = &anchorB;
    selectedKey = kSyncAnchorB;
  }

  if (selected) {
    gLastSyncBroadcastUnix = selected->phaseUnix;
    gSyncIntervalMin = selected->intervalMin;
    gSyncMode = selected->mode;
    Serial.printf("[SYNC] Loaded anchor %s generation=%u phase=%lu interval=%u mode=%u\n",
                  selectedKey, selected->generation,
                  static_cast<unsigned long>(selected->phaseUnix),
                  selected->intervalMin, selected->mode);
    gPrefs.end();
    return;
  }

  const uint32_t legacyPhase = gPrefs.getULong("sync_last_unix", 0);
  gLastSyncBroadcastUnix = legacyPhase;
  if (legacyPhase >= kMinValidPhaseUnix) {
    Serial.println("[SYNC] Falling back to legacy phase anchor");
    if (!hasA && !hasB) {
      SyncAnchorRecord migrated = makeSyncAnchor(0);
      const size_t written = gPrefs.putBytes(kSyncAnchorA, &migrated, sizeof(migrated));
      Serial.printf("[SYNC] Legacy anchor migration to A: %s\n",
                    written == sizeof(migrated) ? "OK" : "FAILED");
    }
  } else {
    gLastSyncBroadcastUnix = 0;
    Serial.println("[WARN] Phase anchor lost - fleet may desync");
  }
  gPrefs.end();
}

void saveSyncRuntimeGuardsToNVS() {
  if (!gPrefs.begin("ui", false)) {
    Serial.println("[SYNC] Anchor save failed: NVS unavailable");
    return;
  }

  SyncAnchorRecord anchorA{};
  SyncAnchorRecord anchorB{};
  const bool validA = readSyncAnchor(gPrefs, kSyncAnchorA, anchorA) && syncAnchorValid(anchorA);
  const bool validB = readSyncAnchor(gPrefs, kSyncAnchorB, anchorB) && syncAnchorValid(anchorB);

  uint16_t nextGeneration = 0;
  if (validA && validB) {
    const uint16_t newest = static_cast<int16_t>(anchorB.generation - anchorA.generation) > 0 ?
                            anchorB.generation : anchorA.generation;
    nextGeneration = static_cast<uint16_t>(newest + 1U);
  } else if (validA) {
    nextGeneration = static_cast<uint16_t>(anchorA.generation + 1U);
  } else if (validB) {
    nextGeneration = static_cast<uint16_t>(anchorB.generation + 1U);
  }

  SyncAnchorRecord record = makeSyncAnchor(nextGeneration);
  const char* key = (nextGeneration & 1U) == 0 ? kSyncAnchorA : kSyncAnchorB;
  bool verified = false;
  for (int attempt = 1; attempt <= 2 && !verified; ++attempt) {
    const size_t written = gPrefs.putBytes(key, &record, sizeof(record));
    SyncAnchorRecord readBack{};
    verified = written == sizeof(record) && readSyncAnchor(gPrefs, key, readBack) &&
               memcmp(&record, &readBack, sizeof(record)) == 0 && syncAnchorValid(readBack);
    Serial.printf("[SYNC] Anchor save %s generation=%u attempt=%d: %s\n",
                  key, nextGeneration, attempt, verified ? "OK" : "FAILED");
  }

  // Keep legacy guard fields current for downgrade compatibility.
  gPrefs.putLong("sync_day", gLastSyncBroadcastEpochDay);
  gPrefs.putULong("sync_last_unix", gLastSyncBroadcastUnix);
  gPrefs.putLong64("sync_slot", gLastSyncIntervalSlot);
  gPrefs.end();
}

// ---------------------------------------------------------------------------
// Sync schedule computation
// ---------------------------------------------------------------------------
String formatSyncTimeHHMM(int hh, int mm) {
  char b[6];
  snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
  return String(b);
}

static bool parseHHMM(const String& hhmm, int& outHour, int& outMinute) {
  if (hhmm.length() != 5 || hhmm[2] != ':') return false;
  if (!isDigit(hhmm[0]) || !isDigit(hhmm[1]) || !isDigit(hhmm[3]) || !isDigit(hhmm[4])) return false;
  int hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
  int mm = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  outHour = hh;
  outMinute = mm;
  return true;
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix);

String computeNextSyncIsoLocal() {
  const uint32_t nowUnix = getRTCTimeUnix();
  const uint32_t nextUnix = computeNextSyncUnix(nowUnix);
  DateTime next(nextUnix > 0 ? nextUnix : nowUnix);
  return formatDateTimeDisplay(next);
}

static uint32_t computeNextSyncUnix(uint32_t nowUnix) {
  if (nowUnix <= 946684800UL) return 0;

  if (gSyncMode == SYNC_MODE_INTERVAL) {
    const uint32_t periodSec = (uint32_t)max(gSyncIntervalMin, 1) * 60UL;
    const uint32_t anchorUnix = (gLastSyncBroadcastUnix > 0)
      ? gLastSyncBroadcastUnix
      : (nowUnix - (nowUnix % 60UL));
    if (nowUnix < anchorUnix) return anchorUnix;
    const uint32_t elapsedSec = nowUnix - anchorUnix;
    const uint32_t slotsElapsed = elapsedSec / periodSec;
    uint32_t next = anchorUnix + (slotsElapsed + 1UL) * periodSec;
    // Ensure the result is at least one full interval in the future
    // (during upload phase, the next slot boundary may be very close)
    while (next <= nowUnix) {
      next += periodSec;
    }
    return next;
  }

  DateTime now(nowUnix);
  DateTime next(now.year(), now.month(), now.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  if (now.unixtime() >= next.unixtime()) {
    DateTime tomorrow(now.unixtime() + 24UL * 60UL * 60UL);
    next = DateTime(tomorrow.year(), tomorrow.month(), tomorrow.day(), gSyncDailyHour, gSyncDailyMinute, 0);
  }
  return next.unixtime();
}

static String computeNextWakeIsoLocal(int intervalMin, uint32_t nodeLastSeenMs, bool nodeIsActive) {
  if (intervalMin <= 0) return String("n/a");
  const uint32_t nowUnix = getRTCTimeUnix();
  if (nowUnix <= 946684800UL) return String("n/a");

  const uint32_t periodSec = (uint32_t)intervalMin * 60UL;
  uint32_t nextUnix = nowUnix + periodSec;
  bool estimated = true;

  if (nodeLastSeenMs > 0) {
    const uint32_t nowMs = millis();
    const uint32_t ageMs = (nowMs >= nodeLastSeenMs) ? (nowMs - nodeLastSeenMs) : 0;
    const uint32_t ageSec = ageMs / 1000UL;
    const uint32_t anchorUnix = (nowUnix > ageSec) ? (nowUnix - ageSec) : nowUnix;
    nextUnix = anchorUnix + periodSec;
    if (nextUnix <= nowUnix) {
      const uint32_t behind = nowUnix - nextUnix;
      const uint32_t jumps = (behind / periodSec) + 1UL;
      nextUnix += jumps * periodSec;
    }
    estimated = !nodeIsActive;
  }

  DateTime now(nowUnix);
  DateTime next(nextUnix);
  const uint32_t deltaSec = (next.unixtime() > now.unixtime()) ? (next.unixtime() - now.unixtime()) : 0;
  const uint32_t deltaMin = (deltaSec + 59UL) / 60UL;

  String out = formatDateTimeDisplay(next);
  out += " (in ";
  out += String((unsigned long)deltaMin);
  out += " min";
  if (estimated) out += ", est";
  out += ")";
  return out;
}

static bool isAllowedInterval(int interval) {
  for (size_t i = 0; i < kAllowedCount; ++i) {
    if (interval == kAllowedIntervals[i]) return true;
  }
  return false;
}

static String formatBytesUi(uint64_t bytes) {
  char b[24];
  if (bytes < 1024ULL) {
    snprintf(b, sizeof(b), "%lu B", (unsigned long)bytes);
  } else if (bytes < (1024ULL * 1024ULL)) {
    snprintf(b, sizeof(b), "%.1f KB", (double)bytes / 1024.0);
  } else if (bytes < (1024ULL * 1024ULL * 1024ULL)) {
    snprintf(b, sizeof(b), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else {
    snprintf(b, sizeof(b), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  }
  return String(b);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static String jsonEscapeLocal(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') { out += "\\n"; }
    else if (c == '\r') { out += "\\r"; }
    else if (c == '\t') { out += "\\t"; }
    else { out += c; }
  }
  return out;
}

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    switch (c) {
      case '&':  out += F("&amp;"); break;
      case '<':  out += F("&lt;"); break;
      case '>':  out += F("&gt;"); break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default:   out += c;
    }
  }
  return out;
}

static const char* nodeStateToString(int s) {
  switch (s) {
    case UNPAIRED: return "UNPAIRED";
    case PAIRED:   return "PAIRED";
    case DEPLOYED: return "DEPLOYED";
    default:       return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
// CSS / JS (copied verbatim from production main.cpp)
// ---------------------------------------------------------------------------
const char COMMON_CSS[] PROGMEM = R"CSS(
:root{
  --bg:#faf8f4; --panel:#ffffff; --text:#4a4640; --sub:#6b665e; --border:#e8e4dc;
  --primary:#5b7553; --success:#7a9b70; --warn:#c47a5a; --danger:#c45a4a;
  --btn-solid:#5b7553;
  --input-bg:#f5f3ee;
  --input-bg-active:#ede9e2;
  --radius:10px; --sp-1:8px; --sp-2:12px; --sp-3:16px; --sp-4:20px;
  --shadow:0 2px 10px rgba(74,74,72,.12);
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{scroll-behavior:smooth}
html,body{margin:0;padding:0;background:linear-gradient(180deg,#faf8f4 0%, #e8e4dc 100%);color:var(--text);
  font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,system-ui,sans-serif}
a{color:var(--primary);text-decoration:none}
:focus-visible{outline:3px solid rgba(33,150,243,.35);outline-offset:2px}

/* Layout */
.container{max-width:600px;margin:0 auto;padding:var(--sp-3)}
.header{padding:var(--sp-3) 0;text-align:center}
.header-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.header-actions{display:flex;align-items:center;gap:8px}
.h1{font-size:22px;font-weight:700;margin:0}
.top-time{display:flex;align-items:center;justify-content:space-between;gap:10px;background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:14px 16px;margin-bottom:12px;min-height:62px}
.top-time__label{color:var(--sub);font-size:.95rem;font-weight:600}
.top-time__value{font-weight:700;color:#566246;font-size:1.25rem}
.section{background:var(--panel);border:1px solid var(--border);border-radius:var(--radius);
  padding:var(--sp-3);box-shadow:var(--shadow);margin:var(--sp-3) 0}
.section h3{margin:0 0 var(--sp-2);font-size:18px}
.section-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:0 0 var(--sp-2)}
.section-head h3{margin:0}
.muted{color:var(--sub);font-size:.95rem}

/* Stats */
.stats{display:grid;grid-template-columns:1fr 1fr 1fr;gap:var(--sp-1);text-align:center}
.stat{background:#fafafa;border:1px solid var(--border);border-radius:8px;padding:10px}
.stat strong{display:block;font-size:13px;color:var(--sub);margin-bottom:2px}
.stat .num{font-size:18px;font-weight:700}
.stats--kpi .stat{padding:14px}
.stats--kpi .stat strong{font-size:12px;letter-spacing:.02em;text-transform:uppercase}
.stats--kpi .stat .num{font-size:24px}
.stats--kpi .stat--deployed-active{background:rgba(122,155,112,.25);border-color:#7a9b70}
.stats--kpi .stat--paired-active{background:rgba(196,122,90,.25);border-color:#c47a5a}
.stats--kpi .stat--unpaired-active{background:rgba(196,90,74,.20);border-color:#c45a4a}

/* Lists/cards */
.list{display:grid;gap:var(--sp-1)}
.item{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:12px;display:block;color:inherit}
.item-row{display:flex;align-items:center;justify-content:space-between;gap:12px}
.item--node{
  padding:14px 12px;
  min-height:86px;
  cursor:pointer;
  border-width:2px;
  border-color:#c9d0c3;
  background:linear-gradient(180deg,#ffffff 0%, #f4f7ef 100%);
  box-shadow:0 4px 12px rgba(74,74,72,.12);
  transition:transform .12s ease, box-shadow .12s ease, border-color .12s ease, background .12s ease;
}
.item--node:hover{
  border-color:#9faf97;
  box-shadow:0 8px 18px rgba(74,74,72,.18);
  background:linear-gradient(180deg,#ffffff 0%, #eef3e5 100%);
}
.item--node:focus-visible{
  outline:3px solid rgba(79,109,122,.35);
  outline-offset:2px;
}
.item--node:active{
  transform:translateY(1px) scale(.995);
  box-shadow:0 3px 9px rgba(74,74,72,.14);
}
.node-row{display:grid;grid-template-columns:minmax(0,1fr) auto auto;align-items:center;gap:12px}
.node-main{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.node-name{margin-left:10px}
.node-timing{display:grid;grid-template-columns:auto auto;gap:10px}
.node-timing-cell{display:flex;flex-direction:column;align-items:flex-start;gap:3px;min-width:86px}
.node-timing-label{font-size:.72rem;color:var(--sub);font-weight:700;letter-spacing:.02em;text-transform:uppercase}
.node-timing-value{font-size:.86rem}
.node-status{display:grid;grid-template-columns:auto auto;gap:10px}
.node-status-cell{display:flex;flex-direction:column;align-items:flex-start;gap:3px;min-width:86px}
.item--node .chip{font-weight:600}
.item--node .chip{white-space:nowrap}

/* Chips */
.chip{display:inline-block;padding:2px 8px;border-radius:999px;border:1px solid var(--border);font-size:.85rem;color:var(--sub)}
.chip--state-deployed{border-color:#7a9b70;background:rgba(122,155,112,.25);color:#3d5e35}
.chip--state-paired{border-color:#c47a5a;background:rgba(196,122,90,.25);color:#8a4a2e}
.chip--state-unpaired{border-color:#c45a4a;background:rgba(196,90,74,.20);color:#7a2a20}
.chip--link-awake{border-color:#b3e5fc;background:#e1f5fe;color:#01579b}
.chip--link-asleep{border-color:#e1bee7;background:#f3e5f5;color:#4a148c}
.chip--link-offline{border-color:#ffcdd2;background:#ffebee;color:#b71c1c}
.chip--cfg-pending{border-color:#ffe0b2;background:#fff8e1;color:#8a4b00}
.chip--cfg-ok{border-color:#c8e6c9;background:#f1f8e9;color:#256029}
.chip--bat-ok{border-color:#7a9b70;background:rgba(122,155,112,.25);color:#3d5e35}
.chip--bat-med{border-color:#c47a5a;background:rgba(196,122,90,.25);color:#8a4a2e}
.chip--bat-low{border-color:#c45a4a;background:rgba(196,90,74,.20);color:#7a2a20}

/* Forms */
.label{display:block;margin:8px 0 6px;color:var(--sub);font-size:.95rem}
.input, input[type="text"], input[type="number"], select{
  width:100%;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--input-bg)
}
.input:focus, input[type="text"]:focus, input[type="number"]:focus, select:focus{
  background:var(--input-bg-active)
}
.help{color:var(--sub);font-size:.85rem;margin-top:6px}
.row{display:flex;gap:var(--sp-1);flex-wrap:wrap}
.col{flex:1 1 220px;min-width:0}
.subpanel{display:none;margin-top:12px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa}
.action-stack .btn{padding:14px 16px;min-height:52px;font-size:1rem}

/* Buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;
  padding:12px 16px;border-radius:8px;border:1px solid var(--border);background:#fff;color:var(--text);
  cursor:pointer;width:100%;margin-top:8px;text-decoration:none}
.btn--primary,.btn--success,.btn--warn{background:var(--btn-solid);color:#fff;border-color:transparent}
.btn--sm{width:auto;min-height:36px;padding:8px 12px;margin-top:0;font-size:.9rem}
.btn--action{padding:14px 16px;min-height:52px;font-size:1rem}
.btn:disabled{opacity:.6;cursor:not-allowed}
.action-choices{display:grid;gap:8px;margin-top:10px}
.action-choice input{position:absolute;opacity:0;pointer-events:none}
.action-choice span{display:flex;align-items:center;justify-content:center;min-height:46px;padding:10px 12px;border-radius:8px;border:1px solid var(--border);font-weight:700;cursor:pointer;transition:filter .12s ease, transform .12s ease, box-shadow .12s ease}
.action-choice--start span{border-color:#7a9b70;background:#fff;color:#3d5e35}
.action-choice--stop span{border-color:#c47a5a;background:#fff;color:#8a4a2e}
.action-choice--unpair span{border-color:#c45a4a;background:#fff;color:#7a2a20}
.action-choice span:hover{filter:brightness(.98)}
.action-choice input:checked + span{box-shadow:0 0 0 2px rgba(79,109,122,.35) inset;transform:translateY(1px)}
.action-choice--start input:checked + span{background:rgba(122,155,112,.25)}
.action-choice--stop input:checked + span{background:rgba(196,122,90,.25)}
.action-choice--unpair input:checked + span{background:rgba(196,90,74,.20)}
.icon{width:1.2em;height:1.2em;display:inline-block;vertical-align:-0.12em;fill:currentColor}
.quick-row{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:0 0 12px}
.quick-row .btn{margin-top:0;min-height:52px}
.quick-row .subpanel{grid-column:2 / 3;margin-top:0}
.quick-row form{margin:0}
.quick-row form .btn{width:100%}

/* Utility */
.center{text-align:center}
.badge{display:inline-block;padding:2px 8px;border:1px solid var(--border);border-radius:999px;color:var(--sub);font-size:.85rem}

/* Time health pills in Node Manager */
.time-health{
  display:flex;
  flex-direction:column;
  align-items:flex-start;
  gap:2px;
  font-size:.85rem;
}
.health-pill{
  display:inline-flex;
  align-items:center;
  justify-content:center;
  padding:2px 8px;
  border-radius:999px;
  font-size:.75rem;
  font-weight:600;
  color:#ffffff;
  white-space:nowrap;
}
.health-fresh{background:#16a34a;}
.health-ok{background:#f97316;}
.health-stale{background:#dc2626;}
.health-unknown{background:#6b7280;}
.health-subtext{
  font-size:.7rem;
  color:#6b7280;
}

@media(max-width:480px){
  /* Prevent horizontal scroll from any wide content */
  body{overflow-x:hidden}

  /* Quick-action grid: 2 columns on phones instead of 3 */
  .quick-row{grid-template-columns:1fr 1fr}
  .quick-row .subpanel{grid-column:1 / -1}

  /* Node cards: stack name above timing/status */
  .node-row{grid-template-columns:1fr;gap:8px}
  .node-timing,.node-status{grid-template-columns:1fr 1fr;gap:6px}
  .node-timing-cell,.node-status-cell{min-width:0}

  /* Small buttons: meet 44px touch target minimum */
  .btn--sm{min-height:44px;padding:10px 14px}

  /* Reduce section spacing to reduce vertical scrolling */
  .section{margin:10px 0;padding:12px}

  /* Sticky header so navigation stays visible while scrolling */
  .header{position:sticky;top:0;z-index:10;background:var(--bg);padding:10px 0}

  /* KPI stat numbers: prevent text overflow on narrow screens */
  .stats--kpi .stat .num{overflow:hidden;text-overflow:ellipsis}

  /* Top time bar: slightly smaller on phones */
  .top-time{padding:10px 12px;min-height:52px}
  .top-time__value{font-size:1.1rem}
}

@media(min-width:768px){.container{max-width:720px}}
)CSS";

const char COMMON_JS[] PROGMEM = R"JS(
function showUiStatus(message, ok){
  const box = document.getElementById('ui-status');
  if (!box) return;
  box.style.display = 'block';
  box.style.borderColor = ok ? '#16a34a' : '#dc2626';
  box.style.color = ok ? '#166534' : '#991b1b';
  box.textContent = message;
}

function asFormBody(form){
  const data = new FormData(form);
  data.append('ajax', '1');
  return new URLSearchParams(data);
}

async function refreshKpiCards(){
  try {
    const resp = await fetch('/ui-status');
    if (!resp.ok) return;
    const status = await resp.json();

    const fleet = status && status.fleet ? status.fleet : null;
    if (fleet) {
      const deployedEl = document.getElementById('kpi-deployed-num');
      const pairedEl = document.getElementById('kpi-paired-num');
      const unpairedEl = document.getElementById('kpi-unpaired-num');
      if (deployedEl) deployedEl.textContent = String(fleet.deployed ?? deployedEl.textContent);
      if (pairedEl) pairedEl.textContent = String(fleet.paired ?? pairedEl.textContent);
      if (unpairedEl) unpairedEl.textContent = String(fleet.unpaired ?? unpairedEl.textContent);
    }

    const modeEl = document.getElementById('kpi-sync-mode');
    if (modeEl && status && status.syncMode) {
      modeEl.textContent = (status.syncMode === 'interval') ? 'Interval' : 'Daily';
    }

    const activeEl = document.getElementById('kpi-active-sync');
    if (activeEl && status) {
      if (status.syncMode === 'interval') {
        activeEl.textContent = 'Every ' + String(status.syncIntervalMinutes || 0) + ' min';
      } else {
        activeEl.textContent = 'Daily @ ' + String(status.syncDailyTime || '--:--');
      }
    }

    const nextEl = document.getElementById('kpi-next-sync');
    if (nextEl && status && status.nextSyncLocal) {
      nextEl.textContent = status.nextSyncLocal;
    }
  } catch (_) {}
}

function wireAsyncForms(){
  const forms = document.querySelectorAll('form.async-form');
  forms.forEach(form => {
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      const btn = form.querySelector('button[type="submit"],input[type="submit"]');
      const original = btn ? (btn.textContent || btn.value) : '';
      if (btn) {
        btn.disabled = true;
        if (btn.tagName === 'INPUT') btn.value = 'Working...';
        else btn.textContent = 'Working...';
      }

      try {
        const resp = await fetch(form.action, {
          method: 'POST',
          body: asFormBody(form),
          headers: {'Content-Type': 'application/x-www-form-urlencoded'}
        });

        const text = await resp.text();
        let msg = text;
        let ok = resp.ok;
        try {
          const json = JSON.parse(text);
          ok = !!json.ok;
          msg = json.message || (ok ? 'Done' : 'Request failed');
        } catch (_) {}

        showUiStatus(msg, ok);

        if (ok && form.action && form.action.indexOf('/discover-nodes') !== -1) {
          setTimeout(() => location.reload(), 900);
        }

        if (ok && form.action && (
            form.action.indexOf('/set-sync-mode') !== -1 ||
            form.action.indexOf('/set-sync-time') !== -1 ||
            form.action.indexOf('/set-wake-interval') !== -1)) {
          await refreshKpiCards();
        }
      } catch (err) {
        showUiStatus('Request failed: ' + err, false);
      } finally {
        if (btn) {
          btn.disabled = false;
          if (btn.tagName === 'INPUT') btn.value = original;
          else btn.textContent = original;
        }
      }
    });
  });
}

function setCurrentTime(){
  const n=new Date();
  const z=n=>String(n).padStart(2,'0');
  const s=`${z(n.getHours())}:${z(n.getMinutes())}:${z(n.getSeconds())} ${z(n.getDate())}-${z(n.getMonth()+1)}-${n.getFullYear()}`;
  const el=document.getElementById('datetime'); if(el) el.value=s;
}
const MONTH_SHORT=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
function formatHubClock(ms){
  const dt=new Date(ms);
  return `${String(dt.getHours()).padStart(2,'0')}:${String(dt.getMinutes()).padStart(2,'0')} \u00b7 ${String(dt.getDate()).padStart(2,'0')} ${MONTH_SHORT[dt.getMonth()]} ${dt.getFullYear()}`;
}
function toggleSettings(){
  const panel=document.getElementById('settings-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
function toggleGlobalInterval(){
  const panel=document.getElementById('global-interval-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
function toggleInfoPanel(){
  const panel=document.getElementById('info-panel');
  if(!panel) return;
  const showing=panel.style.display==='block';
  panel.style.display = showing ? 'none' : 'block';
}
window.addEventListener('DOMContentLoaded', () => {
  setCurrentTime();
  wireAsyncForms();
});

(function(){
  function parseHubClock(str){
    if (!str) return NaN;
    // Accept "HH:MM · DD Mon YYYY" or legacy "HH:MM:SS DD-MM-YYYY"
    const m = str.match(/^(\d{2}):(\d{2})(?::(\d{2}))?\s*[\u00b7\-]\s*(\d{1,2})\s+([A-Za-z]{3})\s+(\d{4})$/);
    if (m){
      const mon = MONTH_SHORT.indexOf(m[5]);
      if (mon < 0) return NaN;
      const dt = new Date(+m[6], mon, +m[4], +m[1], +m[2], m[3] ? +m[3] : 0);
      return isNaN(dt) ? NaN : dt.getTime();
    }
    if (str.length >= 19){
      const H = +str.slice(0,2), M = +str.slice(3,5), S = +str.slice(6,8);
      const d = +str.slice(9,11), mo = +str.slice(12,14), y = +str.slice(15,19);
      const dt = new Date(y, mo-1, d, H, M, S);
      return isNaN(dt) ? NaN : dt.getTime();
    }
    return NaN;
  }
  function startClock(){
    const el = document.getElementById('rtc-now');
    if (!el) return;
    const initial = (el.textContent || '').trim();
    const rtcMs   = parseHubClock(initial);
    const offset  = isNaN(rtcMs) ? 0 : (rtcMs - Date.now());
    function draw(){
      const nowMs = Date.now() + offset;
      el.textContent = formatHubClock(nowMs);
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

// ---------------------------------------------------------------------------
// Page frame helpers
// ---------------------------------------------------------------------------
static bool isAjaxRequest() {
  return server.hasArg("ajax") && server.arg("ajax") == "1";
}

static void sendAjaxResult(bool ok, const String& message) {
  String body;
  body.reserve(message.length() + 40);
  body += "{\"ok\":";
  body += ok ? "true" : "false";
  body += ",\"message\":\"";
  body += message;
  body += "\"}";
  server.send(ok ? 200 : 400, "application/json", body);
}

static void sendCaptivePortalLanding() {
  if (server.method() == HTTP_OPTIONS || server.method() == HTTP_HEAD) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(204);
    return;
  }
  server.send(200, "text/html",
              "<!doctype html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Logger Portal</title></head><body>"
              "<h2>Logger Portal</h2><p>Open local dashboard:</p>"
              "<p><a href='http://192.168.4.1/'>http://192.168.4.1/</a></p>"
              "</body></html>");
}

static String headCommon(const String& title, const String& actionsHtml = String()) {
  String h;
  h.reserve(4500);
  h += F("<!DOCTYPE html><html><head>");
  h += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  h += F("<style>"); h += FPSTR(COMMON_CSS); h += F("</style>");
  h += F("<script>"); h += FPSTR(COMMON_JS);  h += F("</script>");
  h += F("</head><body><div class='container'>");
  h += F("<div class='header'>");
  h += F("<div class='header-top'>");
  h += F("<div class='h1'>"); h += title; h += F("</div>");
  h += F("<div class='header-actions'>");
  if (actionsHtml.length()) {
    h += actionsHtml;
  } else {
    h += F("<a href='/' class='btn btn--sm'>Refresh</a>");
  }
  h += F("</div>");
  h += F("</div>");
  h += F("</div>");
  return h;
}

static inline String footCommon() { return String(F("</div></body></html>")); }

// ---------------------------------------------------------------------------
// Status JSON (renamed from buildBleStatusDataJson)
// ---------------------------------------------------------------------------
static String buildStatusJson() {
  auto allNodes = getRegisteredNodes();

  size_t deployedCount = 0;
  size_t pendingCount = 0;
  size_t pendingToPairedCount = 0;
  size_t pendingToUnpairedCount = 0;
  for (const auto& n : allNodes) {
    if (n.state == DEPLOYED) deployedCount++;
    if (n.stateChangePending) {
      pendingCount++;
      if (n.pendingTargetState == PENDING_TO_PAIRED) pendingToPairedCount++;
      if (n.pendingTargetState == PENDING_TO_UNPAIRED) pendingToUnpairedCount++;
    }
  }

  const size_t pairedCount = getPairedNodes().size();
  const size_t unpairedCount = getUnpairedNodes().size();

  String json;
  json.reserve(420);
  json += "{";
  json += "\"deviceId\":\"";
  json += DEVICE_ID;
  json += "\",";
  json += "\"firmwareVersion\":\"";
  json += FW_VERSION;
  json += "\",";
  json += "\"firmwareBuild\":\"";
  json += FW_BUILD;
  json += "\",";
  json += "\"rtcUnix\":";
  json += String(getRTCTimeUnix());
  json += ",";
  json += "\"apEnabled\":";
  const wifi_mode_t mode = WiFi.getMode();
  const bool apEnabled = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
  json += apEnabled ? "true" : "false";
  json += ",";
  json += "\"espnowChannel\":";
  json += String(ESPNOW_CHANNEL);
  json += ",";
  json += "\"wakeIntervalMinutes\":";
  json += String(gWakeIntervalMin);
  json += ",";
  json += "\"syncIntervalMinutes\":";
  json += String(gSyncIntervalMin);
  json += ",";
  json += "\"syncDailyTime\":\"";
  json += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
  json += "\",";
  json += "\"syncMode\":\"";
  json += (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily";
  json += "\",";
  json += "\"nextSyncLocal\":\"";
  json += computeNextSyncIsoLocal();
  json += "\",";
  json += "\"fleet\":{";
  json += "\"total\":";
  json += String((unsigned)allNodes.size());
  json += ",";
  json += "\"unpaired\":";
  json += String((unsigned)unpairedCount);
  json += ",";
  json += "\"paired\":";
  json += String((unsigned)pairedCount);
  json += ",";
  json += "\"deployed\":";
  json += String((unsigned)deployedCount);
  json += ",";
  json += "\"pending\":";
  json += String((unsigned)pendingCount);
  json += ",";
  json += "\"pendingToPaired\":";
  json += String((unsigned)pendingToPairedCount);
  json += ",";
  json += "\"pendingToUnpaired\":";
  json += String((unsigned)pendingToUnpairedCount);
  json += "}";

  // Upload subsystem status
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);
  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t flashUsagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  json += ",\"upload\":{";
  json += "\"enabled\":";
  json += txSettings.enabled ? "true" : "false";
  json += ",\"cursorOffset\":";
  json += String(cursor.byteOffset);
  json += ",\"pendingBytes\":";
  json += String(pendingBytes);
  json += ",\"pendingRows\":";
  json += String(pendingRows);
  json += ",\"rowsUploaded\":";
  json += String(cursor.rowsUploaded);
  json += ",\"lastUploadUnix\":";
  json += String(cursor.lastUploadUnix);
  json += ",\"retryCount\":";
  json += String(cursor.retryCount);
  json += ",\"flashUsagePct\":";
  json += String(flashUsagePct);
  json += ",\"flashTotalBytes\":";
  json += String((unsigned long)fsTotal);
  json += ",\"flashUsedBytes\":";
  json += String((unsigned long)fsUsed);
  json += "}";

  json += "}";
  return json;
}

// ---------------------------------------------------------------------------
// Data status section (adapted to use flash_logger instead of SD)
// ---------------------------------------------------------------------------
static String buildDataStatusSectionHtml() {
  bool hasFile = flashIsReady() && LittleFS.exists("/datalog.csv");
  uint32_t records = 0;
  uint64_t fileBytes = 0;
  String lastConfirmedSync = "n/a";

  if (hasFile) {
    File file = LittleFS.open("/datalog.csv", "r");
    if (file) {
      fileBytes = (uint64_t)file.size();
      uint32_t lineNo = 0;
      String lastDataLine;
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        lineNo++;
        if (lineNo == 1) continue;
        records++;
        lastDataLine = line;
      }
      file.close();
      if (lastDataLine.length() > 0) {
        const int comma = lastDataLine.indexOf(',');
        if (comma > 0) {
          lastConfirmedSync = lastDataLine.substring(0, comma);
        }
      }
    } else {
      hasFile = false;
    }
  }

  const uint64_t totalBytes = (uint64_t)LittleFS.totalBytes();
  const uint64_t usedBytes  = (uint64_t)LittleFS.usedBytes();
  const uint64_t freeBytes  = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;

  String out;
  out.reserve(900);
  out += F("<div class='section'><h3>Data status</h3>");

  out += F("<div class='stats' style='margin:0 0 10px 0'>"
           "<div class='stat'><strong>Records</strong><span class='num'>");
  out += String(records);
  out += F("</span></div>"
           "<div class='stat'><strong>CSV size</strong><span class='num' style='font-size:16px'>");
  out += hasFile ? formatBytesUi(fileBytes) : String("n/a");
  out += F("</span></div>"
           "<div class='stat'><strong>Storage free</strong><span class='num' style='font-size:16px'>");
  out += (totalBytes > 0) ? formatBytesUi(freeBytes) : String("n/a");
  out += F("</span></div></div>");

  if (!hasFile) {
    out += F("<p class='muted'>No data recorded yet.</p>");
  }

  if (totalBytes > 0) {
    const uint32_t usedPct = (uint32_t)((usedBytes * 100ULL) / totalBytes);
    out += F("<p class='muted'><strong>Storage used:</strong> ");
    out += String(usedPct);
    out += F("% used (");
    out += formatBytesUi(usedBytes);
    out += F(" of ");
    out += formatBytesUi(totalBytes);
    out += F(")</p>");
  }

  out += F("<p class='muted'><strong>Last collection:</strong> ");
  out += lastConfirmedSync;
  out += F("</p>");

  // Upload status summary
  TransmissionSettings txSettings;
  loadTransmissionSettings(txSettings);
  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingRows = gUploadQueue.getPendingRows();
  }
  out += F("<p class='muted'><strong>Cloud upload:</strong> ");
  out += txSettings.enabled ? String("enabled") : String("disabled");
  out += F(" &nbsp;|&nbsp; <strong>Readings waiting:</strong> ");
  out += String(pendingRows);
  out += F(" &nbsp;|&nbsp; <strong>Readings sent:</strong> ");
  out += String(cursor.rowsUploaded);
  if (cursor.lastUploadUnix > 0) {
    DateTime lastUp(cursor.lastUploadUnix);
    out += F(" &nbsp;|&nbsp; <strong>Last upload:</strong> ");
    out += formatDateTimeDisplay(lastUp);
  }
  out += F("</p>");

  out += F("<p class='muted'>Use the Export Data page to download a CSV, or <a href='/settings'>Settings</a> to configure cloud upload.</p></div>");
  return out;
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
  String html = headCommon("fieldMesh");

  char currentTime[24];
  getRTCTimeString(currentTime, sizeof(currentTime));

  auto allNodes      = getRegisteredNodes();
  auto unpairedNodes = getUnpairedNodes();
  auto pairedNodes   = getPairedNodes();

  int deployedNodes = 0;
  for (const auto& node : allNodes) {
    if (node.state == DEPLOYED) deployedNodes++;
  }

  // --- Mothership time bar ---
  html += F("<div class='top-time'><span class='top-time__label'>Mothership time</span><span id='rtc-now' class='top-time__value'>");
  html += currentTime;
  html += F("</span></div>");

  // --- Set time (collapsed, near the time display) ---
  html += F("<details style='margin:4px 0 12px 0'>"
            "<summary style='font-size:14px;color:var(--sub);cursor:pointer;padding:4px 8px'>Set time</summary>"
            "<div style='margin-top:8px;padding:12px;border:1px solid var(--border);border-radius:8px;background:var(--panel)'>"
            "<p class='muted' style='margin:0 0 8px 0'>Only needed for initial setup or clock correction.</p>"
            "<form action='/set-time' method='POST'>"
            "<label class='label' for='datetime'><strong>Set new time</strong></label>"
            "<input class='input' id='datetime' name='datetime' type='text' "
            "placeholder='HH:MM:SS DD-MM-YYYY' inputmode='numeric' autocomplete='off'>"
            "<div class='row'>"
            "<button type='button' class='btn' onclick='setCurrentTime()'>Use browser time</button>"
            "<button type='submit' class='btn btn--success'>Set Time</button>"
            "</div>"
            "<div class='help'>Example: 21:05:00 14-11-2026</div>"
            "</form>"
            "</div>"
            "</details>");

  // --- Quick actions: Find New Nodes + Manage nodes ---
  html += F("<div class='section action-stack' style='margin:0 0 12px 0'>"
            "<form class='async-form' action='/find-stations' method='POST' style='margin:0'>"
            "<button type='submit' class='btn btn--primary' style='width:100%'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3c-3.9 0-7.4 1.6-9.9 4.1l1.4 1.4C5.6 6.4 8.7 5 12 5s6.4 1.4 8.5 3.5l1.4-1.4C19.4 4.6 15.9 3 12 3zm0 5c-2.6 0-5 .9-6.9 2.6l1.4 1.4C8 10.5 9.9 10 12 10s4 .5 5.5 2l1.4-1.4C17 8.9 14.6 8 12 8zm0 5c-1.3 0-2.5.5-3.5 1.5l1.4 1.4c.6-.6 1.3-.9 2.1-.9s1.5.3 2.1.9l1.4-1.4c-1-1-2.2-1.5-3.5-1.5zm0 4a2 2 0 100 4 2 2 0 000-4z'/></svg> Find New Nodes</button>"
            "</form>"
            "<a href='/stations' class='btn btn--success'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 2a4 4 0 110 8 4 4 0 010-8zm-7 12a3 3 0 110 6 3 3 0 010-6zm14 0a3 3 0 110 6 3 3 0 010-6zM9.3 9.8l-3 3.9 1.6 1.2 3-3.9-1.6-1.2zm5.4 0l-1.6 1.2 3 3.9 1.6-1.2-3-3.9z'/></svg> Manage nodes</a>"
            "</div>");

  // --- Status cards row: Mothership battery + Storage ---
  {
    const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
    const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
    const uint32_t storagePct = (fsTotal > 0) ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;
    const float hubBatV = readBatteryVoltage();
    const char* batClass = (hubBatV >= 3.9f) ? "chip--bat-ok"
                         : (hubBatV >= 3.5f) ? "chip--bat-med"
                         : "chip--bat-low";
    const char* batLabel = (hubBatV >= 3.9f) ? "OK"
                         : (hubBatV >= 3.5f) ? "Medium"
                         : "Low";
    char batBuf[10];
    snprintf(batBuf, sizeof(batBuf), "%.2fV", hubBatV);

    html += F("<div class='stats' style='margin:0 0 12px 0;text-align:left'>");
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Mothership battery</strong><br>");
    html += F("<span class='chip ");
    html += batClass;
    html += F("' style='font-size:16px;font-weight:600'>");
    html += batBuf;
    html += F("</span> <span class='muted'>");
    html += batLabel;
    html += F("</span></div>");
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Storage</strong><br><span class='num' style='font-size:18px'>");
    html += String(storagePct);
    html += F("% used</span><br><span class='muted'>");
    html += (fsTotal > 0) ? formatBytesUi(fsUsed) : String("n/a");
    html += F("</span></div>");
    // Third card: last upload time
    html += F("<div class='stat' style='text-align:left'>"
              "<strong>Last upload</strong><br><span class='num' style='font-size:14px'>");
    {
      UploadCursor cursor = {0,0,0,0,0};
      if (flashIsReady()) { gUploadQueue.init(); cursor = gUploadQueue.getCursor(); }
      if (cursor.lastUploadUnix > 0) {
        DateTime lastUp(cursor.lastUploadUnix);
        html += formatDateTimeDisplay(lastUp);
      } else {
        html += F("Never");
      }
    }
    html += F("</span></div>");
    html += F("</div>");
  }

  // --- Node KPI tiles ---
  html += F("<div class='section' aria-live='polite'>"
            "<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>"
            "<h3>Nodes</h3>"
            "<div class='stats stats--kpi' style='margin:0 0 12px 0'>"
              "<div class='stat");
  if (deployedNodes > 0) html += F(" stat--deployed-active");
  html += F("'><strong>Active</strong><span id='kpi-deployed-num' class='num'>");
  html += String(deployedNodes);
  html += F("</span></div>"
              "<div class='stat");
  if (pairedNodes.size() > 0) html += F(" stat--paired-active");
  html += F("'><strong>Connected</strong><span id='kpi-paired-num' class='num'>");
  html += String(pairedNodes.size());
  html += F("</span></div>"
              "<div class='stat");
  if (unpairedNodes.size() > 0) html += F(" stat--unpaired-active");
  html += F("'><strong>New</strong><span id='kpi-unpaired-num' class='num'>");
  html += String(unpairedNodes.size());
  html += F("</span></div>"
            "</div>");
  html += F("</div>");

  // --- Collection schedule ---
  {
    html += F("<div class='section'>"
              "<h3>Collection schedule</h3>"
              "<div class='stats' style='margin:0 0 12px 0'>"
              "<div class='stat'><strong>Recording every</strong><span class='num' style='font-size:16px'>");
    if (gWakeIntervalMin > 0) {
      html += String(gWakeIntervalMin);
      html += F(" min");
    } else {
      html += F("Off");
    }
    html += F("</span></div>"
              "<div class='stat'><strong>Auto upload</strong><span class='num' style='font-size:16px'>");
    html += String(gSyncIntervalMin);
    html += F(" min</span></div>"
              "<div class='stat'><strong>Next upload</strong><span class='num' style='font-size:16px'><span id='kpi-next-sync'>");
    html += computeNextSyncIsoLocal();
    html += F("</span></span></div>"
              "</div>"
              "</div>");
  }

  // --- Cloud upload status ---
  {
    TransmissionSettings txSettings;
    loadTransmissionSettings(txSettings);
    UploadCursor cursor = {0, 0, 0, 0, 0};
    if (flashIsReady()) {
      gUploadQueue.init();
      cursor = gUploadQueue.getCursor();
    }

    const char* dotColour = "#6b7280";
    const char* statusLabel = "Upload off";
    if (txSettings.enabled) {
      if (cursor.retryCount > 0) {
        dotColour = "#c47a5a";
        statusLabel = "Last upload failed";
      } else {
        dotColour = "#7a9b70";
        statusLabel = "Connected";
      }
    }

    html += F("<div class='section'>"
              "<h3>Cloud upload</h3>"
              "<p style='margin:4px 0'><span style='display:inline-block;width:12px;height:12px;border-radius:50%;background:");
    html += dotColour;
    html += F(";margin-right:8px;vertical-align:middle'></span><strong>");
    html += statusLabel;
    html += F("</strong></p>");
    if (cursor.lastUploadUnix > 0) {
      DateTime lastUp(cursor.lastUploadUnix);
      html += F("<p class='muted'>Last upload ");
      html += formatDateTimeDisplay(lastUp);
      html += F(" &middot; ");
      html += String(cursor.rowsUploaded);
      html += F(" readings sent</p>");
    } else {
      html += F("<p class='muted'>No uploads yet &middot; ");
      html += String(cursor.rowsUploaded);
      html += F(" readings sent</p>");
    }
    html += F("</div>");
  }

  // --- Navigation buttons ---
  html += F("<div class='section action-stack'>"
            "<a href='/settings' class='btn'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 00.12-.61l-1.92-3.32a.488.488 0 00-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.484.484 0 00-.48-.41h-3.84a.485.485 0 00-.48.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58a.49.49 0 00-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z'/></svg> Settings</a>"
            "<a href='/export' class='btn'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3a1 1 0 011 1v8.59l2.3-2.3 1.4 1.42-4.7 4.7-4.7-4.7 1.4-1.42 2.3 2.3V4a1 1 0 011-1zm-7 14h14v2H5v-2z'/></svg> Export Data</a>"
            "</div>");
  html += F("<div style='margin:16px 0'>"
            "<form class='async-form' action='/start' method='POST'>"
            "<button type='submit' class='btn btn--primary' style='width:100%;min-height:56px;font-size:18px;font-weight:700'>"
            "▶ Finish &amp; Start Recording"
            "</button>"
            "</form>"
            "</div>"
            "<p class='muted' style='text-align:center;margin-top:6px'>Saves settings, closes the setup network, and begins recording</p>");

  // --- About / Advanced collapsed panel ---
  html += F("<details style='margin:16px 0'>"
            "<summary style='font-weight:bold;cursor:pointer;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--panel)'>About / Advanced</summary>"
            "<div class='section' style='margin-top:8px'>"
            "<div class='help'><strong>Device ID:</strong> ");
  html += DEVICE_ID;
  html += F("<br><strong>Firmware:</strong> ");
  html += FW_VERSION;
  html += F("<br><strong>Build:</strong> ");
  html += FW_BUILD;
  html += F("</div>"
            "<div class='help'><strong>WiFi network:</strong> ");
  html += ssid;
  html += F("<br><strong>Portal URL:</strong> http://192.168.4.1/"
            "<br><strong>Mothership address:</strong> ");
  html += getMothershipsMAC();
  html += F("<br><strong>Radio channel:</strong> ");
  html += String(ESPNOW_CHANNEL);
  html += F("</div>"
            "</div>"
            "</details>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetTime() {
  String dt = server.arg("datetime");
  int year, mm, dd, hh, mi, ss;
  if (sscanf(dt.c_str(), "%d:%d:%d %d-%d-%d", &hh, &mi, &ss, &dd, &mm, &year) == 6 && year >= 2000) {
    if (setRTCTime(year, mm, dd, hh, mi, ss)) {
      String html = headCommon("fieldMesh");
      html += F("<div class='section center'><h3>SUCCESS: RTC Time Updated</h3><p>New time:<br><strong>");
      html += dt;
      html += F("</strong></p><a href='/' class='btn btn--primary'>Back to Main Page</a></div>");
      html += footCommon();
      server.send(200, "text/html", html);
    } else {
      String html = headCommon("fieldMesh");
      html += F("<div class='section center'><h3>ERROR: Failed to Set RTC Time</h3><p>Please try again.</p>"
                "<a href='/' class='btn btn--primary'>Try Again</a></div>");
      html += footCommon();
      server.send(500, "text/html", html);
    }
  } else {
    String html = headCommon("fieldMesh");
    html += F("<div class='section center'><h3>WARNING: Invalid Time Format</h3>"
              "<p>Please use the format: HH:MM:SS DD-MM-YYYY</p><p>You entered: <em>");
    html += dt;
    html += F("</em></p><a href='/' class='btn btn--primary'>Try Again</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
  }
}

static void handleDownloadCSV() {
  if (!flashIsReady() || !LittleFS.exists("/datalog.csv")) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  File file = LittleFS.open("/datalog.csv", "r");
  if (!file) {
    server.send(404, "text/plain", "CSV file not found");
    return;
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "inline; filename=datalog.csv");
  server.sendHeader("Connection", "close");
  server.streamFile(file, "text/csv");
  file.close();
  Serial.println("[CSV] file downloaded by client");
}

static void handleDiscoverNodes() {
  Serial.println("[DISCOVER] Starting node discovery...");
  const uint8_t kDiscoveryBursts = 3;
  bool sentAny = false;
  for (uint8_t i = 0; i < kDiscoveryBursts; ++i) {
    sentAny = sendDiscoveryBroadcast() || sentAny;
    if (i + 1 < kDiscoveryBursts) delay(150);
  }

  if (isAjaxRequest()) {
    sendAjaxResult(sentAny, sentAny ? "Scanning for new nodes... Refreshing list." : "Scan failed");
    return;
  }

  String html = headCommon("fieldMesh");
  html += F("<meta http-equiv='refresh' content='3;url=/stations'>");
  html += F("<div class='section center'><h3>Searching for new nodes...</h3>"
            "<div class='muted'>Scanning for nodes in pairing mode.</div>"
            "<div style='margin:16px auto;width:40px;height:40px;border-radius:50%;"
            "border:4px solid #eee;border-top-color:#2196F3;animation:spin 1s linear infinite'></div>"
            "<style>@keyframes spin{0%{transform:rotate(0)}100%{transform:rotate(360deg)}}</style>"
            "<p class='muted'><small>Redirecting to Nodes in 3 seconds…</small></p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetWakeInterval() {
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 0;
  bool ok = (interval == 0);
  for (size_t i = 0; i < kAllowedCount; ++i)
    if (interval == kAllowedIntervals[i]) { ok = true; break; }
  if (!ok) interval = 0;

  gWakeIntervalMin = interval;
  gSyncIntervalMin = computeAutoSyncMin(gWakeIntervalMin);
  saveWakeIntervalToNVS(interval);

  bool sent = false;
  if (interval > 0) {
    sent = broadcastWakeInterval(interval);
    for (const auto& node : registeredNodes) {
      if (node.state == PAIRED || node.state == DEPLOYED) {
        NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
        dc.wakeIntervalMin = (uint8_t)interval;
        dc.configVersion   = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u;
        setDesiredConfig(node.nodeId.c_str(), dc);
      }
    }
  }

  Serial.printf("[UI] Global wake interval set to %d min (broadcast=%s)\n",
                interval, sent ? "yes" : "no");

  if (isAjaxRequest()) {
    if (interval > 0) {
      sendAjaxResult(true, sent ? "Recording interval applied to all nodes" : "No connected or active nodes");
    } else {
      sendAjaxResult(true, "Recording interval is OFF");
    }
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Recording Interval</h3><p>Selected: ");
  if (interval > 0) {
    html += String(interval);
    html += F(" min.</p><p style='color:#666'>");
    html += sent ? F("Applied to all connected and active nodes.") : F("No connected or active nodes.");
    html += F("</p>");
  } else {
    html += F("OFF.</p><p style='color:#666'>Recording interval disabled.</p>");
  }
  html += F("<a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleSetSyncMode() {
  // The Settings page upload-interval form sends preset="mode:value" (e.g.
  // "interval:18" or "daily:0"). Parse it first; fall back to plain "mode".
  String preset = server.arg("preset");
  String mode = server.arg("mode");
  if (preset.length() > 0) {
    int colonIdx = preset.indexOf(':');
    if (colonIdx > 0) {
      String presetMode = preset.substring(0, colonIdx);
      String presetVal  = preset.substring(colonIdx + 1);
      if (presetMode == "daily") {
        gSyncMode = SYNC_MODE_DAILY;
      } else {
        gSyncMode = SYNC_MODE_INTERVAL;
        int minVal = presetVal.toInt();
        if (minVal > 0) gSyncIntervalMin = minVal;
      }
      mode = (gSyncMode == SYNC_MODE_INTERVAL) ? "interval" : "daily";
    }
  }
  int newMode = (mode == "interval") ? SYNC_MODE_INTERVAL : SYNC_MODE_DAILY;
  gSyncMode = newMode;
  saveSyncModeToNVS(gSyncMode);
  gLastSyncBroadcastEpochDay = -1;
  gLastSyncBroadcastMs = 0;
  gLastSyncBroadcastUnix = 0;
  gLastSyncIntervalSlot = -1;
  saveSyncRuntimeGuardsToNVS();

  const uint32_t modePhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
  const uint16_t modeSyncMin = (gSyncMode == SYNC_MODE_INTERVAL) ? (uint16_t)gSyncIntervalMin : 0u;
  for (const auto& node : registeredNodes) {
    NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
    bool changed = false;
    if (dc.syncIntervalMin != modeSyncMin) { dc.syncIntervalMin = modeSyncMin; changed = true; }
    if (dc.syncPhaseUnix != modePhaseUnix) { dc.syncPhaseUnix = modePhaseUnix; changed = true; }
    if (dc.configVersion == 0) { dc.configVersion = 1; changed = true; }
    else if (changed) { dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u; }
    if (changed) setDesiredConfig(node.nodeId.c_str(), dc);
  }

  Serial.printf("[UI] Sync mode set to %s\n", syncModeLabel());

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Upload schedule set to ") + syncModeLabel());
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Upload Schedule Updated</h3><p>Mode: ");
  html += syncModeLabel();
  html += F("</p><p style='color:#666'>Next collection: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleSetSyncTime() {
  String hhmm = server.arg("sync_time");
  int hh = 0;
  int mm = 0;
  if (!parseHHMM(hhmm, hh, mm)) {
    if (isAjaxRequest()) {
      sendAjaxResult(false, "Invalid sync time. Use HH:MM");
      return;
    }
    String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                    "<h3>⚠️ Invalid upload time</h3><p>Please provide HH:MM (24h).</p>"
                    "<a href='/' style='display:inline-block;padding:10px 16px;"
                    "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
    server.send(400, "text/html", html);
    return;
  }

  gSyncDailyHour = hh;
  gSyncDailyMinute = mm;
  saveDailySyncTimeToNVS(hh, mm);
  gLastSyncBroadcastEpochDay = -1;
  gLastSyncBroadcastMs = 0;
  gLastSyncBroadcastUnix = 0;
  gLastSyncIntervalSlot = -1;
  saveSyncRuntimeGuardsToNVS();

  if (gSyncMode == SYNC_MODE_DAILY) {
    const uint32_t dailyPhaseUnix = computeNextSyncUnix(getRTCTimeUnix());
    for (const auto& node : registeredNodes) {
      NodeDesiredConfig dc = getDesiredConfig(node.nodeId.c_str());
      bool changed = false;
      if (dc.syncIntervalMin != 0u) { dc.syncIntervalMin = 0u; changed = true; }
      if (dc.syncPhaseUnix != dailyPhaseUnix) { dc.syncPhaseUnix = dailyPhaseUnix; changed = true; }
      if (dc.configVersion == 0) { dc.configVersion = 1; changed = true; }
      else if (changed) { dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u; }
      if (changed) setDesiredConfig(node.nodeId.c_str(), dc);
    }
  }

  Serial.printf("[UI] Daily sync time set to %02d:%02d\n", gSyncDailyHour, gSyncDailyMinute);

  if (isAjaxRequest()) {
    sendAjaxResult(true, String("Daily upload time set to ") + formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute));
    return;
  }

  String html = F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<body style='font-family:sans-serif;padding:20px;text-align:center'>"
                  "<h3>Daily Upload Time Updated</h3><p>New daily upload time: ");
  html += formatSyncTimeHHMM(gSyncDailyHour, gSyncDailyMinute);
  html += F("</p><p style='color:#666'>Next collection: ");
  html += computeNextSyncIsoLocal();
  html += F("</p><a href='/' style='display:inline-block;padding:10px 16px;"
            "background:#2196F3;color:#fff;text-decoration:none;border-radius:6px'>Back</a></body>");
  server.send(200, "text/html", html);
}

static void handleRevertNode() {
  String nodeId = server.arg("node_id");
  bool found   = false;
  bool sentCmd = false;

  for (auto& node : registeredNodes) {
    if (node.nodeId == nodeId && node.state == DEPLOYED) {
      node.state = PAIRED;
      savePairedNodes();
      found = true;
      sentCmd = pairNode(nodeId);
      Serial.printf("[REVERT] %s -> PAIRED (cmd=%s)\n", nodeId.c_str(), sentCmd ? "OK" : "FAIL");
      break;
    }
  }

  String html = headCommon("fieldMesh");
  html += F("<div class='section center'>");
  if (found) {
    html += F("<h3>Node stopped</h3><p>Node <strong>");
    html += nodeId;
    html += F("</strong> is now connected but not active.</p>");
    if (!sentCmd) {
      html += F("<p class='muted'>Warning: could not send stop command to the node.</p>");
    }
  } else {
    html += F("<h3>Node not found or not active</h3><p>No action taken.</p>");
  }
  html += F("<a href='/stations' class='btn btn--primary'>Back to Nodes</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleStationsPage() {
  String html = headCommon("Nodes",
    "<a href='/' class='btn btn--sm'>Back</a><a href='/stations' class='btn btn--sm'>Refresh</a>");
  auto allNodes = getRegisteredNodes();

  html += F("<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>");
  html += F("<div class='section'>");
  html += F("<h3>Nodes</h3>");

  if (allNodes.empty()) {
    html += F("<p class='muted'>No nodes yet. Tap “Add New node” below to get started.</p>");
  } else {
    html += F("<div class='list'>");

    for (auto& node : allNodes) {
      String userId = node.userId;
      String name   = node.name;
      if (userId.isEmpty()) userId = getNodeUserId(node.nodeId);
      if (name.isEmpty())   name   = getNodeName(node.nodeId);
      NodeDesiredConfig nodeDesired = getDesiredConfig(node.nodeId.c_str());
      uint8_t observedWakeMin = (node.wakeIntervalMin > 0)
        ? node.wakeIntervalMin
        : node.inferredWakeIntervalMin;
      int nodeIntervalCurrentMin = (nodeDesired.wakeIntervalMin > 0)
        ? (int)nodeDesired.wakeIntervalMin
        : ((isAllowedInterval(gWakeIntervalMin) && gWakeIntervalMin > 0)
            ? gWakeIntervalMin
            : ((observedWakeMin > 0) ? (int)observedWakeMin : 5));

      String displayId = name.length() ? name : (userId.length() ? userId : node.nodeId);

      html += F("<a href='/station?id=");
      html += node.nodeId;
      html += F("' class='item item--node'>"
                "<div class='node-row'>"
                "<div class='node-main'>"
                "<strong>");
      html += htmlEscape(displayId);
      html += F("</strong>");
      if (name.length() && userId.length()) {
        html += F("<span class='muted node-name'>");
        html += htmlEscape(userId);
        html += F("</span>");
      }
      html += F("</div>"
                "<div class='node-status'>"
                "<div class='node-status-cell'>"
                "<span class='node-timing-label'>Status</span>");

      if (node.state == DEPLOYED) html += F("<span class='chip chip--state-deployed'>Active</span>");
      else if (node.state == PAIRED) html += F("<span class='chip chip--state-paired'>Connected</span>");
      else html += F("<span class='chip chip--state-unpaired'>New</span>");

      html += F("</div>"
                "<div class='node-status-cell'>"
                "<span class='node-timing-label'>Battery</span>");
      {
        float batV = node.lastReportedBatV;
        if (isnan(batV)) {
          html += F("<span class='chip'>n/a</span>");
        } else {
          char batBuf[10];
          snprintf(batBuf, sizeof(batBuf), "%.2fV", batV);
          const char* batClass = (batV >= 3.9f) ? "chip--bat-ok"
                               : (batV >= 3.5f) ? "chip--bat-med"
                               : "chip--bat-low";
          html += F("<span class='chip ");
          html += batClass;
          html += F("'>");
          html += batBuf;
          html += F("</span>");
        }
      }

      html += F("</div></div>"
                "<div class='node-timing'>"
                "<div class='node-timing-cell'>"
                "<span class='node-timing-label'>Recording</span>"
                "<span class='chip node-timing-value'>");
      html += String(nodeIntervalCurrentMin);
      html += F(" min</span>"
                "</div>"
                "<div class='node-timing-cell'>"
                "<span class='node-timing-label'>Last seen</span>"
                "<span class='chip node-timing-value'>");
      if (node.lastSeen > 0) {
        const uint32_t nowMs = millis();
        const uint32_t ageMs = (nowMs >= node.lastSeen) ? (nowMs - node.lastSeen) : 0;
        const uint32_t ageMin = ageMs / 60000UL;
        html += String(ageMin);
        html += F(" min ago");
      } else {
        html += F("n/a");
      }
      html += F("</span>"
                "</div>"
                "</div>"
                "</div></a>");
    }

    html += F("</div>");
  }
  html += F("</div>");

  // --- Add New node guided panel ---
  html += F("<div class='section'>"
            "<details>"
            "<summary style='font-weight:bold;cursor:pointer'>+ Add New node</summary>"
            "<div style='margin-top:12px'>"
            "<p class='muted'>1. Press the pair button on the node (hold 3 seconds).<br>"
            "2. Wait for the green light.<br>"
            "3. Tap “Find New Nodes” below.</p>"
            "<p class='muted'>The node will appear in the list as “New”. Tap it to activate and give it a name.</p>"
            "<form class='async-form' action='/find-stations' method='POST'>"
            "<button type='submit' class='btn btn--primary'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M12 3c-3.9 0-7.4 1.6-9.9 4.1l1.4 1.4C5.6 6.4 8.7 5 12 5s6.4 1.4 8.5 3.5l1.4-1.4C19.4 4.6 15.9 3 12 3zm0 5c-2.6 0-5 .9-6.9 2.6l1.4 1.4C8 10.5 9.9 10 12 10s4 .5 5.5 2l1.4-1.4C17 8.9 14.6 8 12 8zm0 5c-1.3 0-2.5.5-3.5 1.5l1.4 1.4c.6-.6 1.3-.9 2.1-.9s1.5.3 2.1.9l1.4-1.4c-1-1-2.2-1.5-3.5-1.5zm0 4a2 2 0 100 4 2 2 0 000-4z'/></svg> Find New Nodes</button>"
            "</form>"
            "</div>"
            "</details>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleStationDetail() {
  String nodeId = server.arg("id");
  if (nodeId.length() == 0) nodeId = server.arg("node_id");

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  String actionsHtml = String("<a href='/stations' class='btn btn--sm'>Back</a><a href='/station?id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Node", actionsHtml);
  html += F("<div class='section'>");

  if (!target) {
    html += F("<h3>Node not found</h3>"
              "<p class='muted'>No node with that ID is currently registered.</p>"
              "<a href='/stations' class='btn btn--primary'>Back to Nodes</a>");
    html += F("</div>");
    html += footCommon();
    server.send(404, "text/html", html);
    return;
  }

  String userId  = getNodeUserId(target->nodeId);
  String name    = getNodeName(target->nodeId);
  String notes   = getNodeNotes(target->nodeId);
  const bool isDeployed = (target->state == DEPLOYED);
  uint8_t observedWakeMin = (target->wakeIntervalMin > 0)
    ? target->wakeIntervalMin
    : target->inferredWakeIntervalMin;
  int activeIntervalMin = (observedWakeMin > 0)
    ? observedWakeMin
    : (isAllowedInterval(gWakeIntervalMin) ? gWakeIntervalMin : 5);

  // --- Status header ---
  html += F("<div class='muted' style='margin:0 0 10px 0'>");
  if (target->state == DEPLOYED) html += F("<span class='chip chip--state-deployed'>Active</span>");
  else if (target->state == PAIRED) html += F("<span class='chip chip--state-paired'>Connected</span>");
  else html += F("<span class='chip chip--state-unpaired'>New</span>");
  {
    float batV = target->lastReportedBatV;
    if (!isnan(batV)) {
      char batBuf[10];
      snprintf(batBuf, sizeof(batBuf), "%.2fV", batV);
      const char* batClass = (batV >= 3.9f) ? "chip--bat-ok"
                           : (batV >= 3.5f) ? "chip--bat-med"
                           : "chip--bat-low";
      html += F(" <span class='chip ");
      html += batClass;
      html += F("'>");
      html += batBuf;
      html += F("</span>");
    }
  }
  html += F("<br><strong>Recording every</strong> ");
  html += String(activeIntervalMin);
  html += F(" min");
  if (target->lastSeen > 0) {
    const uint32_t nowMs = millis();
    const uint32_t ageMs = (nowMs >= target->lastSeen) ? (nowMs - target->lastSeen) : 0;
    const uint32_t ageMin = ageMs / 60000UL;
    html += F("<br><strong>Last seen</strong> ");
    html += String(ageMin);
    html += F(" min ago");
  }
  html += F("</div>");

  html += F("<form action='/station' method='POST' onsubmit='return confirmRemove()'>"
            "<input type='hidden' name='node_id' value='");
  html += target->nodeId;
  html += F("'>");

  // --- Location section (GPS capture + manual entry) ---
  html += F("<div class='section'>"
            "<h3>Location</h3>"
            "<p class='muted'>Stand next to this node and tap \"Use phone GPS\" to capture its location.</p>"
            "<button type='button' class='btn btn--primary' onclick='captureGPS()'>\xF0\x9F\x93\x8D Use phone GPS</button>"
            "<div id='gps-status' class='help' style='margin-top:8px'></div>"
            "<label class='label'>Latitude</label>"
            "<input class='input' type='text' id='lat' name='lat' placeholder='-27.469771' value='");
  if (!isnan(target->latitude)) {
    char latBuf[16];
    snprintf(latBuf, sizeof(latBuf), "%.6f", target->latitude);
    html += latBuf;
  }
  html += F("'>"
            "<label class='label'>Longitude</label>"
            "<input class='input' type='text' id='lon' name='lon' placeholder='153.025124' value='");
  if (!isnan(target->longitude)) {
    char lonBuf[16];
    snprintf(lonBuf, sizeof(lonBuf), "%.6f", target->longitude);
    html += lonBuf;
  }
  html += F("'>"
            "<a id='view-map' href='#' class='btn' style='display:none;margin-top:8px'>View on map</a>"
            "</div>");

  html += F("<div style='margin-bottom:10px;padding:10px;border:1px solid var(--border);border-radius:8px;background:#fafafa'>"
            "<label class='label' style='margin-top:0'>Notes</label>"
            "<input class='input' type='text' name='notes' maxlength='180' placeholder='Notes for this node' value='");
  html += htmlEscape(notes);
  html += F("'></div>");

  if (!isDeployed) {
    html += F("<label class='label'>Node ID (numeric, e.g. 001)</label>"
              "<input class='input' type='text' name='user_id' maxlength='3' "
              "placeholder='001' value='");
    html += userId;
    html += F("'>"
              "<label class='label'>Name</label>"
              "<input class='input' type='text' name='name' "
              "placeholder='e.g. North Hedge 01' value='");
    html += name;
    html += F("'>");
  } else {
    html += F("<div style='margin-top:10px;padding:10px;border:1px solid #E1B17A;border-radius:8px;background:#fff8e1'>"
              "<div style='font-weight:700;color:#8a4b00;margin-bottom:6px'>Are you sure?</div>"
              "<div class='muted' style='margin-bottom:8px'>Changing Node ID/Name on an active node can break tracking if used incorrectly.</div>"
              "<label style='display:flex;gap:8px;align-items:flex-start;margin-bottom:8px'>"
              "<input type='checkbox' name='edit_identity_confirm' value='yes' onchange=\"var en=this.checked;document.getElementById('dep-user-id').disabled=!en;document.getElementById('dep-name').disabled=!en;\">"
              "<span>I understand and want to edit Node ID/Name for this active node.</span>"
              "</label>"
              "<label class='label'>Node ID (numeric, e.g. 001)</label>"
              "<input id='dep-user-id' class='input' type='text' name='user_id' maxlength='3' placeholder='001' disabled value='");
    html += userId;
    html += F("'>"
              "<label class='label'>Name</label>"
              "<input id='dep-name' class='input' type='text' name='name' placeholder='e.g. North Hedge 01' disabled value='");
    html += name;
    html += F("'>"
              "</div>");
  }

  html += F("<div style='margin-bottom:12px'>"
            "<label class='label'>Recording interval (minutes)</label>"
            "<div style='font-size:14px;padding:6px 0'><strong>");
  html += String(gWakeIntervalMin > 0 ? gWakeIntervalMin : 0);
  html += F(" min</strong> &nbsp;<span style='font-size:12px;color:#888'>"
            "(applies to all nodes &mdash; change in Settings)</span></div>");
  if (gSyncIntervalMin > 0) {
    html += F("<div style='font-size:12px;color:#555'>Upload every ");
    html += String(gSyncIntervalMin);
    html += F(" min</div>");
  }
  html += F("</div>");

  html += F("<div class='action-choices'>"
              "<label class='action-choice'><input type='radio' name='action' value='none' checked><span>No change</span></label>"
              "<label class='action-choice action-choice--start'><input type='radio' name='action' value='start'><span>Activate</span></label>"
              "<label class='action-choice action-choice--stop'><input type='radio' name='action' value='stop'><span>Stop Monitoring</span></label>"
              "<label class='action-choice action-choice--unpair'><input type='radio' name='action' value='unpair'><span>Remove node</span></label>"
            "</div>"
            "<button type='submit' class='btn btn--success' style='margin-top:12px'>"
            "Save Changes</button>"
            "</form>");

  html += F("<script>function confirmRemove(){var r=document.querySelector('input[name=action]:checked');if(r&&r.value==='unpair'){return confirm('Remove this node? You will need to re-add it with the pair button.');}return true;}</script>");
  html += F("<script>function captureGPS(){var status=document.getElementById('gps-status');var btn=event.target;btn.textContent='Getting GPS...';btn.disabled=true;status.textContent='';if(!navigator.geolocation){status.textContent='GPS not available on this device.';btn.textContent='\\xF0\\x9F\\x93\\x8D Use phone GPS';btn.disabled=false;return;}navigator.geolocation.getCurrentPosition(function(pos){var lat=pos.coords.latitude.toFixed(6);var lon=pos.coords.longitude.toFixed(6);var acc=Math.round(pos.coords.accuracy);document.getElementById('lat').value=lat;document.getElementById('lon').value=lon;status.innerHTML='Captured: '+lat+', '+lon+'<br>Accuracy: \\u00B1'+acc+'m'+(acc>50?'<br><strong>Low accuracy \\u2014 try again outdoors.</strong>':' (good)');var mapLink=document.getElementById('view-map');mapLink.href='geo:'+lat+','+lon+'?q='+lat+','+lon;mapLink.style.display='inline-block';btn.textContent='\\xF0\\x9F\\x93\\x8D Use phone GPS';btn.disabled=false;},function(err){status.textContent='GPS error: '+err.message;btn.textContent='\\xF0\\x9F\\x93\\x8D Use phone GPS';btn.disabled=false;},{enableHighAccuracy:true,timeout:15000,maximumAge:0});}</script>");
  html += F("</div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleNodeConfigSave() {
  String nodeId   = server.arg("node_id");
  String userId   = server.arg("user_id");
  String name     = server.arg("name");
  String notes    = server.arg("notes");
  bool editIdentityConfirmed = server.hasArg("edit_identity_confirm") && server.arg("edit_identity_confirm") == "yes";
  String action   = server.arg("action");
  int interval = (gWakeIntervalMin > 0) ? gWakeIntervalMin : 5;

  NodeInfo* target = nullptr;
  for (auto& n : registeredNodes) {
    if (n.nodeId == nodeId) { target = &n; break; }
  }

  const bool isCurrentlyDeployed = (target && target->state == DEPLOYED);
  const bool allowIdentityEdit = (!isCurrentlyDeployed) || editIdentityConfirmed;
  if (allowIdentityEdit) {
    if (server.hasArg("user_id")) setNodeUserId(nodeId, userId);
    if (server.hasArg("name")) setNodeName(nodeId, name);
  }
  if (server.hasArg("notes")) setNodeNotes(nodeId, notes);

  // --- GPS coordinates (optional) ---
  if (target) {
    String latStr = server.arg("lat");
    String lonStr = server.arg("lon");
    if (latStr.length() > 0 && lonStr.length() > 0) {
      float lat = latStr.toFloat();
      float lon = lonStr.toFloat();
      if (lat != 0.0f || lon != 0.0f) {
        target->latitude  = lat;
        target->longitude = lon;
      }
    }
  }

  NodeDesiredConfig dc = getDesiredConfig(nodeId.c_str());
  bool cfgChanged = false;
  uint32_t globalPhaseUnix = gLastSyncBroadcastUnix;
  if (globalPhaseUnix == 0) globalPhaseUnix = getRTCTimeUnix();
  if (gSyncMode == SYNC_MODE_DAILY) {
    const uint32_t dayStartUnix = (globalPhaseUnix / 86400UL) * 86400UL;
    const uint32_t targetOffset = (uint32_t)gSyncDailyHour * 3600UL + (uint32_t)gSyncDailyMinute * 60UL;
    uint32_t nextDailyUnix = dayStartUnix + targetOffset;
    if (nextDailyUnix <= getRTCTimeUnix()) nextDailyUnix += 86400UL;
    globalPhaseUnix = nextDailyUnix;
  }
  globalPhaseUnix -= (globalPhaseUnix % 60UL);
  // Update the phase anchor so armNextSyncAlarmPhase uses it when config exits
  gLastSyncBroadcastUnix = globalPhaseUnix;
  saveSyncRuntimeGuardsToNVS();
  if (dc.wakeIntervalMin != (uint8_t)interval) { dc.wakeIntervalMin = (uint8_t)interval; cfgChanged = true; }
  const uint16_t desiredSyncMin = (gSyncMode == SYNC_MODE_DAILY) ? 0u : (uint16_t)computeAutoSyncMin(interval);
  if (dc.syncIntervalMin != desiredSyncMin) { dc.syncIntervalMin = desiredSyncMin; cfgChanged = true; }
  if (dc.syncPhaseUnix != globalPhaseUnix) { dc.syncPhaseUnix = globalPhaseUnix; cfgChanged = true; }
  if (dc.configVersion == 0) { dc.configVersion = 1; cfgChanged = true; }
  else if (cfgChanged) { dc.configVersion = (dc.configVersion < 0xFFFFu) ? (dc.configVersion + 1u) : 1u; }
  setDesiredConfig(nodeId.c_str(), dc);
  if (target) target->wakeIntervalMin = (uint8_t)interval;
  // Persist any updated node fields (name/notes/lat/lon) to NVS.
  if (target) savePairedNodes();

  Serial.printf("[CONFIG] %s interval=%d min desired v%u changed=%d\n",
                nodeId.c_str(), interval, (unsigned)dc.configVersion, cfgChanged ? 1 : 0);

  bool deployOk = false;
  bool revertOk = false;
  bool pairOk   = false;
  bool unpairOk = false;

  if (action == "none" || action.length() == 0) {
    // No lifecycle change requested — only name/notes/schedule saved.
  } else if (action == "start") {
    if (target && target->state == UNPAIRED) {
      pairOk = pairNode(nodeId);
      if (pairOk) {
        target->state = PAIRED;
        savePairedNodes();
      }
    }
    std::vector<String> ids;
    ids.push_back(nodeId);
    deployOk = deploySelectedNodes(ids);
    Serial.printf("[CONFIG] %s start -> deploy: %s\n", nodeId.c_str(), deployOk ? "OK" : "FAIL");
  } else if (action == "stop") {
    if (target) {
      target->state = PAIRED;
      target->deployPending = false;
      savePairedNodes();
      revertOk = pairNode(nodeId);
      Serial.printf("[CONFIG] %s stop -> PAIRED: %s\n", nodeId.c_str(), revertOk ? "OK" : "FAIL");
    }
  } else if (action == "unpair") {
    if (target) {
      bool sent  = sendUnpairToNode(nodeId);
      bool local = unpairNode(nodeId);
      unpairOk = sent && local;
      setNodeUserId(nodeId, "");
      setNodeName(nodeId, "");
      setNodeNotes(nodeId, "");
      target->userId = "";
      target->name   = "";
      Serial.printf("[CONFIG] %s unpair -> send=%s local=%s\n",
                    nodeId.c_str(), sent ? "OK" : "FAIL", local ? "OK" : "FAIL");
    }
  }

  String finalUserId = getNodeUserId(nodeId);
  String finalName   = getNodeName(nodeId);
  String finalNotes  = getNodeNotes(nodeId);
  if (finalUserId.isEmpty()) finalUserId = userId;
  if (finalName.isEmpty())   finalName   = name;
  if (target) {
    target->userId = finalUserId;
    target->name   = finalName;
  }

  String actionsHtml = String("<a href='/stations' class='btn btn--sm'>Back</a><a href='/station?id=")
    + nodeId
    + String("' class='btn btn--sm'>Refresh</a>");
  String html = headCommon("Node", actionsHtml);
  html += F("<div class='section center'>"
            "<h3>Node settings applied</h3>");

  if (!target) {
    html += F("<p class='muted'>Warning: this node ID is not currently in the registered list.</p>");
  }

  html += F("<p><strong>Node ID:</strong> ");
  html += (finalUserId.length() ? htmlEscape(finalUserId) : String("-"));
  html += F("<br><strong>Name:</strong> ");
  html += (finalName.length() ? htmlEscape(finalName) : String("-"));
  html += F("<br><strong>Notes:</strong> ");
  html += (finalNotes.length() ? htmlEscape(finalNotes) : String("-"));
  html += F("<br><strong>Recording interval:</strong> ");
  html += String(interval);
  html += F(" min<br><strong>Action:</strong> ");
  html += action;
  html += F("</p>");

  html += F("<p class='muted'>"
            "Schedule stored for this node (applies on next wake)"
            "<br>Connect (if new): ");
  html += pairOk ? "OK" : "not requested / failed";
  html += F("<br>Activate: ");
  html += deployOk ? "REQUESTED (awaiting node confirmation)" : "not requested / failed";
  html += F("<br>Stop monitoring: ");
  html += revertOk ? "OK" : "not requested / failed";
  html += F("<br>Remove node: ");
  html += unpairOk ? "OK" : "not requested / failed";
  html += F("</p>"
            "<a href='/stations' class='btn btn--primary'>Back to Nodes</a>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// LTE upload settings + manual upload handlers
// ---------------------------------------------------------------------------

static String formatUploadTime(uint32_t unixSec) {
  if (unixSec == 0) return String("never");
  DateTime dt(unixSec);
  return formatDateTimeDisplay(dt);
}

static void handleSettings() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t storagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  String actionsHtml = String("<a href='/' class='btn btn--sm'>Back</a>");
  String html = headCommon("Settings", actionsHtml);

  html += F("<div id='ui-status' class='help' style='display:none;margin-bottom:10px;border:1px solid var(--border);border-radius:8px;padding:8px 10px'></div>");

  html += F("<div class='section'>");

  // --- Recording interval presets ---
  html += F("<h3>Recording interval</h3>"
            "<p class='muted'>How often each node measures.</p>"
            "<form class='async-form' action='/set-recording-interval' method='POST'>"
            "<div class='action-choices'>");
  static const int kRecordingPresets[] = {1, 5, 10, 30};
  static const size_t kRecordingPresetCount = sizeof(kRecordingPresets) / sizeof(kRecordingPresets[0]);
  for (size_t i = 0; i < kRecordingPresetCount; ++i) {
    int v = kRecordingPresets[i];
    html += F("<label class='action-choice action-choice--start'><input type='radio' name='interval' value='");
    html += String(v);
    html += F("'");
    if (v == gWakeIntervalMin) html += F(" checked");
    html += F("><span>");
    html += String(v);
    html += F(" min</span></label>");
  }
  html += F("</div>"
            "<button type='submit' class='btn btn--primary' style='margin-top:8px'>Apply</button>"
            "</form>");
  html += F("<p class='muted' style='margin-top:6px'>Selected: ");
  if (gWakeIntervalMin > 0) {
    html += String(gWakeIntervalMin);
    html += F(" min");
  } else {
    html += F("Off");
  }
  html += F("</p>");
  html += F("</div>");

  // --- Cloud connection ---
  html += F("<div class='section'>");
  html += F("<h3>Cloud connection</h3>");

  html += F("<form class='async-form' action='/save-settings' method='POST'>");

  html += F("<label class='label'><input type='checkbox' name='enabled' value='1'");
  if (tx.enabled) html += F(" checked");
  html += F("> <strong>Enable cloud upload</strong></label>");
  html += F("<div class='help'>When enabled, the Mothership uploads new data during collection rounds (subject to schedule and battery checks).</div>");

  html += F("<label class='label' for='api_key'>API Key</label>");
  if (tx.apiKey.length() > 0) {
    html += F("<p class='muted'>API key configured (last 4: ");
    html += tx.apiKey.substring(tx.apiKey.length() - 4);
    html += F(")</p>");
  }
  html += F("<input class='input' id='api_key' name='api_key' type='password' placeholder='Enter new key to replace' value=''>");
  html += F("<div class='help'>Leave blank to keep the current key, or enter a new <code>fm_xxxxxxxx</code> API key from the fieldMesh dashboard.</div>");

  html += F("<label class='label' for='qr_string' style='margin-top:10px'>QR code string (optional)</label>");
  html += F("<input class='input' id='qr_string' name='qr_string' type='text' placeholder='Paste url|key here'>");
  html += F("<div class='help'>If you have a QR code string of the form <code>url|key</code>, paste it here to set both endpoint and API key at once.</div>");

  html += F("<label class='label' style='margin-top:10px'>Endpoint (read-only)</label>");
  html += F("<p class='muted' style='word-break:break-all;font-size:13px'>");
  html += htmlEscape(tx.endpointUrl);
  html += F("</p>");

  // Cloud status line
  {
    const char* dotColour = "#6b7280";
    const char* statusLabel = "Upload off";
    if (tx.enabled) {
      if (tx.apiKey.length() > 0) {
        if (cursor.retryCount > 0) {
          dotColour = "#c47a5a";
          statusLabel = "Last upload failed";
        } else {
          dotColour = "#7a9b70";
          statusLabel = "Connected";
        }
      } else {
        dotColour = "#c47a5a";
        statusLabel = "Not set up";
      }
    }
    html += F("<p style='margin:8px 0'><span style='display:inline-block;width:12px;height:12px;border-radius:50%;background:");
    html += dotColour;
    html += F(";margin-right:8px;vertical-align:middle'></span><strong>");
    html += statusLabel;
    html += F("</strong>");
    if (cursor.lastUploadUnix > 0) {
      html += F(" &middot; last upload ");
      html += formatUploadTime(cursor.lastUploadUnix);
    }
    html += F("</p>");
  }

  // --- Advanced settings (collapsed) ---
  html += F("<details style='margin-top:14px'>");
  html += F("<summary style='font-weight:bold;cursor:pointer'>Advanced settings</summary>");

  html += F("<div class='row'>");
  html += F("<div class='col'><label class='label' for='min_bat_mv'>Min battery (mV)</label>");
  html += F("<input class='input' id='min_bat_mv' name='min_bat_mv' type='number' min='0' value='");
  html += String(tx.minBatteryMv);
  html += F("'></div>");
  html += F("<div class='col'><label class='label' for='max_bytes'>Max bytes per session</label>");
  html += F("<input class='input' id='max_bytes' name='max_bytes' type='number' min='0' value='");
  html += String(tx.maxBytesPerSession);
  html += F("'></div>");
  html += F("</div>");

  html += F("<div class='row'>");
  html += F("<div class='col'><label class='label' for='max_retries'>Max retries per window</label>");
  html += F("<input class='input' id='max_retries' name='max_retries' type='number' min='0' value='");
  html += String(tx.maxRetriesPerWindow);
  html += F("'></div>");
  html += F("<div class='col'><label class='label' for='upload_min'>Upload interval (min, 0=every collection)</label>");
  html += F("<input class='input' id='upload_min' name='upload_min' type='number' min='0' value='");
  html += String(tx.uploadIntervalMin);
  html += F("'></div>");
  html += F("</div>");

  html += F("<label class='label'><input type='checkbox' name='allow_manual' value='1'");
  if (tx.allowManualUpload) html += F(" checked");
  html += F("> <strong>Allow manual upload from this page</strong></label>");
  html += F("<div class='help'>Manual upload powers on the modem and transmits now. This takes 30-60s and draws extra power.</div>");

  html += F("<p class='muted' style='margin-top:10px'>Legacy fields — leave blank unless advised:</p>");

  html += F("<label class='label' for='token'>Auth token</label>");
  html += F("<input class='input' id='token' name='token' type='password' placeholder='auth token' value='");
  html += htmlEscape(tx.authToken);
  html += F("'>");

  html += F("<label class='label' for='site_id'>Site ID</label>");
  html += F("<input class='input' id='site_id' name='site_id' type='text' placeholder='site-001' value='");
  html += htmlEscape(tx.siteId);
  html += F("'>");

  html += F("<label class='label' for='deploy_id'>Deployment ID</label>");
  html += F("<input class='input' id='deploy_id' name='deploy_id' type='text' placeholder='deploy-001' value='");
  html += htmlEscape(tx.deploymentId);
  html += F("'>");

  html += F("</details>");

  html += F("<button type='submit' class='btn btn--primary' style='margin-top:12px'>Save</button>");
  html += F("</form>");
  html += F("</div>");

  // --- Storage + manual upload status ---
  html += F("<div class='section'><h3>Storage</h3>");
  html += F("<div class='stats' style='margin:0 0 10px 0'>");
  html += F("<div class='stat'><strong>Readings waiting</strong><span class='num' style='font-size:16px'>");
  html += String(pendingRows);
  html += F("</span></div>");
  html += F("<div class='stat'><strong>Last upload</strong><span class='num' style='font-size:14px'>");
  html += formatUploadTime(cursor.lastUploadUnix);
  html += F("</span></div>");
  html += F("<div class='stat'><strong>Storage used</strong><span class='num' style='font-size:16px'>");
  html += String(storagePct);
  html += F("%</span></div></div>");

  if (tx.allowManualUpload) {
    html += F("<form action='/manual-upload' method='POST'>");
    html += F("<input type='hidden' name='ajax' value='1'>");
    html += F("<button type='submit' class='btn btn--warn'>Upload now (30-60s)</button>");
    html += F("</form>");
  } else {
    html += F("<p class='muted'>Manual upload is disabled in advanced settings.</p>");
  }

  html += F("</div>");

  // --- Save & Start Monitoring ---
  html += F("<div class='section action-stack'>"
            "<form class='async-form' action='/start' method='POST'>"
            "<button type='submit' class='btn btn--primary'><svg class='icon' viewBox='0 0 24 24' aria-hidden='true'><path d='M8 5v14l11-7z'/></svg> Save &amp; Start Recording</button>"
            "</form>"
            "</div>");

  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleSetTransmission() {
  TransmissionSettings tx;
  tx.enabled          = server.hasArg("enabled") && server.arg("enabled") == "1";
  tx.endpointUrl      = server.arg("url");
  tx.authToken        = server.arg("token");
  tx.apiKey           = server.arg("api_key");  // may be blank; preserved below
  tx.siteId           = server.arg("site_id");

  // QR code string: if present and contains '|', split into url|key.
  String qrString = server.arg("qr_string");
  qrString.trim();
  if (qrString.length() > 0) {
    int pipeIdx = qrString.indexOf('|');
    if (pipeIdx > 0) {
      tx.endpointUrl = qrString.substring(0, pipeIdx);
      tx.apiKey      = qrString.substring(pipeIdx + 1);
      Serial.printf("[UI] QR string parsed: endpoint set, key length=%u\n",
                    (unsigned)tx.apiKey.length());
    } else {
      // No pipe — treat the whole string as an API key.
      tx.apiKey = qrString;
    }
  }
  tx.deploymentId     = server.arg("deploy_id");
  tx.uploadIntervalMin= (uint16_t)server.arg("upload_min").toInt();
  tx.minBatteryMv     = (uint16_t)server.arg("min_bat_mv").toInt();
  tx.maxBytesPerSession = (uint32_t)server.arg("max_bytes").toInt();
  tx.maxRetriesPerWindow = (uint8_t)server.arg("max_retries").toInt();
  tx.allowManualUpload = server.hasArg("allow_manual") && server.arg("allow_manual") == "1";

  // Preserve phase anchor (not edited via this form)
  TransmissionSettings prev;
  loadTransmissionSettings(prev);
  tx.uploadPhaseUnix = prev.uploadPhaseUnix;
  tx.useJsonUpload = prev.useJsonUpload;
  // Keep the previous API key if the form field was left blank (no QR string).
  if (tx.apiKey.length() == 0 || tx.apiKey.indexOf("\u2022") >= 0) {
    tx.apiKey = prev.apiKey;
  }

  saveTransmissionSettings(tx);
  Serial.printf("[UI] Transmission settings saved: enabled=%d url=%s site=%s\n",
                tx.enabled ? 1 : 0, tx.endpointUrl.c_str(), tx.siteId.c_str());

  if (isAjaxRequest()) {
    sendAjaxResult(true, "Settings saved");
    return;
  }

  String html = headCommon("Settings", "<a href='/' class='btn btn--sm'>Back</a>");
  html += F("<div class='section center'><h3>Settings saved</h3>"
            "<a href='/settings' class='btn btn--primary'>Back to Settings</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleManualUpload() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  if (!tx.allowManualUpload) {
    if (isAjaxRequest()) {
      sendAjaxResult(false, "Manual upload is disabled in settings");
      return;
    }
    String html = headCommon("Settings", "<a href='/settings' class='btn btn--sm'>Back</a>");
    html += F("<div class='section center'><h3>Manual upload disabled</h3>"
              "<p class='muted'>Enable manual upload in advanced settings first.</p>"
              "<a href='/settings' class='btn btn--primary'>Back</a></div>");
    html += footCommon();
    server.send(400, "text/html", html);
    return;
  }

  // NOTE: This is a blocking handler — the web request hangs for 30-60s
  // while the modem powers on, registers, and uploads.  This could be
  // improved later with a background task + status polling, but for V1
  // field use the blocking approach is acceptable.
  Serial.println("[UI] Manual upload requested — starting blocking sequence");

  String resultMsg;
  bool ok = false;

  if (!flashIsReady()) {
    resultMsg = "Storage not ready — cannot read data";
  } else {
    gUploadQueue.init();
    if (gUploadQueue.getPendingBytes() == 0) {
      resultMsg = "No new data to upload";
      ok = true;
    } else {
      ModemDriver modem;
      modem.init();

      if (!modem.powerOn()) {
        resultMsg = "Modem power-on failed";
      } else if (!modem.waitForNetwork(60000)) {
        resultMsg = "Network registration failed/timeout (antenna connected?)";
        modem.gracefulShutdown();
      } else {
        UploadPayload payload = gUploadQueue.getNewData(tx.maxBytesPerSession);
        if (payload.byteLength == 0) {
          resultMsg = "No new data to upload";
          ok = true;
          modem.gracefulShutdown();
        } else {
          String url = buildUploadUrl(tx);
          Serial.printf("[UI] Manual upload POST %u bytes to %s\n", payload.byteLength, url.c_str());
          String authHeader = tx.apiKey.length() > 0 ? tx.apiKey : tx.authToken;
          HttpsPostResult result = modem.httpsPost(url, payload.csvData, "text/csv", authHeader);
          if (result.success && result.httpStatus == 200) {
            uint32_t nowUnix = getRTCTimeUnix();
            gUploadQueue.advanceCursor(payload.startOffset + payload.byteLength, nowUnix);
            gUploadQueue.purgeUploaded();
            gUploadQueue.resetRetryCount();
            char buf[64];
            snprintf(buf, sizeof(buf), "Upload OK: HTTP 200, %u bytes", payload.byteLength);
            resultMsg = String(buf);
            ok = true;
          } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "Upload failed: HTTP %d, %s",
                     result.httpStatus, result.errorDetail.c_str());
            resultMsg = String(buf);
            gUploadQueue.incrementRetryCount();
          }
          modem.gracefulShutdown();
        }
      }
    }
  }

  Serial.printf("[UI] Manual upload result: %s\n", resultMsg.c_str());

  if (isAjaxRequest()) {
    sendAjaxResult(ok, resultMsg);
    return;
  }

  String html = headCommon("Settings", "<a href='/settings' class='btn btn--sm'>Back</a>");
  html += F("<div class='section center'><h3>Manual upload</h3><p>");
  html += resultMsg;
  html += F("</p><a href='/settings' class='btn btn--primary'>Back to Settings</a></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

static void handleUploadStatus() {
  TransmissionSettings tx;
  loadTransmissionSettings(tx);

  UploadCursor cursor = {0, 0, 0, 0, 0};
  uint32_t pendingBytes = 0;
  uint32_t pendingRows = 0;
  if (flashIsReady()) {
    gUploadQueue.init();
    cursor = gUploadQueue.getCursor();
    pendingBytes = gUploadQueue.getPendingBytes();
    pendingRows = gUploadQueue.getPendingRows();
  }
  const uint64_t fsTotal = (uint64_t)LittleFS.totalBytes();
  const uint64_t fsUsed  = (uint64_t)LittleFS.usedBytes();
  const uint32_t flashUsagePct = (fsTotal > 0)
    ? (uint32_t)((fsUsed * 100ULL) / fsTotal) : 0;

  String json;
  json.reserve(320);
  json += "{";
  json += "\"enabled\":";
  json += tx.enabled ? "true" : "false";
  json += ",\"cursorOffset\":";
  json += String(cursor.byteOffset);
  json += ",\"pendingBytes\":";
  json += String(pendingBytes);
  json += ",\"pendingRows\":";
  json += String(pendingRows);
  json += ",\"rowsUploaded\":";
  json += String(cursor.rowsUploaded);
  json += ",\"lastUploadUnix\":";
  json += String(cursor.lastUploadUnix);
  json += ",\"retryCount\":";
  json += String(cursor.retryCount);
  json += ",\"flashUsagePct\":";
  json += String(flashUsagePct);
  json += ",\"flashTotalBytes\":";
  json += String((unsigned long)fsTotal);
  json += ",\"flashUsedBytes\":";
  json += String((unsigned long)fsUsed);
  json += "}";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// Start + loop
// ---------------------------------------------------------------------------
static void handleShutdown() {
  gShutdownRequested = true;
  if (isAjaxRequest()) {
    sendAjaxResult(true, "Sync & power down — arming sync alarm");
    return;
  }
  String html = headCommon("Shutting Down");
  html += F("<div class='section center'><h3>Sync &amp; Power Down</h3>"
            "<p>Arming phase-aligned sync alarm and powering off...</p>"
            "<p class='muted'>The board will wake at the next sync time.</p></div>");
  html += footCommon();
  server.send(200, "text/html", html);
}

void startConfigServer() {
  // WiFi AP + STA (AP for web UI, STA for ESP-NOW on same channel).
  // ESP-NOW was already initialised by initEspNowConfig() before this call.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  bool apOk = WiFi.softAP(ssid.c_str(), password, ESPNOW_CHANNEL, false, 4);
  if (!apOk) {
    Serial.println("[AP] SoftAP failed to start");
  } else {
    Serial.println("[AP] SoftAP started");
  }
  Serial.print("[AP] SSID: "); Serial.println(WiFi.softAPSSID());
  Serial.print("[AP] IP: ");   Serial.println(WiFi.softAPIP());
  Serial.print("[AP] MAC: ");   Serial.println(WiFi.softAPmacAddress());
  Serial.print("[AP] channel: "); Serial.println(WiFi.channel());

  // Routes
  server.on("/", HTTP_ANY, handleRoot);
  server.on("/generate_204", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/gen_204", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/hotspot-detect.html", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/library/test/success.html", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/connecttest.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/ncsi.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/success.txt", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/wpad.dat", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/redirect", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/fwlink", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/mobile/status.php", HTTP_ANY, sendCaptivePortalLanding);
  server.on("/favicon.ico", HTTP_ANY, []() { server.send(204); });

  // --- New canonical routes ---
  server.on("/shutdown", HTTP_POST, handleShutdown);
  server.on("/set-time", HTTP_POST, handleSetTime);
  server.on("/download-csv", HTTP_GET, handleDownloadCSV);
  server.on("/discover-nodes", HTTP_POST, handleDiscoverNodes);
  server.on("/set-wake-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/set-sync-mode", HTTP_POST, handleSetSyncMode);
  server.on("/set-sync-time", HTTP_POST, handleSetSyncTime);
  server.on("/ui-status", HTTP_GET, []() {
    server.send(200, "application/json", buildStatusJson());
  });

  server.on("/stations", HTTP_GET, handleStationsPage);
  server.on("/station", HTTP_GET, handleStationDetail);
  server.on("/station", HTTP_POST, handleNodeConfigSave);
  server.on("/revert-node", HTTP_POST, handleRevertNode);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/set-transmission", HTTP_POST, handleSetTransmission);
  server.on("/manual-upload", HTTP_POST, handleManualUpload);
  server.on("/upload-status", HTTP_GET, handleUploadStatus);

  // --- Legacy route aliases (backwards compatibility) ---
  // GET routes redirect to the new canonical paths.
  server.on("/nodes", HTTP_GET, []() {
    server.sendHeader("Location", "/stations", true);
    server.send(302, "text/plain", "");
  });
  server.on("/node-config", HTTP_GET, []() {
    server.sendHeader("Location", "/station", true);
    server.send(302, "text/plain", "");
  });
  server.on("/upload", HTTP_GET, []() {
    server.sendHeader("Location", "/settings", true);
    server.send(302, "text/plain", "");
  });
  // POST routes keep working on the old paths (clients may still POST here).
  server.on("/node-config", HTTP_POST, handleNodeConfigSave);

  // --- UI form-action aliases (Field UI form actions) ---
  server.on("/start", HTTP_POST, handleShutdown);
  server.on("/find-stations", HTTP_POST, handleDiscoverNodes);
  server.on("/set-recording-interval", HTTP_POST, handleSetWakeInterval);
  server.on("/save-settings", HTTP_POST, handleSetTransmission);
  server.on("/export", HTTP_GET, []() {
    server.sendHeader("Location", "/download-csv", true);
    server.send(302, "text/plain", "");
  });

  server.onNotFound(sendCaptivePortalLanding);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("[AP] Captive portal DNS enabled");

  server.begin();
  Serial.println("[AP] Web server started");
}

void configServerLoop() {
  if (gShutdownRequested) {
    return;  // Signal to main loop to exit config mode
  }
  dnsServer.processNextRequest();
  server.handleClient();
  espnowConfigPoll();
}
