# NOTICE — third-party attributions

OpenPete's own code is licensed under **PolyForm Noncommercial 1.0.0** (see
`LICENSE`). It bundles or depends on the third-party projects below; each
retains its upstream license. This file is the authoritative record of what
travels in a distributed build and under which terms — the permissive
licenses require their notices to accompany the binary, so this file (and,
where noted, the upstream license texts) ships in every release archive.

Distribution builds use the `*-shared` presets, are built with
`OPENPETE_SKIP_DECOMP=ON`, and **contain no game bytes** — the recompiled
game module is generated on the user's machine at first boot from a disc
they supply. See the trademark note at the bottom.

## Linked into the distributed binary — permissive (attribution required)

| Component                         | License                            | Upstream / path |
|-----------------------------------|------------------------------------|-----------------|
| PsyCross                          | MIT                                | <https://github.com/OpenDriver2/PsyCross> · `third_party/PsyCross/` |
| SDL (SDL **3**)                   | Zlib                               | <https://github.com/libsdl-org/SDL> · `third_party/SDL/` |
| miniz                             | MIT                                | <https://github.com/richgel999/miniz> · `third_party/miniz/` |
| tomlc99                           | MIT                                | <https://github.com/cktan/tomlc99> · `third_party/tomlc99/` |
| stb_image_write                   | public domain / MIT (dual)         | <https://github.com/nothings/stb> · `third_party/stb/` |
| libco                             | ISC                                | <https://github.com/higan-emu/libco> · `third_party/libco/` |
| xxHash                            | BSD-2-Clause                       | <https://github.com/Cyan4973/xxHash> · `third_party/xxhash/` |
| glad (GL loader)                  | MIT                                | <https://github.com/Dav1dde/glad> · `third_party/glad/` |
| volk (Vulkan loader)              | MIT                                | <https://github.com/zeux/volk> · `third_party/volk/` |
| Vulkan-Headers                    | Apache-2.0                         | <https://github.com/KhronosGroup/Vulkan-Headers> · `third_party/vulkan-headers/` |
| libstdc++ / libgcc (static)       | GPL-3.0 **with GCC Runtime Library Exception** | GCC — the exception explicitly permits distributing the compiled binary in proprietary form |
| MinGW-w64 CRT / winpthreads (static) | Zlib / BSD / public domain (permissive) | <https://www.mingw-w64.org/> |

## Dynamically linked / shipped alongside — LGPL

| Component                         | License   | Upstream / path |
|-----------------------------------|-----------|-----------------|
| OpenAL Soft 1.25.1 (`OpenAL32.dll`) | **LGPL v2** | <https://github.com/kcat/openal-soft> · `third_party/OpenAL-mingw/` |

OpenAL Soft is linked **dynamically** as a replaceable `OpenAL32.dll`, which
satisfies the LGPL for a source-available/closed application. To stay
compliant, a release **must**: (a) ship the LGPL license text (bundled as
`OpenAL-COPYING.txt` from `third_party/OpenAL-mingw/COPYING`); (b) note here
that OpenAL Soft is used and its source is available upstream; and (c) keep
the DLL user-replaceable (do not statically link it, do not obfuscate the
import). All three hold for the current packaging.

## Bundled first-boot toolchain (Windows archive only) — mere aggregation

Shipped as separate executables under `recomp/tools/`, invoked as
subprocesses to recompile the user's disc at first boot. Not linked into
`openpete-spyro1.exe`; each is an independent program (mere aggregation).

| Component        | License                                   | Notes |
|------------------|-------------------------------------------|-------|
| llvm-mingw (LLVM/Clang, lld, llvm-ar, …) | Apache-2.0 **with LLVM exception** (+ MinGW-w64 CRT, permissive) | first-boot compiler; ship LLVM `LICENSE.TXT` in the bundle |
| PSXRecomp (psxrecomp) | see `third_party/psxrecomp/LICENSE`  | static recompiler; build tool, not shipped-linked |

## Development / build only — NOT present in distributed builds

These are used for building, diagnostics, or the oracle, and are **excluded
from release binaries**. They are listed for completeness because some carry
copyleft terms that would matter if they ever leaked into a shipped binary.

| Component        | License   | Why it is not distributed |
|------------------|-----------|---------------------------|
| FFmpeg + libx264 (`third_party/ffmpeg-mingw/`) | **GPL** | Only linked by the `--record` video feature. The distribution presets set `OPENPETE_ENABLE_RECORD=OFF`, so no libav/x264 is compiled into release binaries. `--record` remains available in developer builds (`windows-mingw` / native `linux-release`), which are **not** for redistribution. |
| pcsx_redux_gte (`third_party/pcsx_redux_gte/`) | **GPL-3.0** | A third GTE reference for trace-logger cross-checks. `OPENPETE_BUILD_PCSX_REDUX_GTE=OFF` in distribution builds; a stub is linked instead. |
| DuckStation (`third_party/duckstation/`) | **GPL-3.0** | The PSX oracle used to capture ground-truth goldens. A separate emulator build; never linked into or shipped with OpenPete. |
| cubeb (`third_party/cubeb/`) | ISC | Present as a submodule but **not linked** (audio goes through OpenAL + SDL). |

## Private game decomp submodule

`games/spyro-1/spyro-1/` (<https://github.com/TheMobyCollective/spyro-1>) is a
private submodule carrying the reverse-engineered Spyro decompilation. It is
**not** compiled into distribution builds (`OPENPETE_SKIP_DECOMP=ON`) and no
bytes derived from it are shipped. See the submodule's own `LICENSE`.

## Trademarks

*Spyro the Dragon* and related marks are trademarks of their respective
owners. OpenPete is not affiliated with or endorsed by Sony Interactive
Entertainment, Insomniac Games, or Activision. OpenPete distributes no game
code or assets; a user supplies their own copy of the game.
