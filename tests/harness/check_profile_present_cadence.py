#!/usr/bin/env python3
"""check_profile_present_cadence.py - the profiler's present cadence is the
display's present cadence under eager production.

Class: under eager production the sim worker publishes scenes into the scene
ring and the present thread renders and presents them. The profiler's PRESENT
record must be written at the present, on the present thread, and the sim
worker's publication record is SCENE; profile-summary.py reads its present
cadence (fps, frame_p99_us, [present]) from PRESENT records only. A PRESENT
written at publication reports the publication cadence (bursts ~0.44 ms apart,
then a wait for the next tick: p99 13-20 ms) as the present cadence.

Row s1_h0_r1: Spyro 1 town_square, the high freecam pose of the hold
benchmark, --lod extended, no mods or texture pack, --vsync 0
--render-fps 1000, OPENPETE_MOBY_HOLD_PX=0, --frame-limit 3600, with
--pace-log beside --profile-out.

Instruments, both of the same run:
  * profile-summary.py --skip-ticks 301 --json: [present] interval p99 and
    the count of PRESENT records on the present thread.
  * the pace log's OP_PACE_PRESENT records (core/diag/pace_log.c), written
    on the present thread at the same present: interval p99 over records
    after canonical tick 300. The pace log carries no tick, and its clock
    (op_now) is not the profiler's (the SDL performance counter; on Linux
    the two differ by seconds and drift by tens of ms over a run), so the
    cut is read through a pairing: the trace's markers (SCENE, and PRESENT
    on the sim worker) and the pace log's PRESENT_SPAN records are written
    one each per produced scene or inline present, in the same order. The
    cut is the PRESENT_SPAN paired with the first marker past tick 300.
  * the exit line "eager: N span(s): M scene(s) produced".

Verdict:
  VACUOUS  eager produced 0 scenes, or the pace log holds no PRESENT after
           the cut: the path did not run.
  FAIL     no PRESENT record on the present thread, markers and
           PRESENT_SPAN records that do not pair one to one, or the two
           p99 values more than --tol-us (500) apart.
  PASS     otherwise.

Platforms: --platform windows (default) runs the cross-built exe in
build/windows-mingw on the real GPU and SKIPs (64) without it; the bench
preconditions (scripts/fences/bench_guard.py) refuse (65) a contended
machine. --platform linux runs the linux-release binary on a private Xvfb
display (scripts/lib/xvfb.sh) on lavapipe, and SKIPs without xvfb-run; its contention is reported, not refused, since the verdict
compares two instruments of one run. --analyze OPP PACE LOG judges captured
files instead of running.

Every launch passes --input-lock --vibration 0 --background 1. The run's
outputs are deleted before it and asserted after it.

Usage: tests/harness/check_profile_present_cadence.py [--platform windows|linux]
           [--exe PATH] [--out DIR] [--frames N] [--tol-us N]
       tests/harness/check_profile_present_cadence.py --analyze OPP PACE LOG
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP, 65 refused by a fence.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
sys.path.insert(0, os.path.join(ROOT, "scripts", "fences"))
sys.path.insert(0, os.path.join(ROOT, "scripts", "pacing"))
import bench_guard  # noqa: E402
import game_conf  # noqa: E402
import pacelog  # noqa: E402
from _opp_reader import read_opp, OppTraceError  # noqa: E402
from hostenv import child_env, sw_render_env, xvfb_prefix  # noqa: E402

ROW = "s1_h0_r1"
GAME = "spyro-1"
LEVEL = "town_square"
POSE = "pos=(117320, 105984, 40000) yaw=0 pitch=992"
CUT_TICK = 300
EXIT_PASS, EXIT_FAIL, EXIT_VACUOUS, EXIT_SKIP = 0, 1, 3, 64
EAGER_RE = re.compile(r"eager: \d+ span\(s\): (\d+) scene\(s\) produced")


def pct(sorted_vals, q):
    """The q quantile of sorted_vals by the floor index, profile-summary.py's
    rule, so both sides of the comparison use one estimator."""
    if not sorted_vals:
        return 0
    return sorted_vals[int((len(sorted_vals) - 1) * q)]


def windows_exe():
    """The cross-built exe in build/windows-mingw, or None."""
    tree = os.path.join(ROOT, "build", "windows-mingw")
    for name in ("openpete.exe", os.path.join("games", GAME, game_conf.exe_name(GAME) + ".exe")):
        p = os.path.join(tree, name)
        if os.path.isfile(p):
            return p
    return None


def run_row(platform, exe, out, frames, prefix=()):
    """Launch the row once; return (opp, pace, log) paths or exit."""
    os.makedirs(out, exist_ok=True)
    base = os.path.join(out, "%s_%s" % (ROW, platform))
    opp, pace, log = base + ".opp", base + ".pace", base + ".log"
    for p in (opp, pace, log, opp + ".json"):
        if os.path.exists(p):
            os.remove(p)
    empty = os.path.join(out, "empty")
    os.makedirs(empty, exist_ok=True)
    g = game_conf.resolve(ROOT, GAME)
    if platform == "windows":
        def host(p):
            return subprocess.check_output(["wslpath", "-w", p], text=True).strip()
    else:
        def host(p):
            return p
    argv = [exe, "--disc", host(g["disc"])] + [a for a in g["run_args"] if a != "--no-card"] + [
        "--no-card", "--no-config", "--vibration", "0", "--input-lock", "--background", "1",
        "--mute", "1", "--skip-to-level", LEVEL, "--freecam-pose", POSE,
        "--lod", "extended", "--mods-dir", host(empty), "--texpack-dir", host(empty),
        "--renderer", "vk", "--vsync", "0", "--render-fps", "1000",
        "--frame-limit", str(frames), "--max-runtime-seconds", "240",
        "--profile-out", host(opp), "--pace-log", host(pace)]
    env = child_env({"OPENPETE_VIBRATION": "0", "OPENPETE_MOBY_HOLD_PX": "0"})
    if platform == "linux":
        sw_render_env(env)
    try:
        p = subprocess.run(list(prefix) + argv, cwd=os.path.dirname(exe), env=env, timeout=330,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text, rc = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired as e:
        text, rc = (e.stdout or b"").decode("utf-8", "replace"), "timeout"
    with open(log, "w") as f:
        f.write(" ".join(argv) + "\n" + text)
    missing = [q for q in (opp, pace) if not os.path.exists(q)]
    if missing:
        print("[present-cadence] row=%s FAIL: rc=%s, missing %s (log %s)"
              % (ROW, rc, ", ".join(missing), log))
        sys.exit(EXIT_FAIL)
    return opp, pace, log


def pace_cut_ns(markers, spans):
    """The pace-clock time of the PRESENT_SPAN paired with the first marker
    past CUT_TICK, or None when the two record sets do not pair one to one
    or no marker is past the cut."""
    if not markers or len(markers) != len(spans):
        return None
    k = next((i for i, m in enumerate(markers) if m.canon_tick > CUT_TICK), None)
    return None if k is None else spans[k].t_ns


def analyze(opp, pace, log, tol_us):
    """Judge one capture; print the verdict line and return the exit code."""
    with open(log, errors="replace") as f:
        m = EAGER_RE.search(f.read())
    produced = int(m.group(1)) if m else 0
    summ_json = opp + ".json"
    if os.path.exists(summ_json):
        os.remove(summ_json)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "profile-summary.py"),
                        opp, "--skip-ticks", str(CUT_TICK + 1), "--json", summ_json],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0 or not os.path.exists(summ_json):
        print("[present-cadence] row=%s FAIL: profile-summary.py rc=%d %s"
              % (ROW, r.returncode, r.stderr.strip()))
        return EXIT_FAIL
    with open(summ_json) as f:
        summ = json.load(f)
    pres = summ.get("present", {})
    summary_p99 = pres.get("interval_p99_us", 0)
    pt_records = pres.get("present_thread_records", 0)
    try:
        tr = read_opp(opp)
    except OppTraceError as e:
        print("[present-cadence] row=%s FAIL: %s" % (ROW, e))
        return EXIT_FAIL
    plog = pacelog.read(pace)
    spans = sorted(plog.of(pacelog.PRESENT_SPAN), key=lambda x: x.t_ns)
    cut = pace_cut_ns(tr.markers, spans)
    pt = sorted(x.t_ns for x in plog.of(pacelog.PRESENT) if cut is not None and x.t_ns >= cut)
    iv = sorted(b - a for a, b in zip(pt, pt[1:]) if b > a)
    pace_p99 = pct(iv, 0.99) // 1000
    reason = ""
    if produced == 0:
        verdict, reason = "VACUOUS", "eager produced no scene"
    elif cut is None:
        verdict, reason = "FAIL", ("%d marker(s) against %d PRESENT_SPAN record(s): "
                                   "no pairing" % (len(tr.markers), len(spans)))
    elif not iv:
        verdict, reason = "VACUOUS", "no pace PRESENT after tick %d" % CUT_TICK
    elif pt_records == 0:
        verdict, reason = "FAIL", "no PRESENT record on the present thread (%s)" % pres.get("source")
    elif abs(summary_p99 - pace_p99) > tol_us:
        verdict, reason = "FAIL", "p99 differ by %d us > %d" % (abs(summary_p99 - pace_p99), tol_us)
    else:
        verdict = "PASS"
    print("[present-cadence] row=%s format=%d source=%s eager_produced=%d "
          "present_thread_records=%d scene_records=%d summary_present_p99_us=%d "
          "pace_present_p99_us=%d pace_intervals=%d summary_scene_p99_us=%d "
          "verdict=%s%s opp=%s"
          % (ROW, tr.version, pres.get("source"), produced, pt_records,
             summ.get("scene", {}).get("records", 0), summary_p99, pace_p99, len(iv),
             summ.get("scene", {}).get("interval_p99_us", 0), verdict,
             (" (" + reason + ")") if reason else "", opp))
    return {"PASS": EXIT_PASS, "FAIL": EXIT_FAIL, "VACUOUS": EXIT_VACUOUS}[verdict]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "profile-present-cadence"))
    ap.add_argument("--frames", type=int, default=3600)
    ap.add_argument("--tol-us", type=int, default=500)
    ap.add_argument("--ctest", action="store_true",
                    help="ctest entry: the Windows leg runs only with OPENPETE_WINDOWS_TESTS=1")
    ap.add_argument("--analyze", nargs=3, metavar=("OPP", "PACE", "LOG"))
    a = ap.parse_args()
    if a.analyze:
        return analyze(*a.analyze, a.tol_us)
    if a.platform == "windows":
        if a.ctest and os.environ.get("OPENPETE_WINDOWS_TESTS") != "1":
            print("[present-cadence] SKIP: a ctest run opens a window on the Windows desktop "
                  "only with OPENPETE_WINDOWS_TESTS=1")
            return EXIT_SKIP
        exe = a.exe or windows_exe()
        if not exe or not os.path.isfile(exe):
            print("[present-cadence] SKIP: no Windows exe in build/windows-mingw")
            return EXIT_SKIP
        bench_guard.check(exe)
    else:
        exe = a.exe or game_conf.resolve(ROOT, GAME)["default_exe"]
        if not os.path.isfile(exe):
            print("[present-cadence] SKIP: no binary at %s" % exe)
            return EXIT_SKIP
        prefix, skip = xvfb_prefix("present-cadence", "1280x960")
        if prefix is None:
            print("[present-cadence] " + skip)
            return EXIT_SKIP
        bench_guard.check(exe, functional=True)
    if not os.path.isfile(game_conf.resolve(ROOT, GAME)["disc"]):
        print("[present-cadence] SKIP: no %s disc" % GAME)
        return EXIT_SKIP
    opp, pace, log = run_row(a.platform, exe, a.out, a.frames,
                             prefix if a.platform == "linux" else ())
    return analyze(opp, pace, log, a.tol_us)


if __name__ == "__main__":
    sys.exit(main())
