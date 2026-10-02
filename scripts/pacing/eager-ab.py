#!/usr/bin/env python3
"""eager-ab - does producing the span ahead of the tick fill the tick hole?

The pull model extracts each intermediate scene on the sim thread at the
moment the pacer decides it is due, so the slots that fall inside the tick's
protective margin are refused and the display holds one image for the length
of the tick (~6.5 ms at 480 Hz, thirty times a second). OPENPETE_EAGER=1 (the default)
extracts every scene of a span while the guest is idle and a present thread
shows them on their own instants, through the next tick's execution.

Reports, per mode, medians over reps: motion presents per second, the gap
the display sees after a tick (p50/p99 and the count of doubled slots per
tick), the animation error at the tick boundary, the sim-side phase, sheds,
and the eager ladder's own census (level, cuts, late picks). Every figure is
read from the child's own summaries or from pace.py cadence over its log -
never from a number this script computes itself.
"""
import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paceleg import REPO, exe_for, run_leg                     # noqa: E402

PACE_RE = re.compile(
    r"pace: (?P<vbl>\d+) VBL .*?; (?P<canon>\d+)\+(?P<inter>\d+)\+(?P<repeat>\d+)"
    r" present; "
    r"phase (?P<phase>[\d.]+)% within 250us, max \+(?P<pmax>[\d.]+) ms.*?"
    r"slips sim (?P<slipsim>\d+)/\d+ render (?P<sliprender>\d+)/\d+; "
    r"sheds (?P<sheds>\d+)")
PT_RE = re.compile(
    r"present-thread: (?P<presented>\d+) scene\(s\) presented, (?P<refused>\d+) refused"
    r".*?(?P<late>\d+) picked late \((?P<late_mean>[\d.]+) ms mean, (?P<late_max>[\d.]+) ms max\)")
EAGER_RE = re.compile(
    r"eager: (?P<spans>\d+) span\(s\): (?P<produced>\d+) scene\(s\) produced, "
    r"(?P<skipped>\d+) skipped.*?(?P<ringfull>\d+) refused.*?(?P<cut>\d+) cut short; "
    r"level now (?:n/a )?(?P<level>\d+) \((?P<down>\d+) down, (?P<up>\d+) up\); "
    r"scene cost (?P<cost>[\d.]+) ms ema.*?lag (?P<lag>[\d.]+) ms \(tick (?P<tick>[\d.]+) ms ema")
CAD_MOTION = re.compile(r"presents: (?P<n>\d+) carrying motion over (?P<s>[\d.]+)s = (?P<rate>\d+)/s")
CAD_GAP = re.compile(r"gap after a tick, DISPLAY\s+n=\s*\d+ p50\s+(?P<p50>[\d.]+) ms.*?p99\s+(?P<p99>[\d.]+) ms\n\s+over 2 slots: \d+ = (?P<dbl>[\d.]+) per tick")
CAD_MID = re.compile(r"gap mid-tick \(intermediate\)\s+n=\s*\d+ p50\s+(?P<p50>[\d.]+) ms.*?p99\s+(?P<p99>[\d.]+) ms\n\s+over 2 slots: \d+ = (?P<dbl>[\d.]+) per tick")
CAD_BERR = re.compile(r"animation error at the tick boundary \(submit-side\) p50 (?P<p50>[+-]?[\d.]+) ms p90 (?P<p90>[+-]?[\d.]+) ms")


def parse(out_text, cad_text):
    m = PACE_RE.search(out_text)
    if not m:
        return None
    d = {k: float(v) for k, v in m.groupdict().items()}
    for rx, prefix in ((PT_RE, "pt_"), (EAGER_RE, "eg_")):
        mm = rx.search(out_text)
        if mm:
            d.update({prefix + k: float(v) for k, v in mm.groupdict().items()})
    for rx, prefix in ((CAD_MOTION, "motion_"), (CAD_GAP, "gap_"),
                       (CAD_MID, "mid_"), (CAD_BERR, "berr_")):
        mm = rx.search(cad_text)
        if mm:
            d.update({prefix + k: float(v) for k, v in mm.groupdict().items()})
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows")
    ap.add_argument("--bk2", default="artisans-bridge")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--render-fps", default="480")
    ap.add_argument("--policy", default="divisor", help="OPENPETE_EAGER_POLICY for the on legs")
    ap.add_argument("--level", default="", help="OPENPETE_EAGER_LEVEL pin for the on legs")
    ap.add_argument("--afford", default="", help="OPENPETE_EAGER_AFFORD pin: force the budget's verdict")
    ap.add_argument("--disc", default=str(
        REPO / "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--modes", default="off,on")
    a = ap.parse_args()

    exe = exe_for(a.platform)
    out = Path(a.out) if a.out else REPO / "build/pacing/eager"
    out.mkdir(parents=True, exist_ok=True)
    bk2 = REPO / f"games/spyro-1/bk2/{a.bk2}.bk2"
    if not bk2.exists():
        sys.exit(f"no bk2 at {bk2}")
    modes = a.modes.split(",")
    flags = ["--bk2", str(bk2), "--renderer", "vk", "--render-fps", a.render_fps]
    rows = {m: [] for m in modes}
    for rep in range(1, a.reps + 1):
        for mode in modes:
            # Eager is the shipped default; the off leg must say so explicitly.
            knobs = {"OPENPETE_EAGER": "0"}
            if mode == "on":
                knobs["OPENPETE_EAGER"] = "1"
                knobs["OPENPETE_EAGER_POLICY"] = a.policy
                if a.level:
                    knobs["OPENPETE_EAGER_LEVEL"] = a.level
                if a.afford:
                    knobs["OPENPETE_EAGER_AFFORD"] = a.afford
            name = f"{a.bk2}-{a.render_fps}-{mode}{rep}"
            # The level load eats ~1700 VBL of fast-forward before a single
            # present goes out; +30 s clears it.
            log = run_leg(exe, a.disc, name, flags, a.seconds + 30, out,
                          headless=False, extra=[], knobs=knobs)
            if not log:
                continue
            text = (out / f"{name}.stdout").read_text()
            cad = subprocess.run([sys.executable, str(REPO / "scripts/pacing/pace.py"),
                                  "cadence", str(log)], capture_output=True, text=True).stdout
            (out / f"{name}.cadence").write_text(cad)
            d = parse(text, cad)
            if d is None:
                print(f"  {name}: no pace summary", file=sys.stderr)
                continue
            total = d["canon"] + d["inter"] + d["repeat"]
            if total < 100:
                print(f"  {name}: VACUOUS ({int(total)} presents)", file=sys.stderr)
                continue
            d["motion"] = d["canon"] + d["inter"]
            rows[mode].append(d)

    def col(mode, key):
        v = [r[key] for r in rows[mode] if key in r]
        return statistics.median(v) if v else float("nan")

    print(f"\n{a.bk2} @ {a.render_fps} fps, {a.reps} rep(s), medians  (on: policy={a.policy}"
          f"{' level=' + a.level if a.level else ''}{' afford=' + a.afford if a.afford else ''})\n")
    print(f"{'':30}" + "".join(f"{m:>12}" for m in modes))
    for label, key in (("motion presents (sum)", "motion"),
                       ("  canonical", "canon"),
                       ("  intermediate", "inter"),
                       ("motion presents /s", "motion_rate"),
                       ("gap after tick p50 ms", "gap_p50"),
                       ("gap after tick p99 ms", "gap_p99"),
                       ("  doubled slots / tick", "gap_dbl"),
                       ("gap mid-tick p50 ms", "mid_p50"),
                       ("gap mid-tick p99 ms", "mid_p99"),
                       ("  doubled slots / tick", "mid_dbl"),
                       ("anim err @boundary p50 ms", "berr_p50"),
                       ("anim err @boundary p90 ms", "berr_p90"),
                       ("phase % <250us", "phase"),
                       ("phase max ms", "pmax"),
                       ("sim slips", "slipsim"),
                       ("sheds", "sheds"),
                       ("eager: level (end)", "eg_level"),
                       ("eager: spans cut short", "eg_cut"),
                       ("eager: scenes skipped late", "eg_skipped"),
                       ("eager: scene cost ms", "eg_cost"),
                       ("eager: lag ms", "eg_lag"),
                       ("eager: tick exec ms ema", "eg_tick"),
                       ("present-thread: late picks", "pt_late"),
                       ("present-thread: late mean ms", "pt_late_mean"),
                       ("present-thread: late max ms", "pt_late_max"),
                       ("present-thread: refused", "pt_refused")):
        print(f"{label:30}" + "".join(f"{col(m, key):12.2f}" for m in modes))
    for m in modes:
        print(f"  {m}: {len(rows[m])} usable rep(s)")


if __name__ == "__main__":
    main()
