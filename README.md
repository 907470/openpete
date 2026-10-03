# OpenPete

Cross-platform PC port of the original PSX Spyro trilogy. The games' MIPS
code is statically recompiled to C at build time, the PSYQ system library
is replaced by a host re-implementation, and a native Vulkan renderer draws
the scene from gameplay state.

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

The recommended build runs in containers (podman or docker) and needs no
compiler on the host:

```bash
git clone --recursive <openpete repository URL> openpete && cd openpete
scripts/container.sh engine                    # core libraries + unit tests
OPENPETE_PSYQ_INCLUDE=/path/to/psyq/include \
    scripts/container.sh decomp spyro-1        # the decompilation
scripts/container.sh game spyro-1              # recompile, build, boot check
scripts/container.sh windows spyro-1           # the Windows cross build
```

A game build needs your own copy of the PSYQ SDK headers (4.0; Spyro 2:
4.7) for its decompilation and a disc image of your own copy of the
original disc (NTSC-U, SCUS-94228; described under
[Disc image](docs/src/developer/building.md#disc-image)).
[Building](docs/src/developer/building.md) lists where each input goes, the
native build and its prerequisites, the presets and each gate's container
form.

## License

Source code is offered under the MIT License (`LICENSE`). The Spyro
decompilation submodule, PsyCross, psxrecomp, and other vendored deps retain
their respective upstream licenses; see `NOTICE.md`. Contributions are
accepted under the Developer Certificate of Origin; see `CONTRIBUTING.md`.

This project is not affiliated with or endorsed by Sony, Insomniac Games,
or Activision.
