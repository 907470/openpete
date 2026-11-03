#!/usr/bin/env python3
"""looptrace.py - decode, digest and compare OPENPETE_LOOP_TRACE files.

The trace is one 64-byte record per host-loop iteration plus a header record
per process and the teardown close steps (layout:
core/include/openpete_host_trace.h).

  looptrace.py summary T...         counts per present kind, runahead outcome,
                                    shed verdict and flag; the close steps
  looptrace.py counts T             one line of KEY=N engagement counts (the
                                    present kinds, runahead outcomes, shed
                                    verdicts as v-<verdict>, flags,
                                    records, iterations, headers, close_steps,
                                    rollbacks), for a harness to grep
  looptrace.py digest T             sha256 of the compared fields of every
                                    record, then the record count
  looptrace.py compare A B [--wall] first differing record on the compared
                                    fields
  looptrace.py dump T [FROM [N]]    one line per record
  looptrace.py eager T              eager edges, inline presents before the
                                    first engage, predictor runs under eager

Compared fields: every decoded field of every record, except on an iteration
record without HT_F_VCLOCK the wall-clock ones (pace flags SLIPPED and
NOT_LIVE, the two deadlines, HT_F_EAGER_LIVE) unless --wall. Two traces have
equal digests exactly when compare reports them identical. `rollbacks` counts
the iteration records whose openpete_frame is below the previous record's (a
state-stress rewind, a rewind hold or a runahead rollback seen by the loop).

Exit 0 identical / done, 1 differ, 2 usage or a malformed trace.
"""
import hashlib
import struct
import sys
from collections import Counter

REC = 64
PRESENT = ["none", "canon-inline", "canon-eager", "canon-lag", "intermediate", "late-admit",
           "probe", "shed", "eager-produce", "paused-repaint"]
RA = ["none", "speculated", "adopted", "left"]
VERDICT = ["none", "strict", "late", "probe", "shed"]
EXIT = ["", "frame-limit", "bk2-end", "psxtrace-eof", "psxtrace-divergence", "psxtrace-error",
        "user-quit", "game-returned", "record-frames", "max-runtime-seconds", "other"]
FLAGS = ["sim_due", "unlock_entry", "ovr_claimed", "rewound", "yield", "ticked", "eager",
         "interp_cap", "inter_rex", "predictor", "did_present", "paused", "step_one",
         "unlock_tail", "vclock", "spine", "eager_live", "native_inter"]
STEPS = [None, "present-thread-stop", "mods-shutdown", "settings-flush", "rebind-summary",
         "bk2-feed-report", "bk2-free", "bk2-finalize", "record-flush", "record-shutdown",
         "capture-report", "position-log-close", "hash-log-close", "psxtrace-summary",
         "psxtrace-close", "exit-log", "pace-census", "swapchain-report", "pace-log-close",
         "slip-totals", "ftl-close", "functrace", "interp-census", "ra-predict-report",
         "vstream-census", "sfxpack-census", "interp-shutdown", "fiber-delete",
         "present-thread-report", "prof-close", "native-present-census", "eager-report",
         "capture-inter-report", "disc-census", "input-parity", "input-log-report",
         "rumble-report", "rewind-report", "savestate-report", "vbl-delivery", "osd-census",
         "version-census", "gpu-audit", "sfx-shutdown", "feature-shutdown",
         "render-shutdown", "exit-stdout", "input-stdout", "pose-stdout", "bk2-discard"]
F_EAGER, F_PREDICTOR, F_VCLOCK, F_EAGER_LIVE = 1 << 6, 1 << 9, 1 << 14, 1 << 16
PACE_WALL = (1 << 0) | (1 << 6)


class TraceError(Exception):
    """A trace file that is not a whole number of known records."""


def read(path):
    """Decode every record of a trace file into a list of dicts."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % REC:
        raise TraceError(f"{path}: {len(data)} bytes, not a multiple of {REC}")
    out = []
    for i in range(0, len(data), REC):
        r = data[i:i + REC]
        t = r[0]
        if t == 2:
            (_typ, pres, ra, ex, flags, it, frame, key, canon, pf, verdict, _r16, _r32,
             nsim, nrend) = struct.unpack_from("<BBBBIIIIIBBHIqq", r)
            out.append(dict(type="iter", present=pres, ra=ra, exit=ex, flags=flags, iter=it,
                            frame=frame, key=key, canon=canon, pace=pf, verdict=verdict,
                            next_sim=nsim, next_render=nrend))
        elif t == 3:
            out.append(dict(type="close", steps=list(r[8:8 + r[1]])))
        elif t == 1:
            out.append(dict(type="header", magic=r[8:12],
                            version=struct.unpack_from("<I", r, 12)[0],
                            hflags=struct.unpack_from("<I", r, 16)[0]))
        else:
            raise TraceError(f"{path}: record {i // REC} has type {t}")
    return out


def key(rec, wall):
    """The compared fields of one record, wall-clock fields masked unless wall."""
    if rec["type"] != "iter":
        return tuple(sorted(rec.items()))
    r = dict(rec)
    if not wall and not (r["flags"] & F_VCLOCK):
        r["pace"] &= ~PACE_WALL
        r["flags"] &= ~F_EAGER_LIVE
        r["next_sim"] = r["next_render"] = 0
    return tuple(sorted(r.items()))


def digest(recs, wall=False):
    """sha256 over the compared fields of every record, in order."""
    h = hashlib.sha256()
    for r in recs:
        h.update(repr(key(r, wall)).encode())
        h.update(b"\n")
    return h.hexdigest()


def fmt(i, r):
    """One record as a line of text."""
    if r["type"] == "header":
        return f"#{i} header {r['magic']!r} v{r['version']} hflags={r['hflags']:#x}"
    if r["type"] == "close":
        return f"#{i} close " + " ".join(STEPS[s] if s < len(STEPS) else str(s)
                                          for s in r["steps"])
    fl = ",".join(n for b, n in enumerate(FLAGS) if r["flags"] >> b & 1)
    return (f"#{i} it={r['iter']} frame={r['frame']} key={r['key']} canon={r['canon']} "
            f"present={PRESENT[r['present']]} verdict={VERDICT[r['verdict']]} ra={RA[r['ra']]} "
            f"pace={r['pace']:#04x} exit={EXIT[r['exit']]} sim={r['next_sim']} "
            f"render={r['next_render']} [{fl}]")


def counts(recs):
    """Engagement counts of a decoded trace, every present kind and outcome named."""
    it = [r for r in recs if r["type"] == "iter"]
    c = Counter()
    for name in PRESENT + RA[1:] + ["v-" + v for v in VERDICT[1:]] + FLAGS:
        c[name] = 0
    for r in it:
        c[PRESENT[r["present"]]] += 1
        if r["verdict"]:
            c["v-" + VERDICT[r["verdict"]]] += 1
        if r["ra"]:
            c[RA[r["ra"]]] += 1
        for b, n in enumerate(FLAGS):
            if r["flags"] >> b & 1:
                c[n] += 1
    c["rollbacks"] = sum(1 for a, b in zip(it, it[1:]) if b["frame"] < a["frame"])
    c["records"] = len(recs)
    c["iterations"] = len(it)
    c["headers"] = sum(1 for r in recs if r["type"] == "header")
    c["close_steps"] = sum(len(r["steps"]) for r in recs if r["type"] == "close")
    return c


def summary(path):
    """Print the counts and the close steps of one trace."""
    recs = read(path)
    it = [r for r in recs if r["type"] == "iter"]
    pc = Counter(PRESENT[r["present"]] for r in it)
    rc = Counter(RA[r["ra"]] for r in it)
    vc = Counter(VERDICT[r["verdict"]] for r in it)
    c = counts(recs)
    steps = [s for r in recs if r["type"] == "close" for s in r["steps"]]
    ex = [EXIT[r["exit"]] for r in it if r["exit"]]
    pred_eager = sum(1 for r in it if r["flags"] & F_PREDICTOR and r["flags"] & F_EAGER)
    print(f"{path}: records={len(recs)} iterations={len(it)} headers={c['headers']} "
          f"close_steps={len(steps)} rollbacks={c['rollbacks']} exit={ex}")
    print("  present: " + " ".join(f"{k}={v}" for k, v in sorted(pc.items())))
    print("  runahead: " + " ".join(f"{k}={v}" for k, v in sorted(rc.items())))
    print("  verdict: " + " ".join(f"{k}={v}" for k, v in sorted(vc.items())))
    print("  flags: " + " ".join(f"{k}={c[k]}" for k in FLAGS))
    print(f"  predictor_under_eager={pred_eager}")
    print("  close: " + " ".join(STEPS[s] if s < len(STEPS) else str(s) for s in steps))


def eager(path):
    """Print the eager engage and disengage edges and the predictor runs."""
    it = [r for r in read(path) if r["type"] == "iter"]
    eng = dis = 0
    first = None
    prev = 0
    for i, r in enumerate(it):
        cur = 1 if r["flags"] & F_EAGER else 0
        if cur and not prev:
            eng += 1
            first = i if first is None else first
        if prev and not cur:
            dis += 1
        prev = cur
    inline_before = sum(1 for r in it[:first if first is not None else len(it)]
                        if r["present"] in (4, 5, 6))
    pred_eager = sum(1 for r in it if r["flags"] & F_PREDICTOR and r["flags"] & F_EAGER)
    pred_inline = sum(1 for r in it if r["flags"] & F_PREDICTOR and not r["flags"] & F_EAGER)
    pc = Counter(PRESENT[r["present"]] for r in it)
    print(f"eager engages={eng} disengages={dis} first_engage_iter={first} "
          f"inline_before_first={inline_before} predictor_under_eager={pred_eager} "
          f"predictor_inline={pred_inline} iterations={len(it)} "
          + " ".join(f"{k}={v}" for k, v in sorted(pc.items())))


def compare(a_path, b_path, wall):
    """Print the first differing record of two traces; 0 identical, 1 differ."""
    A, B = read(a_path), read(b_path)
    for i in range(min(len(A), len(B))):
        if key(A[i], wall) != key(B[i], wall):
            print(f"DIFF at record {i}:\n  A {fmt(i, A[i])}\n  B {fmt(i, B[i])}")
            return 1
    if len(A) != len(B):
        print(f"DIFF length: A {len(A)} records, B {len(B)} (common prefix identical)")
        return 1
    print(f"IDENTICAL {len(A)} records (wall-clock fields "
          f"{'compared' if wall else 'masked off the virtual clock'})")
    return 0


def main(a):
    """Dispatch one subcommand."""
    cmd = a[0] if a else ""
    if cmd == "summary" and len(a) >= 2:
        for p in a[1:]:
            summary(p)
        return 0
    if cmd == "counts" and len(a) == 2:
        c = counts(read(a[1]))
        print(" ".join(f"{k}={v}" for k, v in c.items()))
        return 0
    if cmd == "digest" and len(a) >= 2:
        recs = read(a[1])
        print(digest(recs, "--wall" in a), len(recs))
        return 0
    if cmd == "eager" and len(a) == 2:
        eager(a[1])
        return 0
    if cmd == "dump" and len(a) >= 2:
        recs = read(a[1])
        lo = int(a[2]) if len(a) > 2 else 0
        n = int(a[3]) if len(a) > 3 else len(recs)
        for i in range(lo, min(len(recs), lo + n)):
            print(fmt(i, recs[i]))
        return 0
    if cmd == "compare" and len(a) >= 3:
        return compare(a[1], a[2], "--wall" in a)
    print(__doc__)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (TraceError, OSError) as e:
        print(f"looptrace: {e}", file=sys.stderr)
        sys.exit(2)
