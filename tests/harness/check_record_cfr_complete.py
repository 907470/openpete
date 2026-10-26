#!/usr/bin/env python3
"""check_record_cfr_complete.py - a CFR recording encodes every present the
run composed, however far the GPU falls behind the CPU.

Class: under --record with --record-cfr the offline capture clock advances
one render period per present the pacer owes. A present the renderer refuses
at begin-frame ("never began" in the present census) is never read back, and
the clip loses that instant. The windowed inline present polls the slot's
in-flight fence; a GPU slower than the CPU's present loop (the NVIDIA
Windows driver returns from vkQueuePresentKHR before the GPU finishes) sheds
the present at that poll unless the recorder's capture mode makes the begin
wait. lavapipe's X11 present blocks until the image is rendered, so the
Linux leg cannot shed and is a no-regression smoke only.

Row: Spyro 3 sunrise_spring, the freecam corpus pose
"pos=(59755, 4990, 36743) yaw=1480 pitch=354" (a high view over the
homeworld: the level's whole terrain and its mobys in the frustum), --lod
original, --record-cfr 60, --ssaa 4, --frame-limit ending the run 600 VBLs
(600 owed presents) after the level becomes playable. Windows records at
--window 3840x2160 (scene render target 15360x8640); Linux at --window
960x540 (3840x2160), which lavapipe renders in about 0.3 s per present.

The default leg ends on --frame-limit, so every composed present is owed to
the clip. The --record-frames N leg ends on the recorder's budget instead:
the run quits once N images reached the encoder, which is after N + the
readback ring depth presents composed, so composed exceeds N and the
presents after the Nth are not owed. Under OPENPETE_REC_LOG the leg logs
each present's capture tag ([recq], composition order) and each image the
encoder takes ([recpub]); the published tags must be the first composed
tags in order, so no present inside the first N is lost.

Instruments, both of the same run:
  * the present census exit line "N composed frame(s) total ...; M frame(s)
    never began";
  * ffprobe -count_frames on the clip (decoded video frames); the recorder's
    "finalized ...: K frame(s) captured" and its "cfr: F owed present(s) ...
    filled" warning are reported beside them.

Verdict:
  VACUOUS  the census line is missing or composed is 0: the path did not run.
  FAIL     never began > 0, the clip is missing, or decoded != composed;
           under --record-frames N: the run did not exit on the budget,
           decoded != N, captured != N, or the published tags are not the
           first composed tags (published + filled != N, or a tag differs).
  PASS     otherwise.

Platforms: --platform windows (default) runs the cross-built exe in
build/windows-mingw (or --exe) through WSL interop on the real GPU and SKIPs
without it. --platform linux runs the linux-release binary (or --exe) on the
display in DISPLAY (xvfb-run -a) under lavapipe and SKIPs without a display.
Every launch passes --input-lock --vibration 0 --background 1. The run's
outputs are deleted before it and asserted after it.

Usage: tests/harness/check_record_cfr_complete.py [--platform windows|linux]
           [--exe PATH] [--out DIR] [--frames N] [--window WxH] [--ssaa N]
           [--record-frames N]
Exit 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts", "lib"))
import game_conf  # noqa: E402
from hostenv import child_env, sw_render_env  # noqa: E402

ROW = "s3_sunrise_high"
GAME = "spyro-3"
LEVEL = "sunrise_spring"
POSE = "pos=(59755, 4990, 36743) yaw=1480 pitch=354"
WINDOW = {"windows": "3840x2160", "linux": "960x540"}
TIMEOUT = {"windows": 900, "linux": 1800}
EXIT_PASS, EXIT_FAIL, EXIT_VACUOUS, EXIT_SKIP = 0, 1, 2, 64
# The VBL at which --skip-to-level sunrise_spring logs "level playable -
# presents resume"; no present begins before it. Under --record-cfr 60 the
# pacer owes one present per VBL, so --frame-limit PLAYABLE_VBL + N composes
# about N presents.
PLAYABLE_VBL = 1604
CENSUS_RE = re.compile(r"(\d+) composed frame\(s\) total, .*; (\d+) frame\(s\) never began")
CAPTURED_RE = re.compile(r"finalized .*?: (\d+) frame\(s\) captured, (\d+) dropped")
FILLED_RE = re.compile(r"cfr: (\d+) owed present\(s\) never reached the encoder")
RECQ_RE = re.compile(r"^\[recq\] pts=(\d+)", re.M)
RECPUB_RE = re.compile(r"^\[recpub\] n=\d+ pts=(\d+)", re.M)
# Backstop after the budget: a --record-frames leg that reaches it did not
# stop on the budget.
BUDGET_SLACK_VBLS = 120


def windows_exe():
    """The cross-built exe in build/windows-mingw, or None."""
    tree = os.path.join(ROOT, "build", "windows-mingw")
    for name in ("openpete.exe", os.path.join("games", GAME, game_conf.exe_name(GAME) + ".exe")):
        p = os.path.join(tree, name)
        if os.path.isfile(p):
            return p
    return None


def run_row(platform, exe, out, frames, window, ssaa, budget=0):
    """Launch the row once; return (log path, clip path, exit code).

    budget > 0 ends the run on --record-frames budget, with --frame-limit
    BUDGET_SLACK_VBLS later as the backstop, and logs the capture tags."""
    os.makedirs(out, exist_ok=True)
    base = os.path.join(out, "%s_%s%s" % (ROW, platform, "_rf%d" % budget if budget else ""))
    log, clip = base + ".log", base + ".mkv"
    for p in (log, clip):
        if os.path.exists(p):
            os.remove(p)
    empty = os.path.join(out, "empty")
    os.makedirs(empty, exist_ok=True)
    g = game_conf.resolve(ROOT, GAME)
    if platform == "windows":
        def host(p):
            return subprocess.check_output(["wslpath", "-w", os.path.abspath(p)], text=True).strip()
    else:
        def host(p):
            return os.path.abspath(p)
    argv = [exe, "--disc", host(g["disc"])] + [a for a in g["run_args"] if a != "--no-card"] + [
        "--no-card", "--no-config", "--vibration", "0", "--input-lock", "--background", "1",
        "--mute", "1", "--skip-to-level", LEVEL, "--freecam-pose", POSE, "--lod", "original",
        "--mods-dir", host(empty), "--texpack-dir", host(empty),
        "--renderer", "vk", "--window", window, "--ssaa", str(ssaa),
        "--record-cfr", "60", "--record", host(clip),
        "--frame-limit", str(PLAYABLE_VBL + (budget + BUDGET_SLACK_VBLS if budget else frames)),
        "--max-runtime-seconds", str(TIMEOUT[platform] - 30)]
    knobs = {"OPENPETE_VIBRATION": "0"}
    if budget:
        argv += ["--record-frames", str(budget)]
        knobs["OPENPETE_REC_LOG"] = "1"
    env = child_env(knobs)
    if platform == "linux":
        sw_render_env(env)
    try:
        p = subprocess.run(argv, cwd=os.path.dirname(os.path.abspath(exe)), env=env,
                           timeout=TIMEOUT[platform],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text, rc = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired as e:
        text, rc = (e.stdout or b"").decode("utf-8", "replace"), "timeout"
    with open(log, "w") as f:
        f.write(" ".join(argv) + "\n" + text)
    assert os.path.exists(log)
    return log, clip, rc


def decoded_frames(clip):
    """ffprobe's decoded video frame count of clip, or None."""
    r = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0",
                        "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", clip],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    s = r.stdout.strip()
    return int(s) if r.returncode == 0 and s.isdigit() else None


def budget_reason(text, budget, captured, filled, decoded):
    """The --record-frames leg's failure reason, or "" when the clip holds exactly the first budget presents."""
    if "reason=record-frames" not in text:
        return "run did not stop on --record-frames"
    if decoded != budget:
        return "clip decodes %d frame(s), budget %d" % (decoded, budget)
    if captured != budget:
        return "recorder captured %d frame(s), budget %d" % (captured, budget)
    composed_tags = RECQ_RE.findall(text)
    pub = RECPUB_RE.findall(text)
    if not composed_tags or not pub:
        return "no [recq]/[recpub] tag lines (OPENPETE_REC_LOG unread)"
    if len(pub) + filled != budget:
        return "%d image(s) published + %d filled != budget %d" % (len(pub), filled, budget)
    for i, tag in enumerate(pub):
        if i >= len(composed_tags) or tag != composed_tags[i]:
            return "published image %d is not composed present %d" % (i, i)
    return ""


def analyze(platform, log, clip, rc, budget=0):
    """Judge one run; print the verdict line and return the exit code."""
    with open(log, errors="replace") as f:
        text = f.read()
    cm = CENSUS_RE.findall(text)
    composed, never = (int(cm[-1][0]), int(cm[-1][1])) if cm else (0, 0)
    cap = CAPTURED_RE.findall(text)
    captured = int(cap[-1][0]) if cap else -1
    fm = FILLED_RE.findall(text)
    filled = int(fm[-1]) if fm else 0
    decoded = decoded_frames(clip) if os.path.exists(clip) else None
    reason = ""
    if not cm:
        verdict, reason = "VACUOUS", "no present-census line"
    elif composed == 0:
        verdict, reason = "VACUOUS", "0 composed presents"
    elif never > 0:
        verdict, reason = "FAIL", "%d present(s) never began" % never
    elif decoded is None:
        verdict, reason = "FAIL", "no decodable clip at %s" % clip
    elif budget:
        reason = budget_reason(text, budget, captured, filled, decoded)
        verdict = "FAIL" if reason else "PASS"
    elif decoded != composed:
        verdict, reason = "FAIL", "clip decodes %d frame(s), %d composed" % (decoded, composed)
    else:
        verdict = "PASS"
    print("[record-cfr-complete] row=%s platform=%s rc=%s%s composed=%d never_began=%d "
          "captured=%d cfr_filled=%d decoded=%s verdict=%s%s log=%s"
          % (ROW, platform, rc, " record_frames=%d" % budget if budget else "",
             composed, never, captured, filled,
             "none" if decoded is None else decoded, verdict,
             (" (" + reason + ")") if reason else "", log))
    return {"PASS": EXIT_PASS, "FAIL": EXIT_FAIL, "VACUOUS": EXIT_VACUOUS}[verdict]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--platform", choices=("windows", "linux"), default="windows")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "record-cfr-complete"))
    ap.add_argument("--frames", type=int, default=600, help="VBLs (owed presents) after the level is playable")
    ap.add_argument("--window", default=None, help="default 3840x2160 (windows), 960x540 (linux)")
    ap.add_argument("--ssaa", type=int, default=4)
    ap.add_argument("--record-frames", type=int, default=0,
                    help="end the run on this --record-frames budget instead of --frame-limit")
    a = ap.parse_args()
    if not shutil.which("ffprobe"):
        print("[record-cfr-complete] SKIP: ffprobe not on PATH")
        return EXIT_SKIP
    if a.platform == "windows":
        exe = a.exe or windows_exe()
        if not exe or not os.path.isfile(exe):
            print("[record-cfr-complete] SKIP: no Windows exe in build/windows-mingw")
            return EXIT_SKIP
        if not shutil.which("wslpath"):
            print("[record-cfr-complete] SKIP: no wslpath (not WSL)")
            return EXIT_SKIP
    else:
        exe = a.exe or os.path.join(ROOT, "build", "linux-release", "openpete")
        if not os.path.isfile(exe):
            print("[record-cfr-complete] SKIP: no binary at %s" % exe)
            return EXIT_SKIP
        if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            print("[record-cfr-complete] SKIP: no display (run under xvfb-run -a)")
            return EXIT_SKIP
    if not os.path.isfile(os.path.join(ROOT, game_conf.resolve(ROOT, GAME)["disc"])):
        print("[record-cfr-complete] SKIP: no %s disc" % GAME)
        return EXIT_SKIP
    log, clip, rc = run_row(a.platform, exe, a.out, a.frames,
                            a.window or WINDOW[a.platform], a.ssaa, a.record_frames)
    return analyze(a.platform, log, clip, rc, a.record_frames)


if __name__ == "__main__":
    sys.exit(main())
