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
| `linux-release-shared`  | Linux native                    | shared  | distribution build |
| `windows-mingw-shared`  | Windows x64 cross (MinGW)       | shared  | distribution build |
| `windows-msvc`          | Windows native MSVC             | static  | placeholder, unvalidated |
| `macos-universal`       | macOS x86_64 + arm64            | static  | placeholder, unvalidated |
| `wasm`                  | Emscripten / browser            | static  | placeholder |

`linux-release`, `windows-mingw`, and the `*-shared` presets all imply
`OPENPETE_SKIP_DECOMP=ON` (full-recompilation mode); `shared` *requires*
it (the hybrid decomp path is static-only). The `*-shared` presets also
pin `OPENPETE_ROM_FREE_ICON=ON` so no game bytes are baked in.

### Where preset knobs live: `cmake/presets/<preset>.cmake`

`CMakePresets.json` deliberately pins almost **no** cache variables.
Each preset pins exactly one: `CMAKE_PROJECT_openpete_INCLUDE`, an
immutable pointer at a tracked `cmake/presets/<preset>.cmake` file that
CMake re-executes at the end of `project()` on **every** reconfigure.
All the real knobs are plain `set()`s /
`openpete_preset_default()`s in those files (inheritance is an explicit
`include()`, e.g. `windows-mingw-shared.cmake` includes
`windows-mingw.cmake`). This kills the sticky-cache footgun described
below: the preset files are configure inputs, so editing one
auto-reconfigures every build dir — every worktree — on its next plain
`cmake --build`, and adding/changing/removing a knob never needs a
`cmake --preset` re-run.

The only survivors in `CMakePresets.json` are things CMake needs before
`project()` finishes: `generator`, `binaryDir`, `toolchainFile`, and
`CMAKE_OSX_ARCHITECTURES` on the macOS placeholder.

What the preset files pin (beyond `CMAKE_BUILD_TYPE`):

| Variable | Set by preset file | Why |
|----------------|-----------|-----|
| `OPENPETE_SKIP_DECOMP=ON` | `linux-release`, `windows-mingw*`, `wasm` | full-recompilation mode; only `linux-debug` builds the WIP hybrid decomp path |
| `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS=-march=x86-64-v3` | `windows-mingw*` | portable perf floor for distributable Windows binaries |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` | `linux-release-shared` | LTO. **No Windows preset enables it** — the shared (distributed) exe cannot: GNU LTO DCEs the `.def`-exported `psyq_*` thunks (exports are not liveness roots) and ld silently emits absolute-0 exports → DEP crash at the first PSYQ call. `games/spyro-1/CMakeLists.txt` forces it OFF on that target as a guard. |
| `OPENPETE_HIDE_DEBUG_FLAGS=ON` | `*-shared` | `--help` shows only the user-facing flag block (display-only — every flag still parses) |
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

### Cache variables are sticky — why the preset files exist

CMake presets only inject `cacheVariables` when you run
`cmake --preset <p>`. A `cmake --build --preset <p>` (or any incremental
rebuild) reuses the existing `CMakeCache.txt` as-is — it re-runs the
configure step when CMake files change, but **never re-reads
`CMakePresets.json`**. When presets carried the knobs directly, two
failure modes followed: an added/changed variable silently didn't apply
until a `cmake --preset` re-run, and a *removed* variable stayed in the
cache forever (`cmake --preset` never unsets anything). The removed-var
case once kept `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` alive in
`build/windows-mingw-shared` after the preset dropped it, producing the
DEP crash above — it "reproduced" at known-good commits (checking out
old code doesn't touch the poisoned cache) and masqueraded as a
toolchain regression. Worktrees multiplied the exposure: every build
dir configured at a different time held a different snapshot.

The `cmake/presets/*.cmake` scheme removes the exposure by
construction — the only preset-pinned cache entry is a pointer whose
value never changes. What's still sticky, deliberately:

- **`-DVAR=...` on a preset build dir.** A command-line override beats
  the preset file (detected via the cache entry's command-line
  HELPSTRING) and persists until you drop it:
  `cmake --preset <p> -U <VAR>`.
- **The `CMakePresets.json` survivors** (generator, binaryDir,
  toolchainFile, the include pointer). These are stable by design; if
  one ever changes, existing build dirs need a `cmake --preset <p>`
  re-run (or a wipe, for generator/toolchain changes).

**Migrating a build dir configured before this scheme** (e.g. in
another worktree): the old preset-injected entries carry the
command-line HELPSTRING, so they'd be mistaken for deliberate
overrides. Purge them once:

```bash
cmake --preset <p> -U "OPENPETE_*" -U "CMAKE_INTERPROCEDURAL_OPTIMIZATION"
```

When a build still behaves as if a flag were set that shouldn't be,
`grep <VAR> build/<preset>/CMakeCache.txt` **before** suspecting the
environment or toolchain.

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

- **A preset knob isn't taking effect** — knobs live in
  `cmake/presets/<preset>.cmake` (see "Where preset knobs live" above)
  and propagate on any plain build. If one doesn't: the build dir
  either predates the pointer scheme (run the one-time migration
  command above) or holds a `-D` override
  (`grep <VAR> build/<preset>/CMakeCache.txt`; drop it with
  `cmake --preset <p> -U <VAR>`).
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
