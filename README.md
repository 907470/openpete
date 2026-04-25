# OpenPete

Cross-platform PC port of *Spyro the Dragon* (PSX, 1998), built on a hybrid
static-recompilation + matching-decomp foundation. Designed to share its core
runtime across multiple PSX titles in the Spyro trilogy as their decomps
mature.

> Status: **Phase 0b (bootstrap)** complete. Phase 1 (recompiler integration)
> in progress. See `notes/plans/openpete-pc-port.md` for the full plan.

## Architecture (one paragraph)

The PSX EXE is statically recompiled to C by `psxrecomp` (vendored under
`third_party/psxrecomp/`). The PSYQ system library calls are intercepted at
the symbol level and serviced by `PsyCross`, which provides a modern
SDL2 + OpenGL + audio backend. As individual functions become available
in C from the matching decomp at `games/spyro-1/spyro-1/`, they are
swapped in via a manifest-driven thunk infrastructure (Phase 2+). Per-game
specifics (entry points, overlay layouts, runtime quirks) live behind a
narrow `core/include/game_extras.h` abstraction; `core/` itself stays
game-agnostic so a future Spyro 2 or 3 port reuses the same runtime.

## Quick start

```bash
# 1. Initialize submodules.
git submodule update --init --recursive

# 2. Apply local patches to vendored third-party submodules
#    (e.g. PsyCross Linux build fixes — see patches/ for sources).
./tools/apply_third_party_patches.sh

# 3. Validate setup (disc image + submodules + decomp build artifacts).
./scripts/setup.sh

# 4. Build for native Linux. Requires `libopenal-dev` for PsyCross.
sudo apt install libopenal-dev
cmake --preset linux-release
cmake --build build/linux-release -j

# 5. (Optional) cross-compile to Windows from WSL2.
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
cmake --preset windows-mingw
cmake --build build/windows-mingw -j

# 6. Run the developer-loop sanity check.
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --headless --frame-limit 60 --screenshot-on-frame 30 \
    --output ./shot.png --max-runtime-seconds 30 --log-format json
```

## Repository layout

See `notes/plans/openpete-pc-port.md` §3. Briefly:

- `core/` — game-agnostic runtime (window, input, audio, dispatcher, GL).
  Must contain zero Spyro-specific addresses or symbols.
- `games/spyro-1/` — Spyro 1 binary entry; manifest, generated thunks,
  per-game `game_extras_impl.c`. `games/spyro-1/spyro-1/` is the matching
  decomp submodule.
- `third_party/` — vendored deps (psxrecomp, libco, GLAD, stb) and
  submodules (PsyCross, SDL, cubeb).
- `tools/` — codegen + image diff scripts.

## Disc requirement

Original *Spyro the Dragon* (NTSC-U, SCUS-94228) BIN/CUE pair. The .bin's
SHA256 must be `95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f`.
`scripts/setup.sh` validates this. The default expected location is
`<spyro>/roms/SCUS-94228/SCUS-94228.bin`; override with the
`OPENPETE_DISC` environment variable.

## License

Source code is offered under PolyForm Noncommercial 1.0.0 (`LICENSE`). The
Spyro decompilation submodule, PsyCross, psxrecomp, and other vendored deps
retain their respective upstream licenses; see `NOTICE.md`.

This project is not affiliated with or endorsed by Sony, Insomniac Games,
or Activision.
