#!/usr/bin/env python3
"""Print the results of the boot cases comp_bench and comp_bench_hidpi as
Markdown tables (docs/design/graphics-performance.md).

usage: tools/benchtable.py SERIAL_LOG...

Each log is the serial.txt of one case. A table has one row per scenario:
drag, pointer and resize from the compstat lines that follow the lines
"comp_bench: NAME done", and blink, anim and idle from the lines of
compbench. Times are milliseconds with one decimal, pixels are millions
of device pixels."""
import re
import sys

ORDER = ["drag", "pointer", "resize", "blink", "anim", "idle"]
COLUMNS = [
    ("frames", "frames", lambda v: str(v.get("frames", 0))),
    ("Mpx", "composed Mpx", lambda v: "%.2f" % (v.get("pixels", 0) / 1e6)),
    ("compose", "compose ms", lambda v: "%.1f" % (v.get("compose_us", 0) / 1000)),
    ("flush", "flush ms", lambda v: "%.1f" % (v.get("flush_us", 0) / 1000)),
    ("p50", "frame p50 ms", lambda v: "%.1f" % (v.get("frame_p50_us", 0) / 1000)),
    ("p95", "frame p95 ms", lambda v: "%.1f" % (v.get("frame_p95_us", 0) / 1000)),
    ("commit", "commit latency ms", lambda v: "%.1f" % (v.get("commit_latency_us", 0) / 1000)),
    ("input", "input latency ms", lambda v: "%.1f" % (v.get("input_latency_us", 0) / 1000)),
    ("wakeups", "wakeups", lambda v: str(v.get("wakeups", 0))),
    ("cpu", "X12 CPU ms", lambda v: "%.0f" % (v.get("cpu_us", 0) / 1000)),
    ("cursor", "cursor moves ms", lambda v: "%.1f" % (v.get("cursor_us", 0) / 1000)),
]
PAIRS = re.compile(r"(\w+)=(\d+)")


def values(text):
    return {k: int(v) for k, v in PAIRS.findall(text)}


def parse(path):
    rows, screen, pending = {}, "", None
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"comp_bench: screen (\S+) scale (\d+)", line)
            if m:
                screen = "%s at scale %s" % m.groups()
            m = re.match(r"comp_bench: (\w+) done", line)
            if m:
                pending = m.group(1)
                continue
            if pending and line.startswith("compstat: elapsed_us="):
                rows[pending] = values(line)
                pending = None
                continue
            m = re.match(r"compbench: (\w+) x12 (.*)", line)
            if m:
                rows[m.group(1)] = values(m.group(2))
            m = re.match(r"compbench: (\w+) client (.*)", line)
            if m and m.group(1) in rows:
                client = values(m.group(2))
                rows[m.group(1)]["client_copy_us"] = client.get("copy_us", 0)
    return screen, rows


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    for path in sys.argv[1:]:
        screen, rows = parse(path)
        print("Screen %s:\n" % screen)
        print("| scenario | " + " | ".join(c[1] for c in COLUMNS) + " | client copy ms |")
        print("|---" * (len(COLUMNS) + 2) + "|")
        for name in ORDER:
            if name not in rows:
                continue
            v = rows[name]
            cells = [c[2](v) for c in COLUMNS]
            copy = "%.1f" % (v["client_copy_us"] / 1000) if "client_copy_us" in v else ""
            print("| %s | %s | %s |" % (name, " | ".join(cells), copy))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
