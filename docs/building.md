# Building OpenPete

## Prerequisites

### Linux (Ubuntu / Debian-flavored)
```bash
sudo apt install build-essential cmake ninja-build python3 \
                 libsdl2-dev libgl1-mesa-dev libasound2-dev libpulse-dev
# For cross-compile to Windows from WSL2:
sudo apt install gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix
```

### macOS
```bash
brew install cmake ninja python sdl2
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

OpenPete uses CMake presets. The named presets in `CMakePresets.json` are:

| Preset             | Target                                   |
|--------------------|------------------------------------------|
| `linux-debug`      | Linux native, Debug                      |
| `linux-release`    | Linux native, RelWithDebInfo             |
| `windows-mingw`    | Windows x64 cross from Linux/WSL2 (MinGW)|
| `windows-msvc`     | Windows native MSVC (placeholder)        |
| `macos-universal`  | macOS universal x86_64 + arm64 (placeholder) |

```bash
cmake --preset linux-release
cmake --build build/linux-release -j
```

For the cross-compile path:

```bash
cmake --preset windows-mingw
cmake --build build/windows-mingw -j
# Run from WSL2 directly:
./build/windows-mingw/games/spyro-1/openpete-spyro1.exe --help
```

Output binary: `build/<preset>/games/spyro-1/openpete-spyro1[.exe]`.

## SDL2 sources

For Phase 0b:
- **Linux native** uses the system `libsdl2-dev` package (whatever the
  distro ships).
- **MinGW cross-compile** uses the vendored prebuilt at
  `third_party/SDL2-mingw/`, statically linked into the `.exe` so it runs
  as a single file (no `SDL2.dll` shipped alongside).

A submodule of SDL itself lives at `third_party/SDL/` (release-2.30.x branch)
for future use — at some point we'll switch the build to compile SDL from
source and retire the prebuilt. Until then it's just present so the tree
matches the spec.

## Troubleshooting

- **`cmake: command not found`** — install via your package manager. Minimum
  version is 3.22.
- **`SDL2 not found`** during MinGW configure — make sure
  `third_party/SDL2-mingw/x86_64-w64-mingw32/lib/cmake/SDL2/` exists
  (vendored at Phase 0).
- **MinGW linker errors about `__chkstk_ms` / pthread symbols** — the
  toolchain file at `cmake/toolchains/mingw-w64.cmake` sets the right
  `-static*` flags; don't override `CMAKE_EXE_LINKER_FLAGS` in your own
  CMake invocations.
- **`offscreen not available`** at runtime under `--headless` — the system
  SDL2 wasn't built with the offscreen video driver. The runtime auto-falls
  back to the `dummy` driver, which works for screenshots via the software
  renderer.
