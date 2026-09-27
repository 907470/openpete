# Building OpenPete

## Prerequisites

### Linux (Ubuntu / Debian-flavored)
```bash
sudo apt install build-essential cmake ninja-build python3 glslang-tools \
                 libopenal-dev \
                 libavformat-dev libavcodec-dev libavutil-dev \
                 libswscale-dev libswresample-dev
# Cross-compile to Windows from WSL2:
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
```

What each is for:

- `cmake` 3.22 or newer (`cmake_minimum_required(VERSION 3.22)`,
  `CMakeLists.txt:1`) and `ninja` (every preset's generator).
- `python3`: the build runs generator scripts under `tools/` and fence
  scripts under `scripts/fences/` as custom commands
  (`games/spyro-1/CMakeLists.txt`, `core/CMakeLists.txt`), and several
  CTest entries invoke it directly.
- `glslang-tools`: `core/rhi/CMakeLists.txt:18` does
  `find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)` — configure
  fails without it. The shaders compile to `.spv` on the host, so this is
  identical for native and cross builds.
- `libopenal-dev`: PsyCross calls `find_package(OpenAL REQUIRED)`
  (`third_party/PsyCross/CMakeLists.txt:51`). Cross builds use the
  vendored prebuilt at `third_party/OpenAL-mingw/` instead
  (`CMakeLists.txt:12-24`).
- The five `libav*`/`libsw*` packages: a hard dependency of a native build
  — `core/CMakeLists.txt` runs `pkg_check_modules(LIBAV REQUIRED ...)` over
  `libavformat libavcodec libavutil libswscale libswresample` for `--record`
  video capture. Minimum libav 58 (ffmpeg 4.0). Build without them with
  `-DOPENPETE_ENABLE_RECORD=OFF`, which compiles `--record` down to a
  reports-unavailable stub.

Not needed: no Vulkan SDK (headers are vendored at
`third_party/vulkan-headers`, and volk dlopens `libvulkan.so.1` /
`vulkan-1.dll` at runtime, so no loader import library is linked —
`core/rhi/CMakeLists.txt:7-11`); no `libsdl*-dev` (see [SDL](#sdl)).

SDL3 builds from source and detects its Linux video backends from whatever
dev headers are installed at configure time, silently disabling the ones it
cannot find. Install the X11/Wayland/GL development packages
(`libx11-dev libxext-dev libxrandr-dev libxkbcommon-dev libwayland-dev
libdecor-0-dev libgl1-mesa-dev`) for a build that can open a window;
`--headless` needs none of them. Verify what was detected with
`grep -E '^SDL_(X11|WAYLAND|OPENGL|VULKAN):' build/<preset>/CMakeCache.txt`.

### macOS
```bash
brew install cmake ninja python glslang openal-soft ffmpeg
```
`macos-universal` is an unvalidated placeholder preset.

### Windows native (MSVC)
Visual Studio 2022 with the "Desktop development with C++" workload plus
CMake. The `windows-msvc` preset exists in `CMakePresets.json` but is not
validated; the supported Windows build is the MinGW cross-build from Linux
(`windows-mingw-shared`), which is what releases ship.

## Required before the first configure

Two generation steps run outside CMake. `games/*/generated/` is gitignored
(`.gitignore:8`) and `games/spyro-1/CMakeLists.txt` lists
`generated/PSX.EXE_full.c` and globs `generated/*/*.bin_full.c` as sources,
so a fresh clone cannot configure until they exist. This is true of every
preset, including the `*-shared` distribution ones.

1. Build the matching decompilation. `games/spyro-1/spyro-1/` is a
   submodule of the decomp repo; OpenPete consumes its `build/PSX.EXE`
   (recompiler input), `build/wad/*.ovl` (the level overlays) and
   `build/psx.map` (symbol addresses).

   ```bash
   cd games/spyro-1/spyro-1 && make
   ```

   This step is independent of OpenPete and may take longer than the
   OpenPete build itself. `scripts/setup.sh` checks for its outputs, for
   the initialized submodules, and for the rebuilt disc.

2. Build the recompiler, then run it over the EXE and every overlay.

   ```bash
   cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build \
         -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build third_party/psxrecomp/build -j
   scripts/regen-recomp.sh     # -> games/spyro-1/generated/*.c
   scripts/regen-manifest.sh   # -> manifest/{symbols,globals}.toml + defsyms
   ```

   `regen-recomp.sh` recompiles each overlay under its own `--func-prefix`
   and sed-renames the generated `psx_dispatch_compiled` to
   `psx_dispatch_<name>`, so all 23 same-address overlays coexist at link;
   it also emits the `spyro_overlay_table.c` registry.
   `regen-manifest.sh` parses `psx.map` and defaults to the sibling checkout
   (`PSX_MAP=../spyro-1/build/psx.map`); point it at the submodule with
   `PSX_MAP=games/spyro-1/spyro-1/build/psx.map`.

   Re-run `regen-recomp.sh` after bumping psxrecomp, changing a forced
   entry, or pulling a new EXE dump. For the Windows distribution build,
   `package-dist.sh` also needs the cross-built recompiler at
   `third_party/psxrecomp/build-mingw/PSXRecomp.exe` (same source tree,
   configured with `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake`).

## Configure + build

Every build sits on two axes: the target (which OS) and the recomp linkage
(how the ROM-derived game code reaches the binary).

### The two linkage modes

`OPENPETE_RECOMP_LINKAGE` (`games/spyro-1/CMakeLists.txt:108`) selects how
the recompiled module joins the runtime:

- `static` (default) — the dev build. The recompiled C is an OBJECT library
  linked straight into the exe. One self-contained binary; run it with
  `--disc` and it boots immediately.
- `shared` — the distribution build. The recompiled code is not in the exe.
  The exe ships with a recompiler plus a compiler and builds
  `spyro1-recomp.{so,dll}` from the user's disc on first run
  (`games/spyro-1/dist/recomp_build.cpp`), keying the cache on a hash of the
  PSXRecomp binary, its emission metadata and the disc's SHA-256. Discs live
  in a content-addressed library under the user data dir, or beside the exe
  for a portable install (`games/spyro-1/dist/disc_library.cpp:89-92`). Subject of
  `notes/plans/openpete-distribution-local-recomp.md`.

Both modes build from the same sources; `shared` is an additive second
linkage, not a fork.

### Preset table

| Preset                  | Target                          | Linkage | Notes |
|-------------------------|---------------------------------|---------|-------|
| `linux-debug`           | Linux native, Debug             | static  | the only preset that builds the WIP hybrid decomp path |
| `linux-release`         | Linux native, RelWithDebInfo    | static  | the default dev build |
| `windows-mingw`         | Windows x64 cross (MinGW)       | static  | dev build, run via WSL2 interop |
| `linux-release-shared`  | Linux native, RelWithDebInfo    | shared  | distribution build |
| `windows-mingw-shared`  | Windows x64 cross (MinGW)       | shared  | distribution build |
| `windows-msvc`          | Windows native MSVC             | static  | placeholder, unvalidated |
| `macos-universal`       | macOS x86_64 + arm64            | static  | placeholder, unvalidated |
| `wasm`                  | Emscripten / browser            | static  | placeholder |

Every preset except `linux-debug` sets `OPENPETE_SKIP_DECOMP=ON`
(full-recompilation mode). `shared` requires it: configure aborts with a
`FATAL_ERROR` otherwise, because the hybrid decomp path's `--defsym` aliases
and native thunks assume one image
(`games/spyro-1/CMakeLists.txt:112-117`).

### Where preset knobs live: `cmake/presets/<preset>.cmake`

`CMakePresets.json` pins almost no cache variables. Each preset pins one:
`CMAKE_PROJECT_openpete_INCLUDE`, an immutable pointer at a tracked
`cmake/presets/<preset>.cmake` that CMake re-executes at the end of
`project()` on every reconfigure. The real knobs are `set()`s and
`openpete_preset_default()`s in those files; inheritance is an explicit
`include()` (`windows-mingw-shared.cmake` includes `windows-mingw.cmake`,
`linux-release-shared.cmake` includes `linux-release.cmake`). The preset
files are configure inputs, so editing one auto-reconfigures every build dir
— every worktree — on its next plain `cmake --build`, and
adding/changing/removing a knob never needs a `cmake --preset` re-run.

`openpete_preset_default(VAR VALUE)` applies a knob unless the user
overrode it with `-DVAR=...`, detected via the fixed HELPSTRING CMake stamps
on command-line cache entries (`cmake/presets/_helpers.cmake`). A mere cache
entry is not enough to defer to: `option()`/`set(CACHE)` back-fill their
defaults on any configure where the preset file did not set the knob, so a
knob added to a preset file later would lose to that stale default forever.

The only survivors in `CMakePresets.json` are things CMake needs before
`project()` finishes: `generator`, `binaryDir`, `toolchainFile`,
`architecture`, and `CMAKE_OSX_ARCHITECTURES` on the macOS placeholder.

What the preset files pin, beyond `CMAKE_BUILD_TYPE`:

| Variable | Set by preset file | Why |
|----------|--------------------|-----|
| `OPENPETE_SKIP_DECOMP=ON` | `linux-release*`, `windows-mingw*`, `wasm` | full-recompilation mode |
| `OPENPETE_FUNC_TRACE=OFF` | `linux-release*`, `windows-mingw*` | `-finstrument-functions` per-function tracing is a dev-only drill-down (1-8 ms/tick). Pinned as a tracked knob so no dist build inherits a stale ON; enable in a throwaway dir with `-DOPENPETE_FUNC_TRACE=ON` |
| `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS=-march=x86-64` | `windows-mingw*` | pinned to the baseline ISA (not left implicit — toolchains differ in their default) so the distributable runs on any x86-64 CPU. `x86-64-v3` (AVX2/FMA/BMI2) measures flat: the hot path is the recompiled game's scalar integer code with no vectorizable loops, so the pre-Haswell/Zen1 cutoff would buy nothing |
| `-ffile-prefix-map` on source and binary dir | `*-shared` | `__FILE__`/assert strings are referenced `.rdata` that package-time stripping cannot remove; without the mapping they leak the build machine's username and layout |
| `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` | `linux-release-shared` | LTO. No Windows preset enables it: GNU LTO does not treat `.def` exports as liveness roots, so it DCEs the exported `psyq_*` thunks and ld silently emits absolute-0 exports, giving a DEP crash at the first PSYQ call. `games/spyro-1/CMakeLists.txt:613-614` forces IPO off on the exe target as a guard, and line 150 keeps it off for the module (the shipped module is boot-compiled at plain `-O2`, so the CMake-built one is a dev artifact) |
| `OPENPETE_RECOMP_LINKAGE=shared` | `*-shared` | the linkage axis |
| `OPENPETE_ROM_FREE_ICON=ON` | `*-shared` | no game bytes baked into the exe |
| `OPENPETE_ENABLE_RECORD=OFF` | `*-shared` | compiles out `--record`, dropping the libav dependency |
| `OPENPETE_HIDE_DEBUG_FLAGS=ON` | `*-shared` | `--help` shows only the user-facing flag block. Display-only: `core/config/options.c` splits the help text, every flag still parses |
| `OPENPETE_STAGE_MOD_DLLS=OFF` | `windows-mingw-shared` | stage mod sources only, no cmake-side DLLs — the dist exe's first-boot mod compile is the validation surface, and pre-seeding `.build/` would mask it. Default ON: cross presets pre-build every mod DLL into `<build>/games/spyro-1/mods/<id>/.build/` |
| `OPENPETE_BUILD_TESTS=OFF` | `wasm` | trim non-browser components |

Related non-preset knobs, defaults applying everywhere:
`OPENPETE_COMPILE_MODS=ON` (every `cmake --build` also compiles all mods —
native presets by invoking the exe's `--compile-mods` mode, cross presets
cmake-side) and `OPENPETE_COMPILE_MODS_DIR` (the mods root that tier runs
over, default `<repo>/mods`).

### Cache variables are sticky — why the preset files exist

CMake presets inject `cacheVariables` only on `cmake --preset <p>`. A
`cmake --build --preset <p>`, or any incremental rebuild, reuses the
existing `CMakeCache.txt`: it re-runs configure when CMake files change but
never re-reads `CMakePresets.json`. When presets carried the knobs directly,
two failure modes followed — an added or changed variable silently did not
apply until a `cmake --preset` re-run, and a removed variable stayed in the
cache forever, since `cmake --preset` never unsets anything. The removed-var
case kept `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` alive in
`build/windows-mingw-shared` after the preset dropped it, producing the DEP
crash above; it reproduced at known-good commits, because checking out old
code does not touch a poisoned cache, and so read as a toolchain regression.
Worktrees multiplied the exposure: every build dir configured at a different
time held a different snapshot.

The `cmake/presets/*.cmake` scheme removes the exposure by construction —
the only preset-pinned cache entry is a pointer whose value never changes.
What is still sticky, deliberately:

- `-DVAR=...` on a preset build dir. A command-line override beats the
  preset file and persists until dropped: `cmake --preset <p> -U <VAR>`.
- The `CMakePresets.json` survivors (generator, binaryDir, toolchainFile,
  the include pointer). Stable by design; if one changes, existing build
  dirs need a `cmake --preset <p>` re-run, or a wipe for generator and
  toolchain changes.

Migrating a build dir configured before this scheme (for example in another
worktree): the old preset-injected entries carry the command-line
HELPSTRING, so they would be mistaken for deliberate overrides. Purge them
once:

```bash
cmake --preset <p> -U "OPENPETE_*" -U "CMAKE_INTERPROCEDURAL_OPTIMIZATION"
```

When a build behaves as if a flag were set that should not be, run
`grep <VAR> build/<preset>/CMakeCache.txt` before suspecting the environment
or the toolchain.

### Dev build (static)

```bash
cmake --preset linux-release          # first time only
cmake --build --preset linux-release
./build/linux-release/games/spyro-1/openpete-spyro1 \
    --disc games/spyro-1/spyro-1/build/SCUS-94228.cue --headless \
    --bk2 games/spyro-1/bk2/<name>.bk2 --screenshot-on-frame 3600 \
    --screenshot-output shot.png
```

Cross-compiled Windows dev build, run straight from WSL2 on the real GPU:

```bash
cmake --build --preset windows-mingw
build/windows-mingw/games/spyro-1/openpete-spyro1.exe [--args]
```

Run these from the repo root: the `--disc` and `--bk2` arguments above are
repo-relative paths resolved against the working directory. An explicit
`--disc` also bypasses the disc library entirely, so the dev loop never
touches the first-boot machinery.

Persistent settings come from `openpete.toml`, read from the exe's directory
and then the per-user config dir (`core/config/config.c:1094-1100`) — never
from the working directory, and no such file is staged into a dev build dir.
`--config-report` prints the resolved settings and exits; `--config PATH`
uses only that file; `--no-config` ignores every config file, which is what
the gate scripts do so a personal config cannot change a verdict.

Output binary: `build/<preset>/games/spyro-1/openpete-spyro1[.exe]`.

### Distribution build (shared)

```bash
cmake --preset windows-mingw-shared      # or linux-release-shared
cmake --build build/windows-mingw-shared -j
```

This does not produce a playable folder by itself: the shared exe needs its
recomp inputs staged next to it (manifests, module source kit, PSXRecomp,
and a bundled compiler). `scripts/package-dist.sh` assembles the shippable
archive from a `*-shared` build tree, staging `docs/openpete-dist.toml` as
the shipped `openpete.toml`.

```bash
scripts/setup_linux_clang.sh          # -> .tools/clang (bundled compiler)
scripts/setup_appimagetool.sh         # pinned appimagetool, for --appimage
scripts/package-dist.sh linux --toolchain .tools/clang --appimage
scripts/package-dist.sh windows --toolchain /path/to/llvm-mingw-ucrt-x86_64
```

Archives land in `dist/`. Flags:

- `--toolchain DIR`: an unpacked compiler tree to bundle — llvm-mingw (ucrt
  x86_64) for Windows, a trimmed llvm.org clang for Linux. Without it the
  Windows zip and the Linux tarball fall back to the user's own compiler.
- `--appimage` (Linux): additionally emits
  `OpenPete-<ver>-x86_64.AppImage` from the same staged tree as the tarball,
  so the two artifacts cannot drift. The AppImage is the primary Linux
  download — its squashfs keeps the bundled compiler compressed and never
  unpacks it, and its read-only mount forces the writable-state split
  (library, mods, user assets and recordings resolve through
  `openpete_paths`). It always bundles a compiler.
- `--with-mod-sdk`: stage the mod SDK headers the boot-time mod compiler
  needs. Releases ship without it; the dist gate passes it to keep the path
  exercised.
- `--sign '<command>'` (Windows): Authenticode hook, invoked once per
  first-party PE with the file path appended; a nonzero exit fails the
  package. The RFC3161 timestamp is not optional with Azure Artifact
  Signing — its certs live about three days, and only the countersignature
  keeps a release valid past that.

Extract the archive anywhere and run the exe with no arguments; a bare path
to a `.cue/.bin/.img/.iso/.chd/.pbp` means `--ingest`, which copies the disc
into the library, after which the exe recompiles the module and plays.
`scripts/check_dist_first_boot.sh` checks that whole path end-to-end against
the 20k hardware oracle, in a scratch `HOME`/XDG so it never touches the
developer's real directories; `--appimage` gates the AppImage instead, and
`OPENPETE_DIST_ARTIFACT=<file>` gates an already-built artifact with no
build tree present.

`OPENPETE_PSXRECOMP` and `OPENPETE_CC`
(`games/spyro-1/dist/recomp_build.cpp:298,318`) override the bundled tools for
dev testing of the shared path; without them the exe looks in
`recomp/tools/` next to itself, then the repo build dir, then the system
compiler.

## Disc image

The retail disc is *Spyro the Dragon* (NTSC-U, SCUS-94228) as a BIN/CUE
pair, a single MODE2/2352 data track of 281270 sectors. The .bin's SHA256
is `fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37`
(the Redump image; SHA1 `cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f`, MD5
`387185f71cd0043fd75a19521dc75104`). The same image covers the Original,
Greatest Hits and Collectors' Edition pressings. The disc library checks
the SHA256 at ingest (`games/spyro-1/dist/disc_library.cpp`).

Development runs use the disc rebuilt from the decomp output,
`games/spyro-1/spyro-1/build/SCUS-94228.cue`, the default of every script
and run command; `OPENPETE_DISC` or `--disc` overrides it. Distribution
builds do not read this path: they ingest a disc into a local library on
first run.

## SDL

SDL3 is built from source from the `third_party/SDL/` submodule, pinned to
the `release-3.4.x` branch, and statically linked (`libSDL3.a`) on both
Linux and the MinGW cross build — one self-contained binary, no `SDL3.dll`
alongside. `CMakeLists.txt:49-55` forces `SDL_STATIC=ON`, `SDL_SHARED=OFF`,
`SDL_TEST=OFF`, and `SDL_X11_XTEST=OFF` (openpete only polls input, never
injects it, so disabling XTEST drops the `libXtst-dev` build dependency).

## Troubleshooting

- A preset knob is not taking effect. Knobs live in
  `cmake/presets/<preset>.cmake` and propagate on any plain build. If one
  does not, the build dir either predates the pointer scheme (run the
  one-time migration above) or holds a `-D` override — check with
  `grep <VAR> build/<preset>/CMakeCache.txt`, drop it with
  `cmake --preset <p> -U <VAR>`.
- `cmake: command not found`, or a version error. Minimum is 3.22.
- Configure fails on `GLSLANG_VALIDATOR-NOTFOUND`. Install `glslang-tools`;
  `core/rhi/CMakeLists.txt:18` requires the binary on the host.
- CMake cannot find `generated/PSX.EXE_full.c`. The recompiler outputs are
  gitignored — run the two generation steps above.
- SDL fails to configure or build. Ensure the submodule is checked out
  (`git submodule update --init --recursive`); SDL3 builds from source as
  part of the CMake configure.
- MinGW linker errors about `__chkstk_ms` or pthread symbols.
  `cmake/toolchains/mingw-w64.cmake` sets the `-static*` flags; do not
  override `CMAKE_EXE_LINKER_FLAGS` in your own CMake invocations.
- `--record` reports itself unavailable on the Windows cross build. There is
  no system libav to link against, so `core/CMakeLists.txt` compiles
  `record.c` down to a stub unless a static FFmpeg exists at
  `third_party/ffmpeg-mingw/`. `scripts/setup_mingw_ffmpeg.sh` cross-builds
  one (static GPL FFmpeg with x264, which makes the resulting exe GPL).
- `--headless` logs an SDL_Init failure. `core/runtime/run.c:5920-5936`
  requests the `offscreen` video driver and, if `SDL_Init` fails under it,
  retries with `dummy`, which is sufficient for screenshots. The startup log
  line names the driver that was actually selected.
