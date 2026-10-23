#!/usr/bin/env python3
"""FieldHub v2 r6 bring-up runner.

Flashes one bring-up sketch from mothership/firmware/v2, captures its serial
output into a per-board evidence log, and appends the verdict to the board's
results.csv. Sketches that print RESULT| lines are judged automatically; the
older V1 sketches ask the operator for a verdict.

  scripts/hub_bringup.py list
  scripts/hub_bringup.py run HUB-R6-01 i2c-mux
  scripts/hub_bringup.py run HUB-R6-01 sd-logger --repeat 100     # reboot soak
  scripts/hub_bringup.py run HUB-R6-01 reed --stay                # keep console open
  scripts/hub_bringup.py run HUB-R6-01 monitor                    # safe console, no flash
  scripts/hub_bringup.py record HUB-R6-01 C-rails pass --note "VSYS 3.79 V, 3V3 3.31 V"
  scripts/hub_bringup.py status HUB-R6-01

Serial is always opened with DTR and RTS asserted. On this board DTR=0 with
RTS=1 pulls EN low through Q15; the ESP32 resets, PWR_HOLD drops and, unless
SW10 + USB is holding the latch, the whole rail (and the CH340) goes down.
A deliberate reset (--reset, the default for service-mode tests) is that same
DTR pulse, so it needs SW10 in the USB service position.

Needs pyserial. If the running interpreter lacks it, the script re-runs itself
under PlatformIO's bundled Python (~/.platformio/penv/bin/python).
"""

import argparse
import csv
import datetime as dt
import glob
import os
import re
import select
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    penv = Path.home() / ".platformio/penv"
    # penv/bin/python is a symlink to the system interpreter, so compare the
    # venv prefix, not the resolved executable.
    if (penv / "bin/python").exists() and Path(sys.prefix).resolve() != penv.resolve():
        os.execv(str(penv / "bin/python"), [str(penv / "bin/python"), __file__, *sys.argv[1:]])
    sys.exit("pyserial is missing: run with ~/.platformio/penv/bin/python")

REPO = Path(__file__).resolve().parent.parent
FW_DIR = REPO / "mothership/firmware/v2"
EVIDENCE = REPO / "hardware/FieldHub_v2/revisions/2026-10-10-r6/bringup"
BOARD_RE = re.compile(r"^HUB-R6-\d\d$")

SUMMARY = {"done": r"RESULT\|SUMMARY\|", "pass": r"OVERALL:PASS"}
MODEM = {"done": r"OVERALL (PASS|FAIL)", "pass": r"OVERALL PASS"}
MANUAL = {}


def t(key, env, stage, proves, *, judge=MANUAL, reset=True, timeout=None, start="\n"):
    return {"key": key, "env": env, "stage": stage, "proves": proves,
            "judge": judge, "reset": reset, "timeout": timeout, "start": start}


# Order follows BRINGUP_PLAN_R5.md stages. reset=False marks tests that need
# SW10 in RUN (the board must be able to cut its own power).
LADDER = [
    t("pwr-hold", "mothership-v1-pwrhold", "D", "PWR_HOLD keeps and drops VSYS (15 s on/off)", reset=False),
    t("config-latch", "mothership-v1-config-latch", "D", "U52 config latch read + CONFIG_CLEAR"),
    t("battery-adc", "mothership-v1-battery-adc", "D", "VOLT_ESP divider vs DMM, ESP_PG idle LOW"),
    t("espnow-basic", "mothership-v1-espnow-basic", "D", "ESP-NOW init + receive on ch 11, no AP"),
    t("rtc-alarm", "mothership-v1-rtc-alarm", "E", "DS3231 at 0x68, Alarm 1 fires/clears/re-arms"),
    t("wake-reason", "mothership-v1-wake-reason", "E", "config / RTC / USB wake classification, off-wake", reset=False),
    t("sd-card", "mothership-v1-sd-card", "F", "first SD mount + write/read at 400 kHz",
      judge={"done": r"bring-up PASSED|SD\.begin\(\) FAILED|FAILED to open", "pass": r"bring-up PASSED"},
      timeout=60),
    t("sd-sweep", "mothership-v2-bringup-sd-sweep", "F", "SD at 0.4/4/10/20/40 MHz, 128 KiB verify each",
      judge=SUMMARY, timeout=300),
    t("sd-nocard", "mothership-v2-bringup-sd-sweep", "F", "card removed: no mount, each attempt fails < 3 s",
      judge=SUMMARY, timeout=120, start="n\n"),
    t("sd-logger", "mothership-v2-test-sd-logger", "F", "production sd_logger.cpp archive regression",
      judge=SUMMARY, timeout=120),
    t("i2c-mux", "mothership-v2-bringup-i2c-mux", "H", "root bus set, TCA9546A, CN17-CN20, 400 kHz",
      judge=SUMMARY, timeout=120),
    t("ads1015", "mothership-v2-bringup-ads1015", "H", "ADS1015 config, AIN0-3 convert + noise",
      judge=SUMMARY, timeout=120),
    t("reed", "mothership-v2-bringup-reed", "H", "GPIO36 idle, ADC1/Wi-Fi immunity, close/spin",
      judge=SUMMARY, timeout=180),
    t("modem-power", "mothership-v1-modem-power", "J", "TPS63020 soft-start, ESP_PG follows 4V_EN"),
    t("modem-uart", "mothership-v1-modem-uart", "J", "PWRKEY, STATUS, UART2 AT through level shifters"),
    t("modem-identify", "mothership-v1-modem-identify", "J", "ATI / IMEI / firmware revision", judge=MODEM, timeout=120),
    t("modem-sim", "mothership-v1-modem-sim", "J", "+CPIN: READY, IMSI", judge=MODEM, timeout=120),
    t("modem-network", "mothership-v1-modem-network", "J", "CSQ/CREG/CEREG/COPS (antenna fitted!)", judge=MODEM, timeout=240),
    t("modem-https", "mothership-v1-sim-test", "J", "registration + ingest ?dry_run=true POST via ModemDriver"),
    t("modem-powercycle", "mothership-v1-modem-powercycle", "J", "3 clean on/AT/CPOF/off cycles", judge=MODEM, timeout=180),
    t("production", "mothership-v1-main", "K", "production firmware boot + config UI"),
    t("monitor", None, "-", "safe serial console only (no flash, no reset)", reset=False),
]
BY_KEY = {x["key"]: x for x in LADDER}
CSV_FIELDS = ["timestamp", "board", "test", "env", "commit", "dirty", "verdict",
              "detail", "note", "log"]


def pio_bin():
    if os.environ.get("PIO_BIN"):
        return os.environ["PIO_BIN"]
    penv = Path.home() / ".platformio/penv/bin/pio"
    return str(penv) if penv.exists() else "pio"


def default_port():
    if os.environ.get("HUB_PORT"):
        return os.environ["HUB_PORT"]
    ids = sorted(glob.glob("/dev/serial/by-id/usb-1a86_USB_Serial*"))
    return ids[0] if len(ids) == 1 else "/dev/ttyUSB0"


def git_state():
    def git(*a):
        return subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True).stdout.strip()
    return git("rev-parse", "--short", "HEAD"), "yes" if git("status", "--porcelain", "--", "mothership/firmware") else "no"


def open_port(path):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = path, 115200, 0
    s.dtr = True  # both set BEFORE open(); see module docstring
    s.rts = True
    s.open()
    return s


def pulse_reset(s):
    s.dtr = False  # (DTR=0, RTS=1) -> EN low
    time.sleep(0.12)
    s.dtr = True


def rel(path):
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def append_result(board, row):
    d = EVIDENCE / board
    d.mkdir(parents=True, exist_ok=True)
    f = d / "results.csv"
    new = not f.exists()
    with f.open("a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


class Session:
    """One serial capture: tee to console + log, forward stdin, survive port loss."""

    def __init__(self, port, log):
        self.port, self.log, self.t0 = port, log, time.monotonic()
        self.ser = None
        self.buf = b""
        self.stdin_open = True

    def note(self, msg):
        self._emit(f"## {msg}")

    def _emit(self, line):
        stamp = f"[{time.monotonic() - self.t0:9.3f}] "
        print(line, flush=True)
        self.log.write(stamp + line + "\n")
        self.log.flush()

    def connect(self, wait_s=10.0):
        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            try:
                self.ser = open_port(self.port)
                return True
            except (serial.SerialException, OSError):
                time.sleep(0.25)
        return False

    def send(self, text):
        if self.ser:
            try:
                self.ser.write(text.encode())
            except (serial.SerialException, OSError):
                pass

    def poll(self, timeout=0.05):
        """Return complete lines received; handles stdin forwarding and port loss."""
        lines = []
        rlist = [sys.stdin] if self.stdin_open else []
        try:
            ready, _, _ = select.select(rlist, [], [], timeout)
        except (ValueError, OSError):
            ready = []
        if ready:
            typed = sys.stdin.readline()
            if not typed:
                self.stdin_open = False  # EOF: stop selecting on it, or this loop spins
            else:
                self.log.write(f"[{time.monotonic() - self.t0:9.3f}] >> {typed.rstrip()}\n")
                self.send(typed if typed.endswith("\n") else typed + "\n")
        try:
            chunk = self.ser.read(4096) if self.ser else b""
        except (serial.SerialException, OSError):
            self.note("port lost (board powered off or USB unplugged); waiting for it to return")
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            if self.connect(wait_s=3600):
                self.note("port back")
            return lines
        self.buf += chunk
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            self._emit(line)
            lines.append(line)
        return lines


def run_once(sess, test, args):
    """Capture one sketch run. Returns (verdict, detail) or (None, reason)."""
    judge = test["judge"]
    done_re = re.compile(judge["done"]) if judge else None
    if test["reset"] and not args.no_reset:
        sess.note("reset via DTR pulse (needs SW10 in USB service position)")
        pulse_reset(sess.ser)
    timeout = args.timeout or test["timeout"]
    deadline = time.monotonic() + timeout if timeout and judge else None
    last = None
    try:
        while True:
            for line in sess.poll():
                if "Send Enter to start" in line:
                    sess.send(test["start"])
                if done_re and done_re.search(line):
                    last = line
            if last and not args.stay:
                # let trailing lines (help text, checklists) land in the log
                end = time.monotonic() + 1.0
                while time.monotonic() < end:
                    sess.poll()
                break
            if deadline and not last and time.monotonic() > deadline:
                return "FAIL", f"timed out after {timeout}s without a completion line"
    except KeyboardInterrupt:
        if not last:
            raise
        sess.note("session ended by operator")  # --stay: keep the verdict already printed
    passed = re.search(judge["pass"], last) is not None
    return ("PASS" if passed else "FAIL"), last.strip()


def ask(prompt, choices):
    while True:
        a = input(prompt).strip().lower()
        if a in choices:
            return choices[a]


def cmd_run(args):
    test = BY_KEY.get(args.test)
    if not test:
        sys.exit(f"unknown test '{args.test}' (see: list)")
    if not BOARD_RE.match(args.board):
        print(f"warning: board id '{args.board}' does not look like HUB-R6-NN", file=sys.stderr)
    commit, dirty = git_state()
    port = args.port or default_port()

    if test["env"] and not args.no_flash:
        print(f"==> flashing {test['env']} to {port} (commit {commit}, dirty={dirty})")
        r = subprocess.run([pio_bin(), "run", "-d", str(FW_DIR), "-e", test["env"],
                            "-t", "upload", "--upload-port", port])
        if r.returncode != 0:
            sys.exit("build/upload failed; nothing recorded")

    d = EVIDENCE / args.board
    d.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = d / f"{stamp}_{test['key']}.log"
    with log_path.open("w") as log:
        log.write(f"# board {args.board}  test {test['key']}  env {test['env']}\n")
        log.write(f"# commit {commit} (firmware tree dirty: {dirty})  port {port}  "
                  f"started {dt.datetime.now().isoformat(timespec='seconds')}\n")
        if args.note:
            log.write(f"# note: {args.note}\n")
        sess = Session(port, log)
        if not sess.connect():
            sys.exit(f"could not open {port}")
        sess.note(f"{test['key']}: {test['proves']}")
        if not test["judge"]:
            sess.note("operator-judged test: Ctrl-C when finished, then enter the verdict")
        sess.note("type a line + Enter to send it to the board; Ctrl-C ends the session")

        results = []
        try:
            if test["judge"]:
                for i in range(args.repeat):
                    if args.repeat > 1:
                        sess.note(f"run {i + 1}/{args.repeat}")
                    results.append(run_once(sess, test, args))
                    if args.repeat > 1:
                        sess.note(f"run {i + 1}: {results[-1][0]}")
            else:
                if test["reset"] and not args.no_reset and test["env"]:
                    sess.note("reset via DTR pulse (needs SW10 in USB service position)")
                    pulse_reset(sess.ser)
                while True:
                    sess.poll()
        except KeyboardInterrupt:
            sess.note("session ended by operator")
        finally:
            if sess.ser:
                sess.ser.close()

    if test["judge"]:
        fails = [r for r in results if r[0] != "PASS"]
        if not results:
            verdict, detail = "ABORTED", "no run completed"
        elif args.repeat == 1:
            verdict, detail = results[0]
        else:
            verdict = "PASS" if not fails and len(results) == args.repeat else "FAIL"
            detail = f"{len(results) - len(fails)}/{args.repeat} runs passed" + \
                (f"; first fail: {fails[0][1]}" if fails else "")
    elif args.verdict:
        verdict, detail = args.verdict.upper(), "operator"
    elif test["key"] == "monitor":
        verdict, detail = "INFO", "console capture"
    elif not sys.stdin.isatty():
        verdict, detail = "UNJUDGED", "no operator verdict (pass --verdict)"
    else:
        verdict = ask("Verdict [p]ass / [f]ail / [s]kip: ", {"p": "PASS", "f": "FAIL", "s": "SKIP"})
        detail = "operator"
    note = args.note or (input("Note (measurements, observations; Enter for none): ").strip()
                         if sys.stdin.isatty() and not test["judge"] else "")
    append_result(args.board, {
        "timestamp": dt.datetime.now().isoformat(timespec="seconds"), "board": args.board,
        "test": test["key"], "env": test["env"] or "", "commit": commit, "dirty": dirty,
        "verdict": verdict, "detail": detail, "note": note,
        "log": rel(log_path),
    })
    print(f"\n==> {args.board} {test['key']}: {verdict}  ({detail})\n    log: {rel(log_path)}")
    return 0 if verdict in ("PASS", "INFO", "SKIP") else 1


def cmd_record(args):
    commit, dirty = git_state()
    append_result(args.board, {
        "timestamp": dt.datetime.now().isoformat(timespec="seconds"), "board": args.board,
        "test": args.item, "env": "", "commit": commit, "dirty": dirty,
        "verdict": args.verdict.upper(), "detail": "bench measurement", "note": args.note or "", "log": "",
    })
    print(f"recorded {args.board} {args.item}: {args.verdict.upper()}")


def cmd_status(args):
    f = EVIDENCE / args.board / "results.csv"
    if not f.exists():
        sys.exit(f"no results yet for {args.board}")
    latest = {}
    with f.open() as fh:
        for row in csv.DictReader(fh):
            latest[row["test"]] = row
    order = [x["key"] for x in LADDER]
    keys = [k for k in order if k in latest] + sorted(k for k in latest if k not in order)
    for k in keys:
        r = latest[k]
        print(f"{r['verdict']:5}  {k:18} {r['timestamp']}  {r['detail'][:60]}  {r['note'][:40]}")
    missing = [x["key"] for x in LADDER if x["key"] not in latest and x["key"] != "monitor"]
    if missing:
        print("\nnot yet run: " + ", ".join(missing))


def cmd_list(_args):
    print(f"{'stage':5} {'test':18} {'judge':6} {'SW10':8} proves")
    for x in LADDER:
        judge = "auto" if x["judge"] else "manual"
        sw10 = "service" if x["reset"] else "run/any"
        print(f"{x['stage']:5} {x['key']:18} {judge:6} {sw10:8} {x['proves']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)

    r = sub.add_parser("run", help="flash a bring-up sketch and capture its run")
    r.add_argument("board")
    r.add_argument("test")
    r.add_argument("--port")
    r.add_argument("--no-flash", action="store_true", help="capture only; sketch already on the board")
    r.add_argument("--no-reset", action="store_true", help="do not pulse EN after opening the port")
    r.add_argument("--repeat", type=int, default=1, help="reset + re-run N times (auto-judged tests)")
    r.add_argument("--stay", action="store_true", help="keep the console open after the summary")
    r.add_argument("--timeout", type=int, help="seconds to wait for the completion line")
    r.add_argument("--verdict", choices=["pass", "fail", "skip"], help="operator verdict without a prompt")
    r.add_argument("--note")
    r.set_defaults(fn=cmd_run)

    rec = sub.add_parser("record", help="record a bench measurement (rails, current, inspection)")
    rec.add_argument("board")
    rec.add_argument("item", help="e.g. A-inspection, B-rails, C-first-power, G-5v-load")
    rec.add_argument("verdict", choices=["pass", "fail", "info"])
    rec.add_argument("--note")
    rec.set_defaults(fn=cmd_record)

    s = sub.add_parser("status", help="latest verdict per test for one board")
    s.add_argument("board")
    s.set_defaults(fn=cmd_status)

    args = p.parse_args()
    if getattr(args, "repeat", 1) > 1 and not BY_KEY.get(getattr(args, "test", ""), {}).get("judge"):
        sys.exit("--repeat needs an auto-judged test")
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
