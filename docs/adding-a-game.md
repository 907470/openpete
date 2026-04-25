# Adding a game (Spyro 2, Spyro 3, …)

This guide is the recipe for wiring a new PSX title into OpenPete once a
matching decomp exists for it. The Spyro 1 wiring at `games/spyro-1/` is
the worked example; the steps below mirror that layout.

## Prerequisites

- A matching-decomp git repo whose `make` produces a linker map
  (`build/psx.map`) and the PSX EXE artifact (`build/PSX.EXE` or similar).
- The original disc BIN/CUE for the title.

## 1. Submodule the decomp

```bash
git submodule add <decomp-url> games/<game>/<game>
```

The nested layout (`games/<game>/<game>/`) keeps OpenPete commits cleanly
separated from decomp commits — see `notes/plans/openpete-pc-port.md` §3
("Avoid path crossings").

## 2. Skeleton

Mirror Spyro 1's:

```
games/<game>/
├── CMakeLists.txt              # produces openpete-<game> binary
├── <game>/                     # the decomp submodule
├── manifest/
│   ├── symbols.toml            # function-authority manifest (Phase 2)
│   ├── globals.toml
│   ├── overlays.toml
│   └── psyq.toml               # PSYQ symbol → address (from psx.map)
├── generated/                  # codegen output; gitignored
├── compat/
│   ├── psx_globals.h           # globals as offsets into g_ram
│   └── psx_stubs.h             # forward decls of recomp'd functions
├── game_extras_impl.c          # implements core/include/game_extras.h
└── main.cpp                    # tiny entry: parse opts, hand off to core
```

## 3. Implement `game_extras.h`

Fill in **identity** first (entry point from PSX EXE header, CRC32 of the
EXE, display name) — that's enough to compile and link. Memory layout,
overlay handling, and runtime quirks come as Phases 1b–1c surface them.

```c
uint32_t game_get_entry_point(void) {
    return 0xXXXXXXXXu;  /* read from PSX.EXE header field initial_pc */
}
```

## 4. Wire it into the top-level build

Append the game name to `OPENPETE_GAMES` in the top-level `CMakeLists.txt`:

```cmake
set(OPENPETE_GAMES "spyro-1;<game>" CACHE STRING "...")
```

Or pass it at configure time:

```bash
cmake --preset linux-release -DOPENPETE_GAMES="spyro-1;<game>"
```

## 5. Validate

`./scripts/setup.sh` will start failing for `<game>`'s decomp build artifacts
once Phase 1a needs them. Adapt the script if your game's map path differs.

## 6. Don't pollute `core/`

Resist the urge to add a switch in `core/` for the new game. If a behavior
*can* live in the per-game impl, put it there. Only when a second game
forces a new abstraction should a hook get hoisted into
`core/include/game_extras.h`. See §7.5 of the plan: "premature abstraction
is how speculative designs get built wrong."
