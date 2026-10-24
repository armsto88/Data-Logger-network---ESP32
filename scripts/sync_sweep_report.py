#!/usr/bin/env python3
"""Summarise a sync-rendezvous sweep log: node attendance against hub boot latency.

Reads a serial capture from the bench build (env mothership-v2-bench-sync-sweep),
for example the log written by `scripts/hub_bringup.py run <BOARD> sync-sweep`,
and prints one row per sync window plus a summary per injected delay.

  scripts/sync_sweep_report.py path/to/capture.log
  scripts/sync_sweep_report.py capture.log --csv windows.csv --lead 10

The prediction columns use the aperture equation in
mothership/docs/MOTHERSHIP_SYNC_RENDEZVOUS_DESIGN_NOTE.md section 3:

    O(t) = 25 - t   for t <= 25,   35 - t   for 25 < t < 35,   0   for t >= 35

where t is the hub's boot time, in seconds from its alarm to entering the join
loop. Here t = (lead - to_slot_s) + inject_s: the alarm fires `lead` seconds
before the slot (10 in the current firmware), to_slot_s is how far the slot was
still ahead when the bench hook ran, and inject_s is the delay the bench hook
added. Lines may carry the runner's "[  12.345] " time prefix or not.
"""

import argparse
import csv
import re
import sys
from collections import defaultdict

STAMP = re.compile(r"^\[\s*[\d.]+\]\s?")
BENCH = re.compile(r"\[BENCH\] window=(\d+) inject_ms=(\d+) boot_ms=(\d+) to_slot_s=(-?\d+)")
JOIN = re.compile(r"\[SYNC\] join window=(\d+) ms .*deployed=(\d+)")
ROSTER = re.compile(r"\[SYNC\] roster \+(\S+) queue=(\d+)")
CLOSED = re.compile(r"\[SYNC\] rendezvous closed: responders=(\d+) deployed=(\d+)")
DONE = re.compile(r"\[SYNC\] done node=(\S+) sent=(\d+) remaining=(\d+)")
RELEASE = re.compile(r"\[SYNC\] release node=(\S+) sent=(\d) confirmed=(\d) remaining=(\d+)")
COMPLETE = re.compile(r"\[SYNC\] coordinated window complete: responders=(\d+)")
TIMEOUT = re.compile(r"\[WATCHDOG\] Session timeout")


def new_window():
    return {"win": None, "inject_ms": None, "boot_ms": None, "to_slot_s": None,
            "join_ms": None, "deployed": None, "nodes": [], "attended": None,
            "drained": 0, "left": {}, "complete": False, "timeout": False}


def parse(path):
    windows, cur = [], None
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = STAMP.sub("", raw.rstrip("\n"))
        m = BENCH.search(line)
        if m:
            cur = new_window()
            windows.append(cur)
            cur["win"], cur["inject_ms"], cur["boot_ms"], cur["to_slot_s"] = (
                int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
            continue
        m = JOIN.search(line)
        if m:
            # A second join line with no bench line in between means the bench line
            # of this window was lost (the port reopened after it was printed).
            if cur is None or cur["join_ms"] is not None:
                cur = new_window()
                windows.append(cur)
            cur["join_ms"], cur["deployed"] = int(m.group(1)), int(m.group(2))
            continue
        if cur is None:
            continue
        if (m := ROSTER.search(line)):
            if m.group(1) not in cur["nodes"]:
                cur["nodes"].append(m.group(1))
        elif (m := CLOSED.search(line)):
            cur["attended"] = int(m.group(1))
            if cur["deployed"] is None:
                cur["deployed"] = int(m.group(2))
        elif (m := DONE.search(line)):
            cur["drained"] += int(m.group(2))
        elif (m := RELEASE.search(line)):
            cur["left"][m.group(1)] = int(m.group(4))
        elif COMPLETE.search(line):
            cur["complete"] = True
        elif TIMEOUT.search(line):
            cur["timeout"] = True
    return windows


def predicted_aperture(t):
    if t <= 25:
        return max(25 - t, 0)
    if t < 35:
        return 35 - t
    return 0


def derive(w, lead):
    inject_s = None if w["inject_ms"] is None else w["inject_ms"] / 1000.0
    if inject_s is not None and w["to_slot_s"] is not None:
        w["t_boot"] = (lead - w["to_slot_s"]) + inject_s
        w["pred"] = predicted_aperture(w["t_boot"])
    else:
        w["t_boot"] = w["pred"] = None
    w["inject_s"] = inject_s


def fmt(v, spec="{:.0f}", blank="-"):
    return blank if v is None else spec.format(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--lead", type=float, default=10.0,
                    help="seconds the hub wakes before the slot (default 10, as in rtc_alarm.cpp)")
    ap.add_argument("--csv", help="also write one row per window to this file")
    args = ap.parse_args()

    windows = parse(args.log)
    if not windows:
        sys.exit("no sync windows found: is this a capture from the bench build "
                 "(env mothership-v2-bench-sync-sweep)?")
    for w in windows:
        derive(w, args.lead)

    head = ("win", "inject_s", "t_boot_s", "pred_O_s", "join_ms", "deployed", "attended", "drained", "left", "status")
    rows = []
    for w in windows:
        status = ("ok" if w["complete"] else "INCOMPLETE")
        if w["timeout"]:
            status = "SESSION-TIMEOUT"
        elif w["attended"] is None and not w["complete"]:
            status = "NO-RENDEZVOUS"
        rows.append((fmt(w["win"], "{}"), fmt(w["inject_s"]), fmt(w["t_boot"], "{:.1f}"),
                     fmt(w["pred"], "{:.0f}"), fmt(w["join_ms"], "{}"), fmt(w["deployed"], "{}"),
                     fmt(w["attended"], "{}"), str(w["drained"]), str(sum(w["left"].values())), status))
    widths = [max(len(h), *(len(r[i]) for r in rows)) for i, h in enumerate(head)]
    line = lambda r: "  ".join(c.rjust(widths[i]) for i, c in enumerate(r))
    print(line(head))
    for r in rows:
        print(line(r))

    by_inject = defaultdict(list)
    for w in windows:
        if w["inject_s"] is not None and w["deployed"]:
            by_inject[w["inject_s"]].append(w)
    print("\nAttendance by injected delay (attended / deployed, summed over windows):")
    print(f"{'inject_s':>8}  {'windows':>7}  {'attendance':>10}  {'full':>6}  {'mean pred O_s':>13}  bar")
    for inj in sorted(by_inject):
        ws = by_inject[inj]
        got = sum(w["attended"] or 0 for w in ws)
        want = sum(w["deployed"] for w in ws)
        full = sum(1 for w in ws if (w["attended"] or 0) >= w["deployed"])
        preds = [w["pred"] for w in ws if w["pred"] is not None]
        frac = got / want if want else 0.0
        print(f"{inj:8.0f}  {len(ws):7d}  {got:4d}/{want:<4d} {frac * 100:3.0f}%  {full:3d}/{len(ws):<2d}"
              f"  {fmt(sum(preds) / len(preds) if preds else None, '{:.1f}'):>13}  {'#' * round(frac * 20)}")
    print("\nPrediction (design note section 3): full attendance while predicted O is large, falling to"
          "\n0-1 attendees once t_boot reaches about 25 s. Flat attendance across the sweep means the"
          "\nmodel is wrong and Stage 3 should not be designed until the real mechanism is found.")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            wr = csv.writer(fh)
            wr.writerow(head + ("nodes",))
            for r, w in zip(rows, windows):
                wr.writerow(r + (" ".join(w["nodes"]),))


if __name__ == "__main__":
    main()
