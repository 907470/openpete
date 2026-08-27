#!/usr/bin/env python3
"""pace.py — the pacing analysis suite (one CLI, subcommands).

Reads the unified pacing event log written by `openpete --pace-log PATH`.
Parsing lives in pacelog.py, the maths in pacestats.py (tested against known
inputs by test_pacestats.py), and this file is the presentation layer only —
so a number shown here can be traced to a statistic that has a fixture.

    dump             raw records
    verify           structural + calibration checks
    summary          per-metric triple + percentiles + ledgers + verdict
    drift            OLS frequency error (ppm) + CUSUM undeclared re-anchors
    stability        overlapping Allan / modified Allan deviation
    spectrum         Lomb-Scargle periodogram; named interference lines
    input            latch phase, the guest's own pad-read moment, edge mapping
    attribute        bucket every tail event and slip by cause
    diff             A/B two runs with bootstrap confidence intervals
    export-perfetto  Perfetto trace for eyeballing a single incident
    report           one JSON artifact per run, archivable and comparable

Every subcommand analyses the same masked population (see
`pacelog.Log.analysis_vbls`): paced, non-transient, past warmup. Subcommands
that disagreed about which VBLs count would produce numbers nobody could
compare.
"""
import argparse
import bisect
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pacelog                                                # noqa: E402
import pacestats as ps                                        # noqa: E402

# Nominal VBL period, whole ns. The exact rate is a rational (2250000/37609
# Hz) carried in the log header; this is its rounded form, used only where a
# scalar period is needed for a ratio or a threshold.
def nominal_vbl_ns(log):
    return round(1e9 * log.vbl_den / log.vbl_num)


def us(ns):
    return f"{ns / 1e3:+.1f}us"


def load(path, warmup_s):
    log = pacelog.read(path)
    kept, excluded = log.analysis_vbls(warmup_s)
    return log, kept, excluded


def population_line(log, kept, excluded):
    tot = sum(excluded.values()) + len(kept)
    return (f"population: {len(kept)}/{tot} VBL analysed "
            f"(excluded: " + ", ".join(f"{k} {v}" for k, v in excluded.items()
                                       if v) + ")" if any(excluded.values())
            else f"population: {len(kept)}/{tot} VBL analysed")


def refuse_if_empty(kept, log):
    """A statistic over an empty population is not a passing result."""
    if kept:
        return False
    virtual = log.env.get("virtual_clock", "0") not in ("0", "")
    why = ("the virtual clock cannot express a phase error"
           if virtual else "no paced, non-transient VBL survived masking")
    print(f"VACUOUS: nothing to analyse — {why}", file=sys.stderr)
    return True


# ---------------------------------------------------------------- verify

def cmd_verify(args):
    log = pacelog.read(args.log)
    fail = []

    # Structural: the header must carry the exact NTSC rational, not a rounded
    # rate. A log that says 30 Hz was written against the wrong clock and every
    # number derived from it is wrong in the same direction.
    if (log.vbl_num, log.vbl_den) != (2250000, 37609):
        fail.append(f"pacelog.VBL rate is {log.vbl_num}/{log.vbl_den}, "
                    f"expected 2250000/37609")
    missing = [k for k in ("build", "platform", "cpu", "gpu", "present_mode",
                           "refresh_mhz") if k not in log.env]
    if missing:
        fail.append(f"env block missing mandatory field(s): {', '.join(missing)}")

    # Under --record-cfr op_now reports SCHEDULED time, so exec_start equals
    # the deadline on every VBL by construction. A phase statistic from such a
    # log is a fabricated perfect score, not a measurement.
    virtual = log.env.get("virtual_clock", "0") not in ("0", "")

    vbls = log.of(pacelog.VBL)
    presents = log.of(pacelog.PRESENT)
    # Engagement, not decoration: a log with no VBL records proves the writer
    # opened a file, nothing more.
    if not vbls:
        fail.append("no VBL records — the log engaged nothing (VACUOUS)")

    paced = [r for r in vbls if r.paced]
    # The same population split the runtime census makes.
    TRANSIENT = pacelog.TRANSIENT
    clean = [r for r in paced if not (r.flags & TRANSIENT)]
    print(f"records: {len(log.records)}  VBL {len(vbls)} ({len(paced)} paced)  "
          f"PRESENT {len(presents)}  SLIP {len(log.of(pacelog.SLIP))}  "
          f"INPUT {len(log.of(pacelog.INPUT_EDGE))}  WINDOW {len(log.of(pacelog.WINDOW))}  "
          f"CLOCK {len(log.of(pacelog.CLOCKSAMPLE))}")

    # Within a record type, timestamps must not go backwards: op_now is
    # documented as strictly increasing, so a regression means a torn or
    # misattributed stamp. Across types they legitimately interleave out of
    # order (see Log.timeline), which is why this is checked per type.
    for ty in sorted({r.type for r in log.records}):
        seq = [r.t_ns for r in log.of(ty) if r.t_ns]
        back = sum(1 for a, b in zip(seq, seq[1:]) if b < a)
        if back:
            fail.append(f"{back} {pacelog.TYPE_NAMES.get(ty, f'type-{ty}')} record(s) "
                        f"went backwards in time")

    if virtual:
        print("virtual clock (--record-cfr): phase error is 0 by construction; "
              "no pacing statistic is available from this log")
        if clean:
            fail.append("virtual-clock log contains paced VBLs — the census "
                        "should have classified them unpaced")
    elif clean:
        errs = [r.phase_err_ns for r in clean]
        print(f"phase error (clean, n={len(clean)}): p50 {ps.percentile(errs,50)/1e3:+.1f}us "
              f"p99 {ps.percentile(errs,99)/1e3:+.1f}us max {max(errs)/1e3:+.1f}us")
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


# ------------------------------------------------------------------ dump

def cmd_dump(a):
    log = pacelog.read(a.log)
    wanted = {pacelog.NAME_TYPES[t] for t in a.type} if a.type else None
    print("# env: " + " ".join(f"{k}={v}" for k, v in sorted(log.env.items())))
    print(f"# {log.vbl_num}/{log.vbl_den} Hz VBL = {log.vbl_hz:.7f}, "
          f"tick {log.tick_hz:.7f} Hz, {len(log.records)} record(s)")
    n = 0
    for r in (log.timeline() if a.timeline else log.records):
        if wanted and r.type not in wanted:
            continue
        print(pacelog.fmt_record(r, log.epoch_ns))
        n += 1
        if a.limit and n >= a.limit:
            break
    return 0


# ---------------------------------------------------------------- summary

def cmd_summary(a):
    log, kept, excluded = load(a.log, a.warmup)
    print(f"env: " + " ".join(f"{k}={v}" for k, v in sorted(log.env.items())))
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3

    s1 = [r.phase_err_ns for r in kept]
    starts = [r.exec_start_ns for r in kept]
    # S2, interval error: successive starts against the nominal period. A run
    # can hold a perfect average interval while every individual one is wrong,
    # so this is not implied by S1.
    s2 = [b - a_ - nominal_vbl_ns(log) for a_, b in zip(starts, starts[1:])]

    for name, vals in (("S1 phase error", s1), ("S2 interval error", s2)):
        if not vals:
            continue
        rsig, p99, mx = ps.triple(vals)
        # Three numbers, never a stdev: how tight normal operation is, how bad
        # a typical bad tick is, and the worst thing that happened.
        print(f"{name}: robust sigma {us(rsig)}  p50 {us(ps.percentile(vals,50))} "
              f"p90 {us(ps.percentile(vals,90))} p99 {us(p99)} "
              f"p99.9 {us(ps.percentile(vals,99.9))} max {us(mx)}")
    wakes = log.of(pacelog.WAKE)
    if wakes:
        ov = [r.wake_overshoot_ns for r in wakes]
        # The decomposition that makes a late tick actionable: how much the OS
        # owed us, and how much we spent after it handed control back.
        bh = [r.wake_behind_ns for r in wakes]
        # Two different systems, never summed: what the scheduler owed us, and
        # how far behind the loop already was when it asked to sleep.
        print(f"pacer sleep, OS share:   p50 {us(ps.percentile(ov,50))} "
              f"p99 {us(ps.percentile(ov,99))} max {us(max(ov))}")
        print(f"pacer sleep, loop behind: p50 {us(ps.percentile(bh,50))} "
              f"p99 {us(ps.percentile(bh,99))} max {us(max(bh))}"
              f"  ({len(wakes)} sleeps)")

    worst, at = ps.worst_window([r.exec_start_ns for r in kept],
                                s1, int(a.window * 1e9))
    print(f"worst {a.window:g}s window: {us(worst)} at "
          f"t={(at - log.epoch_ns)/1e9:.3f}s")

    slips = log.of(pacelog.SLIP)
    sim = [r for r in slips if not r.flags]
    ren = [r for r in slips if r.flags]
    print(f"slips: sim {len(sim)} event(s)/{sum(r.a for r in sim)} VBL period(s) "
          f"discarded; render {len(ren)}/{sum(r.a for r in ren)}")
    spans = log.of(pacelog.PRESENT_SPAN)
    if spans:
        c = [r.span_cost_ns for r in spans]
        e = [r.span_extract_ns for r in spans]
        # What an intermediate present costs the sim thread, and how much of
        # it a shed actually avoids.
        print(f"intermediate present cost: p50 {us(ps.percentile(c,50))} "
              f"p99 {us(ps.percentile(c,99))} max {us(max(c))}  "
              f"(scene rebuild p50 {us(ps.percentile(e,50))})")

    sheds = log.of(pacelog.SHED)
    if sheds:
        by_reason = {}
        for r in sheds:
            by_reason[r.aux] = by_reason.get(r.aux, 0) + 1
        names = {1: "slot busy", 2: "no budget before the tick deadline"}
        print("sheds: " + ", ".join(f"{v} {names.get(k, k)}"
                                    for k, v in sorted(by_reason.items())))

    pres = log.of(pacelog.PRESENT)
    print(f"presents: {sum(1 for r in pres if not r.flags)} canonical, "
          f"{sum(1 for r in pres if r.flags)} intermediate; "
          f"sheds {len(log.of(pacelog.SHED))}")

    # The verdict gates on p99/p99.9 and RECORDS max. Max on a general-purpose
    # OS includes SMIs and scheduler noise nobody here controls; gating on it
    # produces a flapping red X that teaches people to ignore the gate.
    p99 = ps.percentile(s1, 99)
    ok = p99 <= a.p99_us * 1000
    print(f"verdict: {'PASS' if ok else 'FAIL'} — p99 {us(p99)} vs goal "
          f"+{a.p99_us:.0f}us (max {us(max(s1))} recorded, not gated)")
    return 0 if ok else 2


# ------------------------------------------------------------------ drift

def cmd_drift(a):
    log, kept, excluded = load(a.log, a.warmup)
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3

    nominal = 1e9 * log.vbl_den / log.vbl_num

    # Segment at every slip. A clamp DISCARDS wall time — the deadline jumps
    # forward and the VBL index does not — so a fit spanning one reads the
    # discontinuity as a frequency error. Those are different defects with
    # different fixes and different meanings for a run's timing: drift is the
    # oscillator being wrong, a slip is wall time the game lost outright.
    # Fitting across a slip attributes the second to the first.
    slip_ns = sorted(r.t_ns for r in log.of(pacelog.SLIP))
    segments, cur = [], []
    si = 0
    for r in kept:
        while si < len(slip_ns) and slip_ns[si] <= r.exec_start_ns:
            if len(cur) >= a.min_segment:
                segments.append(cur)
            cur = []
            si += 1
        cur.append(r)
    if len(cur) >= a.min_segment:
        segments.append(cur)

    if not segments:
        print(f"no slip-free span of >= {a.min_segment} VBL to fit — "
              f"{len(slip_ns)} slip(s) across {len(kept)} analysed VBL",
              file=sys.stderr)
        return 3

    # Pool the segments: sample-weighted mean of the per-segment slopes, with
    # the uncertainty combined in quadrature.
    tot_n = sum(len(seg) for seg in segments)
    ppm = 0.0
    var = 0.0
    for seg in segments:
        si_ = [r.index for r in seg]
        st = [r.exec_start_ns for r in seg]
        p, _ = ps.drift_ppm(si_, st, nominal)
        e = ps.drift_ppm_se(si_, st, nominal)
        w = len(seg) / tot_n
        ppm += w * p
        var += (w * e) ** 2
    se = math.sqrt(var)
    idx = [r.index for r in kept]
    t = [r.exec_start_ns for r in kept]
    slope = nominal * (1 + ppm / 1e6)
    span_s = (t[-1] - t[0]) / 1e9
    print(f"fitted over {len(segments)} slip-free segment(s), {tot_n} VBL "
          f"({len(slip_ns)} slip(s) excluded from the fit)")
    # What the frequency error costs over a real session, which is the form
    # that matters: this plan's deliverable is that RTA and tick time agree.
    err_2h = ppm * 1e-6 * 7200
    print(f"executed period: {slope/1e6:.6f} ms vs nominal "
          f"{nominal/1e6:.6f} ms over {span_s:.1f}s")
    print(f"frequency error: {ppm:+.4f} +/- {se:.4f} ppm (1 sigma) "
          f"({err_2h:+.3f} s over a 2-hour run)")

    # Slip-discarded wall time is reported alongside, never folded in: it is
    # real divergence between RTA and tick time, and it is what the
    # never-silent contract exists to surface.
    discarded_ns = sum(r.a for r in log.of(pacelog.SLIP) if not r.flags) * nominal
    if discarded_ns:
        print(f"slip-discarded wall time: {discarded_ns/1e6:.1f} ms over "
              f"{span_s:.1f}s — separate from drift, and the larger effect "
              f"here ({discarded_ns/1e9/span_s*1e6:+.0f} ppm equivalent)")

    # Three-way, against the GATE rather than against zero: a drift far
    # outside the gate is a definite failure even when the uncertainty is
    # loose, and only an interval straddling the gate is unresolved.
    lo_mag, hi_mag = abs(ppm) - 2 * se, abs(ppm) + 2 * se

    # C1, host-clock honesty. Everything above measures the grid against
    # op_now; this measures op_now itself against a disciplined wall clock.
    # A TSC calibrated dozens of ppm off is invisible to every
    # self-measurement, and at 50 ppm a two-hour run is ~0.36 s from true
    # time — material for RTA, and undetectable from inside the grid.
    #
    # Disclosure only: the sim clock stays monotonic-pure and is never
    # disciplined to NTP.
    cs = log.of(pacelog.CLOCKSAMPLE)
    if len(cs) >= 3:
        mono = [r.a for r in cs]
        wall = [r.b for r in cs]
        c_span = (mono[-1] - mono[0]) / 1e9
        slope_c, _ = ps.ols_slope(mono, wall)
        c_ppm = (slope_c - 1.0) * 1e6
        c_se = ps.ols_slope_se(mono, wall) * 1e6
        offsets = [w - m for m, w in zip(mono, wall)]
        spread = max(offsets) - min(offsets)
        # Two genuinely independent oscillators wander by microseconds over a
        # couple of minutes. A sub-microsecond spread means the platform is
        # deriving both from ONE source, and the estimate below is measuring a
        # clock against itself — a guaranteed +0.00 ppm that reads as a clean
        # bill of health. Report the coupling, not the zero.
        if spread < 1000:
            print(f"host clock (C1): VACUOUS on this platform — the two "
                  f"sources track to {spread}ns over {c_span:.0f}s, so they "
                  f"are not independent and no oscillator error can be seen. "
                  f"Needs a genuinely external reference.")
        else:
            print(f"host clock (C1): monotonic vs wall {c_ppm:+.2f} +/- "
                  f"{c_se:.2f} ppm over {c_span:.0f}s "
                  f"({c_ppm*1e-6*7200:+.3f} s per 2 hours) "
                  f"[UNCALIBRATED — no injector proves this column yet]")
    else:
        print(f"host clock (C1): {len(cs)} sample(s) — need a longer run "
              f"(one per 10 s)")

    hits = ps.cusum([r.phase_err_ns for r in kept])
    # A re-anchor is DECLARED if the log already accounts for it: a masking
    # window, or a slip clamp. A clamp abandons the grid deliberately and says
    # so in the slip ledger, so re-reporting it here as an undeclared
    # re-anchor would flag the instrument's own honest record as a finding.
    declared = [(b, e) for _, b, e in log.windows()]
    declared += [(r.t_ns, r.t_ns) for r in log.of(pacelog.SLIP)]
    unexplained, small = [], 0
    for i, direction in hits:
        when = kept[i].exec_start_ns
        # Magnitude floor, for the same reason `diff` has one. The sign
        # statistic resolves ARBITRARILY small shifts given enough samples —
        # it flagged a 1.7 us wander across 3296 VBLs — but a re-anchor
        # abandons the grid, so its step is a pacing-scale event. A shift
        # below the plan's own phase goal is not one.
        before = ps.median([r.phase_err_ns for r in kept[max(0, i - 200):i]])
        after = ps.median([r.phase_err_ns for r in kept[i:i + 200]])
        shift = abs(after - before)
        if shift < a.step_floor_us * 1000:
            small += 1
            continue
        if not any(b - 2e9 <= when <= e + 2e9 for b, e in declared):
            unexplained.append((i, direction, when, shift))
    if hits:
        print(f"grid steps: {len(hits)} detected, {small} below the "
              f"{a.step_floor_us:g}us floor, {len(unexplained)} undeclared")
        for i, d, when, shift in unexplained[:10]:
            print(f"  VBL #{kept[i].index} at t={(when-log.epoch_ns)/1e9:.3f}s "
                  f"({'up' if d > 0 else 'down'} {shift/1e3:.1f}us) — "
                  f"UNDECLARED re-anchor")
    else:
        print("grid steps: none")

    if lo_mag > a.ppm:
        print(f"verdict: FAIL — |{ppm:+.4f}| ppm exceeds the {a.ppm} ppm gate "
              f"beyond uncertainty, {len(unexplained)} undeclared re-anchor(s)")
        return 2
    if hi_mag < a.ppm and not unexplained:
        print(f"verdict: PASS — |{ppm:+.4f}| +/- {se:.4f} ppm is inside the "
              f"{a.ppm} ppm gate, 0 undeclared re-anchor(s)")
        return 0
    if unexplained:
        print(f"verdict: FAIL — {len(unexplained)} undeclared re-anchor(s)")
        return 2
    # Solve for the span that would resolve the gate: the slope's standard
    # error falls as n^-1.5 at a fixed cadence and spread.
    need = span_s * (2 * se / a.ppm) ** (2.0 / 3.0)
    print(f"verdict: UNRESOLVED — {span_s:.1f}s cannot separate "
          f"|{ppm:+.4f}| ppm from the {a.ppm} ppm gate at this jitter "
          f"(needs roughly {need:.0f}s).")
    return 3


# -------------------------------------------------------------- stability

def cmd_stability(a):
    log, kept, excluded = load(a.log, a.warmup)
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3
    t = [r.exec_start_ns for r in kept]
    nominal = nominal_vbl_ns(log)
    curve = ps.adev(t, nominal)
    if not curve:
        print("too few samples for a stability estimate", file=sys.stderr)
        return 3
    print(f"{'tau (s)':>10} {'ADEV':>12} {'MDEV':>12}")
    m = dict(ps.mdev(t, nominal))
    for tau, d in curve:
        md = m.get(tau)
        print(f"{tau:10.4f} {d:12.3e} " + (f"{md:12.3e}" if md else " " * 12))
    lo = ps.adev_slope(curve, 0.0, 0.5)
    hi = ps.adev_slope(curve, 0.5, 1.0)
    print(f"log-log slope: {lo:+.2f} (short tau), {hi:+.2f} (long tau)")
    # -1 is white phase noise: ADEV equals a plain stddev there and tells you
    # nothing a histogram did not. A departure at LONG tau is the finding, and
    # it is the one thing a histogram structurally cannot show.
    if hi > -0.5:
        print("finding: the curve flattens at long tau — wander or drift, not "
              "white phase noise. A tighter pacer will not fix this; it is a "
              "clock-discipline question.")
    else:
        print("reading: white-phase-noise-dominated (slope ~ -1) — the jitter "
              "is memoryless and the deviation carries no more than a stddev.")
    return 0


# --------------------------------------------------------------- spectrum

def cmd_spectrum(a):
    log, kept, excluded = load(a.log, a.warmup)
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3
    pts = ps.spectrum([r.exec_start_ns for r in kept],
                      [float(r.phase_err_ns) for r in kept])
    if not pts:
        print("spectrum unavailable (needs numpy + scipy, and >= 8 samples)",
              file=sys.stderr)
        return 3
    lines = ps.spectrum_lines(pts, n=a.lines, min_power=a.min_power)
    if not lines:
        print(f"no line above power {a.min_power} — no periodic interference")
        return 0
    refresh_hz = float(log.env.get("refresh_mhz", 0)) / 1000.0
    print("periodic lines in the phase error (a line is a diagnosis):")
    for f, p in lines:
        note = ""
        # The named signature: a present cadence beating against a display
        # whose refresh is not a multiple of the tick rate shows up at the
        # difference frequency, not at either rate.
        if refresh_hz > 0:
            for k in (1, 2, 3):
                beat = abs(refresh_hz - k * log.vbl_hz)
                if beat > 0 and abs(f - beat) < max(0.02, 0.05 * beat):
                    note = (f"  <- beat between {refresh_hz:.3f} Hz refresh "
                            f"and {k}x the VBL rate")
        print(f"  {f:9.4f} Hz  power {p:.3f}  (period {1/f:8.2f} s){note}")
    return 0


# -------------------------------------------------------------- attribute

def cmd_attribute(a):
    log, kept, excluded = load(a.log, a.warmup)
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3

    presents = log.of(pacelog.PRESENT)
    windows = log.windows()
    thr = a.threshold_us * 1000
    tails = [r for r in kept if r.phase_err_ns > thr]
    buckets, unattributed = {}, []

    # Pair each analysed VBL with the sleep that immediately preceded it.
    wakes = sorted(log.of(pacelog.WAKE), key=lambda r: r.t_ns)
    wake_before, wi = {}, 0
    for r in kept:
        while wi < len(wakes) and wakes[wi].t_ns <= r.exec_start_ns:
            wi += 1
        if wi > 0:
            wake_before[r.index] = wakes[wi - 1]

    # Present spans, bucketed by second so the per-tail scan stays linear.
    spans = sorted(log.of(pacelog.PRESENT_SPAN), key=lambda r: r.t_ns)
    spans_by_s = {}
    for sp in spans:
        spans_by_s.setdefault(sp.t_ns // 1_000_000_000, []).append(sp)

    def spans_near(r):
        sec = r.exec_start_ns // 1_000_000_000
        out = []
        for k in (sec - 1, sec):
            out.extend(spans_by_s.get(k, ()))
        return out

    by_index = {r.index: i for i, r in enumerate(kept)}
    for r in tails:
        i = by_index[r.index]
        prev = kept[i - 1] if i > 0 else None
        # The tick began late; the cause has to have happened BEFORE it began
        # and after the previous grid point. Anything else is coincidence.
        lo, hi = r.deadline_ns - nominal_vbl_ns(log), r.exec_start_ns
        cause = None
        # Did the OS return late from the pacer's sleep? That is the machine's
        # contribution, and it is not fixable by anything in the present path
        # — separating it is the difference between tuning pacer slack and
        # rearchitecting the render loop.
        wk = wake_before.get(r.index)
        if wk is not None and wk.wake_overshoot_ns > thr:
            cause = "os/sleep overshoot"
        elif wk is not None and wk.wake_behind_ns > thr:
            # Not the scheduler: the loop asked to sleep to a deadline that had
            # already gone. The cause is upstream, in whatever consumed the
            # time — which the present buckets below then name.
            cause = None
        # The previous tick still running when this one's deadline passed is
        # its own cause: the sim overran its budget, and no present blocked.
        # Checked first — when a long tick and a blocked present overlap, the
        # tick that did not finish is the nearer explanation.
        if prev is not None and prev.exec_end_ns > r.deadline_ns:
            cause = "previous tick overran"
        blocked = [p for p in presents if lo <= p.t_ns <= hi
                   and max(p.fence_block_ns, p.acquire_block_ns,
                           p.queue_block_ns, p.recreate_block_ns) > thr]
        if cause:
            pass
        elif blocked:
            p = max(blocked, key=lambda p: max(
                p.fence_block_ns, p.acquire_block_ns,
                p.queue_block_ns, p.recreate_block_ns))
            worst = max((p.fence_block_ns, "present/fence"),
                        (p.acquire_block_ns, "present/acquire"),
                        (p.queue_block_ns, "present/queue"),
                        (p.recreate_block_ns, "present/recreate"))
            cause = worst[1]
        elif any(s.span_start_ns <= hi and s.t_ns >= r.deadline_ns
                 for s in spans_near(r)):
            # Present CPU — the scene rebuild, recording, submission — landing
            # across this tick's deadline. Not a blocked call; ordinary work
            # on the thread that owns the sim clock.
            cause = "present cpu (span)"
        elif any(b <= hi and lo <= e for _, b, e in windows):
            cause = "declared window"
        if cause:
            buckets[cause] = buckets.get(cause, 0) + 1
        else:
            unattributed.append(r)

    print(f"tail events (> {a.threshold_us}us): {len(tails)}")
    for k, v in sorted(buckets.items(), key=lambda kv: -kv[1]):
        print(f"  {v:5d}  {k}")
    print(f"  {len(unattributed):5d}  UNATTRIBUTED")
    if unattributed:
        # Not a shrug: an unattributed tail means the log is missing an event
        # type. That is a defect in the instrument and the most actionable
        # thing this subcommand can report.
        print("\nunattributed tails are a FINDING, not noise: the log has no "
              "event that explains them, so an event type is missing.")
        for r in sorted(unattributed, key=lambda r: -r.phase_err_ns)[:a.show]:
            print(f"  VBL #{r.index} at "
                  f"t={(r.exec_start_ns-log.epoch_ns)/1e9:8.3f}s  "
                  f"{us(r.phase_err_ns)}  exec "
                  f"{(r.exec_end_ns-r.exec_start_ns)/1e6:.2f}ms")
    return 0


# ------------------------------------------------------------------- diff

def cmd_diff(a):
    log_a, kept_a, _ = load(a.baseline, a.warmup)
    log_b, kept_b, _ = load(a.candidate, a.warmup)

    ka, kb = log_a.env_key(), log_b.env_key()
    if ka != kb:
        # Refusing is the whole point: silently subtracting numbers taken on
        # different machines is how a pacing "improvement" gets shipped.
        print("REFUSED: environment keys differ — these runs are not "
              "comparable.", file=sys.stderr)
        for (k, va), (_, vb) in zip(ka, kb):
            if va != vb:
                print(f"  {k}: {va!r} vs {vb!r}", file=sys.stderr)
        if not a.force:
            return 2
        print("(--force given; continuing anyway)", file=sys.stderr)

    if refuse_if_empty(kept_a, log_a) or refuse_if_empty(kept_b, log_b):
        return 3

    print(f"baseline  {len(kept_a)} VBL   candidate {len(kept_b)} VBL")
    worse = False
    for label, stat in (("p50", lambda v: ps.percentile(v, 50)),
                        ("p99", lambda v: ps.percentile(v, 99)),
                        ("robust sigma", ps.robust_sigma)):
        va = [r.phase_err_ns for r in kept_a]
        vb = [r.phase_err_ns for r in kept_b]
        point, lo, hi = ps.bootstrap_ci(va, vb, stat)
        # Two independent tests, and a delta must clear BOTH.
        #
        # Statistical significance alone over-claims: with a few hundred
        # samples the bootstrap resolves a 2 us shift confidently, and
        # reporting that as IMPROVED invites shipping a change that moved
        # nothing anyone can experience. The magnitude floor is the baseline's
        # own core width — below the run's noise, a real difference is still
        # not a finding.
        floor = a.floor_frac * ps.robust_sigma(va)
        if lo <= 0 <= hi:
            verdict = "no significant change"
        elif abs(point) < floor:
            verdict = (f"below the noise floor (|{point/1e3:.1f}| < "
                       f"{floor/1e3:.1f} us)")
        elif point < 0:
            verdict = "IMPROVED"
        else:
            verdict = "REGRESSED"
            worse = True
        print(f"  {label:>13}: {stat(va)/1e3:+8.1f} -> {stat(vb)/1e3:+8.1f} us  "
              f"delta {point/1e3:+7.1f} us  95% CI [{lo/1e3:+.1f}, {hi/1e3:+.1f}]"
              f"  {verdict}")
    return 2 if worse else 0


# -------------------------------------------------------- export-perfetto

def cmd_export_perfetto(a):
    log = pacelog.read(a.log)
    ev, pid = [], 1
    names = {1: "tick", 2: "present", 3: "shed", 4: "slip",
             5: "input", 6: "window", 7: "clock"}
    for tid, label in names.items():
        ev.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": tid,
                   "args": {"name": label}})
    for r in log.timeline():
        ts = (r.t_ns - log.epoch_ns) / 1000.0          # Perfetto wants us
        if r.type == pacelog.VBL:
            ev.append({"name": f"tick #{r.index}", "ph": "X", "pid": pid,
                       "tid": 1, "ts": (r.exec_start_ns - log.epoch_ns) / 1000.0,
                       "dur": max(0, r.exec_end_ns - r.exec_start_ns) / 1000.0,
                       "args": {"phase_err_us": r.phase_err_ns / 1000.0,
                                "flags": r.flag_str()}})
        elif r.type == pacelog.PRESENT:
            ev.append({"name": "intermediate" if r.flags else "canonical",
                       "ph": "X", "pid": pid, "tid": 2, "ts": ts, "dur": 1,
                       "args": {"fence_us": r.fence_block_ns / 1000.0,
                                "acquire_us": r.acquire_block_ns / 1000.0,
                                "queue_us": r.queue_block_ns / 1000.0,
                                "recreate_us": r.recreate_block_ns / 1000.0}})
        elif r.type == pacelog.SLIP:
            ev.append({"name": f"slip {r.a} VBL", "ph": "i", "pid": pid,
                       "tid": 4, "ts": ts, "s": "g"})
        elif r.type == pacelog.INPUT_EDGE:
            ev.append({"name": "edge", "ph": "i", "pid": pid, "tid": 5,
                       "ts": ts, "s": "g"})
    for wid, b, e in log.windows():
        ev.append({"name": f"window {wid}", "ph": "X", "pid": pid, "tid": 6,
                   "ts": (b - log.epoch_ns) / 1000.0, "dur": (e - b) / 1000.0})
    Path(a.out).write_text(json.dumps({"traceEvents": ev,
                                       "displayTimeUnit": "ms"}))
    print(f"wrote {a.out} ({len(ev)} events) — open at https://ui.perfetto.dev")
    return 0


# ------------------------------------------------------------------ report

def cmd_report(a):
    log, kept, excluded = load(a.log, a.warmup)
    if refuse_if_empty(kept, log):
        return 3
    s1 = [r.phase_err_ns for r in kept]
    t = [r.exec_start_ns for r in kept]
    nominal = 1e9 * log.vbl_den / log.vbl_num
    ppm, _ = ps.drift_ppm([r.index for r in kept], t, nominal)
    rsig, p99, mx = ps.triple(s1)
    curve = ps.adev(t, round(nominal))
    out = {
        "env": log.env,
        "env_key": [list(kv) for kv in log.env_key()],
        "vbl_hz": log.vbl_hz,
        "tick_hz": log.tick_hz,
        "population": {"analysed": len(kept), "excluded": excluded},
        "s1_phase_error_ns": {
            "robust_sigma": rsig, "p50": ps.percentile(s1, 50),
            "p90": ps.percentile(s1, 90), "p99": p99,
            "p99_9": ps.percentile(s1, 99.9), "max": mx,
        },
        "drift_ppm": ppm,
        "drift_s_per_2h": ppm * 1e-6 * 7200,
        "adev": [{"tau_s": tau, "adev": d} for tau, d in curve],
        "adev_slope_long_tau": ps.adev_slope(curve, 0.5, 1.0),
        "slips": {"sim": sum(1 for r in log.of(pacelog.SLIP) if not r.flags),
                  "render": sum(1 for r in log.of(pacelog.SLIP) if r.flags)},
        "presents": {
            "canonical": sum(1 for r in log.of(pacelog.PRESENT) if not r.flags),
            "intermediate": sum(1 for r in log.of(pacelog.PRESENT) if r.flags)},
        "sheds": len(log.of(pacelog.SHED)),
        "uncalibrated": ["present_block_times", "latch_offset", "clocksample"],
    }
    Path(a.out).write_text(json.dumps(out, indent=2))
    print(f"wrote {a.out}")
    return 0


# --------------------------------------------------------------------- input

def input_regime(log):
    """What an input metric from this log can honestly mean.

    Returns (fatal, notes). A fatal regime is one where the quantity is zero or
    undefined BY CONSTRUCTION, so a number computed from it would describe the
    instrument rather than the game — the shape every defect in this suite has
    taken so far."""
    env, fatal, notes = log.env, [], []
    if env.get("virtual_clock", "0") not in ("0", ""):
        fatal.append("the virtual clock advances only when the pacer sleeps, "
                     "so every input instant is scheduled, not observed")
    if env.get("input_legacy", "0") not in ("0", ""):
        fatal.append("the legacy input path never latches a host sample, so "
                     "there is no consumption instant to measure")
    if env.get("thread_split", "1") in ("0", ""):
        # Not fatal: edge->VBL attribution and the pad-read phase are still
        # real. But staleness is the length of ten lines of C, and quoting it
        # as an input-freshness result is how a green gets reported for a
        # regime the shipped default never runs in.
        notes.append("thread split OFF: the producer runs in-loop immediately "
                     "before the latch, so publish staleness is ~0 by "
                     "construction and is NOT evidence about the default")
    if env.get("headless", "0") not in ("0", ""):
        notes.append("headless: the split is refused here, and almost no "
                     "input reaches the process — coverage, not evidence")
    return fatal, notes


def cmd_input(a):
    log, kept, excluded = load(a.log, a.warmup)
    print("env: " + " ".join(f"{k}={v}" for k, v in sorted(log.env.items())))
    fatal, notes = input_regime(log)
    if a.require_pulses and not int(log.env.get("input_pulse", "0") or 0):
        print("REFUSED: --require-pulses but this run had no pulse source; a "
              "sweep result cannot be claimed from a log with no injection",
              file=sys.stderr)
        return 3
    for n in notes:
        print(f"note: {n}")
    if fatal:
        for f in fatal:
            print(f"REFUSED: {f}", file=sys.stderr)
        return 3
    print(population_line(log, kept, excluded))
    if refuse_if_empty(kept, log):
        return 3

    period = nominal_vbl_ns(log)

    # ---- I1 and staleness, over VBLs that actually consumed a host sample.
    consumed = [r for r in kept if r.consumed_input]
    claimed = sum(1 for r in kept if r.latched_seq == pacelog.LATCH_CLAIMED)
    nolatch = sum(1 for r in kept if r.latched_seq == pacelog.LATCH_NONE)
    unknown = sum(1 for r in kept if r.latched_seq is None)
    print(f"input population: {len(consumed)}/{len(kept)} VBL consumed a host "
          f"sample (claimed {claimed}, no-latch {nolatch}"
          + (f", pre-v2 log {unknown}" if unknown else "") + ")")
    if unknown:
        print("REFUSED: this log predates the latch-identity field; its input "
              "numbers cannot be attributed", file=sys.stderr)
        return 3
    if not consumed:
        print("VACUOUS: no VBL consumed a host input sample — another writer "
              "owned the pad throughout (bk2 replay?)", file=sys.stderr)
        return 3

    i1 = [r.latch_ns - r.deadline_ns for r in consumed]
    stale = [r.exec_start_ns - r.latch_ns for r in consumed]
    for name, vals in (("I1 latch phase (latch - deadline)", i1),
                       ("publish staleness (exec_start - latch)", stale)):
        rsig, p99, mx = ps.triple(vals)
        print(f"{name}: robust sigma {us(rsig)}  p50 {us(ps.percentile(vals,50))} "
              f"p99 {us(p99)} max {us(mx)}")

    # ---- Where in the VBL the guest itself reads the pad.
    # This is the moment that decides which sample the frame plays; the latch
    # only decides what is available to be read.
    by_vbl = {}
    for r in log.of(pacelog.PAD_READ):
        by_vbl.setdefault(r.index, []).append(r)
    idx = {r.index for r in kept}
    first = [rs[0] for i, rs in by_vbl.items() if i in idx
             for rs in [sorted(rs, key=lambda x: x.t_ns)]]
    if not by_vbl:
        print("pad read: no probe in this log — the guest's own read moment "
              "is UNMEASURED, not absent")
    elif not first:
        print("pad read: probe present but no read landed in an analysable "
              "VBL")
    else:
        offs = [r.read_offset_ns for r in first if r.read_offset_ns is not None]
        ages = [r.read_age_ns for r in first if r.read_age_ns is not None]
        counts = [len(v) for i, v in by_vbl.items() if i in idx]
        print(f"pad read: {sum(counts)} read(s) in {len(counts)} analysable "
              f"VBL ({min(counts)}-{max(counts)} per VBL)")
        if offs:
            print(f"  first read, into the VBL: p50 {us(ps.percentile(offs,50))} "
                  f"p99 {us(ps.percentile(offs,99))} max {us(max(offs))} "
                  f"({100.0*ps.percentile(offs,50)/period:.1f}% of a period "
                  f"at p50)")
        if ages:
            print(f"  sample age when read:     p50 {us(ps.percentile(ages,50))} "
                  f"p99 {us(ps.percentile(ages,99))} max {us(max(ages))}")

    # ---- Edge -> consuming VBL, keyed on identity.
    #
    # A sequence says WHICH published sample a VBL took. An edge is CONSUMED
    # only if some VBL latched exactly its sequence; if every VBL jumped from a
    # lower sequence to a higher one, the sample carrying that edge was never
    # read and the press did not reach the game. A timestamp comparison cannot
    # tell those apart — it would credit the edge to the next VBL either way,
    # and report a clean result over presses the guest never saw.
    edges = log.of(pacelog.INPUT_EDGE)
    if not edges:
        print("input edges: none — nothing was pressed, so there is no "
              "edge-to-VBL result (this is coverage, not a pass)")
        return 0

    yielded = [r for r in log.of(pacelog.VBL) if r.consumed_input]
    yielded.sort(key=lambda r: r.exec_start_ns)
    if yielded and yielded[0].latched_bits is None:
        print("REFUSED: this log has no latched pad bits; whether a press "
              "reached the game cannot be decided from sequences alone",
              file=sys.stderr)
        return 3
    seqs = [r.latched_seq for r in yielded]

    def first_latch_at_or_after(seq):
        i = bisect.bisect_left(seqs, seq)
        return yielded[i] if i < len(yielded) else None

    win = [(b, e) for _wid, b, e in log.windows()]

    def in_window(t):
        return any(b <= t <= e for b, e in win)

    consumed_edges, lost, masked, unresolved = [], [], 0, 0
    for ed in edges:
        if ed.edge_seq is None:
            unresolved += 1
            continue
        if in_window(ed.t_ns):
            # Paused, rewound, loading: the grid does not apply and neither
            # does an input-latency claim.
            masked += 1
            continue
        v = first_latch_at_or_after(ed.edge_seq)
        if v is None:
            unresolved += 1      # the run ended before the next latch
            continue
        # Did the state this edge announced survive to that latch? The mask is
        # the bits the edge changed; if the latch still agrees with the edge on
        # all of them, the game saw the transition. If not, the press came and
        # went between two latches and never reached the guest at all — which
        # a sequence comparison alone would have scored as a hit.
        mask = ed.edge_delta
        if mask and (v.latched_bits & mask) != (ed.edge_bits & mask):
            # Lost. Record the latch interval that swallowed it: a press
            # available for longer than that interval and still missed is a
            # defect; one shorter is sampling working as specified.
            i = bisect.bisect_left(seqs, ed.edge_seq)
            prev = yielded[i - 1].exec_start_ns if i else None
            gap = (v.exec_start_ns - prev) if prev is not None else None
            lost.append((ed, gap))
            continue
        consumed_edges.append((ed, v))

    total = len(edges)
    print(f"input edges: {total} published; {len(consumed_edges)} reached a "
          f"VBL, {len(lost)} lost between latches, {masked} inside a declared "
          f"window, {unresolved} unresolved (run ended)")
    if lost:
        pct = 100.0 * len(lost) / max(1, total - masked - unresolved)
        print(f"  {pct:.1f}% of edges never reached the game: the state "
              f"changed and changed back between two latches. Input is sampled "
              f"once per VBL, so a transition shorter than a VBL period can "
              f"vanish entirely — this is the number a sweep must not hide.")
    if not consumed_edges:
        print("VACUOUS: no edge reached a VBL", file=sys.stderr)
        return 3

    delay = [v.exec_start_ns - ed.t_ns for ed, v in consumed_edges]
    rsig, p99, mx = ps.triple(delay)
    print(f"edge -> consuming VBL start: p50 {us(ps.percentile(delay,50))} "
          f"p99 {us(p99)} max {us(mx)} (robust sigma {us(rsig)})")
    print("  measured from the PUBLISH instant: the OS and pump time before "
          "it is not in this number, and an external injector is the only "
          "thing that can see it.")

    # ---- Tier A sweep: did every injected pulse reach a VBL?
    #
    # Deliberately reported as plumbing validation, not as an input-latency
    # result. This source lives inside the process, downstream of everything
    # that can actually go wrong on the way in, so a clean sweep here says the
    # log and the attribution arithmetic are sound and says nothing about the
    # OS. The end-to-end number belongs to an external injector.
    try:
        pulse_bit = int(log.env.get("input_pulse_bit", "0"), 0)
    except ValueError:
        pulse_bit = 0
    pulse_n = int(log.env.get("input_pulse", "0") or 0)
    if pulse_n and pulse_bit:
        # A press is the edge on which the bit goes active-low.
        presses = [ed for ed in edges
                   if (ed.edge_delta & pulse_bit)
                   and not (ed.edge_bits & pulse_bit)]
        got = sum(1 for ed, _v in consumed_edges
                  if (ed.edge_delta & pulse_bit) and not (ed.edge_bits & pulse_bit))
        print(f"Tier A sweep (bit 0x{pulse_bit:04x}, {pulse_n} scheduled): "
              f"{len(presses)} press(es) published, {got} consumed")
        if not presses:
            print("VACUOUS: the pulse source was configured but published no "
                  "press — it did not engage", file=sys.stderr)
            return 3
        # --hold-us overrides what the run recorded. It is the calibration
        # control for the explained/unexplained split: raised past every latch
        # interval, every loss must become UNEXPLAINED and the check must FAIL.
        # A check that cannot be made to fail is not checking anything.
        hold_ns = (a.hold_us * 1000 if a.hold_us
                   else int(log.env.get("input_pulse_hold_us", "0") or 0) * 1000)
        lost_pulses = [(ed, gap) for ed, gap in lost
                       if (ed.edge_delta & pulse_bit)
                       and not (ed.edge_bits & pulse_bit)]
        # A press is UNEXPLAINED when it was held longer than the latch
        # interval that dropped it: the sample was there to be taken. Those
        # are plumbing defects. A press dropped across an interval longer than
        # its hold is input sampling working exactly as specified, and gating
        # on it would fail the run for being loaded.
        unexplained = [(ed, gap) for ed, gap in lost_pulses
                       if gap is not None and hold_ns and gap < hold_ns]
        explained = len(lost_pulses) - len(unexplained)
        if lost_pulses:
            gaps = [g for _e, g in lost_pulses if g is not None]
            print(f"  {len(lost_pulses)} press(es) lost: {explained} across a "
                  f"latch interval longer than the {hold_ns/1e6:.1f} ms hold "
                  f"(sampling working as specified"
                  + (f", widest {max(gaps)/1e6:.1f} ms" if gaps else "")
                  + f"), {len(unexplained)} UNEXPLAINED")
        if unexplained:
            print(f"FAIL: {len(unexplained)} injected press(es) were available "
                  f"for longer than the latch interval that dropped them — the "
                  f"sample was there to be taken", file=sys.stderr)
            for ed, gap in unexplained[:5]:
                print(f"  seq {ed.edge_seq} at "
                      f"t={(ed.t_ns - log.epoch_ns)/1e9:.3f}s, latch interval "
                      f"{gap/1e6:.2f} ms < hold {hold_ns/1e6:.2f} ms",
                      file=sys.stderr)
            return 2
        print("  plumbing validated: every injected press is attributable, and "
              "every loss is explained by a latch interval wider than its "
              "hold.")

    # ---- Phase of the edge within the consuming VBL's own grid period.
    #
    # Anchored on that VBL's recorded deadline rather than on a reconstructed
    # global grid: the grid is re-anchored at unlock exits and render-fps
    # changes, and a reconstruction bins edges near an anchor against a grid no
    # VBL ever followed.
    phases = []
    for ed, v in consumed_edges:
        if not v.deadline_ns:
            continue          # regrid VBL: no grid to phase against
        # Forward from the grid point the consuming VBL was paced to: 0 is the
        # deadline itself, 1 is a whole period later. Anchored on that VBL's
        # own recorded deadline, so slips and re-anchors need no reconstruction.
        phases.append(((ed.t_ns - v.deadline_ns) % period) / period)
    if not phases:
        return 0

    nb = 10
    hist = [0] * nb
    for ph in phases:
        hist[min(nb - 1, int(ph * nb))] += 1
    occupied = sum(1 for n in hist if n)
    print(f"edge phase after the consuming VBL's grid point ({len(phases)} "
          f"edge(s), 0 = at the deadline, 1 = a period later):")
    wide = max(hist) or 1
    for i, n in enumerate(hist):
        print(f"  {i/nb:.1f}-{(i+1)/nb:.1f} {n:6d} " + "#" * int(40.0 * n / wide))

    if occupied <= 2:
        # Every edge landed in the same slice of the period. That is not a
        # narrow result — it means the producer publishes at a FIXED point in
        # the loop, so the phase being histogrammed is the loop's, not the
        # injection's. With the split off, the pump, the sample and the latch
        # are consecutive statements in one iteration, and no schedule upstream
        # of them can spread the published phase.
        print(f"  DEGENERATE: {occupied}/{nb} bins occupied. Publishes are "
              f"locked to the loop's own phase, so this histogram describes "
              f"the publish cadence, not when input arrives. A phase sweep "
              f"cannot resolve a latch window from this run — it needs the "
              f"thread split (an independent pump cadence) or an external "
              f"injector.")
    else:
        print("  A flat histogram means edges arrive at uniformly random "
              "phases; it is NOT a latency result on its own. The latch-window "
              "edge shows as a step in WHICH VBL consumes each phase, which "
              "needs an injector driving known phases (M5 Tier B).")
    return 0


# -------------------------------------------------------------------- CLI

def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name, fn, help_, log_arg="log"):
        p = sub.add_parser(name, help=help_)
        p.add_argument(log_arg, type=Path)
        p.add_argument("--warmup", type=float, default=5.0,
                       help="seconds of live running to skip (default 5)")
        p.set_defaults(fn=fn)
        return p

    p = add("dump", cmd_dump, "print raw records")
    p.add_argument("--type", action="append", choices=sorted(pacelog.NAME_TYPES))
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--timeline", action="store_true",
                   help="sort by timestamp instead of emission order")

    p = add("verify", cmd_verify, "structural + calibration checks")
    p.add_argument("--expect-inject-us", type=int, default=0)
    p.add_argument("--expect-inject-every", type=int, default=30)

    p = add("summary", cmd_summary, "percentiles, ledgers, verdict")
    p.add_argument("--p99-us", type=float, default=250.0,
                   help="p99 phase-error goal in us (default 250, the plan's)")
    p.add_argument("--window", type=float, default=1.0)

    p = add("drift", cmd_drift, "frequency error + undeclared re-anchors")
    p.add_argument("--ppm", type=float, default=1.0)
    p.add_argument("--step-floor-us", type=float, default=250.0,
                   help="median shift below which a detected step is wander, "
                        "not a grid re-anchor (default: the phase goal)")
    p.add_argument("--min-segment", type=int, default=100,
                   help="shortest slip-free run of VBLs worth fitting")

    add("stability", cmd_stability, "Allan / modified Allan deviation")

    p = add("spectrum", cmd_spectrum, "periodic interference lines")
    p.add_argument("--lines", type=int, default=5)
    p.add_argument("--min-power", type=float, default=0.05)

    p = add("input", cmd_input,
            "input latch phase, pad-read phase, edge mapping")
    p.add_argument("--hold-us", type=int, default=0,
                   help="override the recorded pulse hold when deciding "
                        "whether a lost press was available to be taken; the "
                        "calibration control for that check")
    p.add_argument("--require-pulses", action="store_true",
                   help="refuse unless the timed pulse source was active — "
                        "the check that makes a sweep leg fail when it did "
                        "not inject")

    p = add("attribute", cmd_attribute, "bucket tail events by cause")
    p.add_argument("--threshold-us", type=float, default=1000.0)
    p.add_argument("--show", type=int, default=10)

    p = sub.add_parser("diff", help="A/B two runs with bootstrap CIs")
    p.add_argument("baseline", type=Path)
    p.add_argument("candidate", type=Path)
    p.add_argument("--warmup", type=float, default=5.0)
    p.add_argument("--force", action="store_true",
                   help="compare across mismatched environments anyway")
    p.add_argument("--floor-frac", type=float, default=1.0,
                   help="magnitude floor as a multiple of the baseline's "
                        "robust sigma (default 1.0); a statistically real "
                        "delta below it is not reported as a change")
    p.set_defaults(fn=cmd_diff)

    p = add("export-perfetto", cmd_export_perfetto, "write a Perfetto trace")
    p.add_argument("--out", default="pace-trace.json")

    p = add("report", cmd_report, "write one JSON artifact for this run")
    p.add_argument("--out", default="pace-report.json")

    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
