#!/usr/bin/env python3
"""bench-matrix.py — the pacing run matrix (M4) and baseline archive (M9).

Runs a fixed set of configurations one at a time, writes a pace log per leg,
and prints them as one table. Legs are NEVER run concurrently: two paced runs
on one machine contend, and contention is the very quantity being measured —
a matrix that parallelises itself measures its own scheduler.

    bench-matrix.py --tier quick                 # 60 s legs, smoke
    bench-matrix.py --tier standard              # 10 min legs, the gate tier
    bench-matrix.py --tier soak --legs default   # hours; drift and thermal
    bench-matrix.py --tier standard --bless      # archive as the baseline

Baselines are keyed on the environment (build, CPU, GPU, driver, refresh,
present mode, governor). `diff` refuses to compare across a mismatched key,
so the archive is per-machine by construction rather than by discipline.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from hostenv import child_env                                  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "fences"))
import wslenv_guard                                            # noqa: E402
import pacelog                                                # noqa: E402
import pacestats as ps                                        # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VBL_HZ = 2250000 / 37609

# The knob axis the 0b findings identified as the driver: presents per tick.
# Each leg names what it is FOR, so a table row can be read without the plan.
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


def exe_for(platform):
    if platform == "windows":
        p = REPO / "build/windows-mingw/games/spyro-1/openpete-spyro1.exe"
    else:
        p = REPO / "build/linux-release/games/spyro-1/openpete-spyro1"
    if not p.exists():
        sys.exit(f"no binary at {p} — build the preset first")
    return p


def run_leg(exe, disc, name, flags, seconds, out_dir, headless, extra):
    log = out_dir / f"{name}.pacelog"
    # Every harness destroys its own history before it runs: a stale log that
    # survives a failed leg reads as a successful one.
    log.unlink(missing_ok=True)
    vbls = int(seconds * VBL_HZ)
    cmd = [str(exe), "--disc", str(disc), "--frame-limit", str(vbls),
           "--pace-log", str(log)] + flags + extra
    knobs = {}
    if headless:
        cmd.append("--headless")
        knobs["OPENPETE_FORCE_PACING"] = "1"   # headless is unpaced otherwise
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True,
                       env=child_env(knobs),
                       timeout=seconds * 4 + 300)
    if r.returncode != 0 or not log.exists():
        tail = (r.stderr or r.stdout or "")[-400:]
        print(f"  {name}: FAILED (rc={r.returncode}) {tail}", file=sys.stderr)
        return None
    print(f"  {name}: {time.time()-t0:.0f}s wall, {log.stat().st_size//1024} KiB")
    return log


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
        "env_key": [list(kv) for kv in log.env_key()],
        "n": len(kept),
        "excluded": excluded,
        "robust_sigma_ns": ps.robust_sigma(s1),
        "p50_ns": ps.percentile(s1, 50),
        "p99_ns": ps.percentile(s1, 99),
        "p99_9_ns": ps.percentile(s1, 99.9),
        "max_ns": max(s1),
        "within_250us_pct": 100.0 * sum(1 for v in s1 if abs(v) < 250_000) / len(s1),
        "presents_per_tick": (canon + inter) / max(1, canon),
        "canonical": canon, "intermediate": inter,
        "sim_slips": sum(1 for r in log.of(pacelog.SLIP) if not r.flags),
        "render_slips": sum(1 for r in log.of(pacelog.SLIP) if r.flags),
        "wake_p99_ns": ps.percentile([r.wake_overshoot_ns for r in wakes], 99)
                       if wakes else None,
    }


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
                    help="archive these results as the baseline for this env")
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
             "env_key": first.get("env_key"), "legs": results}
    (out / "matrix.json").write_text(json.dumps(stamp, indent=2))
    print(f"\nwrote {out / 'matrix.json'}")

    if a.bless:
        key = "-".join(str(v).replace(" ", "_").replace("/", "_")[:24]
                       for _, v in (first.get("env_key") or []))
        dest = REPO / "baselines/pacing" / key
        dest.mkdir(parents=True, exist_ok=True)
        (dest / f"{a.tier}.json").write_text(json.dumps(stamp, indent=2))
        print(f"blessed baseline: {dest / (a.tier + '.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
