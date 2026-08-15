// Bring-up: cross-border modem "warm-up" for a roaming SIM.
//
// WHY THIS EXISTS
// ---------------
// The hub was deployed in Germany and is now being tested in Australia on the
// SAME (Things Mobile) SIM. The first post-deployment upload did not go out.
// Production firmware gives network registration a hard 60 s budget:
//
//   src/main.cpp:961    modem.waitForNetwork(60000)
//
// 60 s is plenty for a modem that already knows where it is — it re-camps on
// the last-used PLMN/band cached in module NVM and the SIM's EF_LOCI. It is
// NOT enough for a first-ever registration in a new region, where the modem
// has to run a full band scan, then a roaming PLMN search, then attach. In
// Australia that scan is made worse by two things:
//
//   1. The A7670G is a global variant (LTE-FDD B1/2/3/4/5/7/8/12/13/18/19/20/
//      25/26/28/66, LTE-TDD B34/38/39/40/41, plus quad-band GSM). With the
//      preferred mode left at "automatic" the module also sweeps the GSM
//      bands — and Australia has NO 2G and NO 3G left (TPG Dec 2023, Telstra
//      and Optus Oct 2024). Every second spent on GSM/WCDMA is wasted.
//   2. A roaming attempt that gets rejected writes the operator into the SIM's
//      forbidden-PLMN list (EF_FPLMN). While a PLMN sits in that list the
//      modem will not retry it in automatic mode — so a bad first attempt can
//      keep failing permanently, across power cycles, until the list is cleared.
//
// WHAT THIS SCRIPT DOES
// ---------------------
// It changes NO production code. Everything it does either lives in the
// modem's own non-volatile memory (AT+CNMP preferred mode, AT+COPS selection,
// last-camped cell) or on the SIM (EF_FPLMN, EF_LOCI) — all of which survive
// the rail being cut, and all of which the shipped firmware then benefits from
// on its next scheduled sync.
//
//   Phase 1  Inventory: SIM, IMEI, ICCID, preferred mode, registration state,
//            signal, serving cell, and the current FPLMN contents.
//   Phase 2  Prepare: CEREG=2 verbose URCs, force LTE-only (no 2G/3G exists
//            here), return operator selection to automatic.
//   Phase 3  Cold registration with a LONG budget (default 10 min), logging
//            elapsed time / CSQ / CPSI every 10 s. This is the measurement
//            that tells us whether 60 s was merely too short.
//   Phase 4  Escalation, only if phase 3 failed: read + optionally clear
//            EF_FPLMN, run a full AT+COPS=? scan and print every visible
//            network, then try manual registration on each Australian PLMN.
//   Phase 5  Data-plane check: CGATT / PDP context on the configured APN.
//   Phase 6  THE POINT OF THE EXERCISE — power-cycle the modem exactly as the
//            firmware does between syncs, then time registration AGAIN. If the
//            warm figure is comfortably under 60 s, the deployed firmware will
//            upload unchanged. If it is not, the report says so explicitly.
//
// SAFE TO RE-RUN. Nothing here writes to LittleFS, NVS or the upload queue.
//
// Build/flash:
//   pio run -e mothership-v1-modem-roaming-warmup -t upload -t monitor
//   (add --upload-port /dev/ttyUSB0 on Linux — the shared [env] block is COM5)

#include <Arduino.h>
#include "comms/modem_driver.h"
#include "config/sim_settings.h"

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

// Cold-registration budget. 10 min is deliberately generous: we want to
// MEASURE the true first-registration time, not re-hit the production limit.
static constexpr uint32_t kColdRegTimeoutMs = 10UL * 60UL * 1000UL;

// Warm re-registration budget after the power cycle. Kept a bit above the
// production 60 s so an "only just too slow" result is visible rather than
// looking like a hard failure.
static constexpr uint32_t kWarmRegTimeoutMs = 3UL * 60UL * 1000UL;

// The production budget we are judging the warm result against.
static constexpr uint32_t kProductionRegBudgetMs = 60UL * 1000UL;

// Force LTE-only (AT+CNMP=38). Correct for Australia — there is no 2G or 3G
// to fall back to. This is stored in module NVM and PERSISTS, which is exactly
// how the shipped firmware inherits the benefit. Set false to leave the
// preferred mode untouched.
static constexpr bool kForceLteOnly = true;

// Clear the SIM's forbidden-PLMN list if phase 3 fails. Off by default: it is
// a write to the SIM, and clearing it at the wrong moment can itself disrupt
// registration. Turn it on for a second run if phase 1/4 reports a non-empty
// list. See the FPLMN notes at the bottom of this file.
static constexpr bool kClearFplmnOnFailure = false;

// Australian PLMNs, tried in order during phase 4 manual selection.
struct PlmnEntry { const char* mccmnc; const char* name; };
static const PlmnEntry kAuPlmns[] = {
  {"50501", "Telstra"},
  {"50502", "Optus"},
  {"50503", "Vodafone AU (TPG)"},
  {"50506", "Optus (secondary)"},
  {"50571", "Telstra (secondary)"},
};
static constexpr size_t kAuPlmnCount = sizeof(kAuPlmns) / sizeof(kAuPlmns[0]);

// Per-operator manual-registration budget in phase 4.
static constexpr uint32_t kManualRegTimeoutMs = 90UL * 1000UL;

// ---------------------------------------------------------------------------

ModemDriver modem;

// Results carried to the final report.
static bool     gColdRegistered  = false;
static uint32_t gColdRegMs       = 0;
static bool     gWarmRegistered  = false;
static uint32_t gWarmRegMs       = 0;
static String   gOperator        = "";
static String   gServingCell     = "";
static bool     gFplmnDirty      = false;
static bool     gFplmnCleared    = false;
static bool     gManualNeeded    = false;
static String   gManualPlmn      = "";

// ---------------------------------------------------------------------------
// AT helper
// ---------------------------------------------------------------------------
//
// ModemDriver::sendAT() is private, so this test drives Serial2 directly —
// the same pattern tests/test_sim_thinkmobile.cpp uses. Serial2 has already
// been opened by modem.powerOn().

static String at(const char* cmd, uint32_t timeoutMs = 3000, bool echo = true) {
  while (Serial2.available()) { Serial2.read(); }
  Serial2.print(cmd);
  Serial2.print("\r\n");
  Serial2.flush();

  String resp;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (Serial2.available()) {
      resp += (char)Serial2.read();
      if (resp.indexOf("OK\r\n") >= 0) goto done;
      if (resp.indexOf("ERROR\r\n") >= 0) goto done;
      if (resp.indexOf("COMMAND NOT SUPPORT") >= 0) goto done;
      start = millis();  // idle-based timeout: long scans keep the line alive
    }
    delay(5);
  }
done:
  if (echo) {
    String pretty = resp;
    pretty.trim();
    pretty.replace("\r\n", " | ");
    Serial.printf("  %-28s -> %s\n", cmd, pretty.length() ? pretty.c_str() : "(no response)");
  }
  return resp;
}

static bool atOk(const char* cmd, uint32_t timeoutMs = 3000) {
  return at(cmd, timeoutMs).indexOf("OK\r\n") >= 0;
}

// Extract the trailing quoted field of a +COPS? response (operator name).
static String parseOperatorName(const String& copsResp) {
  int q1 = copsResp.indexOf('"');
  if (q1 < 0) return String();
  int q2 = copsResp.indexOf('"', q1 + 1);
  if (q2 < 0) return String();
  return copsResp.substring(q1 + 1, q2);
}

// True if CREG/CEREG report 1 (home) or 5 (roaming). Roaming is the expected
// state here — a German SIM on an Australian network is by definition 5.
static bool registeredNow(String* whichOut = nullptr) {
  String r = at("AT+CEREG?", 3000, false);
  if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
    // Guard against matching the <n>=2 URC field rather than <stat>.
    int idx = r.indexOf("+CEREG:");
    if (idx >= 0) {
      int comma = r.indexOf(',', idx);
      if (comma >= 0) {
        int stat = r.substring(comma + 1, comma + 2).toInt();
        if (stat == 1 || stat == 5) {
          if (whichOut) *whichOut = (stat == 5) ? "CEREG roaming" : "CEREG home";
          return true;
        }
      }
    }
  }
  r = at("AT+CREG?", 3000, false);
  int idx = r.indexOf("+CREG:");
  if (idx >= 0) {
    int comma = r.indexOf(',', idx);
    if (comma >= 0) {
      int stat = r.substring(comma + 1, comma + 2).toInt();
      if (stat == 1 || stat == 5) {
        if (whichOut) *whichOut = (stat == 5) ? "CREG roaming" : "CREG home";
        return true;
      }
    }
  }
  return false;
}

// Poll for registration up to timeoutMs, logging progress every 10 s.
// Returns elapsed ms on success, 0 on timeout.
static uint32_t waitRegistered(uint32_t timeoutMs, const char* label) {
  Serial.printf("\n  Waiting for registration — budget %lu s (%s)\n",
                (unsigned long)(timeoutMs / 1000), label);
  const uint32_t start = millis();
  uint32_t lastLog = 0;

  while (millis() - start < timeoutMs) {
    String which;
    if (registeredNow(&which)) {
      const uint32_t elapsed = millis() - start;
      Serial.printf("  >>> REGISTERED after %lu.%lu s (%s)\n",
                    (unsigned long)(elapsed / 1000),
                    (unsigned long)((elapsed % 1000) / 100), which.c_str());
      return elapsed ? elapsed : 1;
    }

    const uint32_t elapsed = millis() - start;
    if (elapsed - lastLog >= 10000) {
      lastLog = elapsed;
      String csq  = at("AT+CSQ", 3000, false);
      String cpsi = at("AT+CPSI?", 3000, false);
      csq.trim();  csq.replace("\r\n", " ");
      cpsi.trim(); cpsi.replace("\r\n", " ");
      Serial.printf("  [%3lus] %s | %s\n", (unsigned long)(elapsed / 1000),
                    csq.c_str(), cpsi.c_str());
    }
    delay(1000);
  }

  Serial.printf("  >>> NOT registered within %lu s\n",
                (unsigned long)(timeoutMs / 1000));
  return 0;
}

// ---------------------------------------------------------------------------
// EF_FPLMN (forbidden PLMN list) — file 0x6F7B = 28539, 12 bytes = 4 entries.
// An empty list reads back as 24 'F' characters. Anything else means at least
// one network has rejected this SIM and been blacklisted on it.
// ---------------------------------------------------------------------------

static bool readFplmn(String& hexOut) {
  String r = at("AT+CRSM=176,28539,0,0,12", 5000);
  int idx = r.indexOf("+CRSM:");
  if (idx < 0) return false;
  int q1 = r.indexOf('"', idx);
  if (q1 < 0) return false;
  int q2 = r.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  hexOut = r.substring(q1 + 1, q2);
  hexOut.toUpperCase();
  return true;
}

static void reportFplmn() {
  String hex;
  if (!readFplmn(hex)) {
    Serial.println("  FPLMN: could not be read (not fatal)");
    return;
  }
  bool empty = true;
  for (size_t i = 0; i < hex.length(); i++) {
    if (hex[i] != 'F') { empty = false; break; }
  }
  gFplmnDirty = !empty;
  Serial.printf("  FPLMN raw: %s\n", hex.c_str());
  if (empty) {
    Serial.println("  FPLMN: empty — no network has blacklisted this SIM.");
  } else {
    Serial.println("  FPLMN: *** NOT EMPTY *** — one or more networks have");
    Serial.println("         rejected this SIM and will not be retried in");
    Serial.println("         automatic mode. Decode: each 3-byte group is a");
    Serial.println("         swapped-nibble MCC/MNC (e.g. '05F505' = 505 05).");
  }
}

static void clearFplmn() {
  Serial.println("\n  Clearing EF_FPLMN (writing 24 x 'F')...");
  bool ok = atOk("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 8000);
  Serial.printf("  Clear %s\n", ok ? "accepted" : "REJECTED by SIM");
  if (ok) {
    gFplmnCleared = true;
    // The modem only re-reads the SIM file on a fresh attach, so bounce the
    // radio rather than the whole module.
    Serial.println("  Bouncing radio (CFUN=0 -> CFUN=1) so the SIM file is re-read...");
    atOk("AT+CFUN=0", 10000);
    delay(2000);
    atOk("AT+CFUN=1", 10000);
    delay(5000);
  }
}

// ---------------------------------------------------------------------------
// Phases
// ---------------------------------------------------------------------------

static void phase1Inventory() {
  Serial.println("\n================ PHASE 1: INVENTORY ================");

  at("ATE0");                    // echo off — keeps the log readable
  at("AT+CPIN?", 5000);
  at("AT+CGSN");                 // IMEI
  at("AT+CICCID", 5000);         // SIM identity — confirms it IS the German SIM
  at("AT+CFUN?");
  at("AT+CNMP?");                // current preferred mode
  at("AT+CNMP=?", 5000);         // what this module actually supports
  at("AT+CNBP?", 5000);          // band preference mask (may not be supported)
  at("AT+COPS?", 5000);
  at("AT+CREG?");
  at("AT+CEREG?");
  at("AT+CGREG?");
  at("AT+CGATT?");
  at("AT+CSQ");
  at("AT+CESQ", 5000);
  at("AT+CPSI?", 5000);

  Serial.println();
  reportFplmn();
}

static void phase2Prepare() {
  Serial.println("\n================ PHASE 2: PREPARE ==================");

  // Verbose registration URCs — location info is printed with the state, which
  // makes it obvious when the modem is scanning vs denied vs searching.
  atOk("AT+CEREG=2");
  atOk("AT+CREG=2");

  if (kForceLteOnly) {
    Serial.println("  Forcing LTE-only (AT+CNMP=38).");
    Serial.println("  Australia has no 2G and no 3G left (TPG 2023, Telstra +");
    Serial.println("  Optus Oct 2024), so scanning GSM/WCDMA is pure wasted");
    Serial.println("  time. This setting lives in module NVM and persists.");
    if (!atOk("AT+CNMP=38", 10000)) {
      Serial.println("  CNMP=38 rejected — falling back to automatic (2).");
      atOk("AT+CNMP=2", 10000);
    }
    delay(2000);
    at("AT+CNMP?");
  }

  // Undo any stale manual operator lock left from a previous session/country.
  Serial.println("  Returning operator selection to automatic (AT+COPS=0).");
  atOk("AT+COPS=0", 30000);
  delay(1000);
}

static void phase3ColdRegistration() {
  Serial.println("\n=========== PHASE 3: COLD REGISTRATION =============");
  Serial.println("  This is the number that explains the missed upload.");
  Serial.printf("  Production firmware allows %lu s (main.cpp:961).\n",
                (unsigned long)(kProductionRegBudgetMs / 1000));

  gColdRegMs = waitRegistered(kColdRegTimeoutMs, "cold / first-in-country");
  gColdRegistered = (gColdRegMs != 0);

  if (gColdRegistered) {
    String cops = at("AT+COPS?", 5000);
    gOperator = parseOperatorName(cops);
    String cpsi = at("AT+CPSI?", 5000);
    cpsi.trim(); cpsi.replace("\r\n", " ");
    gServingCell = cpsi;
    at("AT+CSQ");
    at("AT+CESQ", 5000);
  }
}

static void phase4Escalate() {
  Serial.println("\n============ PHASE 4: ESCALATION ==================");
  Serial.println("  Automatic registration failed. Working through the");
  Serial.println("  known causes in order of likelihood.");

  Serial.println("\n  4a. Forbidden-PLMN list:");
  reportFplmn();
  if (gFplmnDirty && kClearFplmnOnFailure) {
    clearFplmn();
    gColdRegMs = waitRegistered(120000, "after FPLMN clear");
    gColdRegistered = (gColdRegMs != 0);
    if (gColdRegistered) return;
  } else if (gFplmnDirty) {
    Serial.println("  List is dirty but kClearFplmnOnFailure is false.");
    Serial.println("  Set it true and re-run to clear it.");
  }

  Serial.println("\n  4b. Full network scan (AT+COPS=?) — up to ~3 min, be patient.");
  Serial.println("      Every operator listed here is one the radio can SEE;");
  Serial.println("      if the list is empty the problem is RF/antenna, not");
  Serial.println("      roaming policy.");
  String scan = at("AT+COPS=?", 200000);
  Serial.println("  --- raw scan result ---");
  Serial.println(scan);
  Serial.println("  -----------------------");

  Serial.println("\n  4c. Manual registration attempts on Australian PLMNs.");
  Serial.println("      A manual attempt bypasses the FPLMN list, so this");
  Serial.println("      also tells us whether blacklisting is the cause.");
  for (size_t i = 0; i < kAuPlmnCount; i++) {
    String cmd = String("AT+COPS=1,2,\"") + kAuPlmns[i].mccmnc + "\"";
    Serial.printf("\n  Trying %s (%s)...\n", kAuPlmns[i].name, kAuPlmns[i].mccmnc);
    at(cmd.c_str(), 60000);

    uint32_t ms = waitRegistered(kManualRegTimeoutMs, kAuPlmns[i].name);
    if (ms) {
      gColdRegistered = true;
      gColdRegMs      = ms;
      gManualNeeded   = true;
      gManualPlmn     = String(kAuPlmns[i].mccmnc) + " (" + kAuPlmns[i].name + ")";
      String cops = at("AT+COPS?", 5000);
      gOperator = parseOperatorName(cops);
      String cpsi = at("AT+CPSI?", 5000);
      cpsi.trim(); cpsi.replace("\r\n", " ");
      gServingCell = cpsi;

      // Hand control back to automatic. A manual lock is stored in NVM and
      // would follow this hub home to Germany, where that PLMN does not
      // exist — the hub would then never register again. Now that a
      // successful attach has cached the cell, automatic should re-find it.
      Serial.println("\n  Releasing manual lock (AT+COPS=0) — a stored manual");
      Serial.println("  PLMN would break this hub when it leaves Australia.");
      atOk("AT+COPS=0", 30000);
      waitRegistered(120000, "automatic, after successful manual attach");
      return;
    }
  }

  Serial.println("\n  All manual attempts failed. Restoring automatic selection.");
  atOk("AT+COPS=0", 30000);
}

static void phase5DataPlane() {
  Serial.println("\n============ PHASE 5: DATA PLANE ==================");

  SimSettings sim;
  loadSimSettings(sim);
  Serial.printf("  Configured APN: \"%s\"%s\n", sim.apn.c_str(),
                sim.apnUser.length() ? " (with PAP credentials)" : "");

  at("AT+CGATT?");
  String cgdcont = String("AT+CGDCONT=1,\"IP\",\"") + sim.apn + "\"";
  at(cgdcont.c_str(), 10000);
  at("AT+CGACT=1,1", 60000);
  at("AT+CGPADDR=1", 10000);
  Serial.println("  A non-zero IP address above means the data plane is up and");
  Serial.println("  the roaming SIM is authorised for packet service here.");
}

static void phase6WarmRecheck() {
  Serial.println("\n===== PHASE 6: POWER-CYCLE + WARM RE-REGISTRATION =====");
  Serial.println("  Cutting the modem exactly as the firmware does between");
  Serial.println("  syncs, then timing registration again. THIS is the number");
  Serial.println("  that predicts whether the deployed firmware will upload.");

  modem.gracefulShutdown();
  delay(5000);

  Serial.println("\n  Powering back on...");
  if (!modem.powerOn()) {
    Serial.println("  FAIL — modem did not come back up.");
    return;
  }
  at("ATE0");
  atOk("AT+CEREG=2");

  gWarmRegMs = waitRegistered(kWarmRegTimeoutMs, "warm / cached cell");
  gWarmRegistered = (gWarmRegMs != 0);
  if (gWarmRegistered) {
    at("AT+COPS?", 5000);
    at("AT+CPSI?", 5000);
  }
}

static void finalReport() {
  Serial.println("\n\n#####################################################");
  Serial.println("#              ROAMING WARM-UP REPORT               #");
  Serial.println("#####################################################\n");

  Serial.printf("  Cold registration : %s",
                gColdRegistered ? "OK" : "FAILED");
  if (gColdRegistered) Serial.printf("  (%lu s)", (unsigned long)(gColdRegMs / 1000));
  Serial.println();

  Serial.printf("  Warm registration : %s",
                gWarmRegistered ? "OK" : "FAILED");
  if (gWarmRegistered) Serial.printf("  (%lu s)", (unsigned long)(gWarmRegMs / 1000));
  Serial.println();

  Serial.printf("  Operator          : %s\n",
                gOperator.length() ? gOperator.c_str() : "(unknown)");
  Serial.printf("  Serving cell      : %s\n",
                gServingCell.length() ? gServingCell.c_str() : "(unknown)");
  Serial.printf("  FPLMN was dirty   : %s%s\n", gFplmnDirty ? "YES" : "no",
                gFplmnCleared ? " (cleared this run)" : "");
  Serial.printf("  Manual PLMN needed: %s\n",
                gManualNeeded ? gManualPlmn.c_str() : "no");

  Serial.println("\n  VERDICT");
  Serial.println("  -------");
  if (!gColdRegistered) {
    Serial.println("  The modem never registered here at all. This is NOT a");
    Serial.println("  timeout problem — do not just raise the 60 s budget.");
    Serial.println("  Check, in this order: antenna connected and outdoors;");
    Serial.println("  whether AT+COPS=? in phase 4 saw ANY operator; whether");
    Serial.println("  the SIM's plan actually includes Australian roaming.");
  } else if (!gWarmRegistered) {
    Serial.println("  Cold registration worked but the modem did not come back");
    Serial.println("  after a power cycle within the warm budget. Re-run this");
    Serial.println("  script; if it repeats, the hub needs a longer");
    Serial.println("  registration budget in firmware, not just a warm-up.");
  } else if (gWarmRegMs <= kProductionRegBudgetMs) {
    Serial.println("  GOOD — after this warm-up the modem re-registers inside");
    Serial.printf("  the production %lu s budget. The deployed firmware should\n",
                  (unsigned long)(kProductionRegBudgetMs / 1000));
    Serial.println("  now upload unchanged. Flash the production firmware back");
    Serial.println("  and confirm on the next scheduled sync.");
  } else {
    Serial.printf("  MARGINAL — warm registration took %lu s, over the %lu s\n",
                  (unsigned long)(gWarmRegMs / 1000),
                  (unsigned long)(kProductionRegBudgetMs / 1000));
    Serial.println("  production budget. A warm-up alone will not fix this;");
    Serial.println("  waitForNetwork() at main.cpp:961 needs a larger value.");
  }

  if (gManualNeeded) {
    Serial.println("\n  NOTE: registration only succeeded with a MANUAL operator");
    Serial.println("  selection. The script released the lock afterwards on");
    Serial.println("  purpose — a stored manual PLMN would prevent this hub from");
    Serial.println("  ever registering again once it leaves Australia.");
  }
  if (gFplmnDirty && !gFplmnCleared) {
    Serial.println("\n  NOTE: the SIM's forbidden-PLMN list is not empty. Set");
    Serial.println("  kClearFplmnOnFailure = true and re-run to clear it.");
  }

  Serial.println("\n#####################################################");
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n=== MODEM ROAMING WARM-UP (cross-border bring-up) ===");
  Serial.println("Reads and warms modem/SIM non-volatile state only.");
  Serial.println("No firmware behaviour is modified.\n");

  modem.init();
  Serial.println("Powering on modem (rail -> PWRKEY -> boot)...");
  if (!modem.powerOn()) {
    Serial.println("FAIL — modem did not respond to AT. Nothing else can run.");
    Serial.println("Check the 4 V rail, PG, and the PWRKEY pulse first.");
    while (true) delay(1000);
  }
  Serial.println("Modem is up.\n");

  phase1Inventory();
  phase2Prepare();
  phase3ColdRegistration();
  if (!gColdRegistered) phase4Escalate();
  if (gColdRegistered)  phase5DataPlane();
  phase6WarmRecheck();
  finalReport();

  Serial.println("\nLeaving the modem powered so the cached cell settles.");
  Serial.println("Power the hub down normally when you are done.");
}

void loop() {
  delay(10000);
}

// ---------------------------------------------------------------------------
// Reference — why each lever works
// ---------------------------------------------------------------------------
//
// AT+CNMP=38 (LTE only)
//   The A7670G global variant carries quad-band GSM alongside its LTE bands.
//   In automatic mode the module includes those in its search. Australia
//   switched off 3G across all three carriers (TPG Dec 2023, Telstra and
//   Optus Oct 2024) and 2G years before that, so any non-LTE scanning here is
//   guaranteed-wasted airtime. Stored in module NVM; survives power cycles,
//   which is what lets a bring-up script fix a shipped firmware's behaviour.
//   REMEMBER to set it back to 2 (automatic) if this hub returns to a region
//   where 2G fallback is still worth having.
//
// EF_FPLMN (AT+CRSM=176/214,28539,0,0,12)
//   3GPP TS 31.102 file 0x6F7B. Four 3-byte MCC/MNC entries; empty = 24 'F'.
//   A network that rejects a roaming attempt is written here BY THE SIM, and
//   automatic selection then skips it — permanently, across reboots. This is
//   the classic reason a SIM that "worked yesterday in another country" never
//   comes back. A manual AT+COPS=1 attempt ignores the list, which is why
//   phase 4 uses manual selection as the diagnostic.
//
// Warm vs cold registration
//   After a successful attach the module caches the camped frequency/PLMN in
//   NVM and the SIM stores location info in EF_LOCI. The next power-up starts
//   from that cached cell instead of a full band sweep, which is the whole
//   reason a 60 s budget is fine in steady state and hopeless on day one in a
//   new country.
//
// Australian PLMN codes
//   505 01 Telstra, 505 02 Optus, 505 03 Vodafone AU (TPG), plus secondary
//   codes 505 06 and 505 71 that appear on some networks.
