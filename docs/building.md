# Building OpenPete

## Prerequisites

### Linux (Ubuntu / Debian-flavored)
```bash
sudo apt install build-essential cmake ninja-build python3 \
                 libopenal-dev libgl1-mesa-dev \
                 libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
# For cross-compile to Windows from WSL2:
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
```

SDL is vendored (SDL3, built from `third_party/SDL/` and statically
linked) — no `libsdl*-dev` needed. `libopenal-dev` is PsyCross's audio
backend; the `libav*` packages are only for `--record` video capture (the
MinGW cross build compiles `--record` out, so they're optional there).

### macOS
```bash
brew install cmake ninja python openal-soft ffmpeg
```

### Windows native (MSVC)
Install Visual Studio 2022 with the "Desktop development with C++" workload
plus CMake. SDL2 must be available via vcpkg or a system install. (This
preset is in `CMakePresets.json` but is **not validated** as of Phase 0b.)

## Required: matching-decomp build

The `games/spyro-1/spyro-1/` submodule is the matching decompilation. From
Phase 1a onward, OpenPete consumes its `build/psx.map` (linker map) and
`build/PSX.EXE` (the recompilation input). Build it once before configuring
OpenPete:

```bash
cd games/spyro-1/spyro-1/
make
```

That step is independent and may take longer than the OpenPete build itself.
Phase 0b can configure and build OpenPete without it; later phases cannot.

## Configure + build

OpenPete uses CMake presets. Every build sits on two axes: the **target**
(which OS) and the **recomp linkage** (how the ROM-derived game code
reaches the binary). The linkage axis is the one that matters day to day.

### The two linkage modes (this is the important part)

The recompiled game code can be joined to the runtime two ways, selected
by the `OPENPETE_RECOMP_LINKAGE` cache variable:

- **`static` (default) — the dev build.** The recompiled C is linked
  straight into the exe. One self-contained binary; run it with `--disc`
  and it boots immediately. This is what you build and test against all
  day. No first-boot recompile, because there is nothing left to compile.

- **`shared` — the distribution build.** The recompiled code is *not* in
  the exe. Instead the exe ships with a recompiler + compiler and builds
  the game module (`spyro1-recomp.{so,dll}`) from the user's disc on
  first run, caching it in a local `library/` folder. This is the
  end-user experience (drop a disc → recompile → play) and the subject of
  `notes/plans/openpete-distribution-local-recomp.md`.

Both modes build from the *same sources* — `shared` is an additive second
linkage, not a fork. The static build's link semantics are byte-identical
to before the distribution work, so the dev loop is unchanged.

### Preset table

| Preset                  | Target                          | Linkage | Notes |
|-------------------------|---------------------------------|---------|-------|
| `linux-debug`           | Linux native, Debug             | static  | decomp-mode dev / bisect |
| `linux-release`         | Linux native, RelWithDebInfo    | static  | **the default dev build** |
| `windows-mingw`         | Windows x64 cross (MinGW)       | static  | dev build you run via WSL2 |
| `windows-mingw-opt`     | Windows x64 cross (MinGW) + LTO | static  | distributable static exe; slow link |
| `linux-release-shared`  | Linux native                    | shared  | distribution build |
| `windows-mingw-shared`  | Windows x64 cross (MinGW)       | shared  | distribution build |
| `windows-msvc`          | Windows native MSVC             | static  | placeholder, unvalidated |
| `macos-universal`       | macOS x86_64 + arm64            | static  | placeholder, unvalidated |
| `wasm`                  | Emscripten / browser            | static  | placeholder |

`linux-release`, `windows-mingw`, and the `*-shared` presets all imply
`OPENPETE_SKIP_DECOMP=ON` (full-recompilation mode); `shared` *requires*
it (the hybrid decomp path is static-only). The `*-shared` presets also
pin `OPENPETE_ROM_FREE_ICON=ON` so no game bytes are baked in.

### Preset-pinned cache variables

What `CMakePresets.json` deliberately pins where (beyond
`CMAKE_BUILD_TYPE`):

| Cache variable | Pinned by | Why |
|----------------|-----------|-----|
| `OPENPETE_SKIP_DECOMP=ON` | `linux-release`, `windows-mingw*`, `wasm` | full-recompilation mode; only `linux-debug` builds the WIP hybrid decomp path |
| `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS=-march=x86-64-v3` | `windows-mingw*` | portable perf floor for distributable Windows binaries |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` | `windows-mingw-opt`, `linux-release-shared` | LTO. **Never** LTO the Windows *shared* exe: GNU LTO DCEs the `.def`-exported `psyq_*` thunks (exports are not liveness roots) and ld silently emits absolute-0 exports → DEP crash at the first PSYQ call. `games/spyro-1/CMakeLists.txt` forces it OFF on that target as a guard. |
| `OPENPETE_HIDE_DEBUG_FLAGS=ON` | `windows-mingw-opt` | `--help` shows only the user-facing flag block |
| `OPENPETE_RECOMP_LINKAGE=shared` | `*-shared` | the linkage axis (see above) |
| `OPENPETE_ROM_FREE_ICON=ON` | `*-shared` | no game bytes baked into the exe |
| `OPENPETE_ENABLE_RECORD=OFF` | `*-shared` | compiles out `--record` (drops the libav dependency) |
| `OPENPETE_STAGE_MOD_DLLS=OFF` | `windows-mingw-shared` | stage mod *sources* only, no cmake-side DLLs — the dist exe's first-boot mod compile is the validation surface. Default ON: cross presets pre-build every mod DLL into `<build>/games/spyro-1/mods/<id>/.build/`. |
| `OPENPETE_BUILD_PCSX_REDUX_GTE=OFF`, `OPENPETE_BUILD_TESTS=OFF` | `wasm` | trim non-browser components |

Related non-preset knobs (defaults apply everywhere):
`OPENPETE_COMPILE_MODS=ON` (every `cmake --build` also compiles all
mods — native presets via the exe's `--compile-mods` mode, cross
presets cmake-side) and `OPENPETE_COMPILE_MODS_DIR` (the mods root that
tier runs over, default `<repo>/mods`).

### Cache variables are sticky — re-run `cmake --preset` after preset edits

CMake presets only inject `cacheVariables` when you run
`cmake --preset <p>`. A `cmake --build --preset <p>` (or any incremental
rebuild) reuses the existing `CMakeCache.txt` as-is — it re-runs the
configure step when CMake files change, but **never re-reads
`CMakePresets.json`**. Two failure modes follow:

- **Added/changed var doesn't apply.** If a preset gains a variable (or
  changes a value), every already-configured build dir keeps its old
  cache until you re-run `cmake --preset <p>`. That command is safe on a
  warm tree — it re-applies the preset's variables without nuking
  anything else.
- **Removed var keeps applying.** `cmake --preset` never *unsets* a
  variable the preset stops mentioning; it stays in the build dir's
  cache forever. This is the nastier one — it once kept
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` alive in
  `build/windows-mingw-shared` after the preset dropped it, producing
  the DEP crash above that "reproduced" at known-good commits (checking
  out old code doesn't touch the poisoned cache) and masqueraded as a
  toolchain regression. Remove it explicitly
  (`cmake -U <VAR> build/<preset>`) or wipe the build dir.

When a build behaves as if a flag you removed were still set — or a flag
you added weren't — `grep <VAR> build/<preset>/CMakeCache.txt` **before**
suspecting the environment or toolchain.

### Dev build (static — what you use to iterate)

```bash
cmake --preset linux-release
cmake --build build/linux-release -j
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue --headless \
    --bk2 games/spyro-1/bk2/<name>.bk2 --screenshot-on-frame 3600 \
    --screenshot-output shot.png
```

Cross-compiled Windows dev build (run it straight from WSL2 — real GPU):

```bash
cmake --build --preset windows-mingw
build/windows-mingw/games/spyro-1/openpete-spyro1.exe [--args]
```

Run these **from the repo root** — the staged `openpete.opts` beside the
exe supplies `--disc` as a repo-relative path. An explicit `--disc` on
the command line overrides it and bypasses the disc library entirely, so
the dev loop never touches the first-boot machinery.

Output binary: `build/<preset>/games/spyro-1/openpete-spyro1[.exe]`.

### Distribution build (shared — the end-user experience)

```bash
# Build the shared exe + its recompiler
cmake --preset windows-mingw-shared      # or linux-release-shared
cmake --build build/windows-mingw-shared -j
```

This does **not** produce a playable folder by itself — the shared exe
needs its recomp inputs (manifests, module source kit, PSXRecomp, and on
Windows the bundled clang) staged next to it. `scripts/package-dist.sh`
assembles the shippable archive:

```bash
# Linux (system cc used at first boot in v1)
scripts/package-dist.sh linux

# Windows (bundle a trimmed llvm-mingw for the first-boot compile)
scripts/package-dist.sh windows --toolchain /path/to/llvm-mingw-ucrt-x86_64
```

The archive lands in `dist/`. Extract it anywhere, run the exe with no
arguments, drag a disc onto the splash, and it recompiles + plays — the
whole path is checked end-to-end by `scripts/check_dist_first_boot.sh`.
`OPENPETE_PSXRECOMP` / `OPENPETE_CC` override the bundled tools for dev
testing of the shared path.

## SDL

SDL3 is built from source from the `third_party/SDL/` submodule and
statically linked (`libSDL3.a`) on both Linux and the MinGW cross build —
one self-contained binary, no `SDL3.dll` alongside. No system `libsdl*`
package is required.

## Troubleshooting

- **A cache variable you added/removed in `CMakePresets.json` isn't
  taking effect** — preset variables are sticky; see "Cache variables
  are sticky" above. Re-run `cmake --preset <p>` (additions/changes) or
  `cmake -U <VAR> build/<preset>` (removals).
- **`cmake: command not found`** — install via your package manager. Minimum
  version is 3.22.
- **SDL fails to configure/build** — ensure the `third_party/SDL/`
  submodule is checked out (`git submodule update --init --recursive`);
  SDL3 builds from source as part of the CMake configure.
- **MinGW linker errors about `__chkstk_ms` / pthread symbols** — the
  toolchain file at `cmake/toolchains/mingw-w64.cmake` sets the right
  `-static*` flags; don't override `CMAKE_EXE_LINKER_FLAGS` in your own
  CMake invocations.
- **`offscreen not available`** at runtime under `--headless` — the system
  SDL2 wasn't built with the offscreen video driver. The runtime auto-falls
  back to the `dummy` driver, which works for screenshots via the software
  renderer.
