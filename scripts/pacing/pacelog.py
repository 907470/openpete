#!/usr/bin/env python3
"""pacelog.py - reader for the unified pacing event log (--pace-log PATH).

The shared parsing layer for scripts/pacing/. Everything downstream reads a
log through `read()`, so there is exactly one place that knows the binary
layout; see core/include/openpete_pace_log.h for the authoritative definition.

It also owns the ANALYSED POPULATION (`Log.analysis_vbls`): masking lives with
the parser rather than in each subcommand, because two subcommands that
disagree about which VBLs count produce numbers nobody can compare, and the
disagreement is invisible in both outputs.

Run the suite via scripts/pacing/pace.py - this module has no CLI.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x4C50504F  # 'OPPL'
VERSION = 2
HEADER_FIXED = 64
RECORD_BYTES = 64
# v1 records had four payload slots and no `e`. The size comes from the header,
# so an older log still parses; its `e` reads as absent (None), which callers
# must distinguish from a real zero - `e` carries a latch identity where zero
# is a meaningful value, not a missing one.
RECORD_LAYOUT = {48: ("<BBHIqqqqq", 1), 64: ("<BBHIqqqqqqq", 2)}

(VBL, PRESENT, SHED, SLIP, INPUT_EDGE, WINDOW, CLOCKSAMPLE, WAKE,
 PRESENT_SPAN, PAD_READ, FLIP, CD_MISS, HOST_PUMP, PRESENT_DETAIL) = range(1, 15)
TYPE_NAMES = {VBL: "VBL", PRESENT: "PRESENT", SHED: "SHED", SLIP: "SLIP",
              INPUT_EDGE: "INPUT_EDGE", WINDOW: "WINDOW",
              CLOCKSAMPLE: "CLOCKSAMPLE", WAKE: "WAKE",
              PRESENT_SPAN: "PRESENT_SPAN", PAD_READ: "PAD_READ",
              FLIP: "FLIP", CD_MISS: "CD_MISS", HOST_PUMP: "HOST_PUMP",
              PRESENT_DETAIL: "PRESENT_DETAIL"}

# PRESENT_DETAIL flags: the interpolation mode of the present.
PD_INTERP, PD_EAGER, PD_DETERMINISTIC, PD_PAIRED = 1, 2, 4, 8

# HOST_PUMP flags. A summary record aggregates one reporting window; the rest
# are single iterations that crossed the record floor. The two shapes share a
# type because they are one accounting: the summary's totals INCLUDE the
# individually recorded iterations, and `hp_long` says how many of them.
HP_SUMMARY, HP_WINDOW, HP_PARKED = 1 << 0, 1 << 1, 1 << 2

# PRESENT / SHED / PRESENT_SPAN kinds, carried in `flags`.
# REPEAT is the same image presented again (the present thread re-blitting the
# last composed frame). It closes a hole in the display without advancing
# animation, so every statistic has to decide explicitly whether it counts.
CANONICAL, INTERMEDIATE, REPEAT = 0, 1, 2
KIND_NAME = {CANONICAL: "canonical   ", INTERMEDIATE: "intermediate",
             REPEAT: "repeat      "}

# `latched_seq` sentinels (openpete_pace_log.h). Publish sequences are even and
# step by 2, so odd values are free to name the reasons a VBL consumed no host
# sample - reasons a bare zero would conflate.
LATCH_NONE, LATCH_CLAIMED = 0, 1

# PAD_READ flag: the read happened in a fiber run that was not the VBL the
# record names (runahead speculation, rewind replay). Real, and excluded from
# the read-phase distribution.
PR_UNATTRIBUTED = 1 << 0
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
    __slots__ = ("type", "flags", "aux", "index", "t_ns", "a", "b", "c", "d",
                 "e", "f")

    def __init__(self, tup):
        (self.type, self.flags, self.aux, self.index,
         self.t_ns, self.a, self.b, self.c, self.d) = tup[:9]
        self.e = tup[9] if len(tup) > 9 else None
        self.f = tup[10] if len(tup) > 10 else None

    @property
    def name(self):
        return TYPE_NAMES.get(self.type, f"?{self.type}")

    def flag_str(self):
        return ",".join(n for bit, n in FLAG_NAMES if self.flags & bit) or "-"

    # PRESENT: what the frame SHOWED, not when it was submitted. None on a
    # log written before alpha was recorded, which is not the same as 0.0.
    @property
    def present_alpha(self):
        return None if self.e is None else self.e / 1e6

    @property
    def present_paired(self):
        return None if self.f is None else bool(self.f)

    # VBL accessors, named so callers never index the raw payload slots.
    @property
    def deadline_ns(self): return self.a
    @property
    def latch_ns(self): return self.b
    @property
    def exec_start_ns(self): return self.c
    @property
    def exec_end_ns(self): return self.d

    @property
    def latched_seq(self):
        """Which published input sample this VBL consumed - identity, not a
        timestamp. None on a v1 log (the field did not exist), LATCH_NONE if
        this path never latches, LATCH_CLAIMED if another writer owned the pad,
        else an even publish sequence."""
        return self.e

    @property
    def consumed_input(self):
        """True only if this VBL took a host sample. A claimed VBL carries a
        stale-looking latch instant it never used, and folding those into an
        input statistic is how a bk2 replay produces input numbers."""
        return self.e is not None and self.e >= 2

    @property
    def latched_bits(self):
        """Pad bits of the sample this VBL consumed. Whether a press reached
        the game is a question about state: the producer republishes far
        faster than the consumer latches, so a held button arrives through a
        LATER sample than the one whose edge announced it."""
        return None if self.f is None else (self.f & 0xFFFF)

    # INPUT_EDGE accessors.
    @property
    def edge_bits(self): return self.a & 0xFFFF
    @property
    def edge_delta(self): return self.aux
    @property
    def edge_seq(self): return self.e

    # PAD_READ accessors: the guest's own read of its pad buffer.
    @property
    def read_ordinal(self): return self.aux          # 1 = first read of the VBL
    @property
    def read_offset_ns(self):
        """How far into the VBL the guest looked at the pad."""
        return self.t_ns - self.a if self.a else None
    @property
    def read_age_ns(self):
        """Age of the consumed sample at the moment the guest read it."""
        return self.t_ns - self.b if self.b else None

    # PRESENT accessors. `t_ns` is when the present path RETURNED; the block
    # fields are the waits it imposed before that.
    # PRESENT_SPAN accessors: how long a present occupied the thread that
    # also owns the sim clock, and how much of that was the scene rebuild.
    @property
    def span_start_ns(self): return self.a
    @property
    def span_cost_ns(self): return self.b
    @property
    def span_extract_ns(self): return self.c

    # HOST_PUMP accessors - one iteration of the window-owning thread.
    # `hp_active` is the part that is a COST: the park before it is the loop
    # idling, and charging it as time spent would report a healthy pump as a
    # thread that is busy ~100% of the time.
    @property
    def hp_summary(self): return bool(self.flags & HP_SUMMARY)
    @property
    def hp_park_ns(self): return self.b
    @property
    def hp_events_ns(self): return self.c
    @property
    def hp_input_ns(self): return self.d
    @property
    def hp_window_ns(self): return self.e
    @property
    def hp_active_ns(self):
        return self.t_ns - self.a - self.b

    # HOST_PUMP summary accessors (flags & HP_SUMMARY). Its totals cover every
    # iteration of the window, including the ones that also got their own
    # record - `hp_long` is how many those were, not a separate population.
    @property
    def hp_iters(self): return self.index
    @property
    def hp_active_sum_ns(self): return self.b
    @property
    def hp_active_max_ns(self): return self.c
    @property
    def hp_events_max_ns(self): return self.d
    @property
    def hp_long(self): return self.e
    @property
    def hp_park_sum_ns(self): return self.f

    # WAKE accessors.
    @property
    def wake_target_ns(self): return self.a
    @property
    def wake_overshoot_ns(self):
        """Time the OS owed us: measured from the later of the target and the
        request, so a deadline that had already passed is not charged to the
        scheduler."""
        return self.b

    @property
    def wake_behind_ns(self):
        """How far past the target the loop already was when it asked to
        sleep - the loop running behind, which is a different defect from the
        OS being late and points at a different system."""
        return self.c

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
        """Start-phase error: how late the tick began against its grid
        point. Signed - an early start is a different defect from a late one."""
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
        paced and live - a run's opening seconds are load, allocation and
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
        unterminated span ends at the last record - a run killed inside an
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

    # The environment splits into three parts, and conflating them is what
    # made the baseline archive unreadable:
    #
    #   MACHINE - what the numbers were taken ON. Files the baseline.
    #   REGIME  - what the run WAS. Two runs differing here are two different
    #             questions, and subtracting them compares nothing.
    #   BUILD   - what is being MEASURED. It must never be a reason to refuse
    #             a comparison: a baseline that only matches the build that
    #             produced it can never gate a later one, which is precisely
    #             why nothing ever read these files back.
    # The compositor sits directly in the measured path - it owns the
    # swapchain's release cadence - so two runs under different compositors
    # are not the same machine however identical the silicon.
    MACHINE_FIELDS = ("platform", "cpu", "cpu_count", "governor", "gpu",
                      "driver", "refresh_mhz", "compositor")
    # thread_split / input_legacy / headless belong in the regime, not merely
    # in the header: they change which thread owns the sim clock and how input
    # reaches it, so two logs differing in them are two regimes.
    REGIME_FIELDS = ("present_mode", "render_fps", "pacer_slack_ns",
                     "virtual_clock", "thread_split", "input_legacy",
                     "headless", "runahead", "eager", "eager_lattice")

    def eager(self):
        """Eager production: presents and flip waits on their own thread, so
        FLIP records cover the tick boundary too. Absent in older logs."""
        return self.env.get("eager", "0") == "1"

    def machine_key(self):
        """Identity of the machine. Baselines are filed under this."""
        return tuple((k, self.env.get(k, "?")) for k in self.MACHINE_FIELDS)

    def regime_key(self):
        """Identity of the run's configuration."""
        return tuple((k, self.env.get(k, "?")) for k in self.REGIME_FIELDS)

    def env_key(self):
        """Machine + regime: everything a comparison must hold fixed."""
        return self.machine_key() + self.regime_key()

    def build_id(self):
        """The build the run was taken on - reported, never a refusal."""
        return self.env.get("build", "?")


def read(path: Path) -> Log:
    data = Path(path).read_bytes()
    if len(data) < HEADER_FIXED:
        raise SystemExit(f"{path}: too small for a header ({len(data)} B)")
    (magic, version, header_bytes, rec_bytes, env_bytes, _res,
     created_unix_ns, epoch_ns, vbl_num, vbl_den, tick_vbls) = \
        struct.unpack_from("<IIIIIIqqqqq", data, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: bad magic 0x{magic:08x} (expected OPPL)")
    if rec_bytes not in RECORD_LAYOUT:
        raise SystemExit(f"{path}: unsupported record size {rec_bytes}")
    fmt, layout_version = RECORD_LAYOUT[rec_bytes]
    # The record size, not the version field, decides how to parse: a version
    # this reader predates but whose records it can still lay out is readable,
    # and refusing it would throw away a log nobody can regenerate.
    if version > VERSION:
        print(f"{path}: written by a newer build (version {version}); "
              f"parsing its {rec_bytes}-byte records", file=sys.stderr)
    elif version != layout_version:
        raise SystemExit(f"{path}: version {version} with {rec_bytes}-byte "
                         f"records - header is inconsistent")

    env = {}
    for line in data[HEADER_FIXED:HEADER_FIXED + env_bytes].decode(
            "utf-8", "replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v

    body = data[header_bytes:]
    # A run killed mid-write leaves a partial trailing record; truncate rather
    # than refuse, so a SIGKILLed soak run is still readable.
    # Truncate to the size THIS log's header declares, not the current
    # build's. Using the module constant silently refuses every older log -
    # the one thing a format bump must not do, since a log is evidence nobody
    # can regenerate.
    body = body[: (len(body) // rec_bytes) * rec_bytes]
    recs = [Record(t) for t in struct.iter_unpack(fmt, body)]
    return Log(env, epoch_ns, created_unix_ns, vbl_num, vbl_den, tick_vbls, recs)


def fmt_record(r, epoch_ns):
    t = (r.t_ns - epoch_ns) / 1e9
    if r.type == VBL:
        err = f"{r.phase_err_ns / 1e3:+9.1f}us" if r.paced else "  unpaced"
        exec_ms = (r.exec_end_ns - r.exec_start_ns) / 1e6
        seq = {None: "seq n/a", LATCH_NONE: "no latch",
               LATCH_CLAIMED: "claimed"}.get(r.latched_seq,
                                             f"seq {r.latched_seq}")
        return (f"{t:10.4f} VBL     #{r.index:<7} phase {err} "
                f"exec {exec_ms:7.3f}ms [{r.flag_str()}] {seq}")
    if r.type == PRESENT:
        kind = KIND_NAME.get(r.flags, f"kind {r.flags}")
        return (f"{t:10.4f} PRESENT #{r.index:<7} {kind} block: "
                f"fence {r.a/1e6:6.2f} acquire {r.b/1e6:6.2f} "
                f"queue {r.c/1e6:6.2f} recreate {r.d/1e6:6.2f} ms")
    if r.type == SLIP:
        return (f"{t:10.4f} SLIP    {'render' if r.flags else 'sim   '} "
                f"{r.a} period(s), {r.b/1e6:.1f} ms behind")
    if r.type == INPUT_EDGE:
        return (f"{t:10.4f} INPUT   seq {r.edge_seq} bits 0x{r.edge_bits:04x} "
                f"delta 0x{r.edge_delta:04x} analog {r.flags}")
    if r.type == PAD_READ:
        if r.flags & PR_UNATTRIBUTED:
            return (f"{t:10.4f} PADREAD #{r.index:<7} "
                    f"speculative/replayed - not this VBL")
        off = r.read_offset_ns
        age = r.read_age_ns
        return (f"{t:10.4f} PADREAD #{r.index:<7} read {r.read_ordinal} "
                f"{'  n/a' if off is None else f'{off/1e3:+8.1f}us'} into the "
                f"VBL, sample age "
                f"{'n/a' if age is None else f'{age/1e3:+8.1f}us'}")
    if r.type == WINDOW:
        return f"{t:10.4f} WINDOW  id {r.aux} {'begin' if r.flags else 'end'}"
    if r.type == PRESENT_SPAN:
        return (f"{t:10.4f} SPAN    {'intermediate' if r.flags else 'canonical'} "
                f"cost {r.b/1e3:8.1f}us (extract {r.c/1e3:8.1f}us)")
    if r.type == HOST_PUMP:
        if r.hp_summary:
            mean = r.hp_active_sum_ns / r.hp_iters if r.hp_iters else 0
            park = (r.hp_park_sum_ns or 0) / r.hp_iters if r.hp_iters else 0
            return (f"{t:10.4f} PUMP    summary {r.hp_iters:5} iter(s) "
                    f"active mean {mean/1e3:7.1f}us max {r.hp_active_max_ns/1e3:9.1f}us "
                    f"(events {r.hp_events_max_ns/1e3:9.1f}us) "
                    f"park mean {park/1e3:7.1f}us {r.hp_long} over floor")
        return (f"{t:10.4f} PUMP    #{r.index:<7} park {r.hp_park_ns/1e3:10.1f}us "
                f"active {r.hp_active_ns/1e3:9.1f}us = events {r.hp_events_ns/1e3:9.1f} "
                f"input {r.hp_input_ns/1e3:7.1f} window {r.hp_window_ns/1e3:9.1f}us"
                f"{' [parked]' if r.flags & HP_PARKED else ''}"
                f"{' [window op]' if r.flags & HP_WINDOW else ''}")
    if r.type == WAKE:
        return (f"{t:10.4f} WAKE    target {(r.a - epoch_ns)/1e9:.6f}s "
                f"os {r.b/1e3:+.1f}us behind {r.c/1e3:+.1f}us")
    if r.type == CLOCKSAMPLE:
        return f"{t:10.4f} CLOCK   mono {r.a} wall {r.b}"
    if r.type == PRESENT_DETAIL:
        mode = "+".join(n for b, n in ((PD_INTERP, "interp"), (PD_EAGER, "eager"),
                                       (PD_DETERMINISTIC, "det"), (PD_PAIRED, "paired"))
                        if r.flags & b) or "none"
        return (f"{t:10.4f} DETAIL  #{r.index:<7} image {r.a} {r.b}x{r.c} "
                f"tick {(r.d - epoch_ns)/1e9:.6f}s now {(r.e - epoch_ns)/1e9:.6f}s "
                f"mode {mode} presents {r.f}")
    if r.type == SHED:
        return (f"{t:10.4f} SHED    kind {r.flags} reason {r.aux} "
                f"cost {r.a/1e6:.2f} ms margin {r.b/1e6:.2f} ms")
    # An unknown type is a log from a newer build, not a corrupt one: render
    # it raw rather than refusing, so a log genuinely outlives its writer.
    return (f"{t:10.4f} {r.name:<7} flags {r.flags} aux {r.aux} "
            f"index {r.index} {r.a} {r.b} {r.c} {r.d}")


# The CLI lives in pace.py - one entry point, so `summary` and `diff` cannot
# drift apart in what they consider analysable. cmd_verify stays here because
# it is a statement about the FORMAT, which is this module's subject.
if __name__ == "__main__":
    import sys
    sys.exit("pacelog.py is the parsing library; run scripts/pacing/pace.py")
