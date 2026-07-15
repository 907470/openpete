#!/usr/bin/env python3
"""Convert a .opp performance trace into Chrome Trace Event JSON, viewable in
Perfetto (https://ui.perfetto.dev) or chrome://tracing, and loadable by
speedscope. CPU scopes render as a nested flame chart; GPU zones on a second
track; census values as counters; canonical ticks as instant markers.

Usage:
  profile-to-trace.py TRACE.opp -o OUT.json[.gz] [--ticks A:B]
"""
import argparse
import gzip
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _opp_reader import read_opp, KIND_CANONICAL  # noqa: E402

PID_CPU, PID_GPU = 0, 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--ticks", default=None,
                    help="restrict to canon_tick window A:B (inclusive A, exclusive B)")
    args = ap.parse_args()

    lo, hi = None, None
    if args.ticks:
        a, _, b = args.ticks.partition(":")
        lo = int(a) if a else None
        hi = int(b) if b else None

    names, presents, _funcs = read_opp(args.trace)

    def in_window(p):
        return (lo is None or p.canon_tick >= lo) and (hi is None or p.canon_tick < hi)

    presents = [p for p in presents if in_window(p)]

    ev = []
    # process / track names
    ev.append({"ph": "M", "name": "process_name", "pid": PID_CPU, "tid": 0,
               "args": {"name": "CPU (main thread)"}})
    ev.append({"ph": "M", "name": "process_name", "pid": PID_GPU, "tid": 0,
               "args": {"name": "GPU"}})

    prev_tick = None
    for p in presents:
        # CPU spans (Perfetto reconstructs nesting from ts+dur on one tid)
        for (_depth, name, t0, dur, aux) in p.spans:
            e = {"name": name, "ph": "X", "ts": t0, "dur": max(1, dur),
                 "pid": PID_CPU, "tid": 0}
            if aux:
                e["args"] = {"aux": aux}
            ev.append(e)
        # GPU zones anchored at this present's submit time + relative offset
        for (_depth, name, t0rel, dur) in p.gpuz:
            ev.append({"name": name, "ph": "X", "ts": p.t_now_us + t0rel,
                       "dur": max(1, dur), "pid": PID_GPU, "tid": 0})
        # census counters
        if p.counts:
            ev.append({"name": "census", "ph": "C", "ts": p.t_now_us,
                       "pid": PID_CPU, "tid": 0, "args": dict(p.counts)})
        # canonical-tick instant marker (once per new tick)
        if p.kind == KIND_CANONICAL and p.canon_tick != prev_tick:
            ev.append({"name": "tick %d" % p.canon_tick, "ph": "i", "s": "g",
                       "ts": p.t_now_us, "pid": PID_CPU, "tid": 0})
            prev_tick = p.canon_tick

    doc = {"traceEvents": ev, "displayTimeUnit": "ms"}
    blob = json.dumps(doc).encode("utf-8")
    if args.out.endswith(".gz"):
        with gzip.open(args.out, "wb") as f:
            f.write(blob)
    else:
        with open(args.out, "wb") as f:
            f.write(blob)
    print("wrote %s (%d events, %d presents)" % (args.out, len(ev), len(presents)),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
