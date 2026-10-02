#!/usr/bin/env python3
"""bench-matrix.py — the pacing run matrix and baseline archive.

Runs a fixed set of configurations one at a time, writes a pace log per leg,
and prints them as one table. Legs are NEVER run concurrently: two paced runs
on one machine contend, and contention is the very quantity being measured —
a matrix that parallelises itself measures its own scheduler.

    bench-matrix.py --tier quick                 # 60 s legs, smoke
    bench-matrix.py --tier standard              # 10 min legs, the gate tier
    bench-matrix.py --tier soak --legs default   # hours; drift and thermal
    bench-matrix.py --tier standard --bless      # archive as the baseline

Every run is compared against the archived baseline for the same MACHINE (and
exits non-zero if a gated statistic regressed); --no-compare skips it. A run
with nothing archived says SKIP rather than passing quietly.

Baselines are filed under the MACHINE (CPU, GPU, driver, refresh, governor)
and matched per leg on the REGIME (present mode, render-fps, pacer slack,
thread split, ...). The BUILD is what the comparison measures and is never a
reason to refuse one — keying the archive on it, as this did originally, is
what left it write-only: a baseline that only matches the build that produced
it can never gate a later build.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "fences"))
import wslenv_guard                                            # noqa: E402
import pacelog                                                # noqa: E402
from paceleg import REPO, VBL_HZ, exe_for, run_leg              # noqa: E402
import pacestats as ps                                        # noqa: E402

# The knob axis that drives pacing cost: presents per tick.
# Each leg names what it is FOR, so a table row reads on its own.
LEGS = {
    "fps0":      (["--render-fps", "0"],
                  "legacy pacer, one present per tick — the floor"),
    "fps60":     (["--render-fps", "60", "--vsync", "1"],
                  "~2 presents/tick, FIFO"),
    "fps120":    (["--render-fps", "120", "--vsync", "1"],
                  "~4 presents/tick, FIFO"),
    "fps240":    (["--render-fps", "240", "--vsync", "1"],
                  "~8 presents/tick, FIFO"),
    "default":   ([],
                  "shipped default: --render-fps = display refresh, FIFO"),
    "immediate": (["--vsync", "0"],
                  "display refresh, vsync OFF — isolates FIFO blocking"),
    "fps2000":   (["--render-fps", "2000", "--vsync", "0"],
                  "saturation: as many presents as the GPU will take"),
}

TIER_SECONDS = {"quick": 60, "standard": 600, "soak": 4 * 3600}


def analyse(log_path, warmup):
    log = pacelog.read(log_path)
    kept, excluded = log.analysis_vbls(warmup)
    if not kept:
        return None
    s1 = [r.phase_err_ns for r in kept]
    pres = log.of(pacelog.PRESENT)
    canon = sum(1 for r in pres if not r.flags)
    inter = sum(1 for r in pres if r.flags)
    wakes = log.of(pacelog.WAKE)
    return {
        "env": log.env,
        "build": log.build_id(),
        "machine_key": [list(kv) for kv in log.machine_key()],
        "regime_key": [list(kv) for kv in log.regime_key()],
        "n": len(kept),
        "excluded": excluded,
        "robust_sigma_ns": ps.robust_sigma(s1),
        "p50_ns": ps.percentile(s1, 50),
        "p99_ns": ps.percentile(s1, 99),
        "p99_9_ns": ps.percentile(s1, 99.9),
        "max_ns": max(s1),
        "within_250us_pct": 100.0 * sum(1 for v in s1 if abs(v) < 250_000) / len(s1),
        # Late, not late-or-early: a tick that starts early is not the failure
        # this defends against, and folding the two together lets one mask the
        # other.
        "late_per_1000": 1000.0 * sum(1 for v in s1 if v > 250_000) / len(s1),
        "presents_per_tick": (canon + inter) / max(1, canon),
        "canonical": canon, "intermediate": inter,
        "sim_slips": sum(1 for r in log.of(pacelog.SLIP) if not r.flags),
        "render_slips": sum(1 for r in log.of(pacelog.SLIP) if r.flags),
        "wake_p99_ns": ps.percentile([r.wake_overshoot_ns for r in wakes], 99)
                       if wakes else None,
    }


# What a baseline comparison may GATE on, and what it may only report.
#
# The split is not stylistic. Re-running one unchanged configuration five times
# gave an in-level p99 of 44.7 / 81.9 / 92.6 / 256.3 / 256.4 us — a 5.7x spread
# straddling the goal. A gate on p99 would have flagged an unchanged build as a
# regression more often than not, and a gate that cries wolf gets ignored, which
# is worse than no gate. Rates and the distribution's CORE are what hold still;
# a quantile in a sparse tail and a lone maximum do not.
#
#   (key, label, direction, tolerance)
# direction: "lower" = smaller is better, "higher" = larger is better.
# `tol` for a _ns key is a RELATIVE headroom: current may exceed baseline by
# tol x baseline, so 0.5 means "may grow by half", not "may reach 1.5x" — the
# prose and the arithmetic disagreed here once already (2.0 read as "2x" and
# meant 3x). For every other key it is an ABSOLUTE difference in that key's
# own units.
GATED = [
    # Repeats of one unchanged config held sigma to 0.4 -> 0.4 and 0.5 -> 0.5
    # us, so half again is generous, not tight.
    ("robust_sigma_ns", "robust sigma", "lower", 0.5),
    # The late RATE, gated RELATIVELY. `within_250us_pct` was gated here at
    # +/-0.5 points absolute, which is sensible where a leg sits at 94 % and
    # nearly meaningless where it sits at 99.9 %: the same half-point permits
    # a five-fold worse failure rate. The rate says the same thing in units
    # that scale with the quantity being defended.
    #
    # The tolerance is set by the estimator's own run-to-run spread, not by
    # taste: five repeats of one unchanged build in a level measured 4.7-10.8
    # late per 1000, a 2.3x spread. A gate tighter than the noise flaps, and
    # a flapping gate is one people learn to ignore.
    ("late_per_1000", "late/1000", "lower", 1.5),
    ("presents_per_tick", "presents/tick", "higher", 0.15),
    # Slips are the loud failure and they are RARE, which is exactly the shape
    # this project's own rule says to gate as a count rather than trust to a
    # percentile: five extra slips in a 60 s leg move `within_250us` by ~0.28
    # points, inside its tolerance, while destroying the evidence (a clamp
    # re-anchors the phase). Zero tolerance at the quick tier.
    ("sim_slips", "sim slips", "lower", 0.0),
    ("render_slips", "render slips", "lower", 0.0),
]
# A rate cannot be gated relatively near zero: 0.0 -> 0.4 per 1000 is an
# infinite regression by ratio and nothing at all in fact. Below this floor
# the absolute change is what counts.
#
# The floor is COUNTING NOISE, not taste. A 600 s standard leg holds ~36k VBL
# and a 60 s quick leg ~3.6k, so 1.0 per 1000 is ~4 late ticks in the small
# case: about two sigma on a Poisson count of that size, and the smallest
# difference a leg of this length can distinguish from luck. Set it any higher
# and it stops being a floor and starts being the gate — at 3.0 it swallowed
# 0.6 -> 3.0 per 1000, five times the failure rate, which is precisely the
# case switching away from `within_250us_pct` was meant to catch.
RATE_FLOOR = {"late_per_1000": 1.0}
# How much BETTER a gated statistic may get before the baseline is stale. One
# sided gates widen silently: every real improvement leaves the tolerance
# measured against the old, worse number, so the gate defends less each time
# the code improves and never says so.
REBLESS_FACTOR = 2.0

REPORTED = ["p50_ns", "p99_ns", "p99_9_ns", "max_ns", "within_250us_pct",
            "intermediate", "wake_p99_ns"]


def machine_dir(machine_key):
    """Directory a baseline for this machine is filed under."""
    return "-".join(str(v).replace(" ", "_").replace("/", "_")[:24]
                    for _, v in machine_key)


def gate_delta(key, a_, b_, direction, tol):
    """(worse, better, shown) for one statistic against its baseline.

    Both directions are answered, because a gate that only ever asks "did it
    regress" widens itself every time the code improves: the tolerance stays
    measured from the old, worse number.
    """
    delta = b_ - a_
    # Relative for the ns statistics and the rates, absolute for the
    # percentages and ratios — a 2% tolerance on "99.76% within 250us" would
    # be a tolerance of a quarter of the whole failure budget.
    relative = key.endswith("_ns") or key in RATE_FLOOR
    floor = RATE_FLOOR.get(key, 0.0)
    if relative:
        # A relative band around a near-zero baseline is a band of nothing;
        # the floor is what stops 0.0 -> 0.4 per 1000 reading as a regression
        # of infinite size and nothing at all in fact.
        band = max(tol * max(a_, 0.0), floor)
        # Improvement is a RATIO, not the same band mirrored: a statistic can
        # only ever fall by its own size, so "improved by 1.5x the baseline"
        # is unreachable by construction.
        ratio = 1.0 + REBLESS_FACTOR * tol
        shown = (f"{a_/1000:.1f}us -> {b_/1000:.1f}us" if key.endswith("_ns")
                 else f"{a_:.2f} -> {b_:.2f}")
    else:
        band = tol
        ratio = None
        shown = f"{a_:.2f} -> {b_:.2f}"

    if tol == 0.0:
        # A zero-tolerance count (slips) has no tolerance to widen, so an
        # improvement makes nothing stale — and a run that clamped twice
        # yesterday and not today is noise, not news.
        return (delta > band if direction == "lower" else -delta > band), \
               False, shown

    if direction == "lower":
        worse = delta > band
        better = ((a_ > ratio * max(b_, 1e-9)) if ratio is not None
                  else -delta > REBLESS_FACTOR * band) and (a_ - b_) > floor
    else:
        worse = -delta > band
        better = ((b_ > ratio * max(a_, 1e-9)) if ratio is not None
                  else delta > REBLESS_FACTOR * band) and (b_ - a_) > floor
    return worse, better, shown


def compare_to_baseline(results, tier, verbose=True):
    """Diff this matrix against the archived baseline for the same machine.

    Returns 0 (within tolerance), 1 (a gated statistic regressed), or 64 when
    there is nothing to compare against — SKIP, never a silent pass. A gate
    with no baseline is decoration, and saying so is the difference between
    "verified" and "not checked".
    """
    first = next((r for r in results.values() if r), None)
    if not first:
        return 64
    path = (REPO / "baselines/pacing" /
            machine_dir([tuple(kv) for kv in first["machine_key"]]) /
            f"{tier}.json")
    if not path.exists():
        print(f"\nbaseline: SKIP — none archived for this machine at tier "
              f"{tier} ({path.relative_to(REPO)})")
        print("  bless one with --bless; until then this run is measured "
              "against nothing.")
        return 64

    base = json.loads(path.read_text())
    print(f"\nbaseline: {path.relative_to(REPO)}"
          f"  (build {base.get('build', '?')} -> {first.get('build', '?')})")
    if "legs" not in base:
        print("  REFUSED: archived file has no legs", file=sys.stderr)
        return 1
    # A file written before the machine/regime split records no regime, so
    # every leg would refuse and the run would read as a regression. An
    # unreadable baseline is a SKIP with a reason, not a failure of the code
    # under test: blaming the candidate for the archive's format is exactly
    # the kind of misattributed red that trains people to ignore a gate.
    if any("regime_key" not in leg for leg in base["legs"].values()):
        print("  SKIP — archived in a format predating the machine/regime "
              "split; re-bless it.")
        return 64

    regressed = False
    stale = False
    for name, cur in results.items():
        if not cur:
            continue
        old = base["legs"].get(name)
        if not old:
            print(f"  {name:<11} SKIP — not in the baseline")
            continue
        # The regime IS the leg. If the same leg name was taken under a
        # different configuration, the two rows are not the same measurement
        # however alike their numbers look.
        ok = [tuple(kv) for kv in old.get("regime_key", [])]
        ck = [tuple(kv) for kv in cur.get("regime_key", [])]
        if ok != ck:
            print(f"  {name:<11} REFUSED — regime differs from the baseline's")
            for (k, va), (_, vb) in zip(ok, ck):
                if va != vb:
                    print(f"                {k}: {va!r} vs {vb!r}")
            regressed = True
            continue
        for key, label, direction, tol in GATED:
            a_, b_ = old.get(key), cur.get(key)
            if a_ is None or b_ is None:
                continue
            worse, better, shown = gate_delta(key, a_, b_, direction, tol)
            if worse:
                regressed = True
                print(f"  {name:<11} REGRESSED {label}: {shown}")
            else:
                if better:
                    stale = True
                    print(f"  {name:<11} IMPROVED {label}: {shown} "
                          f"— re-bless, the tolerance is measured from the "
                          f"old number")
                elif verbose:
                    print(f"  {name:<11} ok {label}: {shown}")
        if verbose:
            info = []
            for key in REPORTED:
                a_, b_ = old.get(key), cur.get(key)
                if a_ is None or b_ is None:
                    continue
                if key.endswith("_ns"):
                    info.append(f"{key[:-3]} {a_/1000:.0f}->{b_/1000:.0f}us")
                else:
                    info.append(f"{key} {a_}->{b_}")
            # Reported, not gated: these move several-fold between identical
            # runs, so they inform a reader and must never fail a build.
            print(f"  {name:<11} (ungated: " + ", ".join(info) + ")")
    if stale and not regressed:
        print("\nbaseline: this run is better than the archive by more than "
              f"{REBLESS_FACTOR:g}x the tolerance. Re-bless it (--bless), or "
              "the gate keeps defending the old, worse number.")
    return 1 if regressed else 0


def main():
    # A matrix leg configured through the environment that never receives it
    # is a duplicate of another leg wearing a different name.
    _bad = wslenv_guard.scan(str(REPO))
    if _bad:
        sys.exit("FENCE (wslenv_guard): " +
                 ", ".join(p for p, _ in _bad) +
                 " would drop env knobs crossing to a Windows exe")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tier", choices=sorted(TIER_SECONDS), default="quick")
    ap.add_argument("--legs", nargs="*", default=None,
                    help=f"subset of: {' '.join(LEGS)}")
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows",
                    help="windows = the evidentiary machine (real GPU)")
    ap.add_argument("--headless", action="store_true",
                    help="dev-only: no presents, so most legs are meaningless")
    ap.add_argument("--disc", default=str(
        REPO / "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--bless", action="store_true",
                    help="archive these results as the baseline for this machine")
    ap.add_argument("--no-compare", action="store_true",
                    help="skip the comparison against the archived baseline")
    ap.add_argument("--extra", nargs="*", default=[],
                    help="flags appended to every leg")
    a = ap.parse_args()

    seconds = TIER_SECONDS[a.tier]
    names = a.legs or list(LEGS)
    bad = [n for n in names if n not in LEGS]
    if bad:
        sys.exit(f"unknown leg(s): {', '.join(bad)}")

    out = Path(a.out) if a.out else REPO / f"build/pacing/{a.tier}"
    out.mkdir(parents=True, exist_ok=True)
    exe = exe_for(a.platform)

    print(f"matrix: tier={a.tier} ({seconds}s/leg), {len(names)} leg(s), "
          f"{a.platform}, serialised")
    results = {}
    for name in names:
        flags, why = LEGS[name]
        print(f"{name} — {why}")
        log = run_leg(exe, a.disc, name, flags, seconds, out, a.headless,
                      a.extra)
        if log:
            results[name] = analyse(log, a.warmup) or {}

    if not results:
        print("no leg produced a log", file=sys.stderr)
        return 3

    print(f"\n{'leg':<11} {'pres/tick':>9} {'robust s':>9} {'p50':>9} "
          f"{'p99':>10} {'max':>10} {'<250us':>7} {'slips':>7}")
    for name, r in results.items():
        if not r:
            continue
        print(f"{name:<11} {r['presents_per_tick']:>9.1f} "
              f"{r['robust_sigma_ns']/1000:>8.1f}u {r['p50_ns']/1000:>8.1f}u "
              f"{r['p99_ns']/1000:>9.1f}u {r['max_ns']/1e6:>9.2f}m "
              f"{r['within_250us_pct']:>6.1f}% "
              f"{r['sim_slips']:>3}/{r['render_slips']:<3}")

    first = next(iter(results.values()))
    stamp = {"tier": a.tier, "seconds_per_leg": seconds,
             "platform": a.platform, "headless": a.headless,
             "build": first.get("build"),
             "machine_key": first.get("machine_key"), "legs": results}
    (out / "matrix.json").write_text(json.dumps(stamp, indent=2))
    print(f"\nwrote {out / 'matrix.json'}")

    rc = 0
    if not a.no_compare:
        rc = compare_to_baseline(results, a.tier)
        if rc == 64:
            rc = 0          # nothing archived is not a failure, only a SKIP

    if a.bless:
        dest = (REPO / "baselines/pacing" /
                machine_dir([tuple(kv) for kv in first["machine_key"]]))
        dest.mkdir(parents=True, exist_ok=True)
        (dest / f"{a.tier}.json").write_text(json.dumps(stamp, indent=2))
        print(f"blessed baseline: {dest / (a.tier + '.json')}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
