# OpenPete

Cross-platform PC port of *Spyro the Dragon* (PSX, 1998). The game's MIPS
code is statically recompiled to C at build time, the PSYQ system library
is replaced by a host re-implementation, and a native Vulkan renderer draws
the scene from gameplay state. The runtime in `core/` is game-agnostic and
is shared by the Spyro 2 and Spyro 3 ports.

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
