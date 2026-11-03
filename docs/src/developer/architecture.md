# Architecture

openpete runs Spyro the Dragon (SCUS-94228), Spyro 2: Ripto's Rage!
(SCUS-94425) and Spyro: Year of the Dragon (SCUS-94467) as native code. There
is no emulator: the game's MIPS code is translated to C at build time, the PSYQ
system library is replaced by a host re-implementation, and a native Vulkan
renderer draws the scene from gameplay state. The binding constraint: every
gameplay-readable byte matches SCPH-1001 hardware, tick for tick, under any
presentation layer.

## Repository layout

- `core/` - game-agnostic runtime; no Spyro-specific address or symbol.
  `psx/` (CPU state, dispatch, BIOS, CD-ROM, the disc reader and
  [disc cache](disc-io.md)), `runtime/` (run
  loop, ticker, present thread), `renderer/` and `rhi/` (native renderer
  over Vulkan), `input/`, `host/` (memory cards, music and sfx overrides),
  `record/` (bk2 movies, savestates, rewind, video), `config/` (options,
  config file, knob registry), `ui/` (settings menu, dev overlay,
  notifications, free camera), `diag/` (logs, traces, profiling), `mods/`,
  `include/`.
- `games/<game>/` - one game: `recomp.conf`, `manifest/`, `generated/`
  (recompiler output, gitignored), `game_extras_impl.c`,
  `module_imports.h`, per-game `features/`, `renderer/`, `corpus/` and
  `bk2/`; `games/<game>/<game>/` is the matching-decomp submodule.
  `games/common/` holds code shared by the games and compiled once (the
  process entry, PSYQ thunks, overlay dispatch, the default hooks, the
  shared renderer walks).
- `mods/` - shipped and example mods.
- `tests/` - `golden/`, `harness/`, `unit/`, `recomp/`, `bench/`,
  `freecam/`, `state-baselines/`.
- `scripts/` - build, gate and release scripts; `scripts/fences/` holds
  the mechanical rule checks.
- `tools/` - code generators and analysis scripts.
- `third_party/` - vendored dependencies (psxrecomp, libco, stb, volk,
  Vulkan headers, miniz, tomlc99, xxhash, dear_bindings, the MinGW OpenAL
  prebuilt) and submodules (PsyCross, SDL, cubeb, duckstation, glslang,
  imgui).
- `docs/` - this site: `docs/mkdocs.yml` and the source under `docs/src/`.
- `packaging/` - the release inputs: the shipped README template and
  configuration, the AppImage metadata, the licence texts and the release
  container recipe.

## Code that runs the game

Two bodies of C run in one process and share one address space model.

- **Recompiled C.** `third_party/psxrecomp/recompiler` translates `PSX.EXE`
  and each level overlay into C ahead of time; there is no JIT. The main EXE
  emits `games/spyro-1/generated/PSX.EXE_full.c` (the function bodies) and
  `PSX.EXE_dispatch.c` (the vaddr -> function table). Each overlay is
  translated with its own `--func-prefix` into
  `generated/<level>/*.bin_full.c` and `*.bin_dispatch.c` so that overlays
  sharing one PSX load address coexist at link time;
  `games/common/overlay_dispatch.c` routes a call to the resident overlay.
  Regenerate with `scripts/regen-recomp.sh`.
- **PsyCross.** `third_party/PsyCross` re-implements the PSYQ system library
  (`libgpu`, `libcd`, `libspu`, ...) on SDL and OpenGL. Its ABI boundary,
  including the `u_long` and `OT_TAG` width traps, is documented in
  `psycross-boundary.md`; the fork's per-change record is under `psycross/`.

Every function of the game runs as recompiled code, in every preset. The
matching decompile supplies the recompiler's inputs (`PSX.EXE`, the
overlays, `asm/` and `psx.map`); its C is not compiled into the port.

## Shared memory

`core/psx/cpu.c:28` defines `g_ram`, the 2 MiB main RAM array, plus a
1 KiB scratchpad. Recompiled code and host code address the same buffer:
recompiled code through the `psx_runtime.h` load/store callbacks bound in
`core/psx/cpu.c`, host code through `openpete_psx_get_ram()`.
`g_ram` is page-aligned so the sub-tick view can `mprotect` it.

Dispatch glue lives in `core/psx/dispatch.c`: `call_by_address` for
jump-register targets, `psx_override_dispatch` for per-vaddr interception,
and `psx_syscall`. Core holds no link-time reference to the recompiled code;
the game registers a descriptor through `openpete_recomp_register()`, which
is what lets the recompiled code be a separate module: under shared linkage
(`linux-release`, the launcher presets) each game's is `<tag>-recomp.so`
(`.dll`) beside the executable, loaded at boot by
`games/common/recomp_attach_shared.c`.

## Execution model

The game runs on one libco fiber created by `run_game_fiber_create`
(`core/runtime/run.c`) when the host loop's session opens; the host loop
drives it and the game yields back through `openpete_yield_to_host()`
(`core/psx/fiber.c`). Additional fibers are
added only if the game's IRQ semantics force it. Clocks are NTSC-exact: tick
= 1125000/37609 Hz, VBL = 2250000/37609 Hz.

## Host loop

`run_host_loop` (`core/runtime/host_loop.c`) runs on the sim thread (the
worker under the thread split, main otherwise). It opens a session, runs
iterations until the session's exit reason is set, and closes the session.
The loop body is the phase sequence, one call per phase:

| Phase | Function | Work |
|---|---|---|
| Iteration start | `host_iter_begin` | heartbeat; the live tick-scale, render-fps, mod-set and runahead changes; the unlock peek; `sim_due` |
| Override arbitration | `host_input_arbitrate` | the bk2 row stamp (sim-due iterations only), the overlays' neutral stamp and suspension, the cursor; the movie's end leaves the loop before the tick, with exit reason `bk2-end` |
| UI poll | `host_ui_poll` | rebind drain, keymap poll, dev overlay, settings menu, settings save |
| Quiescent ops | `host_quiescent_ops` | with the settings menu shut: the savestate queue, state stress, rewind, the handoff, the bk2 restamp, pause and step, the host hotkeys |
| Input latch | `host_input_latch` | event pump, publish, sample, latch, the input record's staging |
| Tick | `host_tick` | the yield decision and the VBL in the game fiber inside the pace-log bracket; on a tick the game hook, the runahead lead, the interp captures and the runahead settle |
| Tick outputs | `host_tick_outputs` | position and gameplay hash log rows, the input log commit, the `--debug-triangle` re-issue |
| Present | `host_present` | the held-present gate, the canonical present of a drawn tick, the eager or inline strategy between ticks, the paused repaint |
| RAM dump | `host_ram_dump_if_due` | `--ram-dump-on-frame` |
| Exit test | `host_exit_reason` | the psxtrace verdict, a requested shutdown, the game returning, `--frame-limit`, `--record-frames`, `--max-runtime-seconds` |
| Sample | `host_loop_sample` | on sim-due iterations, the loop sample every 64th tick and the primitive counter reset |
| Pace tail | `host_pace_tail` | the VBL pace record, the unlock verdict and its audio sync, the paused idle sleep, the pacer's advance and sleep |

The order is the contract. The live pacer changes precede `sim_due`,
because a regrid moves the deadline `sim_due` reads. The quiescent ops
precede the input latch, because a savestate load rewrites the pad buffer.
The interp captures are taken at the runahead lead's head and the rollback
precedes every other RAM read, so the tick outputs describe the committed
timeline. The present never runs inline while the present thread holds the
display. The unlock verdict follows the present, which consumed the previous
iteration's swap-interval flag, and the pacer runs last. The declaration of
`run_host_loop` in `core/runtime/host_loop_internal.h` lists every ordering
constraint.

State has three lifetimes:

- **Session** (`host_session_t`, `core/runtime/host_loop_internal.h`): one
  per run. `host_session_open` runs the setup in order and records each
  resource it opened; a failed step sets an exit reason naming the step
  (`setup-fiber`, `setup-bk2-load`, ...). `host_session_close` runs the
  teardown in order: a release step runs on every path, an output step
  (reports, the psxtrace verdict, the bk2 movie write) only once the loop
  has entered, so a setup error writes no zero-row movie and no verdict.
  The session holds the options, the thread split, the run start, the exit
  reason and code, the pause and step flags, the pacer, the bk2 session,
  the output files and the canonical-present stamp the world-liveness test
  reads.
- **Iteration** (`host_iter_t`): zeroed by `host_iter_begin`. It holds
  `sim_due`, the unlock peek, the override claim, the rewind step, the
  yield and tick outcomes, whether a canonical present was made, and the
  VBL's pace record.
- **Subsystem**: state one subsystem uses is a file-scope static of that
  subsystem's file. A latch carried across
  iterations by one phase lives in its file's latch struct, reset by an
  `_init` that `host_session_open` calls.

Each session and iteration field carries a comment naming its owning phase
and any phase that appends to it. Phase functions take the session and the
iteration; a phase that only reads takes `const`. The subsystem modules
(`eager.c`, `runahead.c`, `host_pacer.c`, `host_state_ops.c`) never see the
session and take values or their own state.

| File | Role |
|---|---|
| `host_loop.c` | the loop, the iteration start, the tick, the quiescent ops, the tick outputs, the RAM dump, the exit test, the sample and the pace tail |
| `host_session.c` | session open and close, the exit reports of the session's outputs |
| `host_pacer.c` | the sim and render deadlines and periods, tick scale, the slip clamp, the unlock verdict (refused under the `--record-cfr` capture clock), the sleep |
| `host_input_tick.c` | override arbitration, bk2 stamping and replay load, the latch and staging, the input log commit and its census |
| `host_hotkeys.c` | the UI poll, the host hotkeys (toggles, fill-tag inspector, debug cameras), pause and step, the paused repaint |
| `host_state_ops.c` | the savestate queue, the state-stress harnesses, the rewind driver |
| `runahead.c` | runahead arming, the lead and the settle (outcome none, speculated, adopted or left), the prediction check |
| `host_present.c` | the present phase, the world-liveness and sub-tick-liveness predicates, the inline shed predictor |
| `eager.c` | eager production: the span plan at each canonical tick and the scenes published to the present thread's ring |
| `present_thread.c` | the present thread, and the display borrow and return around a blocking read on the sim thread |
| `run.c` | the entry points, the event pump, `run_host_init`, the thread split, the gameplay hash, the live-knob queue, the mute and audio feed |
| `host_trace.c` | the decision trace |

`OPENPETE_LOOP_TRACE=<path>` writes one 64-byte record per iteration (the
gates, the present made, the runahead outcome, the exit reason, the VBL pace
flags, the pacer deadlines) and the teardown steps in the order they ran;
the layout is in `core/include/openpete_host_trace.h`. Traces of two builds
on one run compare decision for decision; the wall-clock fields (the
`NOT_LIVE` and `SLIPPED` pace flags, the deadlines) compare only under the
`--record-cfr` virtual clock. `tests/harness/check_loop_trace.sh` runs the
trace rows against a blessed digest
([Loop decision trace](unattended-runs.md#loop-decision-trace)).

## Presentation

`--renderer` selects the rasterizer (`core/config/options.c:956-963`):
`vk` (the native Vulkan renderer) or `psycross` (the legacy GL path, shown
in the same native window). Under `--headless` the native renderer stays
off and PsyCross rasterizes. The default is `vk` on every
game; a game that must start elsewhere calls
`openpete_options_set_default_renderer` before parsing, and `--help` names
each game's own default.

The native renderer extracts scenes from gameplay state and draws them in
float through openpete's own RHI (`core/rhi/rhi_vulkan.c`), with the
per-game feeders in `games/spyro-1/renderer/spyro.c`. It reads
gameplay state, asset data, and the VRAM mirror; it never writes PSX-readable
RAM. Presentation features that do reach the recompiled path - Hor+
widescreen, extended LOD - are `feature_module_t` entries registered per game
(`core/renderer/no_lod.c`, `games/spyro-1/feature_list.c`) and are presumed to leak
into gameplay state until the oracle clears them.

## Per-game boundary

`core/` contains nothing Spyro-specific. Each game defines one runtime
descriptor (`openpete_game_runtime_t`, `core/include/openpete_game_runtime.h`)
in `games/<game>/game_extras_impl.c`; core reaches the game only through it,
and the `core/include/game_extras.h` entries are forwarders over it. The
build derives a second record per game, `<tag>_game_build`
(`games/common/game_build.h`): its entry, module name, host-to-module
forwarders and manifest tables. `OPENPETE_GAMES` (`CMakeLists.txt:60`)
builds `games/spyro-1/`, `games/spyro-2/` and `games/spyro-3/` by default.
On Linux under shared linkage they link into one executable,
`build/<preset>/openpete`, whose entry (`games/common/game_entry.cpp`)
selects the game from the disc serial, `--game` or the invoked name and
installs its descriptor; on Windows each game still has its own
executable. [Adding a game](adding-a-game.md) walks the files.

## Mods

`core/mods/` loads mods as shared libraries against the ABI in
`core/include/openpete_mod_api.h`, with services for shaders, textures,
sound, draw, and configuration. `tools/gen_mod_sdk.py` generates the
distributable SDK. The shipped mods are under `mods/`; author documentation
is the [Modding section](../modding/index.md), source `docs/src/modding/`.

## Level data

How Spyro 1 stores, loads, and runs its levels, reverse-engineered from the
matching decomp's loader and cross-checked against SpyroEdit:

- [`level-format.md`](../spyro/level-format.md) - on-disc WAD layout, the length-prefixed component
  container format, the environment and scene parsers, and the geometry,
  texture, collision, and object (moby) interiors.
- [`level-overlays.md`](../spyro/level-overlays.md) - per-level code overlays, how they bind to data via
  the `SetOverlayPointers` callback slots, and how openpete recompiles
  same-address overlays into co-resident C.

## Function comments

Every function in `core/` and `games/` carries a doxygen comment (`/** */`,
`/*! */`, `///` or `//!`; a plain `/*` or `//` block does not attach) at one
site: the declaration in the project header the defining file includes, or
the definition when no such header declares it; a `static inline` function in
a header is documented at its definition, and a comment at both sites is a
duplicate. The comment opens with a sentence stating what the function does
(or `@brief`); a function with external linkage also carries `@param <name>`
for every named parameter of the documented declaration and `@return` when it
returns a value, while a static function needs only the brief and any tag it
carries must be correct. A comment attaches only across blank lines, other
comments and attributes: a `#` directive, or a line ending in `;`, `{` or
`}`, between the comment and the declaration leaves it attached to nothing,
so the comment goes below the directive. A group header (`@name`, `@{`) is
not a function's comment. The text states what the code does, the invariant
it keeps and the problem it solves, never its history.
`scripts/fences/doc_ratchet.py` checks form, placement and tags with
libclang in the pre-commit hook for every function a commit adds or edits;
the functions not yet documented are the rows of
`baselines/docs/undocumented.tsv`.
