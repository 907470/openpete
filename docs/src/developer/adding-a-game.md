# Adding a game

How a PSX title becomes an `openpete-<game>` executable. Three ports exist:
`games/spyro-1/` (the first, with the mod SDK and the distribution
pipeline), `games/spyro-2/` and `games/spyro-3/` (every vaddr through the
recompiler, no mod SDK). All three draw through the native renderer: the
walks are under `games/common/renderer/`, and each game supplies its engine
layout and decoder addresses under `games/<game>/renderer/`. The second and
third are the template: a fourth game is `games/<game>/` holding a
`recomp.conf`, a `game_extras_impl.c`, a `psyq_game_provides.h`, a
`manifest/` and a `CMakeLists.txt` that calls `openpete_add_game`.

## Prerequisites

- A matching-decomp git repo whose `make` produces a GNU ld linker map
  (`build/psx.map`), `build/PSX.EXE`, the PSYQ object the map places
  (`build/asm/psyq.o`), and the overlay binaries (`build/wad/*.ovl`) if the
  title uses overlays.
- The original disc BIN/CUE for the title.
- `third_party/psxrecomp/build/PSXRecomp` (built from `third_party/psxrecomp`).

## 1. Submodule the decomp

```bash
git submodule add <decomp-url> games/<game>/<game>
```

The nested layout keeps openpete commits separate from decomp commits. The
submodule is read-only from openpete's side.

## 2. `recomp.conf`

A shell fragment of `KEY=value` lines, sourced by `scripts/regen-recomp.sh`
and `scripts/regen-manifest.sh` and parsed by `tools/gen_game_identity.py`.
Copy `games/spyro-2/recomp.conf` and set:

| Key | Meaning |
|---|---|
| `DISPLAY_NAME`, `EXE_FILENAME` | window title; the EXE's name in the disc root |
| `DECOMP_DIR`, `PSX_EXE`, `WAD_DIR`, `ASM_DIR` | decomp paths, repo-relative |
| `OVERLAY_LOAD_ADDR`, `OVERLAY_RANGE` | the one vaddr every overlay loads at; that address through the largest overlay |
| `LIBCD_BLOCK_ADDR`, `CD_READ_CB_ADDR` | libcd's state block; the slot a translated `CdReadCallback` stores the handler in (empty when the game registers through the thunk) |
| `CHANNEL_MANIFEST` | render-channel substitution table; empty for a game without one |
| `MAIN_EXE_THIRD_PASS`, `RA_SPILL_RETURNS`, `LEVEL_ID_ADDR` | recompiler passes and the level-switch word, per the comments in the file |

The build generates `game_identity.c` from these (`game_get_id`,
`game_get_display_name`, `game_get_exe_filename`, `psyq_game_overlay_vaddr`,
`psyq_game_libcd_block_addr`, `psyq_game_cd_read_cb_addr`), so no
hand-written source carries an identity constant.

## 3. Manifests and codegen

```bash
GAME=<game> bash scripts/regen-manifest.sh     # manifest/symbols.toml, globals.toml from psx.map
python3 tools/gen_psyq_names.py ...            # recover PSYQ names for an unnamed psyq.s (see its docstring)
python3 tools/gen_psyq_toml.py --exe games/<game>/<game>/build/PSX.EXE \
        games/<game>/<game>/build/psx.map build/asm/psyq.o \
        games/<game>/manifest/psyq.toml       # PSYQ symbol -> address
GAME=<game> bash scripts/regen-recomp.sh       # generated/: main EXE, overlays, dispatch, stubs
```

`psyq.toml`'s second header line records the options it was generated
with (`--translate` per symbol translated instead of stubbed, among them
the libgte closure); a regeneration repeats them.

`manifest/` holds `psyq.toml`, `psyq_names.toml`, `forced_entries/` (entry
points the recompiler's scanner misses), `asm_dispatch_targets/`,
`ram_diff_expect.toml` for `scripts/check_ram_diff.sh`, and
`disc_boot_order.toml`, the disc cache loader's boot-order profile, generated
from a boot trace ([disc-io.md](disc-io.md#boot-order-profile)).
`symbols.toml` and `globals.toml` are written by `regen-manifest.sh` and
gitignored, as is `generated/`.

## 4. `CMakeLists.txt`

```cmake
include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(<game>
    SOURCES
        <per-game TUs>)
```

`openpete_add_game` builds the recomp module (`<tag>_recomp_module`, the
generated code plus `games/common/overlay_dispatch.c` and `module_main.c`)
and the executable (`openpete-<tag>`, `<tag>` = the id without its hyphen):
the entry TU, `game_extras_impl.c`, the generated identity TU, the shared
PSYQ-to-PsyCross layer with the inert default for every hook the game does
not name in `psyq_game_provides.h`, the freecam body, the write census, and
the generated abort-on-call PSYQ stubs. `MAIN`, `FEATURE_LIST`,
`ICON_SOURCE`, `MODULE_LINKAGE`, `MODULE_SOURCES` and `SYMBOLS_MANIFEST`
override the defaults. `ORCHESTRATOR`, `INTERP_REGIONS`, `TERRAIN_GAME`,
`MOBY_GAME` and `PLAYER_GAME` name the game's native-renderer TUs; each
defaults to an inert `*_none.c` under `games/common/`
(`cmake/openpete_game.cmake:92-112`). A game whose `generated/`
holds no recompiled output is skipped with a warning.

Append the game to `OPENPETE_GAMES` in the top-level `CMakeLists.txt`, or
pass `-DOPENPETE_GAMES="spyro-1;<game>"` at configure time.

## 5. `game_extras_impl.c` and `psyq_game_provides.h`

`core/include/game_extras.h` declares the per-game hooks; every one outside
the generated identity group must be defined, as a stub if nothing is known
yet. Copy `games/spyro-2/game_extras_impl.c`: overlay handling
(`spyro_set_active_overlay_by_magic` on load and after a savestate restore),
the per-tick entries, `psx_override_dispatch_game` (the per-function
intercept, here the freecam freeze and the write census), the gameplay-live
predicate, the player position, and the CLI resolvers.

`psyq_game_provides.h` names the PSYQ hooks (`games/common/psyq/psyq_game_hooks.h`)
the game defines; each unnamed hook takes its inert default from
`games/common/psyq/psyq_game_hooks_default.c`. Defining a hook without naming it
is a duplicate-symbol error; naming one without defining it is an undefined
reference.

## 6. Running

```bash
build/linux-release/games/<game>/openpete-<tag> --headless --no-card \
  --disc games/<game>/<game>/build/<serial>.cue
```

`--no-card` is required for Spyro 2 and 3: their memory-card driver waits on
a line-7 interrupt the host does not deliver yet. A PSYQ entry with no thunk
aborts with its name; `OPENPETE_PSYQ_STUB_CONTINUE=1` logs each distinct one
and continues, which enumerates the untranslated surface in one run.
`scripts/psyq_stub_census.sh <game>` does that across every level the game's
`--skip-to-level` lists and reports which untranslated symbols were reached.

`scripts/check_ram_diff.sh <game>` compares whole RAM against DuckStation at
an anchor, under the game's oracle golden when it exists.

The oracle gate is per game. `games/<game>/manifest/gameplay_state_regions.toml`
lists the rows the golden hashes (derived from the link map by
`tools/derive_state_regions.py`, minus the classes `ram_diff_expect.toml`
names; `--check` re-derives it in ctest); `games/<game>/oracle.conf` is
the capture and gate recipe (golden path, record count, disc, anchor,
recorder options from `psyq.toml`, the flags the run needs, the
music-routing bounds). The golden itself is generated, never committed:
`tests/harness/check_psx_oracle.sh --game <game>` captures it from the pinned
DuckStation fork on the first run and verifies against it after; a ctest
row `psx_oracle_match_<game>` runs it under `ctest -L oracle`. A new game's
acceptance is the gate at PASS over 20000 records with a POKE negative
control failing at its record ([`duckstation-oracle.md`](duckstation-oracle.md)).

## 7. What stays Spyro-1-specific

- The mod SDK and the shared-linkage distribution build are
  `games/spyro-1/` sources listed in its `CMakeLists.txt`. The launcher
  (the disc library, the drop screen, the version selector, the progress
  screen, and the window and Vulkan device it hands the game) is
  `core/launcher/`, called by every game's main; a game describes itself to
  it in an `openpete_launcher_game` (`core/include/openpete_launcher.h`).
- The dragon-rescue golden tier (`tests/golden/spyro-1/dragons/`) exists
  for Spyro 1 alone. The presentation golden itself is per game
  (`tests/golden/<game>/presentation/blessed.tsv` for spyro-1, spyro-2 and
  spyro-3; ctest rows `presentation_golden` and `presentation_golden_spyro3`,
  none for spyro-2), as is the windowed oracle row (`psx_oracle_windowed`,
  `psx_oracle_windowed_spyro-3`).
- `core/` still carries Spyro-named debug knobs (`SPYRO_SCRATCH_MAT`,
  `SPYRO_UNTEX` in `core/config/knob_registry.c`); the `core/` purity check
  refuses bare `0x800XXXXX` literals and Win32-only APIs, not Spyro-specific
  names.
