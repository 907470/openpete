#!/usr/bin/env python3
"""Convert a .opp performance trace into Chrome Trace Event JSON, viewable in
Perfetto (https://ui.perfetto.dev) or chrome://tracing, and loadable by
speedscope. CPU scopes render as a nested flame chart, one Perfetto track per producer
thread; GPU zones on a second process, one track per rendered frame's submit
time; census values as counters; canonical ticks as instant markers; each
present to the display as an instant on the presenting thread's track.

Usage:
  profile-to-trace.py TRACE.opp -o OUT.json[.gz] [--ticks A:B]
"""
import argparse
import gzip
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _opp_reader import read_opp, OppTraceError, KIND_CANONICAL  # noqa: E402

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

    try:
        tr = read_opp(args.trace)
    except OppTraceError as e:
        sys.stderr.write("profile-to-trace: %s\n" % e)
        return 2
    presents = tr.markers

    def in_window(p):
        return (lo is None or p.canon_tick >= lo) and (hi is None or p.canon_tick < hi)

    presents = [p for p in presents if in_window(p)]

    ev = []
    # process / track names. Each producer thread gets its own CPU track: spans
    # from two threads overlap in time and Perfetto would read them as nesting.
    ev.append({"ph": "M", "name": "process_name", "pid": PID_CPU, "tid": 0,
               "args": {"name": "CPU"}})
    ev.append({"ph": "M", "name": "process_name", "pid": PID_GPU, "tid": 0,
               "args": {"name": "GPU"}})
    for t in tr.threads:
        ev.append({"ph": "M", "name": "thread_name", "pid": PID_CPU, "tid": t,
                   "args": {"name": "producer %d%s"
                            % (t, "".join(lbl for lbl, tids in
                                         ((" (scenes)", tr.scene_tids),
                                          (" (presents)", tr.present_tids))
                                         if t in tids))}})
    # One instant per present to the display, on the presenting thread's track.
    for p in tr.presents:
        if in_window(p):
            ev.append({"name": "present", "ph": "i", "s": "t", "ts": p.t_now_us,
                       "pid": PID_CPU, "tid": p.tid})

    prev_tick = None
    # GPU zones are keyed by RHI frame serial. Anchor each frame's set at the
    # marker time of the first present at or after it in the file, so the GPU
    # track lines up with the CPU tracks without claiming a present binding.
    frame_anchor = {}
    for i, serial in enumerate(sorted(tr.gpu_frames)):
        frame_anchor[serial] = presents[min(i, len(presents) - 1)].t_now_us \
            if presents else 0
    for serial, zones in tr.gpu_frames.items():
        base = frame_anchor.get(serial, 0)
        for (_depth, name, t0rel, dur, _ztid) in zones:
            ev.append({"name": name, "ph": "X", "ts": base + t0rel,
                       "dur": max(1, dur), "pid": PID_GPU, "tid": 0,
                       "args": {"frame_serial": serial}})

    # Present-thread spans bin to that thread's PRESENT records (_opp_reader.py).
    for p in tr.present_bins:
        if not in_window(p):
            continue
        for (_depth, name, t0, dur, aux, tid) in p.spans:
            e = {"name": name, "ph": "X", "ts": t0, "dur": max(1, dur),
                 "pid": PID_CPU, "tid": tid}
            if aux:
                e["args"] = {"aux": aux}
            ev.append(e)

    for p in presents:
        # CPU spans (Perfetto reconstructs nesting from ts+dur on one tid)
        for (_depth, name, t0, dur, aux, tid) in p.spans:
            e = {"name": name, "ph": "X", "ts": t0, "dur": max(1, dur),
                 "pid": PID_CPU, "tid": tid}
            if aux:
                e["args"] = {"aux": aux}
            ev.append(e)
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
    print("wrote %s (%d events, %d markers)" % (args.out, len(ev), len(presents)),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
