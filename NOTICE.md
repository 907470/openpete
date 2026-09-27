# NOTICE — third-party attributions

OpenPete's own code is licensed under **PolyForm Noncommercial 1.0.0** (see
`LICENSE`). It bundles or depends on the third-party projects below; each
retains its upstream license. This file is the authoritative record of what
travels in a distributed build and under which terms. Every release archive
ships this file and, under `licences/`, the upstream license text of each
component below that the archive carries; `docs/licences/manifest.tsv` maps
each row to its text, and packaging fails on a row with no text.

## Linked into the distributed binary — permissive (attribution required)

| Component                            | License                                        | Upstream                                                                                    |
| ------------------------------------ | ---------------------------------------------- | ------------------------------------------------------------------------------------------- |
| spyro-1                              | CC0 1.0 Universal                              | https://github.com/TheMobyCollective/spyro-1                                                |
| PsyCross                             | MIT                                            | https://github.com/OpenDriver2/PsyCross                                                     |
| SDL (SDL **3**)                      | Zlib                                           | https://github.com/libsdl-org/SDL                                                           |
| miniz                                | MIT                                            | https://github.com/richgel999/miniz                                                         |
| tomlc99                              | MIT                                            | https://github.com/cktan/tomlc99                                                            |
| stb_image / stb_image_write          | public domain / MIT (dual)                     | https://github.com/nothings/stb                                                             |
| libco                                | ISC                                            | https://github.com/higan-emu/libco                                                          |
| xxHash                               | BSD-2-Clause                                   | https://github.com/Cyan4973/xxHash                                                          |
| Dear ImGui 1.92.8 (exe; SDK headers `sdk/imconfig.h`) | MIT                               | https://github.com/ocornut/imgui                                                            |
| dear_bindings (generated C API; SDK header `sdk/dcimgui.h`) | MIT                         | https://github.com/dearimgui/dear_bindings                                                  |
| glslang 15.4.0                       | BSD-3-Clause / BSD-2-Clause / MIT / Apache-2.0 / GPL-3.0 with Bison exception (per file; `LICENSE.txt`) | https://github.com/KhronosGroup/glslang |
| glad 0.1.36 (GL loader, generated, vendored in PsyCross) | MIT                        | https://github.com/Dav1dde/glad                                                             |
| volk (Vulkan loader)                 | MIT                                            | https://github.com/zeux/volk                                                                |
| Vulkan-Headers 1.3.204               | Apache-2.0                                     | https://github.com/KhronosGroup/Vulkan-Headers                                              |
| libstdc++ / libgcc (static)          | GPL-3.0 **with GCC Runtime Library Exception** | GCC — the exception explicitly permits distributing the compiled binary in proprietary form |
| MinGW-w64 CRT / winpthreads 8.0.0 (static, Windows) | Zlib / BSD / public domain (permissive) | https://www.mingw-w64.org                                                          |
| OpenAL Soft 1.25.1 (`OpenAL32.dll`, Windows; text also as `OpenAL-COPYING.txt`) | LGPL v2 | https://github.com/kcat/openal-soft                                                  |

## Bundled data — shipped alongside the binary

Data files copied into every release archive. Not linked into
`openpete-spyro1`; read at runtime.

| Component                                | License | Upstream                                          |
| ---------------------------------------- | ------- | ------------------------------------------------- |
| SDL_GameControllerDB (`gamecontrollerdb.txt`) | Zlib    | https://github.com/mdqinc/SDL_GameControllerDB |

Refreshed with `scripts/update_gamecontrollerdb.sh`.

## Bundled first-boot toolchain — mere aggregation

Shipped as separate executables under `recomp/tools/`, invoked as
subprocesses to recompile the user's disc at first boot. Not linked into
`openpete-spyro1`; each is an independent program (mere aggregation).

| Component                                | License                                                          | Notes                                                      |
| ---------------------------------------- | ---------------------------------------------------------------- | ---------------------------------------------------------- |
| llvm-mingw (LLVM/Clang, lld, llvm-ar, …) | Apache-2.0 **with LLVM exception** (+ MinGW-w64 CRT, permissive) | Windows archive; ship LLVM `LICENSE.TXT` in the bundle     |
| LLVM/Clang 18.1.8 + lld (llvm.org build) | Apache-2.0 **with LLVM exception**                               | Linux AppImage; ship LLVM `LICENSE.TXT` in the bundle      |
| PSXRecomp (psxrecomp)                    | PolyForm Noncommercial License 1.0.0                             | static recompiler; build tool, not shipped-linked          |

`recomp/tools/clang/lib/libtinfo.so.5` in the Linux bundle is **ours**, not
ncurses: the llvm.org binaries name that library in their DT_NEEDED but
import no symbol from it, so an empty stub with the expected version node
satisfies the loader without shipping a terminal library (see
`scripts/setup_linux_clang.sh`).

## Mod SDK example mods — bundled under `sdk/examples/`

Present only in archives packaged with the mod SDK. Each example is a
separate directory the loader compiles or reads at boot; none is linked
into `openpete-spyro1`.

| Component                          | License | Notes                                                                 |
| ---------------------------------- | ------- | --------------------------------------------------------------------- |
| crt-beans (`sdk/examples/crt-beans`) | MIT     | port of https://github.com/aduffey/crt-beans; the mod ships its `LICENSE` |

The other examples are original to this project and carry the root licence.

## Linux AppImage — bundled at runtime

Every library under `usr/lib/` ships its notice as
`usr/licences/<soname>.txt`; packaging fails on a bundled library with no
notice.

| Component                              | License                              | Notes                                                              |
| -------------------------------------- | ------------------------------------ | ------------------------------------------------------------------ |
| OpenAL Soft 1.19.1, Debian 11 package (`usr/lib/libopenal.so.1`) | LGPL v2    | dynamic linking preserved                                          |
| sndio (`usr/lib/libsndio.so.7`)        | ISC                                  | required by OpenAL Soft (`DT_NEEDED`), unmodified                  |
| libbsd (`usr/lib/libbsd.so.0`)         | BSD-2/BSD-3/ISC and others           | required by sndio, unmodified                                      |
| libmd (`usr/lib/libmd.so.0`)           | BSD-2/BSD-3 and others               | required by libbsd, unmodified                                     |
| AppImage type-2 runtime                | MIT                                  | the launcher stub prepended to the squashfs                        |

Graphics, display-server and audio-device libraries are deliberately **not**
bundled — they must bind to the host's drivers. sndio is bundled despite
being an audio library because it is a client of the `sndiod` daemon rather
than of a device: with no daemon running, OpenAL falls through to its ALSA
or PulseAudio backend, both of which still resolve against the host. ALSA
itself stays unbundled, so `libasound.so.2` remains a host requirement.

## Trademarks

_Spyro the Dragon_ and related marks are trademarks of their respective
owners. OpenPete is not affiliated with or endorsed by Sony Interactive
Entertainment, Insomniac Games, or Activision. OpenPete distributes no game
code or assets; a user supplies their own copy of the game.
