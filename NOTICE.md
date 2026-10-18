# NOTICE - third-party attributions

OpenPete's own code is licensed under the MIT License (see `LICENSE`). It
bundles or depends on the third-party projects below; each retains its
upstream license. This file is the authoritative record of what
travels in a distributed build and under which terms. Every release archive
ships this file and, under `licences/`, the upstream license text of each
component below that the archive carries; `packaging/licences/manifest.tsv` maps
each row to its text, and packaging fails on a row with no text.

## Linked into the distributed binary - permissive (attribution required)

| Component                            | License                                        | Upstream                                                                                    |
| ------------------------------------ | ---------------------------------------------- | ------------------------------------------------------------------------------------------- |
| spyro-1                              | CC0 1.0 Universal                              | https://github.com/TheMobyCollective/spyro-1                                                |
| PsyCross                             | MIT                                            | https://github.com/OpenDriver2/PsyCross                                                     |
| PsyCross GTE core (`src/gte/PsyX_GTE.cpp`: MAME `gte.cpp` by smf via TOMB5 `LIBGTE.C`) | BSD-3-Clause (smf); MIT (TOMB5)              | https://github.com/mamedev/mame ; https://github.com/TOMB5/TOMB5                           |
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
| libstdc++ / libgcc (static)          | GPL-3.0 **with GCC Runtime Library Exception** | GCC - the exception explicitly permits distributing the compiled binary in proprietary form |
| MinGW-w64 CRT / winpthreads 8.0.0 (static, Windows) | Zlib / BSD / public domain (permissive) | https://www.mingw-w64.org                                                          |
| OpenAL Soft 1.25.1 (`OpenAL32.dll`, Windows; text also as `OpenAL-COPYING.txt`) | LGPL v2 | https://github.com/kcat/openal-soft                                                  |

## Bundled data - shipped alongside the binary

Data files copied into every release archive. Not linked into
`openpete`; read at runtime.

| Component                                | License | Upstream                                          |
| ---------------------------------------- | ------- | ------------------------------------------------- |
| SDL_GameControllerDB (`gamecontrollerdb.txt`) | Zlib    | https://github.com/mdqinc/SDL_GameControllerDB |

Refreshed with `scripts/update_gamecontrollerdb.sh`.

## Bundled first-boot toolchain - mere aggregation

Shipped as separate executables under `recomp/tools/`, invoked as
subprocesses to recompile the user's disc at first boot. Each is an
independent program (mere aggregation), not linked into `openpete`.

| Component                                | License                                                          | Notes                                                      |
| ---------------------------------------- | ---------------------------------------------------------------- | ---------------------------------------------------------- |
| llvm-mingw (LLVM/Clang, lld, llvm-ar, ...) | Apache-2.0 **with LLVM exception** (+ MinGW-w64 CRT, permissive) | Windows archive; ship LLVM `LICENSE.TXT` in the bundle     |
| LLVM/Clang 18.1.8 + lld (llvm.org build) | Apache-2.0 **with LLVM exception**                               | Linux AppImage; ship LLVM `LICENSE.TXT` in the bundle      |
| glibc 2.31 headers (Debian 11 libc6-dev) | LGPL-2.1-or-later (per-file notices in the Debian copyright file) | Linux bundle, `recomp/tools/clang/sysroot/include`: headers a source mod compiles against; no glibc object code ships. `sysroot/lib/libclang_rt.builtins.a` beside them is compiler-rt from the LLVM tarball above |
| Linux 5.10 uapi headers (Debian 11 linux-libc-dev) | GPL-2.0 WITH Linux-syscall-note                                | Linux bundle, `recomp/tools/clang/sysroot/include` (`linux/`, `asm/`, `asm-generic/`, ...): the kernel's userspace API headers, which the syscall note leaves outside the GPL for programs that include them |
| PSXRecomp (psxrecomp)                    | PolyForm Noncommercial License 1.0.0                             | static recompiler, a separate executable; its output, the C translation of the user's disc, is compiled into the game against openpete's own headers (`core/include/psxrt/`). No source or header of PSXRecomp's runner (`third_party/psxrecomp/runner/`) is compiled into or shipped with any build; the unit test `tests/unit/psx_runtime_layout_test.c` includes `runner/include/psx_runtime.h` to prove the `CPUState` layout and is linked into no distributed target |
| fmt 9.1.1 (in PSXRecomp)                 | MIT                                                              | https://github.com/fmtlib/fmt; vendored at `third_party/psxrecomp/recompiler/lib/fmt`, statically linked into PSXRecomp |
| rabbitizer 1.7.10 (in PSXRecomp)         | MIT                                                              | https://github.com/Decompollaborate/rabbitizer; vendored at `third_party/psxrecomp/recompiler/lib/rabbitizer`, statically linked into PSXRecomp |

`recomp/tools/clang/lib/libtinfo.so.5` in the Linux bundle is **ours**, not
ncurses: the llvm.org binaries name that library in their DT_NEEDED but
import no symbol from it, so an empty stub with the expected version node
satisfies the loader without shipping a terminal library (see
`scripts/setup_linux_clang.sh`).

## Mod SDK example mods - bundled under `sdk/examples/`

Present only in archives packaged with the mod SDK. Each example is a
separate directory the loader compiles or reads at boot; none is linked
into `openpete`.

| Component                          | License | Notes                                                                 |
| ---------------------------------- | ------- | --------------------------------------------------------------------- |
| crt-beans (`sdk/examples/crt-beans`) | MIT     | port of https://github.com/aduffey/crt-beans; the mod ships its `LICENSE` |

The other examples are original to this project and carry the root licence.

## Linux AppImage - bundled at runtime

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
bundled - they must bind to the host's drivers. sndio is bundled despite
being an audio library because it is a client of the `sndiod` daemon rather
than of a device: with no daemon running, OpenAL falls through to its ALSA
or PulseAudio backend, both of which still resolve against the host. ALSA
itself stays unbundled, so `libasound.so.2` remains a host requirement.

## In the repository, not in any distributed build

Present under `third_party/` or produced there by a setup script. No release
archive carries them, so they have no table row and no text under
`licences/`.

- cubeb (`third_party/cubeb`, submodule) - ISC (`third_party/cubeb/LICENSE`).
  https://github.com/mozilla/cubeb. No CMake target builds it.
- DuckStation (`third_party/duckstation`, submodule pinning upstream
  `e4c55ad0283f`) - CC BY-NC-ND 4.0 (`third_party/duckstation/LICENSE`).
  https://github.com/stenzek/duckstation. The reference emulator the
  gameplay oracle captures its golden trace from; never linked, never
  packaged. openpete's own changes to it are the patch series
  `third_party/duckstation-patches/`, which `scripts/setup_duckstation.sh`
  applies to the pinned upstream on the developer's machine; no modified
  DuckStation is distributed.
- FFmpeg 6.1.1 and x264 (`third_party/ffmpeg-mingw/`, gitignored) - GPL:
  `scripts/setup_mingw_ffmpeg.sh` configures FFmpeg with `--enable-gpl
  --enable-libx264`. https://ffmpeg.org, https://code.videolan.org/videolan/x264.
  Linked statically into a `windows-mingw` executable for `--record` only
  when the prefix exists, which makes that executable GPL; a dist preset
  refuses the combination at configure (`core/CMakeLists.txt`), and
  `scripts/dist_compliance.py scan` refuses an archive whose binaries carry
  an FFmpeg or x264 marker.

- toml11 3.7.1 and ELFIO 3.11 (`third_party/psxrecomp/recompiler/lib/toml11`,
  `.../ELFIO`) - MIT (`LICENSE`, `LICENSE.txt`).
  https://github.com/ToruNiina/toml11, https://github.com/serge1/ELFIO. On
  PSXRecomp's include path; no PSXRecomp source includes either header.
- PSXRecomp's runner build (`third_party/psxrecomp/runner/CMakeLists.txt`)
  fetches GLFW 3.3.8 and fmt 10.1.1 and compiles the vendored glad under
  `runner/lib/glad`. No openpete CMake file configures that project.

The MinGW OpenAL Soft prebuilt in `third_party/OpenAL-mingw/` (version in
`VERSION`, licence in `COPYING`) is the import library, headers and
`OpenAL32.dll` of the OpenAL Soft 1.25.1 row above.

## Trademarks

_Spyro the Dragon_ and related marks are trademarks of their respective
owners. OpenPete is not affiliated with or endorsed by Sony Interactive
Entertainment, Insomniac Games, or Activision. OpenPete distributes no game
code or assets; a user supplies their own copy of the game.
