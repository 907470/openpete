#!/usr/bin/env python3
"""demo_promo.py - the Spyro 2 / Spyro 3 attract-demo promo video.

    demo_promo.py --game G capture --exe EXE --out WORK [--window WxH] [--ssaa N]
                                   [--lod L] [--frame-limit N] [--disc CUE] [--env NAME=VALUE ...]
    demo_promo.py --game G cuts    --capture WORK [--title-seconds 10] [--title-hold 1.0]
    demo_promo.py --game G post    --capture WORK --out OUT.mp4 [--title-seconds 10]
                                   [--title-hold 1.0] [--title-fade 1.0] [--crf 14]
    demo_promo.py sheet            --video OUT.mp4 --out SHEET.png [--every 5] [--cols 6]
    credits.py preview             --master OUT.mp4 --out OUT-540p.mp4

One boot of the unmodified game records its attract loop (title, demo,
title, demo, ...) at --record-cfr 60 (59.826 fps, one encoded frame per
VBL), logs the guest words of the game's table demo/<game>.toml every tick
they change (OPENPETE_STATE_WATCH, [state] lines) beside the recorder's
present-to-frame map ([recq]/[recpub]), and dumps the SPU's output on the
game's clock (OPENPETE_AUDIO_DUMP: audio.raw, s16 stereo 44.1 kHz, and its
VBL axis audio.raw.idx); a CFR recording muxes no audio of its own.

The cut table is read from those words, never from pixels:

  title    the 10 s window that ends title_hold seconds (the table's, or
           --title-hold) after the title's
           sub-state first reaches the press-start idle (the logo has
           landed), over the title-screen intro the game plays after its
           developer logo; the last title_fade seconds fade to black (video
           and audio), the one fade the cut adds.
  demo k   every span in which the level is resident (`resident` equals
           the table's value) with the demo flag set: from the first VBL
           whose fade level is 255 or more (the game's own fade-in starts
           from black) to the last VBL before the level stops being
           resident (the game's own fade-out has reached black). The
           title screens, loads and loading images between demos are cut.

Every join is black to black: the title's added fade, then each demo's own
fade-in and fade-out. The audio is cut with the video; a 50 ms audio fade
at each join removes the step the cut makes in the waveform. Each audio
segment starts at the dump's sample for the segment's first VBL and is cut
or padded to the video segment's length. The output keeps the capture's
exact rate (59.826 fps), so the game's audio stays in sync without a tempo
change. The capture stage reuses the credits render
stage's launcher conventions (Xvfb for a Linux exe, WSL interop and WSLENV
for openpete.exe); post.finish, post.part_name and post.preview are the
credits pipeline's.
"""
import argparse
import json
import os
import re
import subprocess
import sys

import config
import post

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

sys.path.insert(0, os.path.join(config.ROOT, "scripts", "lib"))
from hostenv import child_env  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
# The capture's exact rate: --record-cfr 60 is two presents a tick, 2250000/37609 Hz.
FPS_NUM, FPS_DEN = 2250000, 37609
FPS = FPS_NUM / FPS_DEN
JOIN_AFADE = 0.05
RS = re.compile(r"\[state\] vbl=(\d+) frame=\d+((?: \w+=-?\d+)+)")
RQ = re.compile(r"\[recq\] pts=(\d+) vbl=(\d+)")
RP = re.compile(r"\[recpub\] n=(\d+) pts=(\d+)")


def table(game):
    """The game's promo table demo/<game>.toml."""
    path = os.path.join(HERE, "demo", game + ".toml")
    if not os.path.exists(path):
        raise SystemExit(f"no promo table for {game}")
    with open(path, "rb") as f:
        return tomllib.load(f)


def title_hold(tab, a):
    """Seconds the title window runs past the press-start idle edge: --title-hold, else the table's."""
    return a.title_hold if a.title_hold is not None else tab["cut"]["title_hold"]


def watch_knob(tab):
    """OPENPETE_STATE_WATCH's value: NAME=ADDR[:SIZE] for every watched word."""
    return ",".join(f"{k}={v}" for k, v in tab["watch"].items())


def winpath(path):
    """The Windows spelling of a WSL path."""
    return subprocess.run(["wslpath", "-w", os.path.abspath(path)], capture_output=True, text=True,
                          check=True).stdout.strip()


def cmd_capture(tab, a):
    """Record one boot through the first attract loop with the state and recorder logs."""
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    for f in ("run.log", "clip.mkv", "cuts.json", "audio.raw", "audio.raw.idx"):
        p = os.path.join(out, f)
        if os.path.exists(p):
            os.unlink(p)
    windows = a.exe.lower().endswith(".exe")
    conv = winpath if windows else os.path.abspath
    clip = os.path.join(out, "clip.mkv")
    argv = ["--game", tab["game"], "--no-config", "--disc", conv(a.disc or config.default_disc(tab["game"])),
            "--renderer", "vk", "--window", a.window, "--lod", a.lod, "--no-card",
            "--input-lock", "--vibration", "0", "--background", "1", "--ssaa", str(a.ssaa),
            "--frame-limit", str(a.frame_limit or tab["frame_limit"]),
            "--record-cfr", "60", "--record", conv(clip), "--record-crf", "10", "--record-pixfmt", "yuv444p"]
    # OpenAL Soft's null backend: the SPU renders every sample on the game's
    # clock into the dump, and nothing plays on the host.
    knobs = {"OPENPETE_STATE_WATCH": watch_knob(tab), "OPENPETE_REC_LOG": "1", "OPENPETE_VIBRATION": "0",
             "OPENPETE_AUDIO_DUMP": conv(os.path.join(out, "audio.raw")), "ALSOFT_DRIVERS": "null"}
    knobs.update(dict(e.partition("=")[::2] for e in a.env))
    exe = os.path.abspath(a.exe)
    if windows:
        cmd, cwd = [exe] + argv, os.path.dirname(exe)
    else:
        w, h = a.window.split("x")
        cmd, cwd = ["xvfb-run", "-a", "-s", f"-screen 0 {w}x{h}x24", exe] + argv, None
    with open(os.path.join(out, "run.log"), "w") as log:
        log.write("knobs " + " ".join(f"{k}={v}" for k, v in sorted(knobs.items())) + "\n")
        log.write("argv " + " ".join(cmd) + "\n")
        log.flush()
        rc = subprocess.call(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, env=child_env(knobs))
    missing = [f for f in ("clip.mkv", "audio.raw", "audio.raw.idx") if not os.path.exists(os.path.join(out, f))]
    if rc or missing:
        raise SystemExit(f"capture rc={rc}, missing {missing} (log {out}/run.log)")
    cuts = cut_table(tab, os.path.join(out, "run.log"), a.title_seconds, title_hold(tab, a))
    with open(os.path.join(out, "cuts.json"), "w") as f:
        json.dump(cuts, f, indent=1)
    print_cuts(cuts)
    return 0


def parse_log(path):
    """([(vbl, {name: value})] state lines, {vbl: first encoded frame showing it}, encoded frames)."""
    states, rq, rp = [], {}, []
    with open(path, errors="replace") as f:
        for line in f:
            if m := RS.match(line):
                states.append((int(m.group(1)), {k: int(v) for k, v in
                                                 (kv.split("=") for kv in m.group(2).split())}))
            elif m := RQ.match(line):
                rq.setdefault(int(m.group(1)), int(m.group(2)))
            elif m := RP.match(line):
                rp.append((int(m.group(1)), int(m.group(2))))
    frame_of = {}
    for n, pts in rp:
        v = rq.get(pts)
        if v is not None and v not in frame_of:
            frame_of[v] = n
    return states, frame_of, len(rp)


def vbl_frame(frame_of, vbl, last=False):
    """The encoded frame of a VBL: the first frame showing it, else the nearest
    frame after it (or, with last, before it)."""
    if vbl in frame_of:
        return frame_of[vbl]
    keys = sorted(frame_of)
    if last:
        prior = [k for k in keys if k < vbl]
        return frame_of[prior[-1]] if prior else None
    after = [k for k in keys if k > vbl]
    return frame_of[after[0]] if after else None


def find_demos(tab, states):
    """Each demo span: level, area, fade-in start, fully faded in, fade-out start, last VBL."""
    res = tab["cut"]["resident"]
    demos, cur = [], None
    for i, (v, s) in enumerate(states):
        live = s["resident"] == res and s["demo"] == 1
        if live and cur is None:
            cur = {"level": s["level"], "area": s.get("area"), "first": None, "in": None, "out": None}
        if cur is not None and not live:
            cur["last"] = v - 1
            if cur["first"] is not None:
                demos.append(cur)
            cur = None
            continue
        if cur is None:
            continue
        if cur["first"] is None and s["fade"] >= 255:
            cur["first"] = v
        elif cur["first"] is not None and cur["in"] is None and s["fade"] == 0:
            cur["in"] = v
        if cur["in"] is not None and s["fade"] > 0 and cur["out"] is None:
            cur["out"] = v
        if cur["out"] is not None and s["fade"] == 0:
            cur["out"] = None   # a fade that cleared again was not the exit fade
    return demos


def cut_table(tab, log, title_seconds, title_hold):
    """The cut table of a capture log: title window and every demo span, in VBLs and encoded frames."""
    states, frame_of, nframes = parse_log(log)
    if not states or not frame_of:
        raise SystemExit(f"{log}: no {'[state]' if not states else '[recq]/[recpub]'} lines")
    idle = next((v for v, s in states if s["title_sub"] == tab["cut"]["title_idle"]), None)
    if idle is None:
        raise SystemExit(f"{log}: the title never reached its press-start idle")
    t_len = round(title_seconds * FPS)
    t_end = idle + round(title_hold * FPS)
    title = {"idle_vbl": idle, "first": t_end - t_len, "last": t_end - 1,
             "f0": vbl_frame(frame_of, t_end - t_len), "frames": t_len}
    demos = find_demos(tab, states)
    seen, out = set(), []
    for d in demos:
        key = (d["level"], d["area"])
        if key in seen:
            break   # the loop repeats
        seen.add(key)
        f0, f1 = vbl_frame(frame_of, d["first"]), vbl_frame(frame_of, d["last"], last=True)
        d.update(f0=f0, f1=f1, frames=None if f0 is None or f1 is None else f1 - f0 + 1)
        out.append(d)
    return {"game": tab["game"], "fps": [FPS_NUM, FPS_DEN], "encoded_frames": nframes,
            "title": title, "demos": out}


def print_cuts(c):
    """Print the cut table, one row per segment, durations in seconds of the 59.826 fps video."""
    t = c["title"]
    print(f"{c['game']}: {c['encoded_frames']} encoded frames")
    print(f"  title  vbl {t['first']}-{t['last']} (idle at {t['idle_vbl']}) frame {t['f0']} "
          f"{t['frames'] / FPS:.2f} s")
    for i, d in enumerate(c["demos"]):
        area = "" if d["area"] is None else f" area {d['area']}"
        dur = "short" if d["frames"] is None else f"{d['frames'] / FPS:.2f} s"
        print(f"  demo {i}: level {d['level']}{area} vbl {d['first']}-{d['last']} "
              f"(clear {d['in']}, exit fade {d['out']}) frames {d['f0']}-{d['f1']} {dur}")


def load_cuts(tab, a):
    """The cut table of a.capture, from its run.log with this run's title arguments."""
    c = cut_table(tab, os.path.join(a.capture, "run.log"), a.title_seconds, title_hold(tab, a))
    bad = [i for i, d in enumerate(c["demos"]) if d["frames"] is None or d["f1"] >= c["encoded_frames"]]
    if c["title"]["f0"] is None or bad:
        raise SystemExit(f"capture short of its cuts (title f0 {c['title']['f0']}, demos {bad})")
    return c


def cmd_cuts(tab, a):
    """Print the cut table of a capture and write it to cuts.json beside it."""
    c = load_cuts(tab, a)
    with open(os.path.join(a.capture, "cuts.json"), "w") as f:
        json.dump(c, f, indent=1)
    print_cuts(c)
    return 0


def audio_axis(path):
    """{device VBL: frames in the dump after that VBL's drain} from audio.raw.idx."""
    axis = {}
    with open(path) as f:
        for line in f:
            if not line.startswith("#"):
                v, n, _ = line.split()
                axis[int(v)] = int(n)
    return axis


def audio_start(axis, vbl):
    """The dump's sample at the start of a VBL: the frames drained through the VBL before it."""
    prior = [v for v in axis if v < vbl]
    if not prior:
        raise SystemExit(f"audio dump has no VBL before {vbl}")
    return axis[max(prior)]


def segments(c, axis):
    """(first frame, frames, first audio sample, is title) of each promo segment, in order."""
    t = c["title"]
    out = [(t["f0"], t["frames"], audio_start(axis, t["first"]), True)]
    out += [(d["f0"], d["frames"], audio_start(axis, d["first"]), False) for d in c["demos"]]
    return out


def graph(segs, title_fade):
    """The filter graph cutting [0:v] (clip) and [1:a] (audio dump) into the promo as [vout]/[aout]."""
    parts, labels = [], ""
    for i, (f0, n, s0, is_title) in enumerate(segs):
        dur = n / FPS
        v = f"[0:v]trim=start_frame={f0}:end_frame={f0 + n},setpts=PTS-STARTPTS"
        a = (f"[1:a]atrim=start_sample={s0},asetpts=PTS-STARTPTS,apad,atrim=end={dur:.6f},"
             f"afade=t=in:d={JOIN_AFADE}")
        if is_title:
            v += f",fade=t=out:st={dur - title_fade:.6f}:d={title_fade:.6f}"
            a += f",afade=t=out:st={dur - title_fade:.6f}:d={title_fade:.6f}"
        else:
            a += f",afade=t=out:st={dur - JOIN_AFADE:.6f}:d={JOIN_AFADE}"
        parts += [v + f"[v{i}]", a + f"[a{i}]"]
        labels += f"[v{i}][a{i}]"
    return ";".join(parts) + f";{labels}concat=n={len(segs)}:v=1:a=1[vc][aout];[vc]format=yuv420p[vout]"


def cmd_post(tab, a):
    """Encode the promo: title intro, its fade to black, then every demo of the loop."""
    c = load_cuts(tab, a)
    clip = os.path.join(a.capture, "clip.mkv")
    raw = os.path.join(a.capture, "audio.raw")
    segs = segments(c, audio_axis(raw + ".idx"))
    part = post.part_name(a.out)
    if os.path.exists(part):
        os.unlink(part)
    args = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-stats", "-y", "-i", clip,
            "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", raw,
            "-filter_complex", graph(segs, a.title_fade), "-map", "[vout]", "-map", "[aout]",
            "-r", f"{FPS_NUM}/{FPS_DEN}", "-c:v", "libx264", "-preset", "slow", "-crf", str(a.crf),
            "-profile:v", "high", "-c:a", "aac", "-b:a", "320k", "-movflags", "+faststart", part]
    total = sum(n for _, n, _, _ in segs)
    print(f"post: title + {len(c['demos'])} demos, {total} frames, {total / FPS:.2f} s -> {a.out}")
    if subprocess.call(args):
        raise SystemExit("ffmpeg failed")
    post.finish(part, a.out)
    return 0


def cmd_sheet(tab, a):
    """Write a contact sheet of a video: one frame every a.every seconds, a.cols per row, a.width wide.

    Each tile is the first frame at or after a multiple of a.every seconds,
    labelled with that frame's own timestamp. The sheet is kept to a.width
    pixels so an image viewer shows it without its own downscale, which
    re-quantises a large sheet into visible colour banding."""
    d = post.duration(a.video)
    if d is None:
        raise SystemExit(f"{a.video}: no duration")
    n = int(d // a.every) + 1
    rows = (n + a.cols - 1) // a.cols
    tw = (a.width - 2 * (a.cols + 1)) // a.cols // 2 * 2
    vf = (f"select='isnan(prev_selected_t)+gte(floor(t/{a.every}),floor(prev_selected_t/{a.every})+1)',"
          f"drawtext=text='%{{pts\\:hms}}':x=6:y=6:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.6,"
          f"scale={tw}:-2,tile={a.cols}x{rows}:padding=2:margin=2,format=rgb24")
    if os.path.exists(a.out):
        os.unlink(a.out)
    if subprocess.call(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", a.video, "-vf", vf,
                        "-frames:v", "1", "-update", "1", a.out]) or not os.path.exists(a.out):
        raise SystemExit("contact sheet failed")
    print(f"{a.out}: {n} frames, every {a.every} s")
    return 0


def main():
    """Parse the stage and run it."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", choices=["spyro-2", "spyro-3"])
    sub = ap.add_subparsers(dest="stage", required=True)
    p = sub.add_parser("capture")
    p.add_argument("--exe", required=True, help="the openpete executable (openpete.exe runs on Windows)")
    p.add_argument("--out", required=True)
    p.add_argument("--window", default="3840x2160")
    p.add_argument("--ssaa", type=int, default=4)
    p.add_argument("--lod", default="extended")
    p.add_argument("--frame-limit", type=int)
    p.add_argument("--disc")
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    for name in ("cuts", "post"):
        q = sub.add_parser(name)
        q.add_argument("--capture", required=True)
        if name == "post":
            q.add_argument("--out", required=True)
            q.add_argument("--title-fade", type=float, default=1.0)
            q.add_argument("--crf", type=int, default=14)
    for q in (p, sub.choices["cuts"], sub.choices["post"]):
        q.add_argument("--title-seconds", type=float, default=10.0)
        q.add_argument("--title-hold", type=float,
                       help="seconds the title window runs past the press-start idle edge (table default)")
    q = sub.add_parser("sheet")
    q.add_argument("--video", required=True)
    q.add_argument("--out", required=True)
    q.add_argument("--every", type=float, default=5.0)
    q.add_argument("--cols", type=int, default=6)
    q.add_argument("--width", type=int, default=1536, help="sheet width in pixels")
    a = ap.parse_args()
    if a.stage != "sheet" and not a.game:
        ap.error("--game is required")
    tab = table(a.game) if a.game else None
    return globals()["cmd_" + a.stage](tab, a) or 0


if __name__ == "__main__":
    sys.exit(main())
