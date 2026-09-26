# Native `-vk` draw-tick golden (W1.2)

The golden of the `native_visual_regression` ctest
(`tests/harness/check_native_visual.sh`): the native Vulkan renderer's output
at seven attract-demo draw-ticks, captured headless through the software
rasterizer (surfaceless Vulkan on lavapipe; no GPU, no display) inside the
pinned capture image (`docs/linux-capture.Dockerfile`).

The verdict is the digest manifest `games/spyro-1/native-tick-corpus.manifest`,
in the `blessed.tsv` form of the presentation golden (tile, verdict digest,
blessed revision, recomp input digest, generated-tree digest, origin; the
header records the capture environment). No image is tracked here: the tiles
live in the per-user reference store (`scripts/presentation-blessed.py`). The
capture is byte-deterministic on the pinned rasterizer, so the compare is exact
and carries no tolerance.

## Corpus

Deterministic attract demo, keyed on canonical draw-ticks
(`--screenshot-on-tick`), not VBLs: the VBL-to-tick mapping drifts with host
timing (CD-load VSync spins, the audio backend), while the Nth canonical
present is the same sim moment in every run.

| Row | Tick | Scene |
|-----|------|-------|
| `demo-tick-1001-vk.png`  | 1001  | attract title screen |
| `demo-tick-2194-vk.png`  | 2194  | Artisans-style grass level |
| `demo-tick-2694-vk.png`  | 2694  | grass level, second view |
| `demo-tick-4888-vk.png`  | 4888  | orange/canyon level |
| `demo-tick-7581-vk.png`  | 7581  | blue/dusk level |
| `demo-tick-9775-vk.png`  | 9775  | dark-green level |
| `demo-tick-10275-vk.png` | 10275 | teal/water scene |

The pause-freeze backdrop is not a row. It is drawn from `frozen_tex`, which is
captured only in the live-present path (`compose_variant`), a no-op under
`--headless`, so the headless screenshot path cannot reproduce it.

## Check and re-bless

```sh
tests/harness/check_native_visual.sh        # enters the capture image itself
```

Re-bless only an intended native-render change, after adjudicating the heat
diffs the check writes. Promote the kept candidate inside the image; the
promote rewrites the changed rows, files the tiles into the store and records
the capture environment in the manifest header:

```sh
scripts/golden-capture-container.sh run -- \
  python3 scripts/presentation-blessed.py promote spyro-1 --tier native-vk \
  build/native-visual-candidate --all
```
