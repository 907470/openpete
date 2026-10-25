#!/usr/bin/env python3
"""credits.py - the Spyro 2 / Spyro 3 end-credits fly-through video pipeline.

    credits.py --game G paths   --out WORK/paths [--disc CUE]
    credits.py --game G bk2     --out WORK/speech.bk2
    credits.py --game G music   --flac "49 - Spyro End Titles.flac" --out WORK/music.flac
    credits.py --game G render  --exe EXE --paths WORK/paths --out WORK/render/TAG
                                [--bk2 WORK/speech.bk2] [--scenes 0,3-5] [--jobs N]
                                [--window WxH] [--ssaa N] [--env NAME=VALUE ...] [--disc CUE]
    credits.py --game G qc      --paths WORK/paths --render WORK/render/TAG
    credits.py --game G post    --paths WORK/paths --render WORK/render/TAG --music WORK/music.flac
                                --out OUT.mp4 [--scenes A-B] [--clip I=DIR ...]
    credits.py --game G splice  --paths WORK/paths --render WORK/render/TAG --master IN.mp4
                                --frames A-B --out OUT.mp4 [--clip I=DIR ...]
    credits.py preview          --master OUT.mp4 --out OUT-540p.mp4
    credits.py --game G publish (post's arguments; then the preview beside OUT.mp4)

Stages and the files they own are described in README.md; the per-game
table is games/<game>.toml.
"""
import argparse
import json
import os
import subprocess
import sys

import config


def overrides(items):
    """{scene index: capture directory} from I=DIR arguments."""
    out = {}
    for it in items or []:
        i, _, d = it.partition("=")
        out[int(i)] = d
    return out


def cmd_paths(cfg, a):
    """Extract, edit and smooth the scene paths."""
    import paths
    paths.run(cfg, a.disc or config.default_disc(cfg["game"]), a.out)


def cmd_bk2(cfg, a):
    """Write the speech-advance movie with scripts/make_bk2.py."""
    b = cfg.get("bk2")
    if not b:
        raise SystemExit(f"{cfg['game']} needs no speech movie")
    holds = []
    for s in range(b["first"], b["frames"], b["every"]):
        holds += ["--hold", f"{s}:{s + b['hold']}:{b['button']}"]
    if os.path.exists(a.out):
        os.unlink(a.out)
    subprocess.run([sys.executable, os.path.join(config.ROOT, "scripts", "make_bk2.py"), a.out,
                    "--frames", str(b["frames"]), "--load-delay", str(b["load_delay"]),
                    "--feed-key", b["feed_key"]] + holds, check=True)
    print(f"{a.out}: {b['button']} {b['hold']} VBLs every {b['every']} from {b['first']}, "
          f"{b['frames']} VBLs")


def cmd_music(cfg, a):
    """Build the music track: the FLAC's one-section repeat with its crossfade."""
    m = cfg["music"]
    part = a.out + ".part.flac"
    if os.path.exists(part):
        os.unlink(part)
    fc = (f"[0:a]asplit=2[x][y];[x]atrim=end_sample={m['end_sample']}[a];"
          f"[y]atrim=start_sample={m['start_sample']},asetpts=PTS-STARTPTS[b];"
          f"[a][b]acrossfade=d={m['crossfade']}:c1=tri:c2=tri")
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", a.flac, "-filter_complex", fc,
                    "-c:a", "flac", part], check=True)
    import post
    post.finish(part, a.out)


def cmd_render(cfg, a):
    """Capture the scene clips."""
    import capture_scenes
    return capture_scenes.run(cfg, a)


def cmd_qc(cfg, a):
    """Locate and check every captured scene; write each qc.json."""
    import clipmap
    fails = 0
    for i in config.scene_set(a.scenes, len(cfg["scene"])):
        d = config.scene_dir(a.render, i)
        try:
            q = clipmap.analyze(os.path.join(d, "run.log"), config.scene_keys(a.paths, i))
        except (OSError, ValueError) as e:
            print(f"scene {i}: FAIL {e}")
            fails += 1
            continue
        with open(os.path.join(d, "qc.json"), "w") as f:
            f.write(json.dumps(q) + "\n")
        bad = q.get("short_clip") or {float(k) for k in q["alpha_hist"]} - {0.0, 0.5} or \
            q.get("key1_pose_check")
        fails += bool(bad)
        print(f"scene {i}: f1 {q['f1']} take {q['take']} frames {q['encoded_frames']} "
              f"alpha {q['alpha_hist']} mismatches {q['key_mismatch']} last_dev {q['last_dev']} "
              f"vbls/tick {q['vbls_per_tick']}{'  FAIL' if bad else ''}")
    return 1 if fails else 0


def cmd_post(cfg, a):
    """Encode the master (or a run of consecutive scenes)."""
    import post
    keep = config.scene_set(a.scenes, len(cfg["scene"]))
    if keep != list(range(keep[0], keep[-1] + 1)):
        raise SystemExit("post takes consecutive scenes")
    post.post(cfg, post.scenes_from_render(cfg, a.paths, a.render, overrides(a.clip), keep), a.music, a.out)


def cmd_splice(cfg, a):
    """Re-encode a frame span of an existing master."""
    import post
    lo, _, hi = a.frames.partition("-")
    post.splice(cfg, a.paths, a.render, overrides(a.clip), a.master, a.out, int(lo), int(hi or lo))


def cmd_preview(cfg, a):
    """Encode the 540p preview of a master."""
    import post
    post.preview(a.master, a.out)


def cmd_publish(cfg, a):
    """post, then the preview beside the master."""
    import post
    cmd_post(cfg, a)
    stem, ext = os.path.splitext(a.out)
    post.preview(a.out, f"{stem}-540p{ext}")


def main():
    """Parse the stage and run it."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", choices=config.games())
    sub = ap.add_subparsers(dest="stage", required=True)
    p = sub.add_parser("paths")
    p.add_argument("--out", required=True)
    p.add_argument("--disc")
    p = sub.add_parser("bk2")
    p.add_argument("--out", required=True)
    p = sub.add_parser("music")
    p.add_argument("--flac", required=True)
    p.add_argument("--out", required=True)
    p = sub.add_parser("render")
    p.add_argument("--exe", required=True, help="the openpete executable (openpete.exe runs on Windows)")
    p.add_argument("--paths", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--bk2")
    p.add_argument("--disc")
    p.add_argument("--scenes")
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--window")
    p.add_argument("--ssaa", type=int)
    p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    p = sub.add_parser("qc")
    p.add_argument("--paths", required=True)
    p.add_argument("--render", required=True)
    p.add_argument("--scenes")
    for name in ("post", "publish", "splice"):
        p = sub.add_parser(name)
        p.add_argument("--paths", required=True)
        p.add_argument("--render", required=True)
        p.add_argument("--out", required=True)
        p.add_argument("--clip", action="append", metavar="I=DIR")
        if name == "splice":
            p.add_argument("--master", required=True)
            p.add_argument("--frames", required=True, metavar="A-B")
        else:
            p.add_argument("--music", required=True)
            p.add_argument("--scenes")
    p = sub.add_parser("preview")
    p.add_argument("--master", required=True)
    p.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.stage != "preview" and not a.game:
        ap.error("--game is required")
    cfg = config.load(a.game) if a.game else None
    return globals()["cmd_" + a.stage](cfg, a) or 0


if __name__ == "__main__":
    sys.exit(main())
