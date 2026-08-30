#!/usr/bin/env python3
"""present-thread-ab — does presenting during the tick recover the refused slots?

At 480 Hz the sub-tick grid loses ~0.8 slots per tick: the slot inside the
tick's protective margin is refused because presenting and executing the guest
share a thread. OPENPETE_PRESENT_THREAD=1 takes those slots on a second
thread, re-presenting the last composed frame.

The rung it tests is a MECHANISM, not a look: a re-presented frame is a
duplicate, and under FIFO the display was already showing those pixels through
the hold. So this reports present rate, the sim-side phase it cost, and what
the sim spent waiting at the close of each tick window -- never a smoothness
claim.
"""
import argparse
import re
import statistics
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
    r"present-thread: (?P<presents>\d+) re-present\(s\) over (?P<windows>\d+) "
    r"tick window\(s\), (?P<refused>\d+) refused; sim waited at close "
    r"(?P<joins>\d+) time\(s\), (?P<join_ms>[\d.]+) ms total, "
    r"(?P<join_max>[\d.]+) ms max")


def parse(out_text):
    m = PACE_RE.search(out_text)
    if not m:
        return None
    d = {k: float(v) for k, v in m.groupdict().items()}
    p = PT_RE.search(out_text)
    d["pt_presents"] = float(p.group("presents")) if p else 0.0
    d["pt_refused"] = float(p.group("refused")) if p else 0.0
    d["pt_joins"] = float(p.group("joins")) if p else 0.0
    d["pt_join_ms"] = float(p.group("join_ms")) if p else 0.0
    d["pt_join_max"] = float(p.group("join_max")) if p else 0.0
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows")
    ap.add_argument("--bk2", default="artisans-bridge")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--render-fps", default="480")
    ap.add_argument("--disc", default=str(
        REPO / "games/spyro-1/spyro-1/build/SCUS-94228.cue"))
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    exe = exe_for(a.platform)
    out = Path(a.out) if a.out else REPO / "build/pacing/present-thread"
    out.mkdir(parents=True, exist_ok=True)
    bk2 = REPO / f"games/spyro-1/bk2/{a.bk2}.bk2"
    if not bk2.exists():
        sys.exit(f"no bk2 at {bk2}")

    # The level load eats ~1700 VBL of fast-forward before a single present
    # goes out; a leg that does not clear it measures nothing and says so in
    # exactly the same shape as a clean result.
    flags = ["--bk2", str(bk2), "--renderer", "vk",
             "--render-fps", a.render_fps]
    rows = {"off": [], "on": []}
    for rep in range(1, a.reps + 1):
        for mode in ("off", "on"):
            knobs = {"OPENPETE_PRESENT_THREAD": "1"} if mode == "on" else {}
            name = f"{a.bk2}-{mode}{rep}"
            log = run_leg(exe, a.disc, name, flags, a.seconds + 30, out,
                          headless=False, extra=[], knobs=knobs)
            text = (out / f"{name}.stdout").read_text() if (out / f"{name}.stdout").exists() else ""
            if not text:
                continue
            d = parse(text)
            if d is None:
                print(f"  {name}: no pace summary", file=sys.stderr)
                continue
            # Motion and display are different rates; summing them would be the
            # flattering number. `total` is only the vacuity guard.
            total = d["canon"] + d["inter"] + d["repeat"]
            if total < 100:
                print(f"  {name}: VACUOUS ({int(total)} presents)", file=sys.stderr)
                continue
            d["total"] = total
            d["motion"] = d["canon"] + d["inter"]
            rows[mode].append(d)

    def col(mode, key):
        v = [r[key] for r in rows[mode]]
        return statistics.median(v) if v else float("nan")

    print(f"\n{a.bk2} @ {a.render_fps} fps, {a.reps} rep(s), medians\n")
    print(f"{'':22} {'off':>12} {'on':>12}")
    for label, key in (("motion presents", "motion"),
                       ("  canonical", "canon"),
                       ("  intermediate", "inter"),
                       ("repeats (duplicates)", "repeat"),
                       ("reaching display", "total"),
                       ("phase % <250us", "phase"),
                       ("phase max ms", "pmax"),
                       ("sim slips", "slipsim"),
                       ("sheds", "sheds"),
                       ("join waits", "pt_joins"),
                       ("join total ms", "pt_join_ms"),
                       ("join max ms", "pt_join_max")):
        print(f"{label:22} {col('off', key):12.2f} {col('on', key):12.2f}")


if __name__ == "__main__":
    main()
