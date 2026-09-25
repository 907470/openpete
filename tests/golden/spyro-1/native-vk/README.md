# Native `-vk` render golden archive (W1.2)

Frozen output of the **native Vulkan renderer** over a curated set of scenes,
captured **headless** through a software rasterizer (surfaceless Vulkan / lavapipe
— plan W1.1, no GPU and no display). These are the goldens for the
`native_visual_regression` ctest (`tests/harness/check_native_visual.sh`).

## What these are (and aren't)

- `demo-tick-<tick>-vk.png` is the native renderer's `native_rtt` readback
  (`openpete_native_render_screenshot_all`, the `-vk` variant) at reference
  resolution.
- The gate is **native-vs-native**: the candidate is the same native pipeline on
  the same software rasterizer as the golden, so a matching build reproduces them
  ~exactly. The tolerance (`center_crop_diff.py` threshold + count) only absorbs
  minor run-to-run FP/order noise — it is **not** a fidelity budget against
  PsyCross. For native-vs-PsyCross *fidelity* comparison, use the
  `../psycross-ref` archive; the live side-by-side and colour-diff renderer
  modes that once served this were retired 2026-09-10.
- Determinism note: these are captured under lavapipe with a fixed ICD. A large
  GPU/driver change could perturb pixels; the gate forces the software
  rasterizer (`VK_ICD_FILENAMES`/`MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`) so it
  stays deterministic in CI regardless of host GPU.

## Corpus

Deterministic attract demo, the SAME sim moments as
`../psycross-ref` so the two archives line up scene-for-scene — but keyed on
**canonical draw-ticks** (`--screenshot-on-tick`), not VBL frames. Frame-keyed
captures forked run-to-run because the VBL↔tick mapping drifts with host timing
(CD-load VSync spins, audio backend — the 2026-07-01 gate flake); the Nth
canonical present is the same sim moment in every run. The "frame" column is
the psycross-ref frame number each tick corresponded to at the 2026-07-02
capture (nominal — the mapping is host-timing-dependent, which is the point).

| File | Tick | ~Frame | Scene |
|------|------|--------|-------|
| `demo-tick-1001-vk.png`  | 1001  | 2000  | attract title screen |
| `demo-tick-2194-vk.png`  | 2194  | 4000  | Artisans-style grass level |
| `demo-tick-2694-vk.png`  | 2694  | 5000  | grass level, second view |
| `demo-tick-4888-vk.png`  | 4888  | 9000  | orange/canyon level |
| `demo-tick-7581-vk.png`  | 7581  | 14000 | blue/dusk level |
| `demo-tick-9775-vk.png`  | 9775  | 18000 | dark-green level |
| `demo-tick-10275-vk.png` | 10275 | 19000 | teal/water scene |

**`freeze-1185` is intentionally absent.** The pause-freeze backdrop is drawn
from `frozen_tex`, which is captured only in the live-present path
(`compose_variant`) — a no-op under `--headless` — so it cannot be reproduced by
the headless screenshot path. Covering it needs a live window (WSLg / real GPU),
out of scope for this CI gate.

## Regenerate (re-bless)

Re-bless only when a native-render change is intentional; review the diffs first.

```sh
tests/harness/check_native_visual.sh --build-dir=build/linux-release --bless
```

Verify with a plain (compare) run before committing:

```sh
tests/harness/check_native_visual.sh --build-dir=build/linux-release
```
