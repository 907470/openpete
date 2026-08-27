#!/usr/bin/env python3
"""pacelog.py — reader for the unified pacing event log (--pace-log PATH).

The shared parsing layer for scripts/pacing/. Everything downstream reads a
log through `read()`, so there is exactly one place that knows the binary
layout; see core/include/openpete_pace_log.h for the authoritative definition.

It also owns the ANALYSED POPULATION (`Log.analysis_vbls`): masking lives with
the parser rather than in each subcommand, because two subcommands that
disagree about which VBLs count produce numbers nobody can compare, and the
disagreement is invisible in both outputs.

Run the suite via scripts/pacing/pace.py — this module has no CLI.
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
# The population split the runtime census also makes: a paced VBL that was
# paused, stepped, rewound, loading, or is the first tick after a clamp is
# reported but is not pacing evidence.
TRANSIENT = F_PAUSED | F_STEPPED | F_REWIND | F_NOT_LIVE | F_SLIPPED

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

    def analysis_vbls(self, warmup_s=5.0):
        """The VBLs a pacing statistic may be computed over, plus a census of
        what was excluded and why.

        Lives here rather than in each subcommand deliberately: if `summary`
        and `diff` disagree about which VBLs count, their numbers are not
        comparable, and the disagreement is invisible in both outputs.

        Excluded: unpaced VBLs (no deadline was in force), transients
        (paused / stepped / rewound / world-not-live / first tick after a
        clamp), and a warmup span measured from the first VBL that is both
        paced and live — a run's opening seconds are load, allocation and
        cache-cold work, and including them prices the steady state wrongly.

        Returns (kept, excluded) where excluded maps reason -> count."""
        excluded = {"unpaced": 0, "transient": 0, "warmup": 0}
        candidates = []
        for r in self.of(VBL):
            if not r.paced:
                excluded["unpaced"] += 1
            elif r.flags & TRANSIENT:
                excluded["transient"] += 1
            else:
                candidates.append(r)
        if not candidates:
            return [], excluded
        t0 = candidates[0].exec_start_ns + int(warmup_s * 1e9)
        kept = [r for r in candidates if r.exec_start_ns >= t0]
        excluded["warmup"] = len(candidates) - len(kept)
        return kept, excluded

    def windows(self):
        """WINDOW records paired into (id, begin_ns, end_ns) spans. An
        unterminated span ends at the last record — a run killed inside an
        unlock still describes it."""
        open_at, spans = {}, []
        for r in self.of(WINDOW):
            if r.flags & 1:
                open_at[r.aux] = r.t_ns
            elif r.aux in open_at:
                spans.append((r.aux, open_at.pop(r.aux), r.t_ns))
        last = max((r.t_ns for r in self.records), default=0)
        for wid, start in open_at.items():
            spans.append((wid, start, last))
        return sorted(spans, key=lambda s: s[1])

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


# The CLI lives in pace.py — one entry point, so `summary` and `diff` cannot
# drift apart in what they consider analysable. cmd_verify stays here because
# it is a statement about the FORMAT, which is this module's subject.
if __name__ == "__main__":
    import sys
    sys.exit("pacelog.py is the parsing library; run scripts/pacing/pace.py")
