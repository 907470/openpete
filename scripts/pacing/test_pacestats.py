#!/usr/bin/env python3
"""test_pacestats.py — synthetic fixtures for the pacing statistics.

Every check here feeds an input whose answer is known by construction and
asserts the statistic recovers it. That is the whole point: these numbers will
be quoted in a timing certificate, and a wrong Allan deviation is worse than
no Allan deviation because it looks authoritative and nobody re-derives it.

Run: python3 scripts/pacing/test_pacestats.py
"""
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pacestats as ps                                        # noqa: E402

VBL_NS = 16_715_111          # whole-ns part of the NTSC 240p VBL
FAILURES = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
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
    # of RTA error — material, and invisible to every percentile.
    ppm_true = 50.0
    period = VBL_NS * (1 + ppm_true / 1e6)
    idx = list(range(20000))
    t = [int(i * period) for i in idx]
    ppm, slope = ps.drift_ppm(idx, t, VBL_NS)
    check("recovers a 50 ppm frequency error",
          approx(ppm, ppm_true, 0.01), f"got {ppm:.4f} ppm")

    # An exact grid must read below the plan's 1 ppm gate.
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
    # notices it — the run is tight before and tight after.
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
    # outlier here — observed as 60 spurious change points in a run whose
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

    # MDEV separates white from flicker phase noise where ADEV cannot:
    # for white PM, MDEV falls as tau^-1.5.
    m_slope = ps.adev_slope(ps.mdev(t_wpm[:1500], VBL_NS))
    check("MDEV slope ~ -1.5 for white phase noise",
          approx(m_slope, -1.5, 0.25), f"got {m_slope:.3f}")


# ------------------------------------------------------------------------
def test_spectrum():
    print("spectrum")
    if ps.np is None:
        check("numpy present", False, "skipped")
        return
    # A 0.06 Hz line is the signature the plan names: a FIFO present cadence
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


if __name__ == "__main__":
    for fn in (test_summary, test_drift, test_stability, test_spectrum,
               test_diff):
        fn()
    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} check(s): {', '.join(FAILURES)}")
        sys.exit(1)
    print("all pacing statistics verified against known inputs")
