#!/usr/bin/env python3
"""Gameplay-state regression gate for the sub-tick present path.

WHY THIS EXISTS, when the 20k oracle already guards gameplay state: the
oracle replays canonical ticks against real-hardware traces. Intermediate
(sub-tick) presents never run under it. But the sub-tick path historically
publishes interpolated state by WRITING gameplay-readable PSX RAM and
restoring the canonical bytes afterwards -- so a missed or mis-ordered
restore perturbs gameplay silently, and the project's strongest gate is
structurally unable to see it. This closes that hole.

Two independent checks, and they fail for different reasons:

  INVARIANT (mode `ab`) -- the same binary, same movie, run twice: once with
  intermediate re-extracts armed and once without. The gameplay-state hash
  logs must be IDENTICAL. This tests the actual contract -- the display path
  must not affect gameplay state -- needs no stored file, and cannot be
  fooled by a baseline that already contains a leak. This is the check that
  catches a broken bracket today.

  REGRESSION (mode `baseline`) -- the canonical run against a blessed hash
  log committed under tests/state-baselines/. This catches drift against a
  previous version of ourselves, including in the canonical path. It is a
  regression detector, NOT a correctness oracle: it locks in current
  behaviour, bugs included. Only the oracle says a state is *right*.

ENGAGEMENT: the armed leg must report a non-zero sub-tick present count or
the run is VACUOUS and the harness fails rather than passing. An empty view
resolves every address to real memory, so "the hashes matched" is the
expected output of a run that never exercised the path at all. Note the
live sub-tick present path is headless-blocked -- only the explicit
intermediate capture reaches the re-extract headless, which is why the
armed leg drives --screenshot-intermediate-on-frame rather than a high
--render-fps.

PROVEN SENSITIVITY (fault injection, moby-crossfade, 2026-08-26). An
instrument that has only ever passed is not yet an instrument, so both
checks were run against a deliberately broken restore:

  fault                     hash-log diff        bracket check
  skip ONE moby restore     NOT DETECTED         DETECTED, 17 presents,
                                                 names moby@800e5a0c
  skip EVERY moby restore   detected, row 1605   DETECTED

The hash-log comparison alone is therefore a WEAK detector of this defect
class and must not be quoted as the guarantee: moby and particle records are
rewritten by the game every tick, so a leaked write is usually transient and
only surfaces if it feeds something persistent. The bracket check is what
makes a pass here mean something, which is why the armed leg always sets it.

What a PASS does and does not claim: that on these movies, at these sampled
frames, no sub-tick present left a write in a bracketed region, and the
canonical gameplay-state hashes are unchanged. It says nothing about frames
that were not sampled, regions no producer brackets, or whether the state is
CORRECT -- only the oracle speaks to that.

Kept deliberately small by default so it can run inside an iteration loop;
--corpus full is the arc-end sweep.
"""
import argparse
import gzip
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set in the environment
# rather than at each invocation because this script builds its argument list
# in more than one place, and a flag appended to the wrong one is a silent
# miss. (scripts/fences/rumble_guard.py enforces this.)
os.environ.setdefault("OPENPETE_VIBRATION", "0")

ROOT = Path(__file__).resolve().parent.parent
# OPENPETE_BIN/OPENPETE_DISC let a caller point at a build tree other than
# linux-release; CTest passes the configured binary this way.
EXE = Path(os.environ.get(
    "OPENPETE_BIN",
    ROOT / "build" / "linux-release" / "games" / "spyro-1" / "openpete-spyro1"))
DISC = Path(os.environ.get(
    "OPENPETE_DISC",
    ROOT / "games" / "spyro-1" / "spyro-1" / "build" / "SCUS-94228.cue"))
BK2DIR = ROOT / "games" / "spyro-1" / "bk2"
BASEDIR = ROOT / "tests" / "state-baselines"

# Chosen for what they put in the BRACKETED regions, not for scene variety:
# the two pools (up to 1024 moby records, 512 particle slots) are the largest
# and most dynamic bracket targets, and camera/Spyro/flame/shadow are written
# on any gameplay frame. Terrain-heavy movies add little here.
CORPUS = {
    # Fast enough to sit inside an iteration loop.
    "quick": ["moby-crossfade", "dream-weavers-particle"],
    # Adds the distant-terrain/vista movies. Both carry a --skip-to-level
    # sidecar (auto-spliced by --bk2) and are 1.7-2.7x longer, so this tier
    # costs several times `quick` -- it is the arc-end sweep, not the loop.
    "full": ["moby-crossfade", "dream-weavers-particle",
             "artisans-lp-waterfall", "inventory-moby-swim",
             "stone-hill-distant-terrain", "cliff-town-vista"],
}

# Safety cap only. Runs end on --bk2-stop-on-end; this exists so a movie that
# never signals end cannot hang the harness. A hand-tuned per-movie limit was
# tried and removed: every movie in this corpus carries a --skip-to-level
# sidecar that fast-forwards ~1200 frames before gameplay begins, so a limit
# guessed from the bk2's input-row count truncated the movies mid-skip and
# still reported PASS -- a shorter DIFFERENT test wearing the same name.
FRAME_CAP = 20000

# Sub-tick captures are capped at OPENPETE_MAX_SCREENSHOT_FRAMES (64). A leak
# perturbs gameplay state, which then diverges for the REST of the run, so a
# sampled grid detects it -- the hash log records every yield regardless.
MAX_SAMPLES = 64


def sample_grid(frames, n=MAX_SAMPLES, head=0):
    """Frames to arm, spread over the movie's PLAYING window.

    `head` is where gameplay actually starts, measured from the canonical
    leg rather than assumed: a --skip-to-level movie fast-forwards for
    ~1200 frames plus a level load, and captures armed inside that window
    sample a game that is not yet running the scene under test. Clamped to
    half the movie so a mis-parse degrades coverage instead of erasing it."""
    lo = min(max(int(frames * 0.15), head), max(1, frames // 2))
    hi = max(lo + 1, frames - 4)
    step = max(1, (hi - lo) // n)
    return list(range(lo, hi, step))[:n]


def run_leg(bk2, hashlog, frames, armed, shotdir, verbose, head=0):
    env_extra = {}
    cmd = [
        str(EXE), "--headless", "--no-config", "--disc", str(DISC),
 "--renderer", "vk", "--mute", "1",
        "--bk2", str(BK2DIR / f"{bk2}.bk2"),
        "--gameplay-hash-log", str(hashlog),
        # Stop when the movie ends rather than at a guessed frame count; the
        # limit below is only a hang guard.
        "--bk2-stop-on-end",
        "--frame-limit", str(frames), "--max-runtime-seconds", "280",
    ]
    if armed:
        grid = ",".join(str(f) for f in sample_grid(frames, head=head))
        # Per-present restore check: hashes the bracketed regions before any
        # lerp write and re-checks after every restore, so a leak is caught in
        # the present that caused it. The hash-log comparison alone is a
        # PROBABILISTIC and LATE detector -- moby/particle records are
        # rewritten by the game each tick, so a leak is usually transient and
        # only surfaces if it feeds something persistent (measured: skipping
        # every moby restore took 1605 frames to diverge; skipping ONE was
        # never detected at all). This check has neither weakness.
        env_extra["OPENPETE_BRACKET_CHECK"] = "1"
        # --screenshot-intermediate-on-frame implies --screenshot-native-only,
        # which matters: forcing the PsyCross OT path for a whole run is what
        # made an earlier sim-slip "reproduction" measure the harness instead
        # of the game.
        cmd += ["--screenshot-intermediate-on-frame", grid,
                "--screenshot-output", str(shotdir / bk2)]
    env = dict(os.environ)
    env.update(env_extra)
    icd = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if Path(icd).exists():
        env.setdefault("VK_ICD_FILENAMES", icd)
    env.setdefault("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe")
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
    if verbose and p.returncode != 0:
        sys.stderr.write(p.stderr[-2000:])
    presents = 0
    m = re.search(r"subtick-view: presents=(\d+)", p.stderr)
    if m:
        presents = int(m.group(1))
    leaks = re.findall(r"\[bracket-check [^\]]*\] RESTORE MISMATCH ([^\n]*)", p.stderr)
    # Where the level actually starts, for the armed leg's grid.
    skip = re.search(r"\[skip-to-level\][^\n]*at frame=(\d+)", p.stderr)
    skip_at = int(skip.group(1)) if skip else 0
    return p.returncode, presents, leaks, skip_at


def read_log(path):
    """Read a hash log, gzipped or not. Baselines are stored gzipped: they are
    ~1 MB of hex per movie uncompressed and the corpus is meant to grow."""
    if not path.exists():
        return None
    if path.suffix == ".gz":
        text = gzip.decompress(path.read_bytes()).decode()
    else:
        text = path.read_text()
    return [l for l in text.splitlines() if l and not l.startswith("#")]


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        i = min(len(a), len(b))
        return i, (a[i] if i < len(a) else "<end>"), (b[i] if i < len(b) else "<end>")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", choices=sorted(CORPUS), default="quick")
    ap.add_argument("--only", help="comma-separated bk2 names, overrides --corpus")
    ap.add_argument("--frames", type=int, default=0,
                    help="override the per-movie frame limit (default: FRAMES table)")
    ap.add_argument("--mode", choices=["both", "ab", "baseline"], default="both")
    ap.add_argument("--bless", action="store_true",
                    help="write the canonical run as the new baseline (deliberate act: commit it)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not EXE.exists():
        print(f"[subtick-state] SKIP: no binary at {EXE}")
        return 64
    if not DISC.exists():
        print(f"[subtick-state] SKIP: no disc at {DISC}")
        return 64

    movies = args.only.split(",") if args.only else CORPUS[args.corpus]
    BASEDIR.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix="subtick_state_"))
    failures, vacuous, blessed = [], [], []

    try:
        for bk2 in movies:
            if not (BK2DIR / f"{bk2}.bk2").exists():
                failures.append(f"{bk2}: no such bk2")
                continue

            frames = args.frames or FRAME_CAP
            plain, armed_log = tmp / f"{bk2}.plain.csv", tmp / f"{bk2}.armed.csv"
            rc_p, pres_p, _, skip_at = run_leg(bk2, plain, frames, False, tmp,
                                               args.verbose)
            if rc_p != 0:
                failures.append(f"{bk2}: canonical leg exited {rc_p}")
                continue
            log_p = read_log(plain)
            if not log_p:
                failures.append(f"{bk2}: canonical leg wrote no hash rows")
                continue
            # True movie length, observed. The armed leg replays the same movie,
            # so its grid is built from this rather than from a guess. The +600
            # covers the level load that follows the skip's transition.
            real_frames = len(log_p)
            head = skip_at + 600 if skip_at else 0

            if args.mode in ("both", "ab"):
                rc_a, pres_a, leaks, _ = run_leg(bk2, armed_log, real_frames, True, tmp,
                                                 args.verbose, head=head)
                if rc_a != 0:
                    failures.append(f"{bk2}: armed leg exited {rc_a}")
                elif leaks:
                    uniq = sorted(set(leaks))
                    failures.append(
                        f"{bk2}: RESTORE LEAK -- {len(leaks)} present(s) left the sub-tick "
                        f"write in gameplay RAM; first {min(3, len(uniq))} distinct:\n      "
                        + "\n      ".join(uniq[:3]))
                elif pres_a == 0:
                    # A green must prove it engaged.
                    vacuous.append(f"{bk2}: armed leg ran ZERO sub-tick presents "
                                   f"-- the path under test never executed")
                else:
                    log_a = read_log(armed_log)
                    d = first_diff(log_p, log_a) if log_a else (0, "<none>", "<none>")
                    if d:
                        i, x, y = d
                        failures.append(
                            f"{bk2}: INVARIANT BROKEN -- sub-tick presents changed "
                            f"gameplay state at row {i}\n"
                            f"      off:   {x}\n      armed: {y}")
                    else:
                        print(f"[subtick-state] {bk2}: invariant OK "
                              f"({len(log_p)} rows, {pres_a} sub-tick presents)")

            if args.mode in ("both", "baseline"):
                base = BASEDIR / f"{bk2}.csv.gz"
                if args.bless:
                    base.write_bytes(gzip.compress(plain.read_bytes(), 9))
                    blessed.append(f"{bk2} ({len(log_p)} rows)")
                elif not base.exists():
                    failures.append(f"{bk2}: no baseline at {base.relative_to(ROOT)} "
                                    f"-- run with --bless and commit it")
                else:
                    log_b = read_log(base)
                    d = first_diff(log_b, log_p)
                    if d:
                        i, x, y = d
                        failures.append(
                            f"{bk2}: REGRESSION vs baseline at row {i}\n"
                            f"      baseline: {x}\n      current:  {y}")
                    else:
                        print(f"[subtick-state] {bk2}: baseline OK ({len(log_b)} rows)")
    finally:
        if not args.verbose:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"[subtick-state] artifacts kept in {tmp}")

    if blessed:
        print("\n[subtick-state] BLESSED (commit these):")
        for b in blessed:
            print(f"  {b}")
        return 0
    if vacuous:
        print("\n[subtick-state] VACUOUS -- not a pass:")
        for v in vacuous:
            print(f"  {v}")
        return 3
    if failures:
        print("\n[subtick-state] FAIL:")
        for f in failures:
            print(f"  {f}")
        return 1
    print(f"\n[subtick-state] PASS ({len(movies)} movie(s), mode={args.mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
