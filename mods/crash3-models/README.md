# crash3-models

A Crash Bandicoot 3 demo model, installed over a Spyro 1 moby class.
By default Crash himself (`Cr10G`) replaces the crystal dragon statue
(class 250) in every level. Static pose only: no animation, no behaviour.

The Spyro 1 disc ships a playable Crash 3 demo under `S0/`. The converter
in `tools/crash3/` reads its NSF archives straight off the disc image,
poses every model on an animation frame, and encodes each one as a
Spyro 1 animated-model blob:

```
python3 tools/crash3/nsf.py     --disc games/spyro-1/spyro-1/build/SCUS-94228.cue   # census
python3 tools/crash3/convert.py --disc games/spyro-1/spyro-1/build/SCUS-94228.cue   # -> assets/
python3 tools/crash3/verify_pack.py --preview /tmp/crash3-preview                    # mirror walk + PNGs
```

`convert.py` writes `assets/crash3_s1pack.bin` (88 models) and the
texture atlases `assets/textures/own/c30N.png`, which the engine registers
at boot under those ids (the faces bake the mode-3 tpage + id). Knobs:
`--models`, `--pose MODEL=ANIM:FRAME`, `--height` (world units, Spyro is
about 480), `--yaw` (degrees about the vertical axis; the default 270
turns Crash to face the statue pedestal's front), `--budget` (vertex
ceiling, 255), `--lp-faces`, `--double-sided`.

Format facts the converter rests on live in its docstrings; the Crash side
is a port of the cbhacks/CrashEdit loaders (TGEO poly-item strip state
machine, SVTX bit-packed frames, texture records, GOOL vertex groups for
the animation->model link).

## Config (`config.toml`)

| key | default | meaning |
|---|---|---|
| `model` | `"Cr10G"` | pack model eName to install (see the census) |
| `target.class` | `250` | `g_Models` row to replace |
| `debug.dump` | `false` | log the pack table at entry and the live target-class mobys (with positions) once per level |

Any class works: the mod widens its header at install to the stock
model's animation and frame counts (pre-filled reserve slots that all
alias the one pose), so whatever animation/frame a live moby sits on
resolves to the converted pose.

## Ceilings

Crash is 305 vertices in the demo; the Spyro 1 header counts vertices in
one byte per tier, so the converter collapses shortest edges down to 255
and builds a ~200-face LP tier the same way. Artisans polybuf headroom
with Crash on the pedestal: 31.5 KB closest-ever free (stock 34.4 KB), no
overflow.
