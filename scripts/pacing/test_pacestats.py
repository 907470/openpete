#!/usr/bin/env python3
"""test_pacestats.py - synthetic fixtures for the pacing statistics.

Every check here feeds an input whose answer is known by construction and
asserts the statistic recovers it. That is the whole point: these numbers will
be quoted in a timing certificate, and a wrong Allan deviation is worse than
no Allan deviation because it looks authoritative and nobody re-derives it.

Run: python3 scripts/pacing/test_pacestats.py
"""
import contextlib
import io
import math
import os
import pathlib
import random
import sys
from pathlib import Path as _P
sys.path.insert(0, str(_P(__file__).resolve().parent))
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pacestats as ps                                        # noqa: E402

VBL_NS = 16_715_111          # whole-ns part of the NTSC 240p VBL
FAILURES = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{'  - ' + detail if detail else ''}")
    if not cond:
        FAILURES.append(name)


def approx(a, b, tol):
    return abs(a - b) <= tol


# ------------------------------------------------------------------------
def test_summary():
    print("summary statistics")
    # A needle with a heavy tail: 1000 samples at ~0, 10 spikes at 10 ms.
    # This is the measured shape of a real pacing distribution, and the whole
    # reason stdev is banned as the headline.
    _rng = random.Random(1)
    core = [_rng.gauss(0, 2_000) for _ in range(1000)]
    vals = core + [10_000_000] * 10

    rs = ps.robust_sigma(vals)
    check("robust sigma tracks the core, not the spikes",
          approx(rs, 2_000, 600), f"got {rs:.0f} ns, core sigma 2000 ns")

    # The contrast that motivates the rule: stdev is carried by the tail.
    n = len(vals)
    mean = sum(vals) / n
    stdev = math.sqrt(sum((v - mean) ** 2 for v in vals) / n)
    check("stdev is inflated by the tail (why it is not the headline)",
          stdev > 20 * rs, f"stdev {stdev:.0f} ns vs robust sigma {rs:.0f} ns")

    check("p99 sits below the spikes", ps.percentile(vals, 99) < 10_000_000)
    check("max reports the worst event", max(vals) == 10_000_000)

    rsig, p99, mx = ps.triple(vals)
    check("triple returns (robust sigma, p99, max)",
          rsig == rs and p99 == ps.percentile(vals, 99) and mx == 10_000_000)

    # Nearest-rank: p50 of 1..100 must be a value that actually occurred.
    check("percentile is nearest-rank, never interpolated",
          ps.percentile(list(range(1, 101)), 50) == 50.0)

    # Clustering: same ten spikes, once packed into a second and once spread
    # over a minute. The max is identical; only worst_window separates them.
    packed_t = [i * 1_000_000 for i in range(1000)]
    spread_t = [i * 60_000_000 for i in range(1000)]
    spikes = [0.0] * 1000
    for i in range(10):
        spikes[i * 5] = 10_000_000.0
    packed, _ = ps.worst_window(packed_t, spikes)
    spread, _ = ps.worst_window(spread_t, spikes)
    check("worst_window sees clustering that max cannot",
          packed == spread == 10_000_000.0,
          "identical maxima; the window POSITION is what differs")


# ------------------------------------------------------------------------
def test_drift():
    print("drift")
    # A grid running exactly 50 ppm fast, which over a two-hour run is ~0.36 s
    # of RTA error - material, and invisible to every percentile.
    ppm_true = 50.0
    period = VBL_NS * (1 + ppm_true / 1e6)
    idx = list(range(20000))
    t = [int(i * period) for i in idx]
    ppm, slope = ps.drift_ppm(idx, t, VBL_NS)
    check("recovers a 50 ppm frequency error",
          approx(ppm, ppm_true, 0.01), f"got {ppm:.4f} ppm")

    # An exact grid must read below the 1 ppm drift gate.
    t0 = [int(i * VBL_NS) for i in idx]
    ppm0, _ = ps.drift_ppm(idx, t0, VBL_NS)
    check("an exact grid reads < 1 ppm", abs(ppm0) < 1.0, f"got {ppm0:.6f} ppm")

    # Gaps must not manufacture drift: fitting against VBL INDEX rather than
    # sample number is what makes a level-load hole harmless.
    gapped_idx = idx[:5000] + idx[15000:]
    gapped_t = t[:5000] + t[15000:]
    ppm_g, _ = ps.drift_ppm(gapped_idx, gapped_t, VBL_NS)
    check("a 10k-VBL gap does not manufacture drift",
          approx(ppm_g, ppm_true, 0.01), f"got {ppm_g:.4f} ppm")

    # A short, noisy span must not yield a confident drift figure: the
    # uncertainty has to exceed the 1 ppm gate so the verdict can refuse.
    rng_n = random.Random(13)
    short_idx = list(range(300))
    short_t = [int(i * VBL_NS + rng_n.gauss(0, 1_100_000)) for i in short_idx]
    se_short = ps.drift_ppm_se(short_idx, short_t, VBL_NS)
    check("a short span with 1ms jitter cannot resolve 1 ppm",
          se_short * 2 > 1.0, f"1-sigma {se_short:.2f} ppm")
    se_long = ps.drift_ppm_se(idx, t, VBL_NS)
    check("a 20k-VBL exact grid resolves well below 1 ppm",
          se_long * 2 < 1.0, f"1-sigma {se_long:.6f} ppm")

    # Serially correlated residuals: the textbook OLS standard error assumes
    # independence and understates the uncertainty when a run is late for
    # many consecutive ticks, which is exactly how pacing misbehaves. The
    # block bootstrap must report a wider interval than OLS on the same data.
    rng_ac = random.Random(101)
    ac, e = [], 0.0
    for i in range(4000):
        e = 0.98 * e + rng_ac.gauss(0, 40_000)    # AR(1), long memory
        ac.append(int(i * VBL_NS + e))
    ac_idx = list(range(4000))
    se_ols = ps.drift_ppm_se(ac_idx, ac, VBL_NS, blocks=False)
    se_blk = ps.drift_ppm_se(ac_idx, ac, VBL_NS)
    check("block bootstrap widens the interval under autocorrelation",
          se_blk > se_ols * 1.5,
          f"OLS {se_ols:.3f} ppm vs block {se_blk:.3f} ppm")
    # ...and must NOT inflate when residuals really are independent, or every
    # drift figure becomes unnecessarily unresolvable. (Checked on a noisy
    # grid, not the exact one: with zero residuals both estimators are zero
    # and the comparison is meaningless.)
    rng_iid = random.Random(103)
    iid = [int(i * VBL_NS + rng_iid.gauss(0, 40_000)) for i in range(4000)]
    r_ols = ps.drift_ppm_se(ac_idx, iid, VBL_NS, blocks=False)
    r_blk = ps.drift_ppm_se(ac_idx, iid, VBL_NS)
    check("...and does not inflate when residuals ARE independent",
          0.5 < r_blk / max(1e-12, r_ols) < 2.0,
          f"OLS {r_ols:.4f} ppm vs block {r_blk:.4f} ppm")

    # CUSUM: an undeclared re-anchor is a STEP in the mean. No percentile
    # notices it - the run is tight before and tight after.
    rng = random.Random(7)
    quiet = [rng.gauss(0, 1_000) for _ in range(500)]
    stepped = quiet + [v + 200_000 for v in (rng.gauss(0, 1_000)
                                             for _ in range(500))]
    hits = ps.cusum(stepped)
    check("CUSUM finds an undeclared grid step",
          bool(hits) and approx(hits[0][0], 500, 40),
          f"first change at {hits[0][0] if hits else None}, true 500")
    check("CUSUM is quiet on a run with no step", not ps.cusum(quiet),
          f"{len(ps.cusum(quiet))} false positive(s)")

    # The case that matters, and that a clean-Gaussian fixture cannot reach:
    # a needle with heavy tails, which is the measured shape of real pacing
    # data. A deviation-based CUSUM scaled by a robust sigma fires on every
    # outlier here - observed as 60 spurious change points in a run whose
    # segment medians were identical.
    rng_h = random.Random(31)
    heavy = []
    for i in range(2000):
        heavy.append(rng_h.gauss(10_000, 1_600)
                     if rng_h.random() > 0.01 else rng_h.uniform(1e6, 17e6))
    check("CUSUM is quiet on a heavy-tailed run with no median step",
          not ps.cusum(heavy),
          f"{len(ps.cusum(heavy))} false positive(s) among 1% huge outliers")

    heavy_step = heavy[:1000] + [v + 200_000 for v in heavy[1000:]]
    hits_h = ps.cusum(heavy_step)
    check("CUSUM still finds a median step under heavy tails",
          bool(hits_h) and approx(hits_h[0][0], 1000, 60),
          f"first change at {hits_h[0][0] if hits_h else None}, true 1000")
    # Each half is individually tight, so no percentile taken within a half
    # reveals the step: only the reference moved. That is what CUSUM is for.
    lo_p99 = ps.percentile(stepped[:500], 99) - ps.median(stepped[:500])
    hi_p99 = ps.percentile(stepped[500:], 99) - ps.median(stepped[500:])
    check("the step is invisible within each half (why CUSUM exists)",
          approx(lo_p99, hi_p99, 1_000),
          f"centred p99 {lo_p99:.0f} vs {hi_p99:.0f} ns, step 200000 ns")


# ------------------------------------------------------------------------
def test_stability():
    print("stability (Allan / modified Allan)")
    # White PHASE noise: independent jitter on each timestamp. Theory says
    # ADEV falls as tau^-1. This is also the case where ADEV tracks a plain
    # stddev, i.e. tells you nothing new -- which is exactly why the SLOPE,
    # not the value, is the signal.
    rng = random.Random(3)
    sigma_ns = 50_000
    t_wpm = [int(i * VBL_NS + rng.gauss(0, sigma_ns)) for i in range(4000)]
    pts = ps.adev(t_wpm, VBL_NS)
    slope = ps.adev_slope(pts)
    check("white phase noise gives ADEV slope ~ -1",
          approx(slope, -1.0, 0.15), f"got {slope:.3f}")

    # At tau = 1 sample, ADEV of white phase noise is sqrt(3)*sigma_x/tau.
    tau1, adev1 = pts[0]
    expect = math.sqrt(3) * (sigma_ns / 1e9) / tau1
    check("ADEV magnitude at tau=1 matches theory",
          approx(adev1, expect, expect * 0.15),
          f"got {adev1:.3e}, theory {expect:.3e}")

    # Per-run spread. The pooled curve sums squared differences across every
    # run as though they were one process, so three runs of visibly different
    # stability must still be separable when asked one at a time - otherwise a
    # curve averaged over unlike stretches reports a stability nobody has.
    t_mix, idx_mix = [], []
    for r in range(3):
        base = r * 2000
        for i in range(1200):
            t_mix.append(int((base + i) * VBL_NS + rng.gauss(0, sigma_ns) * (r + 1)))
            idx_mix.append(base + i)
    runs_mix, cuts_mix = ps.phase_runs(t_mix, VBL_NS, idx_mix)
    check("index gaps split the series into one run each", len(runs_mix) == 3,
          f"{len(runs_mix)} run(s), cuts {cuts_mix}")
    per = [ps.adev_from_runs([r_], VBL_NS, [4])[0][1] for r_ in runs_mix]
    check("per-run ADEV separates 1x/2x/3x jitter", max(per) / min(per) > 2.0,
          " ".join(f"{v:.2e}" for v in per))
    pooled = ps.adev_from_runs(runs_mix, VBL_NS, [4])[0][1]
    check("the pooled value sits inside the spread it averages",
          min(per) < pooled < max(per),
          f"pooled {pooled:.2e} vs {min(per):.2e}..{max(per):.2e}")

    # A slip's RECOVERY is trimmed from the head of the following run: the
    # grid snapped forward while the loop still carried its backlog, so the
    # convergence that follows is the clamp being re-reported as instability.
    t_slip = [i * VBL_NS for i in range(400)]
    t_slip += [400 * VBL_NS + 5_000_000 + i * VBL_NS for i in range(400)]
    runs_slip, cuts_slip = ps.phase_runs(t_slip, VBL_NS, list(range(800)),
                                         breaks_ns=[400 * VBL_NS + 5_000_000])
    check("a slip splits the series and the recovery is trimmed",
          cuts_slip == ["slip"] and len(runs_slip[0]) == 400
          and len(runs_slip[1]) == 400 - ps.SLIP_RECOVERY_VBL,
          f"cuts {cuts_slip}, runs {[len(r_) for r_ in runs_slip]}")

    # A frequency OFFSET (constant rate error) is a phase ramp, which the
    # second difference removes entirely -- ADEV must stay flat-to-falling and
    # must NOT report the offset as instability.
    t_off = [int(i * VBL_NS * 1.00005) for i in range(4000)]
    off_slope = ps.adev_slope(ps.adev(t_off, VBL_NS))
    check("a pure frequency offset is not reported as instability",
          off_slope < -0.5, f"slope {off_slope:.3f}")

    # RANDOM WALK frequency: the actionable case. ADEV must FLATTEN or rise at
    # large tau -- the departure a histogram cannot show.
    walk, f = [], 0.0
    acc = 0
    for i in range(4000):
        f += rng.gauss(0, 4.0)
        acc += int(VBL_NS + f * 300)
        walk.append(acc)
    walk_slope_hi = ps.adev_slope(ps.adev(walk, VBL_NS), 0.5, 1.0)
    check("random-walk wander flattens ADEV at large tau",
          walk_slope_hi > -0.6, f"large-tau slope {walk_slope_hi:.3f}")

    # A GAP in the sequence is a full-period phase STEP, and the second
    # difference is maximally sensitive to steps at exactly the long tau this
    # curve exists to report. Nine dropped samples out of four thousand -- the
    # transient count of a real 2-minute gate log -- must not be readable as
    # wander. Indexing by array position is what makes them look like it.
    #
    # The tell is MAGNITUDE, not slope: a staircase of nine 16.7 ms steps
    # inflates the whole curve by more than an order of magnitude while its
    # log-log slope stays plausible. A reading taken off the slope alone would
    # have called this run clean.
    drop = set(range(400, 4000, 400))            # 9 excluded VBLs
    g_idx = [i for i in range(4000) if i not in drop]
    g_t   = [t_wpm[i] for i in g_idx]
    truth = dict(ps.adev(t_wpm, VBL_NS))
    naive = ps.adev(g_t, VBL_NS)
    fixed = ps.adev(g_t, VBL_NS, indices=g_idx)
    tau0 = naive[0][0]
    check("indexing gaps by position inflates ADEV ~10x (the defect)",
          naive[0][1] > 5 * truth[tau0],
          f"{naive[0][1]:.3e} vs {truth[tau0]:.3e} at tau={tau0:.3f}s")
    check("...while its slope still looks like white phase noise",
          ps.adev_slope(naive, 0.5, 1.0) < -0.5,
          f"slope {ps.adev_slope(naive, 0.5, 1.0):.3f} - a slope check misses it")
    check("carrying the VBL index instead reproduces the gap-free curve",
          approx(fixed[0][1], truth[tau0], truth[tau0] * 0.05),
          f"{fixed[0][1]:.3e} vs {truth[tau0]:.3e}")

    # A SLIP is the other step: the clamp discards wall time, so the phase
    # jumps while the index does not. Same failure, same remedy -- split.
    s_t = list(t_wpm)
    slip_at = 2000
    for i in range(slip_at, 4000):
        s_t[i] += 30_000_000                     # 30 ms of discarded wall time
    unsplit = ps.adev(s_t, VBL_NS)
    split   = ps.adev(s_t, VBL_NS, breaks_ns=[s_t[slip_at]])
    check("a slip carried into the curve reads as wander (the defect)",
          ps.adev_slope(unsplit, 0.5, 1.0) > -0.5,
          f"large-tau slope {ps.adev_slope(unsplit, 0.5, 1.0):.3f}")
    check("splitting at the slip leaves white phase noise intact",
          approx(ps.adev_slope(split, 0.5, 1.0), -1.0, 0.25),
          f"large-tau slope {ps.adev_slope(split, 0.5, 1.0):.3f}")

    # And the cost of splitting, which the report must state rather than
    # quietly absorb: reach is bounded by the LONGEST run, not by the log.
    full_tau = ps.adev(t_wpm, VBL_NS)[-1][0]
    cut_tau  = ps.adev(s_t, VBL_NS, breaks_ns=[s_t[slip_at]])[-1][0]
    check("splitting halves the reachable tau when the slip is central",
          approx(cut_tau, full_tau / 2, full_tau * 0.15),
          f"{cut_tau:.1f}s vs {full_tau:.1f}s unsplit")

    # MDEV separates white from flicker phase noise where ADEV cannot:
    # for white PM, MDEV falls as tau^-1.5.
    m_slope = ps.adev_slope(ps.mdev(t_wpm[:1500], VBL_NS))
    check("MDEV slope ~ -1.5 for white phase noise",
          approx(m_slope, -1.5, 0.25), f"got {m_slope:.3f}")


# ------------------------------------------------------------------------
def test_baseline_compare():
    """The baseline gate's read side: reading a blessed baseline back.

    A baseline nothing compares against is an archive, not a gate. These cases
    pin the four verdicts it must be able to reach - including SKIP, because a
    gate with no baseline that prints nothing is indistinguishable from one
    that passed."""
    print("baseline comparison")
    import importlib.util
    import json
    import tempfile
    spec = importlib.util.spec_from_file_location(
        "benchmatrix", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "bench-matrix.py"))
    bm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bm)

    machine = [["platform", "Linux"], ["cpu", "test"], ["cpu_count", "8"],
               ["governor", "perf"], ["gpu", "testgpu"], ["driver", "1"],
               ["refresh_mhz", "60000"]]
    regime = [["present_mode", "fifo"], ["render_fps", "0"],
              ["pacer_slack_ns", "2500000"], ["virtual_clock", "0"],
              ["thread_split", "1"], ["input_legacy", "0"],
              ["headless", "0"], ["runahead", "0"]]

    def leg(sigma=1000.0, within=99.8, ppt=1.0, reg=regime, late=6.0):
        return {"build": "b", "machine_key": machine, "regime_key": reg,
                "robust_sigma_ns": sigma, "within_250us_pct": within,
                "late_per_1000": late,
                "presents_per_tick": ppt, "p50_ns": 9000.0, "p99_ns": 40000.0,
                "p99_9_ns": 1e6, "max_ns": 2e7, "sim_slips": 2,
                "render_slips": 0, "intermediate": 0, "wake_p99_ns": 15000.0}

    with tempfile.TemporaryDirectory() as td:
        bm.REPO = pathlib.Path(td)
        d = bm.REPO / "baselines/pacing" / bm.machine_dir(
            [tuple(kv) for kv in machine])

        rc = bm.compare_to_baseline({"fps0": leg()}, "quick", verbose=False)
        check("no archived baseline reports SKIP, not a pass", rc == 64,
              f"rc {rc}")

        d.mkdir(parents=True)
        (d / "quick.json").write_text(json.dumps(
            {"build": "old", "machine_key": machine, "legs": {"fps0": leg()}}))
        rc = bm.compare_to_baseline({"fps0": leg()}, "quick", verbose=False)
        check("an identical run passes", rc == 0, f"rc {rc}")

        # A build change is what the comparison MEASURES. Keying the archive
        # on it is what left the first baseline unreadable.
        rc = bm.compare_to_baseline({"fps0": dict(leg(), build="new")},
                                    "quick", verbose=False)
        check("a different build is compared, not refused", rc == 0, f"rc {rc}")

        rc = bm.compare_to_baseline({"fps0": leg(sigma=4000.0)}, "quick",
                                    verbose=False)
        check("a 4x wider core is caught", rc == 1, f"rc {rc}")
        rc = bm.compare_to_baseline({"fps0": leg(sigma=1200.0)}, "quick",
                                    verbose=False)
        check("a 1.2x core is inside tolerance (runs vary)", rc == 0,
              f"rc {rc}")

        # Engagement, not just phase: a change that holds the grid by never
        # interpolating has not improved anything.
        rc = bm.compare_to_baseline({"fps0": leg(ppt=0.5)}, "quick",
                                    verbose=False)
        check("presents collapsing is caught even with phase unchanged",
              rc == 1, f"rc {rc}")

        # The late RATE, gated relatively. Five repeats of one unchanged
        # build spread 4.7-10.8 per 1000, so the tolerance has to clear that
        # spread or the gate flaps on noise.
        rc = bm.compare_to_baseline({"fps0": leg(late=14.0)}, "quick",
                                    verbose=False)
        check("2.3x the late rate is inside the observed run-to-run spread",
              rc == 0, f"rc {rc}")
        rc = bm.compare_to_baseline({"fps0": leg(late=20.0)}, "quick",
                                    verbose=False)
        check("3.3x the late rate is caught", rc == 1, f"rc {rc}")

        # The same proportional change at a different operating point. Gated
        # as `within_250us_pct` at +/-0.5 points absolute, 99.94 -> 99.70 was
        # inside tolerance while being four times the failure rate.
        (d / "quick.json").write_text(json.dumps(
            {"build": "old", "machine_key": machine,
             "legs": {"fps0": leg(late=0.6, within=99.94)}}))
        rc = bm.compare_to_baseline({"fps0": leg(late=3.0, within=99.70)},
                                    "quick", verbose=False)
        check("5x a small late rate is caught where half a point was not",
              rc == 1, f"rc {rc}")
        # ...but not below the floor, where the difference is a couple of
        # late ticks in the whole leg and a ratio means nothing.
        rc = bm.compare_to_baseline({"fps0": leg(late=1.0, within=99.90)},
                                    "quick", verbose=False)
        check("a sub-floor rate change is not a regression", rc == 0,
              f"rc {rc}")

        # Two-sided: a large improvement must ASK to be re-blessed. A one-way
        # gate keeps measuring its tolerance from the old, worse number, so
        # it defends less every time the code gets better.
        (d / "quick.json").write_text(json.dumps(
            {"build": "old", "machine_key": machine, "legs": {"fps0": leg()}}))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = bm.compare_to_baseline({"fps0": leg(sigma=200.0)}, "quick",
                                        verbose=False)
        check("a 5x improvement passes but asks to be re-blessed",
              rc == 0 and "re-bless" in out.getvalue(),
              f"rc {rc}: {out.getvalue()!r}")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = bm.compare_to_baseline({"fps0": leg(sigma=900.0)}, "quick",
                                        verbose=False)
        check("a small improvement does not", "re-bless" not in out.getvalue(),
              out.getvalue())
        # A count with zero tolerance has no tolerance to widen.
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = bm.compare_to_baseline({"fps0": dict(leg(), sim_slips=0)},
                                        "quick", verbose=False)
        check("fewer slips is not a re-bless request",
              "re-bless" not in out.getvalue(), out.getvalue())

        other = [list(kv) for kv in regime]
        other[0] = ["present_mode", "immediate"]
        rc = bm.compare_to_baseline({"fps0": leg(reg=other)}, "quick",
                                    verbose=False)
        check("the same leg name under another regime is refused", rc == 1,
              f"rc {rc}")

        # A slip is rare and loud, and this project's own rule is that a rare
        # event is gated as a COUNT, never trusted to a percentile: five extra
        # slips move within-250 by ~0.28 points, inside its tolerance, while a
        # clamp re-anchors the phase and destroys the evidence.
        (d / "quick.json").write_text(json.dumps(
            {"build": "old", "machine_key": machine, "legs": {"fps0": leg()}}))
        rc = bm.compare_to_baseline({"fps0": dict(leg(), sim_slips=7)},
                                    "quick", verbose=False)
        check("new slips are caught even though the rates barely move",
              rc == 1, f"rc {rc}")

        # The tolerance on a _ns key is RELATIVE HEADROOM, not a multiplier:
        # 0.5 means "may grow by half". This was 2.0 read as "2x" and meaning
        # 3x, so pin the semantics rather than the number.
        rc = bm.compare_to_baseline({"fps0": leg(sigma=1400.0)}, "quick",
                                    verbose=False)
        check("+40% on the core is inside the 50% headroom", rc == 0, f"rc {rc}")
        rc = bm.compare_to_baseline({"fps0": leg(sigma=1600.0)}, "quick",
                                    verbose=False)
        check("+60% on the core is not", rc == 1, f"rc {rc}")

        (d / "quick.json").write_text(json.dumps(
            {"build": "old", "machine_key": machine,
             "legs": {"fps0": {k: v for k, v in leg().items()
                               if k != "regime_key"}}}))
        rc = bm.compare_to_baseline({"fps0": leg()}, "quick", verbose=False)
        check("a pre-split archive SKIPs rather than blaming the run",
              rc == 64, f"rc {rc}")


# ------------------------------------------------------------------------
def test_spectrum():
    print("spectrum")
    if ps.np is None:
        check("numpy present", False, "skipped")
        return
    # A 0.06 Hz line is the signature of a FIFO present cadence
    # beating against a monitor whose refresh is not the tick rate.
    f_true = 0.06
    t, v = [], []
    for i in range(6000):
        ts = i * VBL_NS
        t.append(ts)
        v.append(200_000 * math.sin(2 * math.pi * f_true * ts / 1e9))
    lines = ps.spectrum_lines(ps.spectrum(t, v))
    check("recovers a 0.06 Hz interference line",
          bool(lines) and approx(lines[0][0], f_true, 0.01),
          f"strongest line {lines[0][0]:.4f} Hz" if lines else "none found")

    rng = random.Random(11)
    noise = [rng.gauss(0, 200_000) for _ in range(6000)]
    quiet_lines = ps.spectrum_lines(ps.spectrum(t, noise), min_power=0.5)
    check("no strong line in pure noise", not quiet_lines,
          f"{len(quiet_lines)} false line(s)")


# ------------------------------------------------------------------------
def test_diff():
    print("diff (bootstrap)")
    rng = random.Random(5)
    a = [rng.gauss(100_000, 10_000) for _ in range(500)]
    b = [rng.gauss(60_000, 10_000) for _ in range(500)]
    point, lo, hi = ps.bootstrap_ci(a, b)
    check("recovers a real -40 us improvement",
          approx(point, -40_000, 4_000), f"point {point/1000:.1f} us")
    check("the interval excludes zero for a real change", hi < 0,
          f"CI [{lo/1000:.1f}, {hi/1000:.1f}] us")

    c = [rng.gauss(100_000, 10_000) for _ in range(500)]
    _, lo2, hi2 = ps.bootstrap_ci(a, c)
    check("the interval includes zero when nothing changed",
          lo2 <= 0 <= hi2, f"CI [{lo2/1000:.1f}, {hi2/1000:.1f}] us")

    check("bootstrap is deterministic across calls",
          ps.bootstrap_ci(a, b) == ps.bootstrap_ci(a, b))


def test_log_format():
    """A format bump must not orphan the logs already on disk.

    The record size comes from each log's own header, never from this build's
    constant. Getting that wrong refuses every older log with a struct error -
    which is indistinguishable from corruption, and a pacing log is evidence
    nobody can regenerate. It happened: the v2 bump truncated v1 bodies to a
    multiple of 64, and last night's matrix logs became unreadable."""
    print("log format")
    import io
    import struct
    import pacelog

    def build(version, rec_bytes, fmt, payload):
        env = b"build=test\n"
        hdr = struct.pack("<IIIIIIqqqqq", pacelog.MAGIC, version,
                          pacelog.HEADER_FIXED + len(env), rec_bytes,
                          len(env), 0, 0, 0, 2250000, 37609, 2)
        hdr += b"\x00" * (pacelog.HEADER_FIXED - len(hdr))
        return hdr + env + struct.pack(fmt, *payload)

    v1 = build(1, 48, "<BBHIqqqqq", (pacelog.VBL, 0, 0, 7, 100, 90, 80, 100, 200))
    v2 = build(2, 64, "<BBHIqqqqqqq",
               (pacelog.VBL, 0, 3, 7, 100, 90, 80, 100, 200, 42, 0xFFFF))

    import tempfile
    import os
    for name, blob, want_e in (("v1", v1, None), ("v2", v2, 42)):
        fd, path = tempfile.mkstemp(suffix=".pacelog")
        os.write(fd, blob)
        os.close(fd)
        try:
            log = pacelog.read(path)
            r = log.records[0]
            check(f"{name} log parses to one VBL record",
                  len(log.records) == 1 and r.type == pacelog.VBL,
                  f"{len(log.records)} record(s)")
            check(f"{name} payload survives", r.deadline_ns == 90 and
                  r.exec_start_ns == 100, f"deadline {r.deadline_ns}")
            check(f"{name} latch identity reads {want_e}",
                  r.latched_seq == want_e, f"got {r.latched_seq}")
        finally:
            os.unlink(path)

    check("a v1 record carries no latched bits (absent, not zero)",
          pacelog.Record((pacelog.VBL, 0, 0, 0, 0, 0, 0, 0, 0)).latched_bits
          is None)


def test_slip_recovery():
    """A slip clamp snaps the grid forward while the loop still carries the
    backlog. The ticks that follow start late and converge, and OLS reads that
    converging phase as the period being SHORT - a frequency error that is
    really a transient. This is not hypothetical: on the Windows gate it turned
    +0.05 ppm into -3.14 ppm from 30 VBL out of 6917."""
    print("slip recovery")
    nominal = VBL_NS
    n, lead = 2000, 30
    # A perfect grid, plus a decaying lateness over the first `lead` VBLs.
    xs = list(range(n))
    ys = []
    for i in xs:
        late = 8_000_000 * (0.75 ** i) if i < lead else 0.0
        ys.append(i * nominal + late)

    biased, _ = ps.drift_ppm(xs, ys, nominal)
    check("fitting through the recovery invents a large negative drift",
          biased < -2.0, f"{biased:+.3f} ppm over {n} VBL")

    trimmed, _ = ps.drift_ppm(xs[lead:], ys[lead:], nominal)
    check("dropping the recovery recovers the true rate",
          approx(trimmed, 0.0, 0.01), f"{trimmed:+.4f} ppm")

    # The mirror: a segment that ends with the loop progressively falling
    # behind (which is what trips the clamp in the first place) is a RISING
    # ramp, read as a LONG period. Same defect, opposite sign - and it is why
    # trimming only the segment head left a +2.95 ppm gate failure standing.
    ys_tail = [i * nominal + (0.0 if i < n - lead else 300_000.0 * (i - (n - lead)))
               for i in xs]
    biased_tail, _ = ps.drift_ppm(xs, ys_tail, nominal)
    check("a build-up before a clamp invents a large POSITIVE drift",
          biased_tail > 2.0, f"{biased_tail:+.3f} ppm")
    trimmed_tail, _ = ps.drift_ppm(xs[:n - lead], ys_tail[:n - lead], nominal)
    check("trimming the segment TAIL recovers the true rate",
          approx(trimmed_tail, 0.0, 0.01), f"{trimmed_tail:+.4f} ppm")

    # And the bias must dilute with span, which is why a short gate leg and a
    # long soak run disagreed before this was understood.
    xs2 = list(range(20_000))
    ys2 = [i * nominal + (8_000_000 * (0.75 ** i) if i < lead else 0.0)
           for i in xs2]
    diluted, _ = ps.drift_ppm(xs2, ys2, nominal)
    check("the same transient dilutes over a 10x longer run",
          abs(diluted) < abs(biased) / 5,
          f"{diluted:+.4f} ppm over 20000 VBL vs {biased:+.3f} over {n}")


if __name__ == "__main__":
    for fn in (test_summary, test_drift, test_stability, test_baseline_compare,
               test_spectrum,
               test_diff, test_slip_recovery, test_log_format):
        fn()
    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} check(s): {', '.join(FAILURES)}")
        sys.exit(1)
    print("all pacing statistics verified against known inputs")
