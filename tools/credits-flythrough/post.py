"""post - the post, splice and preview stages: scene clips to the master.

A scene of N disc keys is 2N presents of the video: from the clip's encoded
frame f1 (key 1's on-tick present, clipmap.py) `take` presents (2N-3, or
2N-4 when the clip's last present shows the next shot), then the last
present cloned up to 2N (the credits' switch-tick hold). Over each scene the
credits fade is applied as the game draws it, a subtractive grey quad:
out = max(0, in - grey * v) per RGB channel with k = present/2 + 1 and
v = T - k for k < T, min(k - (N - T), T - 1) for k > N - T, else 0
(T = fade_ticks). The scenes are concatenated, retimed from 59.826 fps
(--record-cfr 60, two presents a tick) to exactly 60 fps by timestamps
alone (no frame added or dropped), and encoded H.264 (libx264 slow, crf from
the table, high profile) with the music as AAC 320k from the video's start
offset; with music_fade_out > 0 the music is cut at the video end with that
fade-out.

splice() replaces a frame span of an existing master without re-encoding the
rest: the span from the last keyframe at or before A to the first keyframe
after B is rebuilt from the scene clips with the same filter and encoder and
joined to stream copies of the master on both sides, each piece given its
exact duration; the audio is copied.

preview() writes the 960x540 30 fps two-pass H.264 + AAC 96k preview.
Every output is written to a .part file and renamed into place only after
ffmpeg exits 0 and ffprobe reads its duration.
"""
import os
import shutil
import subprocess
import tempfile

import clipmap
import config

FPS = 60


class Scene:
    """One scene's clip in the video: index, clip, f1, take, disc keys, start frame."""

    def __init__(self, index, clip, f1, take, nkeys, start):
        self.index, self.clip, self.f1, self.take = index, clip, f1, take
        self.nkeys, self.start = nkeys, start

    @property
    def period(self):
        """Presents the scene lasts in the video (2N)."""
        return 2 * self.nkeys


def scene_spans(cfg, paths_dir):
    """(first video frame, disc keys) of every scene, in order."""
    out, start = [], 0
    for sc in cfg["scene"]:
        nk = config.scene_keys(paths_dir, sc["index"])
        out.append((start, nk))
        start += 2 * nk
    return out


def scenes_from_render(cfg, paths_dir, render_dir, overrides=None, keep=None):
    """The scenes `keep` (default all) with their clips located by clipmap.

    overrides maps a scene index to another capture directory (a re-render
    of that scene). Raises SystemExit naming each scene whose clip does not
    hold the 2N-3 present window."""
    overrides = overrides or {}
    spans = scene_spans(cfg, paths_dir)
    out, bad = [], []
    for i in (range(len(spans)) if keep is None else keep):
        start, nk = spans[i]
        d = overrides.get(i, config.scene_dir(render_dir, i))
        try:
            qc = clipmap.analyze(os.path.join(d, "run.log"), nk)
        except (OSError, ValueError) as e:
            bad.append(f"scene {i}: {e}")
            continue
        if qc.get("short_clip"):
            bad.append(f"scene {i} ({d}): f1 {qc['f1']} take {qc['take']} of {qc['encoded_frames']} frames")
        out.append(Scene(i, os.path.join(d, "clip.mkv"), qc["f1"], qc["take"], nk, start))
    if bad:
        raise SystemExit("clips missing their window:\n  " + "\n  ".join(bad))
    return out


def fade_expr(nkeys, ticks, offset):
    """The fade level v of present N + offset of a scene of nkeys keys (ffmpeg geq syntax)."""
    tail = nkeys - ticks
    return (f"if(lt(st(1,floor((N+{offset})/2)+1),{ticks}),{ticks}-ld(1),"
            f"if(gt(ld(1),{tail}),min(ld(1)-{tail},{ticks - 1}),0))")


def scene_filter(i, sc, l0, l1, pcfg):
    """Filter chain of input i producing scene-local presents [l0, l1) of sc as [v<i>]."""
    a = min(l0, sc.take - 1)
    cnt = min(l1, sc.take) - a
    extra = (l1 - l0) - cnt
    ticks, grey = pcfg["fade_ticks"], pcfg["fade_grey"]
    v = fade_expr(sc.nkeys, ticks, l0)
    ch = ":".join(f"{c}='max(0,{c}(X,Y)-{grey}*({v}))'" for c in ("r", "g", "b"))
    on = f"lt(n+{l0},{2 * (ticks - 1)})+gte(n+{l0},{2 * (sc.nkeys - ticks)})"
    return (f"[{i}:v]trim=start_frame={sc.f1 + a}:end_frame={sc.f1 + a + cnt},setpts=PTS-STARTPTS,"
            f"tpad=stop_mode=clone:stop={extra},format=gbrp,geq={ch}:enable='{on}'[v{i}]")


def video_graph(pieces, pcfg):
    """Inputs and filter graph concatenating (scene, l0, l1) pieces, retimed to 60 fps, as [vout]."""
    ins, parts = [], []
    for i, (sc, l0, l1) in enumerate(pieces):
        ins += ["-i", sc.clip]
        parts.append(scene_filter(i, sc, l0, l1, pcfg))
    n = len(parts)
    fc = ";".join(parts) + ";" + "".join(f"[v{i}]" for i in range(n)) + \
        f"concat=n={n}:v=1:a=0,setpts=N/({FPS}*TB),format=yuv420p[vout]"
    return ins, fc


def x264(pcfg):
    """The master's video encoder arguments."""
    return ["-r", str(FPS), "-c:v", "libx264", "-preset", "slow", "-crf", str(pcfg["crf"]),
            "-profile:v", "high"]


def duration(path):
    """ffprobe's container duration of path, or None."""
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    d = r.stdout.strip()
    return float(d) if r.returncode == 0 and d and d != "N/A" else None


def finish(part, out):
    """Rename part to out when ffprobe reads its duration; raise otherwise."""
    d = duration(part)
    if d is None:
        raise SystemExit(f"{part}: no duration, left in place")
    os.replace(part, out)
    print(f"{out} duration {d:.6f}")


def part_name(out):
    """The temporary name an output is written under (same directory, same container)."""
    d, b = os.path.split(os.path.abspath(out))
    stem, ext = os.path.splitext(b)
    return os.path.join(d, f".{stem}.part{ext}")


def post(cfg, scenes, audio, out):
    """Encode the given scenes (consecutive, in order) with the music into out."""
    pcfg = cfg["post"]
    ins, fc = video_graph([(sc, 0, sc.period) for sc in scenes], pcfg)
    n = len(scenes)
    dur = sum(sc.period for sc in scenes) / FPS
    offset = scenes[0].start / FPS
    fo = pcfg.get("music_fade_out", 0.0)
    amap = f"{n}:a"
    if fo > 0:
        fc += f";[{n}:a]atrim=end={dur:.6f},afade=t=out:st={dur - fo:.6f}:d={fo:.6f}[aout]"
        amap = "[aout]"
    part = part_name(out)
    if os.path.exists(part):
        os.unlink(part)
    args = (["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning", "-stats", "-y"] + ins
            + ["-ss", f"{offset:.6f}", "-i", audio, "-filter_complex", fc, "-map", "[vout]", "-map", amap]
            + x264(pcfg) + ["-c:a", "aac", "-b:a", "320k", "-t", f"{dur:.6f}", "-movflags", "+faststart", part])
    print(f"post: {n} scenes, {sum(sc.period for sc in scenes)} presents, {dur:.3f} s -> {out}")
    for sc in scenes:
        print(f"  scene {sc.index}: {sc.clip} f1 {sc.f1} take {sc.take} of {sc.period}")
    if subprocess.call(args):
        raise SystemExit("ffmpeg failed")
    finish(part, out)


def keyframes(path):
    """(keyframe indices, packet count) of path's video stream at 60 fps."""
    pk = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries",
                         "packet=pts_time,flags", "-of", "csv=p=0", path],
                        capture_output=True, text=True, check=True).stdout.split()
    keys = sorted(round(float(t) * FPS) for t, f in (ln.split(",")[:2] for ln in pk) if "K" in f)
    return keys, len(pk)


def splice(cfg, paths_dir, render_dir, overrides, orig, out, A, B):
    """Write orig with output frames A..B (inclusive) re-encoded from the scene clips."""
    pcfg = cfg["post"]
    keys, total = keyframes(orig)
    k0 = max(k for k in keys if k <= A)
    nxt = [k for k in keys if k > B]
    k1 = nxt[0] if nxt else total
    print(f"keyframes around [{A},{B}]: re-encode [{k0},{k1}) = {k1 - k0} frames of {total}")
    hit = [i for i, (s, nk) in enumerate(scene_spans(cfg, paths_dir)) if s < k1 and s + 2 * nk > k0]
    scenes = scenes_from_render(cfg, paths_dir, render_dir, overrides, hit)
    pieces = [(sc, max(k0, sc.start) - sc.start, min(k1, sc.start + sc.period) - sc.start)
              for sc in scenes]
    ins, fc = video_graph(pieces, pcfg)
    tmp = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(out)))
    patch = os.path.join(tmp, "patch.mp4")
    if subprocess.call(["ffmpeg", "-nostdin", "-v", "error", "-y"] + ins
                       + ["-filter_complex", fc, "-map", "[vout]"] + x264(pcfg) + [patch]):
        raise SystemExit(f"patch encode failed (kept {tmp})")
    cuts = ",".join(str(k) for k in (k0, k1) if 0 < k < total)
    if subprocess.call(["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", orig, "-map", "0:v", "-c", "copy",
                        "-f", "segment", "-segment_frames", cuts, "-reset_timestamps", "1",
                        os.path.join(tmp, "seg%d.mp4")]):
        raise SystemExit(f"segmenting {orig} failed (kept {tmp})")
    names = sorted((f for f in os.listdir(tmp) if f.startswith("seg")), key=lambda f: int(f[3:-4]))
    pieces = []   # (file, frames): each piece's exact length, so a join adds no gap
    if k0 > 0:
        pieces.append((os.path.join(tmp, names.pop(0)), k0))
    if names:
        names.pop(0)          # the span being replaced
    pieces.append((patch, k1 - k0))
    if k1 < total:
        pieces.append((os.path.join(tmp, names.pop(0)), total - k1))
    lst = os.path.join(tmp, "list.txt")
    with open(lst, "w") as f:
        for x, nf in pieces:
            f.write(f"file '{x}'\nduration {nf / FPS:.6f}\n")
    part = part_name(out)
    if os.path.exists(part):
        os.unlink(part)
    if subprocess.call(["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                        "-i", orig, "-map", "0:v", "-map", "1:a", "-c", "copy", "-movflags", "+faststart", part]):
        raise SystemExit(f"join failed (kept {tmp})")
    shutil.rmtree(tmp)
    finish(part, out)


def preview(master, out):
    """Write the 960x540 30 fps two-pass preview of master."""
    part = part_name(out)
    if os.path.exists(part):
        os.unlink(part)
    tmp = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(out)))
    enc = ["-vf", "scale=960:540,fps=30", "-c:v", "libx264", "-preset", "slow", "-b:v", "530k",
           "-maxrate", "620k", "-bufsize", "1240k", "-passlogfile", os.path.join(tmp, "p540")]
    base = ["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", master]
    ok = (subprocess.call(base + enc + ["-pass", "1", "-an", "-f", "null", os.devnull], cwd=tmp) == 0
          and subprocess.call(base + enc + ["-pass", "2", "-c:a", "aac", "-b:a", "96k", "-movflags",
                                            "+faststart", part], cwd=tmp) == 0)
    shutil.rmtree(tmp)
    if not ok:
        raise SystemExit("preview encode failed")
    finish(part, out)
