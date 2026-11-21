"""capture_scenes - the render stage: one recorded clip per credits scene.

Each scene boots the level (--skip-to-level, or --skip-to-area for a
level:area row) with the player hidden, replays the speech movie when the
game has one, engages the freecam on the scene's smoothed path at the
scene's engage VBL (OPENPETE_FREECAM_AT_VBL) and records at --record-cfr 60
(two presents a tick, 59.826 fps) to a near-lossless yuv444p clip, from boot
to engage + 2N + lead-out VBLs (or the row's frame_limit). The capture logs
([fcpath], [alpha], [cut-census], [recq], [recpub]) go to run.log, from
which clipmap.py locates the shot; qc.json is written beside the clip.

The executable decides the platform: an openpete.exe runs on Windows
through WSL interop (paths as Windows paths, every OPENPETE_* knob listed in
WSLENV by child_env, the working directory the exe's), anything else runs
under Xvfb with a screen the size of the window. Every launch is windowed
and unattended: --input-lock and --vibration 0, OPENPETE_VIBRATION=0.
A Windows run owns the GPU; --jobs above 1 is for Linux only.
"""
import concurrent.futures
import json
import os
import subprocess
import sys

import clipmap
import config

sys.path.insert(0, os.path.join(config.ROOT, "scripts", "lib"))
from hostenv import child_env, sandbox_run_root  # noqa: E402

# Capture-log knobs clipmap reads, set on every scene.
LOG_KNOBS = {"OPENPETE_CUT_CENSUS": "1", "OPENPETE_FCPATH_LOG": "1", "OPENPETE_ALPHA_LOG": "1",
             "OPENPETE_REC_LOG": "1"}


def winpath(path):
    """The Windows spelling of a WSL path."""
    return subprocess.run(["wslpath", "-w", os.path.abspath(path)], capture_output=True, text=True,
                          check=True).stdout.strip()


def scene_argv(cfg, sc, a, out_dir, nkeys, conv):
    """The game arguments of one scene (paths passed through conv)."""
    r = cfg["render"]
    lim = sc.get("frame_limit") or sc["engage_vbl"] + 2 * nkeys + r["lead_out_vbls"]
    level = ["--skip-to-area", sc["level"]] if ":" in sc["level"] else ["--skip-to-level", sc["level"]]
    argv = ["--game", cfg["game"], "--no-config", "--disc", conv(a.disc or config.default_disc(cfg["game"])),
            "--renderer", "vk", "--window", a.window or r["window"], "--lod", r["lod"], "--no-card",
            "--mute", "1", "--hide-player", "1"] + level
    if r.get("use_bk2"):
        if not a.bk2:
            raise SystemExit(f"{cfg['game']} needs --bk2 (the speech movie: credits.py bk2)")
        argv += ["--bk2", conv(a.bk2)]
    path = os.path.join(a.paths, f"scene_{sc['index']:02d}.path")
    argv += ["--freecam-path", "@" + conv(path), "--frame-limit", str(lim),
             "--input-lock", "--vibration", "0", "--background", "1", "--ssaa", str(a.ssaa or r["ssaa"]),
             "--record-cfr", "60", "--record", conv(os.path.join(out_dir, "clip.mkv")),
             "--record-crf", "10", "--record-pixfmt", "yuv444p"]
    return argv, lim


def extra_knobs(extra):
    """{NAME: VALUE} from --env NAME=VALUE arguments."""
    return dict(e.partition("=")[::2] for e in extra)


def capture(cfg, sc, a):
    """Record one scene; return its summary line."""
    i = sc["index"]
    out_dir = os.path.abspath(config.scene_dir(a.out, i))
    os.makedirs(out_dir, exist_ok=True)
    for f in ("run.log", "clip.mkv", "clip.mkv.part", "qc.json"):
        p = os.path.join(out_dir, f)
        if os.path.exists(p):
            os.unlink(p)
    nkeys = config.scene_keys(a.paths, i)
    windows = a.exe.lower().endswith(".exe")
    argv, lim = scene_argv(cfg, sc, a, out_dir, nkeys, winpath if windows else os.path.abspath)
    knobs = {**sc["env"], **LOG_KNOBS}
    knobs["OPENPETE_FREECAM_AT_VBL"] = str(sc["engage_vbl"])
    knobs["OPENPETE_VIBRATION"] = "0"
    knobs.update(extra_knobs(a.env))
    exe = os.path.abspath(a.exe)
    if windows:
        cmd, cwd = [exe] + argv, os.path.dirname(exe)
    else:
        w, h = (a.window or cfg["render"]["window"]).split("x")
        cmd, cwd = ["xvfb-run", "-a", "-s", f"-screen 0 {w}x{h}x24", exe] + argv, None
    with open(os.path.join(out_dir, "run.log"), "w") as log:
        log.write("knobs " + " ".join(f"{k}={v}" for k, v in sorted(knobs.items())) + "\n")
        log.write("argv " + " ".join(cmd) + "\n")
        log.flush()
        root = sandbox_run_root()
        base = dict(os.environ, OPENPETE_SANDBOX=root, OPENPETE_CARD_DIR=os.path.join(root, "cards"))
        rc = subprocess.call(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                             env=child_env(knobs, base=base))
    line = f"scene {i:02d} {sc['level']} engage={sc['engage_vbl']} limit={lim} rc={rc}"
    clip = os.path.join(out_dir, "clip.mkv")
    if rc or not os.path.exists(clip):
        return line + ("" if os.path.exists(clip) else " NO CLIP")
    try:
        q = clipmap.analyze(os.path.join(out_dir, "run.log"), nkeys)
    except ValueError as e:
        return line + f" QC FAILED: {e}"
    with open(os.path.join(out_dir, "qc.json"), "w") as f:
        f.write(json.dumps(q) + "\n")
    return line + f" f1={q['f1']} take={q['take']} frames={q['encoded_frames']}" + \
        (" SHORT" if q.get("short_clip") else "")


def run(cfg, a):
    """Record the selected scenes, a.jobs at a time."""
    if a.exe.lower().endswith(".exe") and a.jobs != 1:
        raise SystemExit("a Windows capture runs one scene at a time (--jobs 1)")
    todo = [cfg["scene"][i] for i in config.scene_set(a.scenes, len(cfg["scene"]))]
    fails = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for line in pool.map(lambda sc: capture(cfg, sc, a), todo):
            print(line, flush=True)
            fails += " rc=0" not in line or any(w in line for w in ("NO CLIP", "SHORT", "FAILED"))
    return 1 if fails else 0
