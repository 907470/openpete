# Architecture

The full design lives at `notes/plans/openpete-pc-port.md` (in the
sibling `notes` repo). That plan is the source of truth — this page is a
short pointer + a TL;DR.

## TL;DR

- **Three worlds.** Decomp C (matching, byte-identical to the original
  build) → Recomp C (psxrecomp's translation of MIPS for everything not yet
  decomped) → PsyCross (SDL2/GL re-implementation of the PSYQ system
  library). All three share the 2 MiB `g_ram` buffer.
- **Static recompilation.** psxrecomp processes `PSX.EXE` once at build
  time and emits `spyro_full.c` + `spyro_dispatch.c`. No JIT.
- **Cooperative threading via libco.** One fiber by default; more added
  only if/when the game's IRQ semantics force it.
- **Game-agnostic core.** `core/` knows nothing Spyro-specific; per-game
  data routes through `core/include/game_extras.h` and is implemented in
  `games/<game>/game_extras_impl.c`.

For everything else — phase plan, manifest format, thunk codegen,
overlay handling, `game_extras.h` design rules, `g_ram` semantics, build
pipeline — see the plan.

## Level data

How Spyro 1 stores, loads, and runs its levels — reverse-engineered from the
matching decomp's actual loader and cross-checked against SpyroEdit:

- **`level-format.md`** — on-disc WAD layout, the length-prefixed "component"
  container format, the environment/scene parsers, and the geometry / texture /
  collision / object (moby) interiors.
- **`level-overlays.md`** — per-level **code** overlays, how they bind to data
  via the `SetOverlayPointers` callback slots, and how openpete recompiles 23
  same-address overlays into co-resident C.
- **`level-modding.md`** — how the above maps onto the `openpete-*` modification
  plans (widescreen, `--no-lod`, transform-interp, RAM virtualization, custom
  levels) and the gameplay-vs-presentation determinism boundary.
