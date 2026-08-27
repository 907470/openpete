#!/usr/bin/env python3
"""pacelog.py — reader for the unified pacing event log (--pace-log PATH).

The shared parsing layer for scripts/pacing/. Everything downstream reads a
log through `read()` so there is exactly one place that knows the binary
layout; see core/include/openpete_pace_log.h for the authoritative definition.

Subcommands:
    dump    LOG [--type VBL] [--limit N]   human-readable records
    verify  LOG [--expect-inject-us N]     structural checks; with the
                                          expectation, assert the log actually
                                          measured a known injected stall

`verify` is what makes this log trustworthy rather than merely present. A log
that parses proves the writer ran; only a known input with a known answer
proves it measured the right quantity.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x4C50504F  # 'OPPL'
VERSION = 1
HEADER_FIXED = 64
RECORD_BYTES = 48

VBL, PRESENT, SHED, SLIP, INPUT_EDGE, WINDOW, CLOCKSAMPLE = range(1, 8)
TYPE_NAMES = {VBL: "VBL", PRESENT: "PRESENT", SHED: "SHED", SLIP: "SLIP",
              INPUT_EDGE: "INPUT_EDGE", WINDOW: "WINDOW",
              CLOCKSAMPLE: "CLOCKSAMPLE"}
NAME_TYPES = {v: k for k, v in TYPE_NAMES.items()}

F_SLIPPED, F_UNLOCK, F_PAUSED, F_STEPPED = 1, 2, 4, 8
F_REWIND, F_SPECULATIVE, F_NOT_LIVE, F_QUIESCE = 16, 32, 64, 128
FLAG_NAMES = [(F_SLIPPED, "slipped"), (F_UNLOCK, "unlock"), (F_PAUSED, "paused"),
              (F_STEPPED, "stepped"), (F_REWIND, "rewind"),
              (F_SPECULATIVE, "spec"), (F_NOT_LIVE, "not-live"),
              (F_QUIESCE, "quiesce")]

# Bucket edges must match openpete_pace_hist_edges_ns so the census and the
# offline analysis bucket a phase error identically.
HIST_EDGES_NS = [0, 100_000, 250_000, 500_000, 1_000_000, 2_000_000,
                 5_000_000, 16_715_111, None]


class Record:
    __slots__ = ("type", "flags", "aux", "index", "t_ns", "a", "b", "c", "d")

    def __init__(self, tup):
        (self.type, self.flags, self.aux, self.index,
         self.t_ns, self.a, self.b, self.c, self.d) = tup

    @property
    def name(self):
        return TYPE_NAMES.get(self.type, f"?{self.type}")

    def flag_str(self):
        return ",".join(n for bit, n in FLAG_NAMES if self.flags & bit) or "-"

    # VBL accessors, named so callers never index the raw payload slots.
    @property
    def deadline_ns(self): return self.a
    @property
    def latch_ns(self): return self.b
    @property
    def exec_start_ns(self): return self.c
    @property
    def exec_end_ns(self): return self.d

    # PRESENT accessors. `t_ns` is when the present path RETURNED; the block
    # fields are the waits it imposed before that.
    @property
    def fence_block_ns(self): return self.a
    @property
    def acquire_block_ns(self): return self.b
    @property
    def queue_block_ns(self): return self.c
    @property
    def recreate_block_ns(self): return self.d

    @property
    def paced(self):
        return self.type == VBL and not (self.flags & F_UNLOCK) and self.a != 0

    @property
    def phase_err_ns(self):
        """Start-phase error (S1): how late the tick began against its grid
        point. Signed — an early start is a different defect from a late one."""
        return self.exec_start_ns - self.deadline_ns


class Log:
    def __init__(self, env, epoch_ns, created_unix_ns, vbl_num, vbl_den,
                 tick_vbls, records):
        self.env = env
        self.epoch_ns = epoch_ns
        self.created_unix_ns = created_unix_ns
        self.vbl_num, self.vbl_den, self.tick_vbls = vbl_num, vbl_den, tick_vbls
        self.records = records

    @property
    def vbl_hz(self): return self.vbl_num / self.vbl_den

    @property
    def tick_hz(self): return self.vbl_hz / self.tick_vbls

    def of(self, *types):
        return [r for r in self.records if r.type in types]

    def timeline(self):
        """Records in timestamp order.

        File order is EMISSION order, which is not timestamp order: a VBL
        record is stamped with the tick's exec_start but written at the end of
        the iteration, after that iteration's PRESENT. Anything correlating
        across types (which present blocked which tick) must sort first."""
        return sorted(self.records, key=lambda r: r.t_ns)

    def env_key(self):
        """The fields a baseline is keyed on. Two logs whose keys differ were
        taken on different machines and must never be subtracted."""
        keep = ("build", "platform", "cpu", "cpu_count", "governor", "gpu",
                "driver", "refresh_mhz", "present_mode", "render_fps",
                "pacer_slack_ns", "virtual_clock")
        return tuple((k, self.env.get(k, "?")) for k in keep)


def read(path: Path) -> Log:
    data = Path(path).read_bytes()
    if len(data) < HEADER_FIXED:
        raise SystemExit(f"{path}: too small for a header ({len(data)} B)")
    (magic, version, header_bytes, rec_bytes, env_bytes, _res,
     created_unix_ns, epoch_ns, vbl_num, vbl_den, tick_vbls) = \
        struct.unpack_from("<IIIIIIqqqqq", data, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: bad magic 0x{magic:08x} (expected OPPL)")
    if version != VERSION:
        raise SystemExit(f"{path}: unsupported version {version}")
    if rec_bytes != RECORD_BYTES:
        raise SystemExit(f"{path}: unsupported record size {rec_bytes}")

    env = {}
    for line in data[HEADER_FIXED:HEADER_FIXED + env_bytes].decode(
            "utf-8", "replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v

    body = data[header_bytes:]
    # A run killed mid-write leaves a partial trailing record; truncate rather
    # than refuse, so a SIGKILLed soak run is still readable.
    body = body[: (len(body) // RECORD_BYTES) * RECORD_BYTES]
    recs = [Record(t) for t in struct.iter_unpack("<BBHIqqqqq", body)]
    return Log(env, epoch_ns, created_unix_ns, vbl_num, vbl_den, tick_vbls, recs)


def fmt_record(r, epoch_ns):
    t = (r.t_ns - epoch_ns) / 1e9
    if r.type == VBL:
        err = f"{r.phase_err_ns / 1e3:+9.1f}us" if r.paced else "  unpaced"
        exec_ms = (r.exec_end_ns - r.exec_start_ns) / 1e6
        return (f"{t:10.4f} VBL     #{r.index:<7} phase {err} "
                f"exec {exec_ms:7.3f}ms [{r.flag_str()}]")
    if r.type == PRESENT:
        kind = "intermediate" if r.flags else "canonical   "
        return (f"{t:10.4f} PRESENT #{r.index:<7} {kind} block: "
                f"fence {r.a/1e6:6.2f} acquire {r.b/1e6:6.2f} "
                f"queue {r.c/1e6:6.2f} recreate {r.d/1e6:6.2f} ms")
    if r.type == SLIP:
        return (f"{t:10.4f} SLIP    {'render' if r.flags else 'sim   '} "
                f"{r.a} period(s), {r.b/1e6:.1f} ms behind")
    if r.type == INPUT_EDGE:
        return (f"{t:10.4f} INPUT   bits 0x{r.a & 0xFFFF:04x} "
                f"delta 0x{r.aux:04x} analog {r.flags}")
    if r.type == WINDOW:
        return f"{t:10.4f} WINDOW  id {r.aux} {'begin' if r.flags else 'end'}"
    if r.type == CLOCKSAMPLE:
        return f"{t:10.4f} CLOCK   mono {r.a} wall {r.b}"
    if r.type == SHED:
        return (f"{t:10.4f} SHED    kind {r.flags} reason {r.aux} "
                f"cost {r.a/1e6:.2f} ms margin {r.b/1e6:.2f} ms")
    # An unknown type is a log from a newer build, not a corrupt one: render
    # it raw rather than refusing, so a log genuinely outlives its writer.
    return (f"{t:10.4f} {r.name:<7} flags {r.flags} aux {r.aux} "
            f"index {r.index} {r.a} {r.b} {r.c} {r.d}")


def cmd_dump(args):
    log = read(args.log)
    wanted = None
    if args.type:
        wanted = {NAME_TYPES[t] for t in args.type}
    print(f"# env: " + " ".join(f"{k}={v}" for k, v in sorted(log.env.items())))
    print(f"# {log.vbl_num}/{log.vbl_den} Hz VBL = {log.vbl_hz:.7f}, "
          f"tick {log.tick_hz:.7f} Hz, {len(log.records)} record(s)")
    n = 0
    for r in log.records:
        if wanted and r.type not in wanted:
            continue
        print(fmt_record(r, log.epoch_ns))
        n += 1
        if args.limit and n >= args.limit:
            break
    return 0


def pct(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    i = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[i]


def cmd_verify(args):
    log = read(args.log)
    fail = []

    # Structural: the header must carry the exact NTSC rational, not a rounded
    # rate. A log that says 30 Hz was written against the wrong clock and every
    # number derived from it is wrong in the same direction.
    if (log.vbl_num, log.vbl_den) != (2250000, 37609):
        fail.append(f"VBL rate is {log.vbl_num}/{log.vbl_den}, "
                    f"expected 2250000/37609")
    missing = [k for k in ("build", "platform", "cpu", "gpu", "present_mode",
                           "refresh_mhz") if k not in log.env]
    if missing:
        fail.append(f"env block missing mandatory field(s): {', '.join(missing)}")

    # Under --record-cfr op_now reports SCHEDULED time, so exec_start equals
    # the deadline on every VBL by construction. A phase statistic from such a
    # log is a fabricated perfect score, not a measurement.
    virtual = log.env.get("virtual_clock", "0") not in ("0", "")

    vbls = log.of(VBL)
    presents = log.of(PRESENT)
    # Engagement, not decoration: a log with no VBL records proves the writer
    # opened a file, nothing more.
    if not vbls:
        fail.append("no VBL records — the log engaged nothing (VACUOUS)")

    paced = [r for r in vbls if r.paced]
    # Same population split the runtime census makes: a paced VBL that was
    # paused/stepped/rewound/loading is reported but is not pacing evidence.
    TRANSIENT = F_PAUSED | F_STEPPED | F_REWIND | F_NOT_LIVE | F_SLIPPED
    clean = [r for r in paced if not (r.flags & TRANSIENT)]
    print(f"records: {len(log.records)}  VBL {len(vbls)} ({len(paced)} paced)  "
          f"PRESENT {len(presents)}  SLIP {len(log.of(SLIP))}  "
          f"INPUT {len(log.of(INPUT_EDGE))}  WINDOW {len(log.of(WINDOW))}  "
          f"CLOCK {len(log.of(CLOCKSAMPLE))}")

    # Within a record type, timestamps must not go backwards: op_now is
    # documented as strictly increasing, so a regression means a torn or
    # misattributed stamp. Across types they legitimately interleave out of
    # order (see Log.timeline), which is why this is checked per type.
    for ty in sorted({r.type for r in log.records}):
        seq = [r.t_ns for r in log.of(ty) if r.t_ns]
        back = sum(1 for a, b in zip(seq, seq[1:]) if b < a)
        if back:
            fail.append(f"{back} {TYPE_NAMES.get(ty, f'type-{ty}')} record(s) "
                        f"went backwards in time")

    if virtual:
        print("virtual clock (--record-cfr): phase error is 0 by construction; "
              "no pacing statistic is available from this log")
        if clean:
            fail.append("virtual-clock log contains paced VBLs — the census "
                        "should have classified them unpaced")
    elif clean:
        errs = [r.phase_err_ns for r in clean]
        print(f"phase error (clean, n={len(clean)}): p50 {pct(errs,50)/1e3:+.1f}us "
              f"p99 {pct(errs,99)/1e3:+.1f}us max {max(errs)/1e3:+.1f}us")
    if not virtual and len(paced) != len(clean):
        terrs = [r.phase_err_ns for r in paced if r.flags & TRANSIENT]
        print(f"phase error (transient, n={len(terrs)}): "
              f"max {max(terrs)/1e3:+.1f}us — paused/stepped/rewound/loading/slipped")

    if args.expect_inject_us:
        # Calibration against a known input. The check is a COUNT, not an
        # extremum: a real run carries genuine multi-millisecond outliers
        # (scheduler, page faults, a level load), so the largest phase error
        # says nothing about whether the injected stalls were seen. How many
        # VBLs came in at the injected magnitude does.
        want_ns = args.expect_inject_us * 1000
        # The injector fires every EVERY-th PACED VBL, so the expected count
        # must be taken over the same population — using `clean` would
        # under-count by the transient fraction on load-heavy runs.
        expect_n = len(paced) / float(args.expect_inject_every)
        hits = [r.phase_err_ns for r in clean if r.phase_err_ns >= want_ns * 0.8]
        errs = sorted(r.phase_err_ns for r in clean)
        med = errs[len(errs) // 2] if errs else 0
        print(f"injection check: {len(hits)} VBL(s) at >= {want_ns*0.8/1e3:.0f}us, "
              f"expected ~{expect_n:.0f}; median {med/1e3:+.1f}us")
        if not clean:
            fail.append("injection expected but no clean paced VBL was recorded")
        elif len(hits) < expect_n * 0.5:
            fail.append(f"only {len(hits)} VBL(s) reached the injected "
                        f"{args.expect_inject_us} us, expected ~{expect_n:.0f} — "
                        f"the log is not measuring the injected stall")
        elif med > want_ns * 0.5:
            # If everything is late, a late tail is not evidence of injection.
            fail.append(f"median phase error {med/1e3:.1f} us is within reach of "
                        f"the injected {args.expect_inject_us} us — the run is "
                        f"late everywhere, so this proves nothing")

    for f in fail:
        print(f"FAIL: {f}", file=sys.stderr)
    print("verify: " + ("FAIL" if fail else "PASS"))
    return 2 if fail else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="print records")
    d.add_argument("log", type=Path)
    d.add_argument("--type", action="append", choices=sorted(NAME_TYPES),
                   help="restrict to this record type (repeatable)")
    d.add_argument("--limit", type=int, default=0)
    d.set_defaults(fn=cmd_dump)

    v = sub.add_parser("verify", help="structural + calibration checks")
    v.add_argument("log", type=Path)
    v.add_argument("--expect-inject-us", type=int, default=0,
                   help="assert the log measured an OPENPETE_PACE_INJECT_US stall")
    v.add_argument("--expect-inject-every", type=int, default=30,
                   help="the injection period used (OPENPETE_PACE_INJECT_US=N,EVERY)")
    v.set_defaults(fn=cmd_verify)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
