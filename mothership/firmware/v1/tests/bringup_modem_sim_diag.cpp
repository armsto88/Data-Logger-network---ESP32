// Mothership V1 bringup: deep SIM detection diagnostic
// Powers on the A7670G and runs a multi-phase SIM probe to distinguish:
//   1. Card-detect (USIM1_DET) line issue  -> modem thinks no SIM inserted
//   2. Signal routing issue (VDD/DATA/CLK/RST) -> SIM interface alive but no card response
//   3. Slow detection -> CPIN eventually flips to READY/SIM PIN
//
// Prints every raw AT response. Ends with a DIAGNOSIS line.
// Graceful power-off at end.

#include <Arduino.h>
#include "modem_at_helper.h"

// ------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------

static void printBanner(const char* title) {
  Serial.println();
  Serial.print("=== ");
  Serial.print(title);
  Serial.println(" ===");
}

static void printCmd(const char* cmd) {
  Serial.print("[CMD] ");
  Serial.println(cmd);
}

static void printRespBlock(const String& resp) {
  Serial.println("----- raw response -----");
  if (resp.length()) {
    Serial.print(resp);
    if (!resp.endsWith("\n")) Serial.println();
  } else {
    Serial.println("(no response)");
  }
  Serial.println("-------------------------");
}

// Returns one of: "READY", "SIM PIN", "SIM PUK", "NOT INSERTED", "UNKNOWN"
static const char* classifyCpin(const String& resp) {
  if (resp.indexOf("+CPIN: READY") >= 0) return "READY";
  if (resp.indexOf("+CPIN: SIM PIN") >= 0) return "SIM PIN";
  if (resp.indexOf("+CPIN: SIM PUK") >= 0) return "SIM PUK";
  if (resp.indexOf("+CPIN: NOT INSERTED") >= 0) return "NOT INSERTED";
  if (resp.indexOf("SIM REMOVED") >= 0) return "NOT INSERTED";
  if (resp.indexOf("SIM not inserted") >= 0) return "NOT INSERTED";
  return "UNKNOWN";
}

// ------------------------------------------------------------------
// Diagnostic state (for summary)
// ------------------------------------------------------------------

struct DiagState {
  // Phase 1
  int cpinAttempts = 0;
  int cpinNotInserted = 0;
  int cpinReady = 0;
  int cpinSimPin = 0;
  bool cpinEverChanged = false;
  String firstCpin;
  String lastCpin;
  // Phase 2
  String csimResp;
  bool csimResponded = false;
  String csvnResp;
  String ccidResp;
  String cimiResp;
  // Phase 3
  String cfunResp;
  String cpinAfterCfun[3];
  int cpinAfterCfunCount = 0;
  // Phase 4
  String csimApduResp;
  bool csimApduResponded = false;
};

static DiagState g_diag;

// ------------------------------------------------------------------
// Phases
// ------------------------------------------------------------------

static void phase1_cpinPoll() {
  printBanner("PHASE 1: Repeated AT+CPIN? polling (30s, every 2s)");
  String prevClass = "";
  for (int i = 1; i <= 15; ++i) {
    String resp;
    modemSendATRaw("AT+CPIN?", resp, 5000);
    const char* cls = classifyCpin(resp);
    Serial.print("#");
    Serial.print(i);
    Serial.print("  t=");
    Serial.print(millis() / 1000);
    Serial.print("s  -> ");
    Serial.println(cls);
    printRespBlock(resp);

    g_diag.cpinAttempts++;
    if (strcmp(cls, "NOT INSERTED") == 0) g_diag.cpinNotInserted++;
    else if (strcmp(cls, "READY") == 0) g_diag.cpinReady++;
    else if (strcmp(cls, "SIM PIN") == 0) g_diag.cpinSimPin++;

    if (g_diag.firstCpin.length() == 0) g_diag.firstCpin = cls;
    g_diag.lastCpin = cls;
    if (prevClass.length() && String(cls) != prevClass) g_diag.cpinEverChanged = true;
    prevClass = cls;

    if (i < 15) delay(2000);
  }

  Serial.println();
  Serial.print("Phase 1 result: attempts=");
  Serial.print(g_diag.cpinAttempts);
  Serial.print(" NOT_INSERTED=");
  Serial.print(g_diag.cpinNotInserted);
  Serial.print(" READY=");
  Serial.print(g_diag.cpinReady);
  Serial.print(" SIM_PIN=");
  Serial.print(g_diag.cpinSimPin);
  Serial.print(" changed=");
  Serial.println(g_diag.cpinEverChanged ? "yes" : "no");
}

static void phase2_simVoltage() {
  printBanner("PHASE 2: SIM voltage / interface probes");
  printCmd("AT+CSVN");
  modemSendATRaw("AT+CSVN", g_diag.csvnResp, 3000);
  printRespBlock(g_diag.csvnResp);

  printCmd("AT+CSIM=0");
  modemSendATRaw("AT+CSIM=0", g_diag.csimResp, 3000);
  printRespBlock(g_diag.csimResp);
  g_diag.csimResponded = g_diag.csimResp.length() > 0;

  printCmd("AT+CCID");
  modemSendATRaw("AT+CCID", g_diag.ccidResp, 3000);
  printRespBlock(g_diag.ccidResp);

  printCmd("AT+CIMI");
  modemSendATRaw("AT+CIMI", g_diag.cimiResp, 3000);
  printRespBlock(g_diag.cimiResp);

  Serial.println();
  Serial.print("Phase 2 result: CSIM responded=");
  Serial.println(g_diag.csimResponded ? "yes" : "no");
}

static void phase3_cfun() {
  printBanner("PHASE 3: Functionality mode (CFUN) check + re-poll");
  printCmd("AT+CFUN?");
  modemSendATRaw("AT+CFUN?", g_diag.cfunResp, 3000);
  printRespBlock(g_diag.cfunResp);

  // Ensure full functionality.
  printCmd("AT+CFUN=1");
  String cfunSet;
  modemSendATRaw("AT+CFUN=1", cfunSet, 5000);
  printRespBlock(cfunSet);
  delay(2000);

  for (int i = 0; i < 3; ++i) {
    String resp;
    modemSendATRaw("AT+CPIN?", resp, 5000);
    const char* cls = classifyCpin(resp);
    Serial.print("re-poll #");
    Serial.print(i + 1);
    Serial.print(" -> ");
    Serial.println(cls);
    printRespBlock(resp);
    g_diag.cpinAfterCfun[i] = cls;
    g_diag.cpinAfterCfunCount++;
    if (i < 2) delay(2000);
  }

  Serial.println();
  Serial.print("Phase 3 result: CFUN resp=");
  Serial.print(g_diag.cfunResp.length() ? "(see above)" : "(none)");
  Serial.print("  re-polls: ");
  for (int i = 0; i < g_diag.cpinAfterCfunCount; ++i) {
    if (i) Serial.print(", ");
    Serial.print(g_diag.cpinAfterCfun[i]);
  }
  Serial.println();
}

static void phase4_apdu() {
  printBanner("PHASE 4: Raw SIM APDU (dummy SELECT)");
  printCmd("AT+CSIM=10,\"A0000000000000\"");
  modemSendATRaw("AT+CSIM=10,\"A0000000000000\"", g_diag.csimApduResp, 5000);
  printRespBlock(g_diag.csimApduResp);
  g_diag.csimApduResponded = g_diag.csimApduResp.length() > 0;

  Serial.println();
  Serial.print("Phase 4 result: APDU responded=");
  Serial.println(g_diag.csimApduResponded ? "yes" : "no");
}

static void printSummary() {
  printBanner("SUMMARY");
  Serial.println("Phase 1 (CPIN poll):");
  Serial.print("  attempts=");
  Serial.print(g_diag.cpinAttempts);
  Serial.print("  NOT_INSERTED=");
  Serial.print(g_diag.cpinNotInserted);
  Serial.print("  READY=");
  Serial.print(g_diag.cpinReady);
  Serial.print("  SIM_PIN=");
  Serial.print(g_diag.cpinSimPin);
  Serial.print("  first=");
  Serial.print(g_diag.firstCpin);
  Serial.print("  last=");
  Serial.print(g_diag.lastCpin);
  Serial.print("  changed=");
  Serial.println(g_diag.cpinEverChanged ? "yes" : "no");

  Serial.println("Phase 2 (SIM interface):");
  Serial.print("  CSIM=0 responded: ");
  Serial.println(g_diag.csimResponded ? "yes" : "no");
  Serial.print("  CSIM=0 response: ");
  Serial.println(g_diag.csimResp.length() ? g_diag.csimResp : "(none)");

  Serial.println("Phase 3 (CFUN + re-poll):");
  Serial.print("  CFUN?: ");
  Serial.println(g_diag.cfunResp.length() ? g_diag.cfunResp : "(none)");
  Serial.print("  re-poll classes: ");
  for (int i = 0; i < g_diag.cpinAfterCfunCount; ++i) {
    if (i) Serial.print(", ");
    Serial.print(g_diag.cpinAfterCfun[i]);
  }
  Serial.println();

  Serial.println("Phase 4 (APDU):");
  Serial.print("  responded: ");
  Serial.println(g_diag.csimApduResponded ? "yes" : "no");
  Serial.print("  response: ");
  Serial.println(g_diag.csimApduResp.length() ? g_diag.csimApduResp : "(none)");

  // ---- Diagnosis ----
  Serial.println();
  printBanner("DIAGNOSIS");

  bool allNotInserted = (g_diag.cpinNotInserted == g_diag.cpinAttempts) &&
                        (g_diag.cpinAfterCfunCount == 0 ||
                         (g_diag.cpinAfterCfun[0] == "NOT INSERTED" &&
                          g_diag.cpinAfterCfun[1] == "NOT INSERTED" &&
                          g_diag.cpinAfterCfun[2] == "NOT INSERTED"));
  bool everReady = (g_diag.cpinReady > 0) ||
                   (g_diag.cpinAfterCfunCount > 0 &&
                    (g_diag.cpinAfterCfun[0] == "READY" ||
                     g_diag.cpinAfterCfun[1] == "READY" ||
                     g_diag.cpinAfterCfun[2] == "READY"));
  bool everSimPin = (g_diag.cpinSimPin > 0);

  // Did CSIM=0 give a non-"not inserted" error (protocol/timeout)?
  bool csimNonNotInserted = g_diag.csimResponded &&
                             g_diag.csimResp.indexOf("not inserted") < 0 &&
                             g_diag.csimResp.indexOf("NOT INSERTED") < 0;

  if (everReady || everSimPin) {
    Serial.println("  -> SLOW DETECTION: SIM became READY/SIM PIN during polling.");
    Serial.println("     SIM is electrically connected; just needs more time.");
    Serial.println("     PASS: SIM interface functional.");
  } else if (allNotInserted && !csimNonNotInserted) {
    Serial.println("  -> CARD-DETECT (USIM1_DET) LINE ISSUE or VDD not reaching SIM.");
    Serial.println("     All CPIN responses say NOT INSERTED and CSIM gives 'not inserted'.");
    Serial.println("     Modem never sees the card-detect line assert.");
    Serial.println("     Check: USIM1_DET wiring/pull-up, SIM tray presence switch, VDD to SIM.");
    Serial.println("     FAIL: SIM not detected by card-detect.");
  } else if (allNotInserted && csimNonNotInserted) {
    Serial.println("  -> SIGNAL ROUTING ISSUE: CPIN says NOT INSERTED but CSIM gives a");
    Serial.println("     different error (protocol/timeout). VDD may be present but");
    Serial.println("     DATA/CLK/RST to the SIM are wrong or the footprint is misrouted.");
    Serial.println("     Check: SIM VDD present, SIM_DATA/CLK/RST routing, footprint pinout.");
    Serial.println("     FAIL: SIM interface alive but card not responding.");
  } else {
    Serial.println("  -> INCONCLUSIVE: mixed responses. Review raw output above.");
    Serial.println("     first CPIN class: ");
    Serial.print(g_diag.firstCpin);
    Serial.print("  last: ");
    Serial.println(g_diag.lastCpin);
    Serial.println("     FAIL: could not classify.");
  }

  Serial.println();
  Serial.println("Interpretation guide:");
  Serial.println("  - ALL CPIN='NOT INSERTED' + CSIM 'not inserted'  -> card-detect / VDD issue");
  Serial.println("  - CPIN='NOT INSERTED' + CSIM different error     -> signal routing issue");
  Serial.println("  - CPIN eventually READY/SIM PIN                 -> slow detection (OK)");
}

// ------------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------------

void setup() {
  // CRITICAL: assert PWR_HOLD immediately.
  pinMode(PIN_PWR_HOLD, OUTPUT);
  digitalWrite(PIN_PWR_HOLD, HIGH);

  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("=== Mothership V1 Modem SIM Deep Diagnostic ===");
  Serial.println("Distinguishes card-detect vs signal-routing vs slow-detection.");

  // --- Power on ---
  printBanner("Power on: 4V rail + PWRKEY + boot");
  bool pg = modemRailOn();
  if (!pg) {
    Serial.println("FAIL: Power-good did not assert. Aborting.");
    Serial.println("=== Test ABORTED ===");
    return;
  }
  Serial.println("PASS: 4V rail up (ESP_PG HIGH).");

  modemInitUart();
  modemPulsePwrkey();
  if (!modemWaitBoot(15000)) {
    Serial.println("FAIL: Modem did not boot / respond to AT. Aborting.");
    modemGracefulOff();
    Serial.println("=== Test ABORTED ===");
    return;
  }
  Serial.println("PASS: Modem booted.");

  String echo;
  modemSendAT("ATE0", echo, 2000);

  // --- Phases ---
  phase1_cpinPoll();
  phase2_simVoltage();
  phase3_cfun();
  phase4_apdu();

  printSummary();

  // --- Graceful power off ---
  printBanner("Graceful power off");
  modemGracefulOff();
  Serial.println("=== Done. Board stays powered via PWR_HOLD. ===");
}

void loop() {
  // Idle — test runs once in setup().
  delay(5000);
}