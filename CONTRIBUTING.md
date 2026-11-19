# Contributing

openpete is not accepting contributions at this time. Pull requests are
turned off, and a patch sent another way is not reviewed or merged. The
source is published to be read and learned from. This notice is removed
when contributions open.

The rest of this file states the terms and checks that apply once they do.

openpete is licensed under the MIT License (`LICENSE`). A contribution is
accepted under the same licence; no copyright assignment and no separate
contributor agreement.

## Developer Certificate of Origin

Every commit carries a `Signed-off-by:` line (`git commit -s`) with the
author's real name and an email address that reaches them. The sign-off
certifies the Developer Certificate of Origin 1.1
(<https://developercertificate.org/>): the author wrote the change or has
the right to submit it under the project's licence, and understands that
the contribution and the sign-off are public and kept with the project. A
pull request with an unsigned commit is not merged until the commit is
signed off.

## Before opening a pull request

- Build and run from `docs/src/developer/building.md`: `scripts/container.sh
  <engine|decomp <game>|game <game>|windows <game>|shell>` builds each
  surface in a container, and a native build follows the prerequisites
  listed there. `scripts/setup.sh` arms the tracked git hooks (`githooks/`),
  which refuse a commit that breaks the documentation, complexity or
  golden-image rules checked at commit time; a commit from the container
  is made in `scripts/container.sh shell`, which runs it.
- Run the gates the change owes. A change to gameplay, the recompiler, the
  GTE or the state-region manifest runs `tests/harness/check_psx_oracle.sh`;
  a change to rendering runs `tests/harness/check_presentation_golden.sh`
  for every game that links the changed file; a docs-only change runs
  `scripts/build-docs.sh`. The table under Checks lists what each check
  needs; the developer pages describe each gate and how to read its
  verdict.
- Every C/C++ function carries a doxygen comment opening with a one-line
  `@brief`; the doc ratchet refuses a touched function without one.
- A comment describes what the code does and the invariant it keeps, not
  who wrote it or when.
- A commit message states what changed and why; it carries no attribution
  trailer other than the sign-off.

## Checks

`ctest --test-dir build/linux-release -LE slow` runs every CTest row
without the `slow` label. In that set a row whose game binary, disc or
other input is missing exits 64, which CTest reports as skipped.

| Needs | Check | Command |
|---|---|---|
| Nothing beyond the build | Unit tests (`tests/unit/`), the fence and ratchet checks (label `fence`), `pacing_stats`, `channel_registry_audit`, `channel_signatures`, `mod_shader_compile`, `mod_vec_test` | `ctest --test-dir build/linux-release -LE slow` |
| The PSXRecomp build (`third_party/psxrecomp/build/PSXRecomp`) | `recomp_gte_sideeffects` | same |
| A MinGW cross compiler | `sdk_windows_include` | same |
| The disc | The sub-tick, Vulkan validation, run-ahead and terrain-cache checks (`*_subtick_<game>`, `vk_validation_<game>`, `runahead_ident_<game>`, `terrain_walk_cache_<game>`), `mod_imgui` | same |
| The disc | Determinism and savestate checks; `determinism_matrix`, `determinism_soak` and `aspect_screenshot_symmetry` fail, not skip, without it | `ctest --test-dir build/linux-release -L determinism` |
| The disc | Mod ladder | `tests/harness/check_mods.sh` (`ctest -L mods`) |
| The disc, and podman or docker | Presentation golden, captured inside the image `tests/harness/linux-capture.Dockerfile` | `tests/harness/check_presentation_golden.sh [--game G]` |
| The disc and the DuckStation fork (`scripts/setup_duckstation.sh`) | Oracle; the first run captures the golden trace (about 15 minutes) | `tests/harness/check_psx_oracle.sh [--game G]` (`ctest -L oracle`) |
| `mkdocs-material` | Documentation site: runs `tools/gen_mod_docs.py` and `tools/gen_code_docs.py` (the `build/linux-release` compile database), then `mkdocs build --strict`; exits 0 without building the site when mkdocs is not installed | `scripts/build-docs.sh` |

## Code conventions

- An exported function takes the `openpete_` prefix. `rhi_` names the
  render hardware interface under `core/rhi/`. `op_` names the host
  utility headers: time and period arithmetic (`openpete_time.h`), the
  profiler (`openpete_profile.h`), atomic file writes
  (`openpete_atomic_file.h`), string helpers (`openpete_str.h`), function
  tracing (`openpete_func_trace.h`), the mod API's guest memory accessors
  (`openpete_mod_api.h`), WAV loading and sound-effect identity
  (`core/host/`). Game code under `games/` also uses subsystem prefixes
  (`moby_`, `player_`, `psyq_`).
- A new header uses an `#ifndef` include guard; 134 of the 153 headers
  under `core/` and `games/` do, the other 19 use `#pragma once`.

## What is not accepted

- Game data: disc images, extracted assets, savestates or recordings that
  contain copyrighted game bytes. The repository holds no game content and
  the hooks refuse a tracked image under `tests/golden/`.
- Third-party code without its licence text: a new dependency adds its
  row to `NOTICE.md` and `packaging/licences/manifest.tsv` in the same change,
  and packaging refuses a row with no text.
