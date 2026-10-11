#!/usr/bin/env python3
"""
BK2 determinism harness.

Runs games/spyro-1/bk2/frame-time-test.bk2 several times with different
--unlock-frames patterns and diffs the per-yield position logs. All runs
must produce byte-identical position-log output. Any divergence is a
determinism bug in the openpete pacing/yield path.

Usage:
    python3 scripts/bk2_determinism_harness.py [--seed N] [--runs M] [--keep]

Output goes to /tmp/openpete_det/. If anything diverges the script exits
non-zero and prints the first ~10 differing lines.
"""
import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set in the environment
# rather than at each invocation because this script builds its argument list
# in more than one place, and a flag appended to the wrong one is a silent
# miss. (scripts/fences/rumble_guard.py enforces this.)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = Path(__file__).resolve().parent.parent
EXE  = ROOT / "build" / "linux-release" / "openpete"
DISC = ROOT / "games" / "spyro-1" / "spyro-1" / "build" / "SCUS-94228.cue"
BK2  = ROOT / "games" / "spyro-1" / "bk2" / "frame-time-test.bk2"
TOTAL_FRAMES = 3419  # baseline run-length; harness reseeds within this range


def make_pattern(rng, total, density=0.4, max_segments=8):
    """Pick a random set of [lo, hi] frame ranges covering ~density of [0,total]."""
    n = rng.randint(0, max_segments)
    ranges = []
    for _ in range(n):
        lo = rng.randint(0, total - 1)
        span = rng.randint(10, max(11, int(total * density / max(n, 1))))
        hi = min(total - 1, lo + span)
        ranges.append((lo, hi))
    return ranges


def fmt_pattern(ranges):
    return ",".join(f"{lo}-{hi}" for lo, hi in ranges)


def run_once(label, unlock_spec, out_dir):
    pos = out_dir / f"{label}.csv"
    log = out_dir / f"{label}.log"
    cmd = [
        str(EXE),
        "--disc", str(DISC), "--disc-cache", "0",
        "--bk2", str(BK2),
        "--bk2-stop-on-end",
        "--position-log", str(pos),
        "--max-runtime-seconds", "180",
        # The run opens a window: host keyboard, mouse and pads are ignored so
        # the replay is the only input. (scripts/fences/input_lock_guard.py)
        "--input-lock",
    ]
    if unlock_spec:
        cmd.extend(["--unlock-frames", unlock_spec])
    print(f"[run] {label}: unlock={unlock_spec or '(none)'}")
    with open(log, "wb") as lf:
        proc = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, timeout=240)
    if proc.returncode != 0:
        print(f"[run] {label}: FAILED rc={proc.returncode} (see {log})")
        return None
    return pos


def diff_csvs(baseline_path, other_path, label, max_show=10):
    """Return list of (line_no, baseline_line, other_line) for differences."""
    with open(baseline_path) as b, open(other_path) as o:
        b_lines = b.readlines()
        o_lines = o.readlines()
    diffs = []
    n = min(len(b_lines), len(o_lines))
    for i in range(n):
        if b_lines[i] != o_lines[i]:
            diffs.append((i + 1, b_lines[i].rstrip(), o_lines[i].rstrip()))
            if len(diffs) >= max_show:
                break
    if len(b_lines) != len(o_lines):
        diffs.append(("len", f"baseline={len(b_lines)}",
                      f"{label}={len(o_lines)}"))
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--runs", type=int, default=4,
                    help="number of randomised runs in addition to baseline + always-unlock")
    ap.add_argument("--keep", action="store_true",
                    help="keep /tmp/openpete_det artifacts on success")
    args = ap.parse_args()

    out_dir = Path("/tmp/openpete_det")
    out_dir.mkdir(exist_ok=True)
    for f in out_dir.iterdir():
        if f.is_file():
            f.unlink()

    if not EXE.exists():
        print(f"[setup] missing build: {EXE}", file=sys.stderr)
        sys.exit(2)

    rng = random.Random(args.seed)

    # Baseline: no unlocks, paced 60 Hz throughout.
    baseline = run_once("baseline", "", out_dir)
    if baseline is None:
        sys.exit(2)

    # Never-unlocked canary: explicit empty pattern, sanity-checks the
    # --unlock-frames pass-through with zero ranges. Should match
    # baseline trivially (no flag at all).
    far = TOTAL_FRAMES + 1_000_000
    never = run_once("never", f"{far}-{far + 10}", out_dir)

    # Always-unlocked: useful canary - should also match baseline because
    # vbl is yield-driven.
    always = run_once("always", f"0-{TOTAL_FRAMES}", out_dir)

    # Random injection runs.
    randoms = []
    for i in range(args.runs):
        ranges = make_pattern(rng, TOTAL_FRAMES)
        spec = fmt_pattern(ranges)
        path = run_once(f"rand_{i:02d}", spec, out_dir)
        randoms.append((f"rand_{i:02d}", spec, path))

    # Compare.
    failures = []
    canaries = [("never", never), ("always", always)]
    for label, path in canaries + [(l, p) for l, _, p in randoms]:
        if path is None:
            failures.append((label, [("setup", "run failed", "")]))
            continue
        diffs = diff_csvs(baseline, path, label)
        if diffs:
            failures.append((label, diffs))

    if not failures:
        print(f"\n[ok] all {1 + len(randoms)} runs match baseline ({baseline}).")
        if not args.keep:
            for f in out_dir.iterdir():
                if f.is_file():
                    f.unlink()
        sys.exit(0)

    print(f"\n[fail] {len(failures)} runs diverged:")
    for label, diffs in failures:
        print(f"  --- {label} ---")
        for entry in diffs[:10]:
            if isinstance(entry[0], int):
                ln, b, o = entry
                print(f"    line {ln}:")
                print(f"      baseline: {b}")
                print(f"      {label:8s}: {o}")
            else:
                print(f"    {entry[0]}: baseline={entry[1]} other={entry[2]}")
    sys.exit(1)


if __name__ == "__main__":
    main()
