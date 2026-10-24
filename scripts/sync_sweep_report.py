#!/usr/bin/env python3
"""Summarise a sync-rendezvous sweep log: node attendance against hub boot latency.

Reads a serial capture from the bench build (env mothership-v2-bench-sync-sweep),
for example the log written by `scripts/hub_bringup.py run <BOARD> sync-sweep`,
and prints one row per sync window plus a summary per injected delay.

  scripts/sync_sweep_report.py path/to/capture.log
  scripts/sync_sweep_report.py capture.log --csv windows.csv

The prediction column is the overlap O(t), in seconds, between the hub
accepting HELLOs and a node still listening, computed the way the firmware does
it (design note section 3 gives the original form). With the slot at 0:

    hub starts     s = t - lead
    join length    j = clamp((lead - t) + join_post, 15, join_cap)
    hub accepts    [s, s + j + late_grace]
    node listens   [-56, +25]       (wakes 60 s early, SYNC_MARKER_GRACE_SEC = 25)
    O(t)           = max(0, min(s + j + late_grace, 25) - max(s, -56))

t is the hub's boot time, from its alarm to entering the join loop:
t = (lead - to_slot_s) + inject_s, where to_slot_s is how far the slot was still
ahead when the bench hook ran and inject_s is the delay the hook added. The
cliff is where the hub starts after the node stops listening: t = lead + 25.

Two firmware models, picked automatically: a log with [SYNC-STAT] lines comes
from the Stage 0 build (lead read from the log, join to slot + 25, cap 75 s,
20 s late-join grace); one without is the original build (lead 10, join to
slot + 15, cap 45 s, no grace). Lines may carry the runner's "[  12.345] " time
prefix or not.
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
LATE = re.compile(r"\[SYNC\] late join (\S+)")
STAT = re.compile(r"\[SYNC-STAT\] .*lead_s=(\d+)")
TIMEOUT = re.compile(r"\[WATCHDOG\] Session timeout")

# (lead_s, join_post_s, join_cap_s, late_grace_s) for each firmware.
MODELS = {"original": (10, 15, 45, 0), "stage0": (45, 25, 75, 20)}
NODE_START_S, NODE_END_S, JOIN_FLOOR_S = -56, 25, 15


def new_window():
    return {"win": None, "inject_ms": None, "boot_ms": None, "to_slot_s": None,
            "join_ms": None, "deployed": None, "nodes": [], "attended": None,
            "late": 0, "drained": 0, "left": {}, "complete": False, "timeout": False}


def parse(path):
    """Return (windows, lead_s from [SYNC-STAT] or None)."""
    windows, cur, stat_lead = [], None, None
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = STAMP.sub("", raw.rstrip("\n"))
        if (m := STAT.search(line)):
            stat_lead = int(m.group(1))
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
        elif LATE.search(line):
            cur["late"] += 1
        elif (m := COMPLETE.search(line)):
            cur["complete"] = True
            # The final roster, including any late joiners admitted after the
            # rendezvous closed (Stage 0c).
            cur["attended"] = int(m.group(1))
        elif TIMEOUT.search(line):
            cur["timeout"] = True
    return windows, stat_lead


def predicted_aperture(t, lead, join_post, join_cap, late_grace):
    start = t - lead
    join = min(max((lead - t) + join_post, JOIN_FLOOR_S), join_cap)
    accept_end = start + join + late_grace
    return max(0.0, min(accept_end, NODE_END_S) - max(start, NODE_START_S))


def derive(w, model):
    lead = model[0]
    inject_s = None if w["inject_ms"] is None else w["inject_ms"] / 1000.0
    if inject_s is not None and w["to_slot_s"] is not None:
        w["t_boot"] = (lead - w["to_slot_s"]) + inject_s
        w["pred"] = predicted_aperture(w["t_boot"], *model)
    else:
        w["t_boot"] = w["pred"] = None
    w["inject_s"] = inject_s


def fmt(v, spec="{:.0f}", blank="-"):
    return blank if v is None else spec.format(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--model", choices=("auto",) + tuple(MODELS), default="auto",
                    help="firmware model for the prediction (default: detect from [SYNC-STAT] lines)")
    ap.add_argument("--lead", type=float,
                    help="seconds the hub wakes before the slot (default: from the model, "
                         "or from the log's [SYNC-STAT] lead_s)")
    ap.add_argument("--csv", help="also write one row per window to this file")
    args = ap.parse_args()

    windows, stat_lead = parse(args.log)
    if not windows:
        sys.exit("no sync windows found: is this a capture from the bench build "
                 "(env mothership-v2-bench-sync-sweep)?")
    name = args.model if args.model != "auto" else ("stage0" if stat_lead is not None else "original")
    model = list(MODELS[name])
    if args.lead is not None:
        model[0] = args.lead
    elif name == "stage0" and stat_lead is not None:
        model[0] = stat_lead
    model = tuple(model)
    for w in windows:
        derive(w, model)
    print(f"model: {name} (lead {model[0]:g} s, join to slot+{model[1]} s, cap {model[2]} s, "
          f"late-join grace {model[3]} s); predicted cliff at t_boot = {model[0] + NODE_END_S:g} s\n")

    head = ("win", "inject_s", "t_boot_s", "pred_O_s", "join_ms", "deployed", "attended", "late",
            "drained", "left", "status")
    rows = []
    for w in windows:
        status = ("ok" if w["complete"] else "INCOMPLETE")
        if w["timeout"]:
            status = "SESSION-TIMEOUT"
        elif w["attended"] is None and not w["complete"]:
            status = "NO-RENDEZVOUS"
        rows.append((fmt(w["win"], "{}"), fmt(w["inject_s"]), fmt(w["t_boot"], "{:.1f}"),
                     fmt(w["pred"], "{:.0f}"), fmt(w["join_ms"], "{}"), fmt(w["deployed"], "{}"),
                     fmt(w["attended"], "{}"), str(w["late"]), str(w["drained"]),
                     str(sum(w["left"].values())), status))
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
    print(f"\nPrediction: full attendance while predicted O is a few seconds or more, and none once"
          f"\nt_boot passes {model[0] + NODE_END_S:g} s, where the hub starts after the nodes stop listening."
          "\nFlat attendance past that point, or a cliff somewhere else, means the model is wrong.")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            wr = csv.writer(fh)
            wr.writerow(head + ("nodes",))
            for r, w in zip(rows, windows):
                wr.writerow(r + (" ".join(w["nodes"]),))


if __name__ == "__main__":
    main()
