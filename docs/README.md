# openpete docs

Index of the design/reference docs in this directory. The **source of truth**
for the overall design is `notes/plans/openpete-pc-port.md` (sibling `notes`
repo) plus the per-feature `notes/plans/openpete-*.md`; the pages here are
focused references and TL;DRs.

## Start here

- [`architecture.md`](architecture.md) — one-paragraph design: the three worlds
  (decomp C / recomp C / PsyCross), static recompilation, shared `g_ram`.
  Pointer to the full plan.
- [`building.md`](building.md) — toolchain + CMake presets for native Linux and
  the MinGW Windows cross-build.
- [`headless-runs.md`](headless-runs.md) — driving the binary headless

## Level data (how Spyro 1 stores/loads/runs levels)

- [`level-format.md`](level-format.md) — on-disc WAD layout, the length-prefixed
  "component" container format, the environment + scene parsers, and the
  geometry / texture / collision / object (moby) interiors. Reverse-engineered
  from the matching decomp's actual loader, cross-checked against SpyroEdit.
- [`level-overlays.md`](level-overlays.md) — per-level **code** overlays, how
  they bind to data via the `SetOverlayPointers` callback slots, and how openpete
  recompiles 23 same-address overlays into co-resident C.
- [`level-modding.md`](level-modding.md) — maps the format onto the `openpete-*`
  modification plans and the gameplay-vs-presentation determinism boundary; uses
  the sibling `../SpyroEdit` repo as the working-editor reference.

## Recompiler & system boundaries

- [`manifest-format.md`](manifest-format.md) — the function-authority manifest
  (which symbol resolves to decomp vs recomp); design deferred to Phase 2.
- [`psycross-boundary.md`](psycross-boundary.md) — the PsyCross (PSYQ
  re-implementation) ABI boundary, including LP64/LLP64 type-width pitfalls.

## Determinism, oracle & debugging

- [`duckstation-oracle.md`](duckstation-oracle.md) — DuckStation as the canonical
  real-hardware oracle: capture + the 20k-tick gameplay-state gate.
- [`gte-trace-vs-reference.md`](gte-trace-vs-reference.md) — verifying psxrecomp's
  GTE math against two independent reference implementations.
- [`ram-diff-vs-pcsxr.md`](ram-diff-vs-pcsxr.md) — primitive for the iterative
  debugging loop: diff openpete RAM against pcsx-redux at a chosen point.
- [`missing-geometry-bug.md`](missing-geometry-bug.md) — worked investigation of
  geometry dropping out in Artisans/Stone Hill/Dark Hollow.

## Porting

- [`adding-a-game.md`](adding-a-game.md) — recipe for wiring a new PSX title into
  the game-agnostic `core/` once its decomp/recomp exists.

## Assets

- `phase1/` — captured screenshots/bitmaps from early bring-up milestones.
