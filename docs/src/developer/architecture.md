# Architecture

openpete runs Spyro the Dragon (SCUS-94228) as a native executable. There is
no emulator: the game's MIPS code is translated to C at build time, the PSYQ
system library is replaced by a host re-implementation, and a native Vulkan
renderer draws the scene from gameplay state. The binding constraint is the
north star — every gameplay-readable byte matches
SCPH-1001 hardware, tick for tick, under any presentation layer.

## Repository layout

- `core/` — game-agnostic runtime; no Spyro-specific address or symbol.
  `psx/` (CPU state, dispatch, BIOS, CD-ROM, disc cache), `runtime/` (run
  loop, ticker, present thread), `renderer/` and `rhi/` (native renderer
  over Vulkan), `input/`, `host/` (memory cards, music and sfx overrides),
  `record/` (bk2 movies, savestates, rewind, video), `config/` (options,
  config file, knob registry), `ui/` (settings menu, dev overlay,
  notifications, free camera), `diag/` (logs, traces, profiling), `mods/`,
  `include/`.
- `games/<game>/` — one executable per game: `recomp.conf`, `manifest/`,
  `generated/` (recompiler output, gitignored), `game_extras_impl.c`,
  per-game `features/`, `renderer/` and `corpus/`; `games/<game>/<game>/`
  is the matching-decomp submodule. `games/common/` holds code shared by
  the games (PSYQ thunks, overlay dispatch, the default hooks).
- `mods/` — shipped and example mods; `mods-attic/` — retired mods.
- `tests/` — `golden/`, `harness/`, `unit/`, `recomp/`, `bench/`,
  `freecam/`, `state-baselines/`.
- `scripts/` — build, gate and release scripts; `scripts/fences/` holds
  the mechanical rule checks.
- `tools/` — code generators and analysis scripts.
- `third_party/` — vendored dependencies (psxrecomp, libco, glad, stb,
  volk, Vulkan headers, miniz, tomlc99, xxhash, among others) and
  submodules (PsyCross, SDL, cubeb, duckstation, glslang, imgui).
- `docs/` — this site's source under `docs/src/`, the modding site under
  `docs/modding/`, and the release inputs.

## Code that runs the game

Three bodies of C are linked into one binary and share one address space
model.

- **Recompiled C.** `third_party/psxrecomp/recompiler` translates `PSX.EXE`
  and each level overlay into C ahead of time; there is no JIT. The main EXE
  emits `games/spyro-1/generated/PSX.EXE_full.c` (the function bodies) and
  `PSX.EXE_dispatch.c` (the vaddr → function table). Each overlay is
  translated with its own `--func-prefix` into
  `generated/<level>/*.bin_full.c` and `*.bin_dispatch.c` so that overlays
  sharing one PSX load address coexist at link time;
  `games/spyro-1/dispatch_chain.c` routes a call to the active overlay.
  Regenerate with `scripts/regen-recomp.sh`.
- **Rewritten decomp C.** Functions with a matching decompile in the sibling
  `spyro-1/src/` can be compiled directly instead of recompiled. `tools/rewrite_tu.py`
  (passes under `tools/rewrite/`) rewrites a whole translation unit for an
  LP64/LLP64 host — pointer widths, struct relayout, PSYQ boundary
  stride — and the result lands in `generated/rewritten/<tu>.rewritten.c`.
  Which TUs take this path, and which globals and calls need link-time
  aliasing or dispatch interception, is curated in
  `games/spyro-1/manifest/native.toml`. The unit is the translation unit, not
  the function; there is no per-function `kind` switch.
- **PsyCross.** `third_party/PsyCross` re-implements the PSYQ system library
  (`libgpu`, `libcd`, `libspu`, …) on SDL and OpenGL. Its ABI boundary,
  including the `u_long` and `OT_TAG` width traps, is documented in
  `psycross-boundary.md`; the fork's per-change record is under `psycross/`.

Decomp mode is work in progress. `linux-release`, `windows-mingw`, and `wasm`
pin `OPENPETE_SKIP_DECOMP=ON` (`cmake/presets/linux-release.cmake:4`), which
drops the rewritten TUs and routes every vaddr through the recompiler;
`linux-debug` leaves it OFF.

## Shared memory

`core/psx/cpu.c:26` defines `g_ram`, the 2 MiB main RAM array, plus a
1 KiB scratchpad. All three bodies of code address the same buffer:
recompiled code through the `psx_runtime.h` load/store callbacks bound in
`core/psx/cpu.c`, rewritten decomp C through link-time
`--defsym=<symbol>=g_ram+<offset>` aliases generated from
`manifest/globals.toml`, and host code through `openpete_psx_get_ram()`.
`g_ram` is page-aligned so the sub-tick view can `mprotect` it.

Dispatch glue lives in `core/psx/dispatch.c`: `call_by_address` for
jump-register targets, `psx_override_dispatch` for per-vaddr interception,
and `psx_syscall`. Core holds no link-time reference to the recompiled code;
the game registers a descriptor through `openpete_recomp_register()`, which
is what lets the distribution presets ship the recompiled code as a separate
module.

## Execution model

The game runs on one libco fiber created in `core/runtime/run.c:2480`; the
host loop drives it and the game yields back through
`openpete_yield_to_host()` (`core/psx/fiber.c`). Additional fibers are
added only if the game's IRQ semantics force it. Clocks are NTSC-exact: tick
= 1125000/37609 Hz, VBL = 2250000/37609 Hz.

## Presentation

`--renderer` selects the display path (`core/config/options.c:727`):
`vk` (the native Vulkan renderer), `psycross` (the legacy GL path), `ab`, and
`diff`. Headless runs fall back to PsyCross. The default is `vk` on every
game; a game that must start elsewhere calls
`openpete_options_set_default_renderer` before parsing, and `--help` names
each game's own default.

The native renderer extracts scenes from gameplay state and draws them in
float through openpete's own RHI (`core/rhi/rhi_vulkan.c`), with the
per-game feeders in `games/spyro-1/renderer/spyro.c`. It reads
gameplay state, asset data, and the VRAM mirror; it never writes PSX-readable
RAM. Presentation features that do reach the recompiled path — Hor+
widescreen, extended LOD — are `feature_module_t` entries registered per game
(`core/renderer/no_lod.c`, `games/spyro-1/feature_list.c`) and are presumed to leak
into gameplay state until the oracle clears them.

## Per-game boundary

`core/` contains nothing Spyro-specific. Entry point, CRC, overlay layout,
disc layout, and runtime quirks are declared in `core/include/game_extras.h`
and implemented in `games/<game>/game_extras_impl.c`. Spyro 1 is the only
implemented game; `games/spyro-2/` and `games/spyro-3/` hold TODO notes and,
for Spyro 3, generated recompiler output used by model-porting tools.

## Mods

`core/mods/` loads mods as shared libraries against the ABI in
`core/include/openpete_mod_api.h`, with services for shaders, textures,
sound, draw, and configuration. `tools/gen_mod_sdk.py` generates the
distributable SDK. The shipped mods are under `mods/`; author documentation
is under `modding/`.

## Level data

How Spyro 1 stores, loads, and runs its levels, reverse-engineered from the
matching decomp's loader and cross-checked against SpyroEdit:

- [`level-format.md`](../spyro/level-format.md) — on-disc WAD layout, the length-prefixed component
  container format, the environment and scene parsers, and the geometry,
  texture, collision, and object (moby) interiors.
- [`level-overlays.md`](../spyro/level-overlays.md) — per-level code overlays, how they bind to data via
  the `SetOverlayPointers` callback slots, and how openpete recompiles
  same-address overlays into co-resident C.

## Where the design lives

The port's origin plan, `notes/plans/openpete-pc-port.md`, is superseded
and is kept as history. The live plans are
`notes/plans/openpete-source-rewriter.md` (decomp migration),
`notes/plans/openpete-native-renderer.md` (the Vulkan renderer),
`notes/plans/openpete-presentation-decoupling.md` (the presentation-to-gameplay
channels), and the per-feature `notes/plans/openpete-*.md`, indexed by
`notes/plans/INDEX.md`.
