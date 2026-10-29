# Credits fly-through video pipeline

Films the Spyro 2 (10 scenes) and Spyro 3 (12 scenes) end-credits camera
paths through the live levels (every moby updating, extended LOD, no player,
Sparx or HUD), cuts the scenes together with the credits' own fade, scores
them with the 2022 soundtrack release, and encodes a 3840x2160 60 fps
master, a 540p preview and a thumbnail.

One implementation per stage serves both games; what differs per game is
the table `games/<game>.toml`: WAD layout of the paths, per-scene level,
engage VBL, frame limit, engine knobs and path edits, speech movie, render
defaults, fade and music fit.

## Stages

| Stage | Command | Writes |
|---|---|---|
| extract-paths | `credits.py --game G paths --out WORK/paths` | `raw/scene_NN.path` (disc keys plus the table's edits), `scene_NN.path` (smoothed, flown by render), `manifest.json` (disc key count per scene) |
| speech movie | `credits.py --game spyro-3 bk2 --out WORK/speech.bk2` | Cross 2 VBLs every 20 VBLs from VBL 1700 (`scripts/make_bk2.py`); Spyro 3 only |
| music | `credits.py --game G music --flac "49 - Spyro End Titles.flac" --out WORK/music.flac` | the track with one section repeated (343.503 s) |
| render | `credits.py --game G render --exe EXE --paths WORK/paths [--bk2 WORK/speech.bk2] --out WORK/render/TAG` | `sN/run.log`, `sN/clip.mkv` (59.826 fps yuv444p), `sN/qc.json` |
| QC | `credits.py --game G qc --paths WORK/paths --render WORK/render/TAG` | `sN/qc.json`; exit 1 on a clip short of its window, an alpha outside {0, 0.5}, or a key-1 pose failure |
| post | `credits.py --game G post --paths ... --render ... --music WORK/music.flac --out OUT.mp4` | the master |
| splice | `credits.py --game G splice --paths ... --render ... --clip I=DIR --master IN.mp4 --frames A-B --out OUT.mp4` | IN with frames A..B re-encoded from scene I's re-render in DIR |
| preview | `credits.py preview --master OUT.mp4 --out OUT-540p.mp4` | 960x540 30 fps two-pass preview (aspect kept, padded with black) |
| publish | `credits.py --game G publish` (post's arguments) | post, then `OUT-540p.mp4` beside the master |
| thumbnail title | `thumbnail.py title --out-dir WORK/title [--jobs N]` | `title-black.png`, `title-white.png` (the title over black and over white), `title.log` |
| thumbnail | `thumbnail.py compose --clip WORK/render/TAG/s8/clip.mkv --index 1369 --title-black WORK/title/title-black.png --title-white WORK/title/title-white.png --mark MARK.png --logo LOGO.png --out-dir DIR --tag TAG` | layouts A and B at 3840x2160 and 1280x720 |

Every stage is run from any directory; `WORK` is any directory the user
chooses. The disc defaults to the game's `games/<game>/oracle.conf` DISC
(`--disc` overrides). `post`, `splice` and `qc` locate each scene's shot in
its clip from `run.log` (`clipmap.py`), so they never read a stale
`qc.json`. `--clip I=DIR` substitutes a re-render of scene I; post's
`--scenes A-B` encodes a run of consecutive scenes (music from scene A's
start).

Standalone tools:

- `clipmap.py RUN.log NKEYS`: the QC record of one capture (encoded frame of
  key 1, alpha histogram, key mismatches, the last present's distance from
  its key, the take).
- `campath.py measure|smooth|logmetric`: modelled and measured camera jerk
  of a path or a capture (`OPENPETE_CAM_DBG` `[campose]` lines); `smooth` is
  the extract stage's smoother.
- `flickdet.py CLIP --start S --dur D`: one-frame polygon flicker detector.
- `midcheck.py A.png I.png B.png`: whether particles (bubbles) on an
  intermediate present sit half way between their canonical positions.

## End to end

From the repository root, with `W` a work directory and `FLAC` the user's
copy of "49 - Spyro End Titles.flac" (2022 soundtrack release):

```
T=tools/credits-flythrough/credits.py
EXE=build/windows-mingw/openpete.exe

# Spyro 3
python3 $T --game spyro-3 paths --out $W/s3/paths
python3 $T --game spyro-3 bk2 --out $W/s3/speech.bk2
python3 $T --game spyro-3 music --flac "$FLAC" --out $W/music.flac
python3 $T --game spyro-3 render --exe $EXE --paths $W/s3/paths --bk2 $W/s3/speech.bk2 --out $W/s3/render/master
python3 $T --game spyro-3 qc --paths $W/s3/paths --render $W/s3/render/master
python3 $T --game spyro-3 publish --paths $W/s3/paths --render $W/s3/render/master \
    --music $W/music.flac --out $W/openpete-spyro3-credits-4k60.mp4

# Spyro 2
python3 $T --game spyro-2 paths --out $W/s2/paths
python3 $T --game spyro-2 music --flac "$FLAC" --out $W/music.flac
python3 $T --game spyro-2 render --exe $EXE --paths $W/s2/paths --out $W/s2/render/master
python3 $T --game spyro-2 qc --paths $W/s2/paths --render $W/s2/render/master
python3 $T --game spyro-2 publish --paths $W/s2/paths --render $W/s2/render/master \
    --music $W/music.flac --out $W/openpete-spyro2-credits-4k60.mp4
```

The master settings are the table's: `--window 3840x2160`, `--ssaa 4`,
`--lod extended`, one scene at a time on the Windows exe (the real GPU).
An exe whose name ends in `.exe` runs through WSL interop: paths cross as
Windows paths and every `OPENPETE_*` knob is listed in `WSLENV`
(`scripts/lib/hostenv.py` `child_env`). Any other exe runs under `xvfb-run`
with a screen the size of the window; a Linux prototype is
`--exe build/linux-release/openpete --window 1920x1080 --ssaa 1 --jobs 3`.
Every launch passes `--input-lock --vibration 0` and
`OPENPETE_VIBRATION=0`. `--env NAME=VALUE` adds a knob to every scene.

Requirements: ffmpeg and ffprobe (libx264, flac, aac), `xvfb-run` for
Linux renders, Python 3 with numpy, scipy and Pillow (`tomli` below 3.11).
A Spyro 3 scene at 4K SSAA 4 on the Windows exe records in about 2 min to a
330-600 MB clip; the post encodes about 10 frames/s (35 min for Spyro 3).

## Engine knobs the render stage needs

On main: `--freecam-path @FILE` with fractional waypoints and `cut`
entries, `--record-cfr` capture mode, `--record-crf`, `--record-pixfmt`,
`--hide-player`, `--skip-to-area`, `--input-lock`, `--bk2`,
`OPENPETE_FREECAM_AT_VBL`, `OPENPETE_CUT_CENSUS` (the `shown=camera`
line), `OPENPETE_GLINT`, `OPENPETE_HIDE_HUD` (`--hide-hud`: no HUD),
`OPENPETE_HIDE_CLASSES` (`--hide-classes`: moby classes and their shadows
not drawn), and the log lines `qc` reads: `OPENPETE_FCPATH_LOG` (`[fcpath]`
path key per tick), `OPENPETE_ALPHA_LOG` (`[alpha]` per present),
`OPENPETE_REC_LOG` (`[recq]`/`[recpub]` encoder present-to-frame map).

Gameplay knobs, latched at boot and off by default; each changes gameplay
state, so a clip recorded with them is not a replay of the unmodified game:

| Knob | Game | Effect |
|---|---|---|
| `OPENPETE_D1` | 2, 3 | every moby passes the update-list gate each tick (gems spin at any range) |
| `OPENPETE_VIDEO_ZOE_NEAR` | 3 | Zoe's distance reads take the near branch |
| `OPENPETE_VIDEO_NOSPARX` | 2 | with `OPENPETE_D1`, class 0x78 off the update list (no Sparx sparkles or glow) |
| `OPENPETE_VIDEO_OPEN_PANIM` | 2, 3 | keyframe-animated sectors (doors) by id run open once from keyframe 0 |
| `OPENPETE_VIDEO_HOME_PANIM` | 2, 3 | keyframe-animated sectors by id sent to keyframe 0 and held there |

The thumbnail's title matte is the game's 3D letter font drawn by the
`OPENPETE_VIDEO_TITLE*` knobs, which are not on main: they are
`patches/title-matte.patch`, pinned to 14340c09d (its `Pin:` line).
`thumbnail.py title` creates the throwaway worktree `worktrees/credits-title`
at the pin with `scripts/worktree.sh`, links the untracked build inputs to
the main checkout's, applies the patch, builds `openpete-spyro3`
(linux-release, about the time of a full engine build), renders a plain boot
at VBL 400 under Xvfb with the table's `[thumbnail]` knobs over
`OPENPETE_VIDEO_TITLE_BG=000000` and `FFFFFF`, and removes the worktree
(`--keep-worktree` keeps it). The renders are the disc's font and are never
committed. The mark is `tools/gen_dist_icon.py`'s; the game logo is an
upscale of the title-screen logo texture, supplied as a file.

The Spyro 3 table sets `OPENPETE_GLINT=1` (near glint stars only) on
scenes 0, 6, 9 and 10, where a far glint cross flashes for one tick under
`OPENPETE_D1`; the fix of that pairing defect makes it unnecessary.

## Attract-demo promo

`demo_promo.py` cuts one video per game from its attract loop: 10 s of the
title-screen intro, a fade to black, then every demo level of the loop in
order with no title screen between them. One boot records the loop
(`capture`); the cut table is read from guest words the game's table
`demo/<game>.toml` names, logged by `OPENPETE_STATE_WATCH` (`[state]` lines
keyed on the VBL the recorder's `[recq]` lines carry):

| Segment | From | To |
|---|---|---|
| title | `title_hold` s (the table's `[cut]` value) after the title sub-state first reaches its press-start idle, minus 10 s | `title_hold` s after that edge; the last `title_fade` s fade to black |
| demo | first VBL with the level resident, the demo flag set and the fade level at 255+ (the game's own fade-in from black) | last VBL before the level stops being resident (the game's own fade-out at black) |

A loop ends at the first repeated (level, area). The audio is the SPU's
output on the game's clock (`OPENPETE_AUDIO_DUMP`, `audio.raw` and its VBL
axis `audio.raw.idx`; a `--record-cfr` recording muxes none), cut with the
video, a 50 ms fade at each join. The output keeps the capture's 59.826 fps.

```
D=tools/credits-flythrough/demo_promo.py
python3 $D --game spyro-2 capture --exe EXE --out $W/s2   # --window 3840x2160 --ssaa 4 --lod extended by default
python3 $D --game spyro-2 cuts --capture $W/s2             # prints the table, writes cuts.json
python3 $D --game spyro-2 post --capture $W/s2 --out $W/openpete-spyro2-demos.mp4
python3 $D sheet --video $W/openpete-spyro2-demos.mp4 --out $W/sheet.png --every 5
python3 tools/credits-flythrough/credits.py preview --master $W/openpete-spyro2-demos.mp4 --out $W/openpete-spyro2-demos-540p.mp4
```

A Linux prototype is `--exe build/linux-release/openpete --window 960x720
--ssaa 1` (Xvfb, OpenAL Soft's null backend: nothing plays on the host).
