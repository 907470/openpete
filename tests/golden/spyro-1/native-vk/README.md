# Native `-vk` render golden archive (W1.2)

Frozen output of the **native Vulkan renderer** over a curated set of scenes,
captured **headless** through a software rasterizer (surfaceless Vulkan / lavapipe
— plan W1.1, no GPU and no display). These are the goldens for the
`native_visual_regression` ctest (`scripts/check_native_visual.sh`).

## What these are (and aren't)

- `demo-<frame>-vk.png` is the native renderer's `native_rtt` readback
  (`openpete_native_render_screenshot_all`, the `-vk` variant) at reference
  resolution.
- The gate is **native-vs-native**: the candidate is the same native pipeline on
  the same software rasterizer as the golden, so a matching build reproduces them
  ~exactly. The tolerance (`center_crop_diff.py` threshold + count) only absorbs
  minor run-to-run FP/order noise — it is **not** a fidelity budget against
  PsyCross. For native-vs-PsyCross *fidelity* comparison, use the
  `../psycross-ref` archive with `--renderer ab`/`diff` (where the native scene
  is frame-delayed to match PsyCross's lagged present).
- Determinism note: these are captured under lavapipe with a fixed ICD. A large
  GPU/driver change could perturb pixels; the gate forces the software
  rasterizer (`VK_ICD_FILENAMES`/`MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`) so it
  stays deterministic in CI regardless of host GPU.

## Corpus

Deterministic attract demo (`--run-recomp 0x80012204`), the SAME (scene, frame)
pairs as `../psycross-ref`, so the two archives line up scene-for-scene:

| File | Frame | Scene |
|------|-------|-------|
| `demo-2000-vk.png`  | 2000  | attract title screen |
| `demo-4000-vk.png`  | 4000  | Artisans-style grass level |
| `demo-5000-vk.png`  | 5000  | grass level, second view |
| `demo-9000-vk.png`  | 9000  | orange/canyon level |
| `demo-14000-vk.png` | 14000 | blue/dusk level |
| `demo-18000-vk.png` | 18000 | dark-green level |
| `demo-19000-vk.png` | 19000 | teal/water scene |

**`freeze-1185` is intentionally absent.** The pause-freeze backdrop is drawn
from `frozen_tex`, which is captured only in the live-present path
(`compose_variant`) — a no-op under `--headless` — so it cannot be reproduced by
the headless screenshot path. Covering it needs a live window (WSLg / real GPU),
out of scope for this CI gate.

## Regenerate (re-bless)

Re-bless only when a native-render change is intentional; review the diffs first.

```sh
scripts/check_native_visual.sh --build-dir=build/linux-release --bless
```

Verify with a plain (compare) run before committing:

```sh
scripts/check_native_visual.sh --build-dir=build/linux-release
```
