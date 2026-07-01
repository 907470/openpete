# PsyCross reference render archive (W0.1)

Frozen PsyCross (legacy GL renderer) output over a curated set of scenes,
captured **before** PsyCross is retired. Two purposes:

1. **The reference that replaces A/B once PsyCross is gone.** Today the native
   Vulkan renderer is validated side-by-side against PsyCross (`--renderer ab`).
   When PsyCross is deleted (native-renderer plan, Wave-2/PsyCross-deletion),
   that live reference disappears — this archive preserves it.
2. **The reference leg for the future native visual-regression gate** (plan
   W1.2): compare a headless native `-vk` render at the same (scene, frame)
   against these.

## What these are (and aren't)

- `*-og.png` is the **PsyCross present FBO** (`openpete_psyx_take_screenshot`),
  captured **headless** — under `--headless` PsyCross rasterizes through SDL
  software GL, so this is deterministic and needs no GPU/display.
- They are **not** pixel-identical to the native renderer. Native uses float
  raster + optional SSAA and legitimately differs. Any gate comparing native
  against these must use a **tolerance** metric (e.g. `center_crop_diff.py`
  threshold+count), never an exact hash, and should mask the overscan bands.

## Corpus

Deterministic attract demo (`--run-recomp 0x80012204`), sim-frame → scene:

| File | Frame | Scene |
|------|-------|-------|
| `demo-2000-og.png`  | 2000  | attract title screen |
| `demo-4000-og.png`  | 4000  | Artisans-style grass level |
| `demo-5000-og.png`  | 5000  | grass level, second view |
| `demo-9000-og.png`  | 9000  | orange/canyon level |
| `demo-14000-og.png` | 14000 | blue/dusk level |
| `demo-18000-og.png` | 18000 | dark-green level |
| `demo-19000-og.png` | 19000 | teal/water scene |
| `freeze-1185-og.png`| 1185  | pause freeze backdrop (green tint), via `bk2/pause-menu.bk2` |

Frames were chosen from a scout sweep (every 1000 frames) to be visually
**distinct** — the title recurs between demo segments; frames 8000/13000 are
loading/fade (near-black) and were excluded.

## Regenerate

```sh
BIN=build/linux-release/games/spyro-1/openpete-spyro1
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
REF=tests/golden/spyro-1/psycross-ref

# demo scenes (one headless run)
$BIN --headless --disc $DISC --run-recomp 0x80012204 \
  --screenshot-on-frame 2000,4000,5000,9000,14000,18000,19000 \
  --screenshot-output $REF/demo-{n}.png --frame-limit 19100

# pause-freeze scene
$BIN --headless --disc $DISC --run-recomp 0x80012204 \
  --skip-to-level 10 --bk2 games/spyro-1/bk2/pause-menu.bk2 \
  --screenshot-on-frame 1185 --screenshot-output $REF/freeze-{n}.png --frame-limit 1240
```

Re-bless only when a PsyCross-render change is intentional; review the diff.
