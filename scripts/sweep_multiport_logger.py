#!/usr/bin/env python3
"""Log every /dev/ttyUSB* that appears, and route each power-up session to a
hub log or a node log by what the board prints. Ports come and go because the
hub and the nodes all cut their own power between wakes.

Use this instead of `hub_bringup.py run` when nodes are on USB as well as the
hub. CH340 adapters carry no serial number, so /dev/serial/by-id/ has one link
for all of them, and every board that wakes takes whichever ttyUSB number is
free: a runner pinned to one path ends up logging a node. The hub log is what
scripts/sync_sweep_report.py reads; the node log shows why a node missed.

Usage: scripts/sweep_multiport_logger.py <hub_log> <node_log>
"""
import glob
import sys
import time
import serial

HUB_LOG, NODE_LOG = sys.argv[1], sys.argv[2]
NODE_MARKS = ("role=node", "FieldMesh Node:", "[A1] Next alarm", "[BAT-V2]", "[FINALIZE]")
# Only markers the node firmware never prints: both firmwares emit [CONFIG] and
# [DEPLOY], so those would misfile a node session whose banner was missed.
HUB_MARKS = ("role=mothership", "[BENCH]", "[SYNC] Loaded", "[SYNC-STAT]", "[PWR]",
             "[SYNC] join window", "[RTC] DS3231", "[STORAGE]", "[WAIT] Board still alive")
T0 = time.time()


class Port:
    def __init__(self, path):
        self.path = path
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = path, 115200, 0
        s.dtr = True  # both asserted before open: no reset, no power drop on the hub
        s.rts = True
        s.open()
        self.ser = s
        self.buf = b""
        self.kind = None
        self.pending = []


def stamp():
    return f"[{time.time() - T0:9.3f}] "


def main():
    hub = open(HUB_LOG, "a", buffering=1)
    node = open(NODE_LOG, "a", buffering=1)
    other = open(NODE_LOG + ".unclassified", "a", buffering=1)
    ports = {}

    def emit(p, text):
        line = stamp() + text
        if p.kind == "hub":
            hub.write(line + "\n")
        elif p.kind == "node":
            node.write(f"{line} <{p.path}>\n")
        else:
            p.pending.append(line)

    def close(p, why):
        try:
            p.ser.close()
        except Exception:
            pass
        if p.buf:
            # Power dropped mid-line: keep the fragment, marked as partial.
            emit(p, p.buf.decode("utf-8", "replace").rstrip("\r") + "  [partial line, port lost]")
            p.buf = b""
        if p.kind is None and p.pending:
            for ln in p.pending:
                other.write(f"{ln} <{p.path}>\n")
        tag = p.kind or "unclassified"
        if p.kind == "hub":
            hub.write(stamp() + f"## port lost ({p.path}, {why})\n")
        ports.pop(p.path, None)

    while True:
        for path in sorted(glob.glob("/dev/ttyUSB*")):
            if path not in ports:
                try:
                    ports[path] = Port(path)
                except Exception:
                    pass
        for p in list(ports.values()):
            try:
                chunk = p.ser.read(4096)
            except Exception as e:
                close(p, type(e).__name__)
                continue
            if not chunk:
                continue
            p.buf += chunk
            while b"\n" in p.buf:
                raw, p.buf = p.buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").rstrip("\r")
                if p.kind is None:
                    if any(m in text for m in NODE_MARKS):
                        p.kind = "node"
                    elif any(m in text for m in HUB_MARKS):
                        p.kind = "hub"
                        hub.write(stamp() + f"## port back ({p.path})\n")
                    if p.kind:
                        held, p.pending = p.pending, []
                        for ln in held:
                            (hub if p.kind == "hub" else node).write(
                                ln + ("\n" if p.kind == "hub" else f" <{p.path}>\n"))
                emit(p, text)
        for path in list(ports):
            if not glob.glob(path):
                close(ports[path], "gone")
        time.sleep(0.02)


main()
