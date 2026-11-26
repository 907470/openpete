# OpenPete

Cross-platform PC port of *Spyro the Dragon* (PSX, 1998). The game's MIPS
code is statically recompiled to C at build time, the PSYQ system library
is replaced by a host re-implementation, and a native Vulkan renderer draws
the scene from gameplay state. The runtime in `core/` is game-agnostic and
is shared by the Spyro 2 and Spyro 3 ports.

## iOS port (experimental fork)

> **Status: work in progress. On `ios-port-oct` the engine libraries
> (`openpete_core`, `openpete_rhi`, `openpete_imgui`, plus PsyCross, SDL3,
> glslang and OpenAL Soft) build for iOS arm64 in CI (Xcode 16.4, iPhoneOS
> 18.5 SDK). Nothing has been linked into an app or run on a device, no game
> code is included, and there is no Vulkan-on-Metal path yet.** This fork (`907470/openpete`, branches `ios-port` and `ios-port-oct`) is an attempt
> to build OpenPete for iOS arm64 using GitHub Actions on hosted macOS
> runners. It is not affiliated with the upstream project.

### What exists so far

- `cmake/toolchains/ios.cmake`: iOS arm64 toolchain file (unsigned builds).
- `ios-arm64` preset in `CMakePresets.json` and `cmake/presets/ios-arm64.cmake`:
  static recomp linkage, with tests, `--record` and mod compilation off.
- `.github/workflows/ios.yml`: a manually triggered workflow on `macos-15`
  that configures and builds the **engine libraries only** (`-DOPENPETE_GAMES=`)
  and uploads the logs.
- `third_party/PsyCross` points at the `ios-port` branch of
  [907470/psycross-archive](https://github.com/907470/psycross-archive)
  (`5f33a9c`): the SDL3 PsyCross at `9b5e29c` plus three small iOS fixes
  (clang narrowing, arm64 primitive-tag layout, Apple `LIBCD`). `tyscorp/PsyCross`
  is not publicly readable, so CI cannot clone it, and the public
  `OpenDriver2/PsyCross` needs SDL2. The archive's provenance is not verified.
- **This branch (`ios-port-oct`) is based on OpenPete `ab0b4e7` (2026-10-03),**
  the last commit that pinned PsyCross `9b5e29c`. Every PsyCross symbol used
  at that commit exists in the archive. Newer OpenPete needs about 45 PsyCross
  functions (the `PsyX_SPUFeed_*` audio system, CD hooks, `g_PsyX_Pad_StickMap`)
  that are only in the private fork, so the `ios-port` branch (current
  OpenPete) cannot build.

### Known blockers and planned work

- **No game builds in public CI.** The recompiled game C needs your own PSYQ
  SDK headers and disc dump. Neither may be committed to a public repository.
- **Rendering.** The renderer loads Vulkan at runtime through volk. iOS needs
  MoltenVK (Vulkan to Metal) linked in and a Metal surface path.
- **Static linkage only.** iOS cannot load unsigned code or compile on the
  device, so the first-boot module build and shared modules do not apply, and
  mods cannot be loaded as dynamic libraries.
- **Platform layer.** The disc library and user data paths need an iOS branch
  (the app's Documents and Application Support directories). The Info.plist
  needs file sharing enabled (`UIFileSharingEnabled`,
  `LSSupportsOpeningDocumentsInPlace`) so a disc image can be copied in
  through the Files app.
- **Smaller risks.** An arm64 path for libco, and PsyCross's OpenAL and GL
  window use on iOS.
- Touch controls, and packaging into a signed app.

### Credits and sources

- **OpenPete** by [tyscorp](https://github.com/tyscorp/openpete) (MIT). All of
  the engine, recompilation pipeline and renderer is their work; this fork
  only adds iOS build scaffolding.
- **[OpenDriver2/PsyCross](https://github.com/OpenDriver2/PsyCross)**, the
  public PSYQ reimplementation now used as the submodule.
- **[SDL](https://github.com/libsdl-org/SDL)**, which provides the iOS
  windowing and input layer.
- **[OpenAL Soft](https://github.com/kcat/openal-soft)** (LGPL-2.0), fetched
  and built from source for iOS because Apple's OpenAL.framework lacks the
  extensions PsyCross uses.
- **[MoltenVK](https://github.com/KhronosGroup/MoltenVK)** (planned) for
  Vulkan on Metal.
- **[TheMobyCollective](https://github.com/TheMobyCollective)** Spyro
  decompilations, used as submodules by upstream.
- The iOS scaffolding in this fork (toolchain, preset, workflow and these
  notes) was written with **Claude** (Anthropic's Claude Code, Claude
  Sonnet 5.5) working with the fork owner, [907470](https://github.com/907470).
  The AI-written changes are unreviewed and untested; treat them accordingly.
  Claude's inputs were the upstream repository and its build docs, plus its
  general knowledge of CMake, iOS and MoltenVK. No other code was copied.

Spyro is a trademark of its respective owners. You must supply your own copy
of the game disc.

## Documentation

The documentation source is under `docs/src/`, one directory per section.
Build it with `scripts/build-docs.sh`; `scripts/build-docs.sh --serve` runs a
live preview.

- [User](docs/src/user/index.md) - installing, playing, settings, the
  config file, key bindings, texture, music and sound packs.
- [Modding](docs/src/modding/index.md) - writing mods against the SDK,
  published at https://openpete.com/docs/modding/.
- [Developer](docs/src/developer/README.md) - architecture,
  [building](docs/src/developer/building.md), unattended runs, the oracle,
  and the recompiler and renderer notes.
- [Spyro](docs/src/spyro/index.md) - the game's level format and code
  overlays.

## Building

```bash
git submodule update --init --recursive
./scripts/setup.sh
cmake --preset linux-release -DOPENPETE_GAMES=
cmake --build --preset linux-release --target openpete_core openpete_rhi
```

These commands build the engine alone. A game executable needs the
decompilation built, which needs your own copy of the PSYQ SDK headers (4.0; Spyro 2: 4.7),
and the recompiled C regenerated before the configure;
[building](docs/src/developer/building.md#step-0-before-the-first-configure)
lists the prerequisites, those steps, the unit tests, the presets and the
Windows cross build. Running the game needs a disc image of your own copy
of the original disc (NTSC-U, SCUS-94228); the required image is described
under [Disc image](docs/src/developer/building.md#disc-image).

## License

Source code is offered under the MIT License (`LICENSE`). The Spyro
decompilation submodule, PsyCross, psxrecomp, and other vendored deps retain
their respective upstream licenses; see `NOTICE.md`. Contributions are
accepted under the Developer Certificate of Origin; see `CONTRIBUTING.md`.

This project is not affiliated with or endorsed by Sony, Insomniac Games,
or Activision.
