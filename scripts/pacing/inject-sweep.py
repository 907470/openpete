#!/usr/bin/env python3
"""inject-sweep.py — locate the canonical-present cliff (Phase 3).

`OPENPETE_PACE_INJECT_CANON_US` makes the one present that CANNOT be shed
expensive, which is the fault a fast machine never produces. A first pass at
0 / 5 / 15 / 25 ms only BRACKETED the edge: 5 ms was absorbed entirely and
15 ms put 46 % of ticks past the goal. A decimation trigger written against a
bracket is written against a guess, so this sweeps the interior and reports
where the rate actually crosses.

    inject-sweep.py --doses 0,5,10,12.5,14,15,17,25 --seconds 90

Two failure modes are distinguished, because they have different consequences
for a run's timing:

  - LATE: ticks start after their deadline but the grid is intact. Recoverable,
    and invisible to a savestate's timing.
  - SLIPPING: the pacer clamps and discards wall time. The grid re-anchors.

The canonical present count is reported on every row, and not as decoration:
declining a canonical present IS decimation, so a configuration that improves
its phase numbers while displaying fewer frames has not improved. A row whose
canonical count falls short of its tick count is called out.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pacelog                                                # noqa: E402
import pacestats as ps                                        # noqa: E402
from paceleg import REPO, exe_for, run_leg                    # noqa: E402

TICK_HZ = 1125000 / 37609


def analyse(log_path, warmup, goal_us):
    log = pacelog.read(log_path)
    kept, excluded = log.analysis_vbls(warmup)
    if not kept:
        return None
    s1 = [r.phase_err_ns for r in kept]
    spans = [r for r in log.of(pacelog.PRESENT_SPAN)
             if r.flags == pacelog.CANONICAL]
    pres = log.of(pacelog.PRESENT)
    # Two clocks slip, and only one of them is the grid this plan defends.
    # Reporting a single "slips" column hides the other entirely: at 25 ms the
    # sim grid never clamped once while the RENDER clock clamped 206 times and
    # discarded 3504 VBL of render deadlines.
    slips = [r for r in log.of(pacelog.SLIP) if not r.flags]
    rslips = [r for r in log.of(pacelog.SLIP) if r.flags]
    return {
        "n": len(kept),
        "canon_cost_ns": ps.percentile([r.span_cost_ns for r in spans], 50)
                         if spans else None,
        "late_per_k": 1000.0 * sum(1 for v in s1 if v > goal_us * 1000) / len(s1),
        "p99_ns": ps.percentile(s1, 99),
        "slips": len(slips),
        "slip_vbls": sum(r.a for r in slips),
        "render_slips": len(rslips),
        "render_slip_vbls": sum(r.a for r in rslips),
        "canonical": sum(1 for r in pres if not r.flags),
        "intermediate": sum(1 for r in pres if r.flags),
        "sheds": len(log.of(pacelog.SHED)),
        "env": log.env,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--doses", default="0,5,10,12.5,14,15,17,25",
                    help="injected canonical present cost, in ms")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--platform", choices=["linux", "windows"], default="linux")
    ap.add_argument("--level", default="twilight_harbor",
                    help='level to run in; "" for the attract demo')
    ap.add_argument("--render-fps", default="240")
    ap.add_argument("--goal-us", type=float, default=250.0)
    ap.add_argument("--late-per-1000", type=float, default=15.0,
                    help="the gate's rate; the cliff is located against it")
    ap.add_argument("--warmup", type=float, default=5.0)
    ap.add_argument("--disc", default=str(
        REPO / "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
    ap.add_argument("--out", default=str(REPO / "build/pacing/inject-sweep"))
    a = ap.parse_args()

    doses = [float(x) for x in a.doses.split(",") if x.strip()]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    exe = exe_for(a.platform)

    flags = ["--render-fps", a.render_fps, "--vsync", "1"]
    extra = []
    if a.level:
        # --skip-to-level fast-forwards ~1800 unpaced VBL to get there, and the
        # analyser excludes them, so the leg is given them back on top of its
        # budget or it measures a fraction of what was asked for.
        extra += ["--skip-to-level", a.level]
    seconds = a.seconds + (1800 / (2 * TICK_HZ) if a.level else 0)

    print(f"sweep: {len(doses)} dose(s) x {a.seconds:g}s, {a.platform}, "
          f"{a.level or 'attract demo'}, --render-fps {a.render_fps}")
    rows = []
    for d in doses:
        name = f"canon{d:g}ms".replace(".", "p")
        knobs = {"OPENPETE_PACE_INJECT_CANON_US": str(int(round(d * 1000)))}
        log = run_leg(exe, a.disc, name, flags, seconds, out, False, extra,
                      knobs=knobs if d else None)
        if not log:
            continue
        r = analyse(log, a.warmup, a.goal_us)
        if r:
            r["dose_ms"] = d
            rows.append(r)

    if not rows:
        print("no leg produced a log", file=sys.stderr)
        return 3

    print(f"\n{'inject':>8} {'canon cost':>11} {'late/1000':>10} {'p99':>10} "
          f"{'sim sl':>7} {'rnd sl':>7} {'canon':>7} {'inter':>7} {'sheds':>7}")
    for r in rows:
        cc = f"{r['canon_cost_ns']/1000:.0f}us" if r["canon_cost_ns"] else "n/a"
        print(f"{r['dose_ms']:>7g}m {cc:>11} {r['late_per_k']:>10.1f} "
              f"{r['p99_ns']/1000:>9.0f}u {r['slips']:>7} {r['render_slips']:>7} "
              f"{r['canonical']:>7} {r['intermediate']:>7} {r['sheds']:>7}")

    # Where does the rate cross the gate? Reported as the two doses it lies
    # between, never as a single number: the sweep has no resolution it did
    # not run.
    ok = [r for r in rows if r["late_per_k"] <= a.late_per_1000]
    bad = [r for r in rows if r["late_per_k"] > a.late_per_1000]
    if ok and bad:
        hi = min(r["dose_ms"] for r in bad)
        below = [r["dose_ms"] for r in ok if r["dose_ms"] < hi]
        if below:
            print(f"\nlate-rate cliff: between {max(below):g} ms and {hi:g} ms "
                  f"(gate {a.late_per_1000:g} per 1000)")
        else:
            print(f"\nlate-rate cliff: at or below {hi:g} ms, and the rate is "
                  f"NOT monotonic in the dose — read the table, not a bracket")
    elif bad:
        print(f"\nlate-rate cliff: at or below the lowest dose run "
              f"({min(r['dose_ms'] for r in rows):g} ms) — sweep lower")
    else:
        print(f"\nlate-rate cliff: above the highest dose run "
              f"({max(r['dose_ms'] for r in rows):g} ms) — sweep higher")

    for key, what, why in (
            ("slips", "sim slip onset",
             "past here the SIM grid re-anchors, which is a different failure "
             "from late"),
            ("render_slips", "render slip onset",
             "the sim grid is intact here; render deadlines are being "
             "abandoned")):
        base = rows[0][key]
        hit = [r for r in rows if r[key] > base]
        if hit:
            first = min(r["dose_ms"] for r in hit)
            below = [r["dose_ms"] for r in rows if r["dose_ms"] < first]
            prev = f"{max(below):g} ms" if below else "the lowest dose run"
            print(f"{what}: between {prev} and {first:g} ms — {why}")
        else:
            print(f"{what}: not reached at any dose run")

    # A dose that quietly stops displaying frames would flatter every column
    # above it.
    ticks = a.seconds * TICK_HZ
    for r in rows:
        if r["canonical"] < 0.9 * ticks:
            print(f"note: {r['dose_ms']:g} ms displayed {r['canonical']} "
                  f"canonical presents against ~{ticks:.0f} ticks — the phase "
                  f"numbers on that row are about a run that stopped "
                  f"displaying frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
