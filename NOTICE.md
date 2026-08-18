# NOTICE — third-party attributions

OpenPete's own code is licensed under **PolyForm Noncommercial 1.0.0** (see
`LICENSE`). It bundles or depends on the third-party projects below; each
retains its upstream license. This file is the authoritative record of what
travels in a distributed build and under which terms — the permissive
licenses require their notices to accompany the binary, so this file (and,
where noted, the upstream license texts) ships in every release archive.

## Linked into the distributed binary — permissive (attribution required)

| Component                            | License                                        | Upstream                                                                                    |
| ------------------------------------ | ---------------------------------------------- | ------------------------------------------------------------------------------------------- |
| spyro-1                              | CC0 1.0 Universal                              | https://github.com/TheMobyCollective/spyro-1                                                |
| PsyCross                             | MIT                                            | https://github.com/OpenDriver2/PsyCross                                                     |
| SDL (SDL **3**)                      | Zlib                                           | https://github.com/libsdl-org/SDL                                                           |
| miniz                                | MIT                                            | https://github.com/richgel999/miniz                                                         |
| tomlc99                              | MIT                                            | https://github.com/cktan/tomlc99                                                            |
| stb_image_write                      | public domain / MIT (dual)                     | https://github.com/nothings/stb                                                             |
| libco                                | ISC                                            | https://github.com/higan-emu/libco                                                          |
| xxHash                               | BSD-2-Clause                                   | https://github.com/Cyan4973/xxHash                                                          |
| glad (GL loader)                     | MIT                                            | https://github.com/Dav1dde/glad                                                             |
| volk (Vulkan loader)                 | MIT                                            | https://github.com/zeux/volk                                                                |
| Vulkan-Headers                       | Apache-2.0                                     | https://github.com/KhronosGroup/Vulkan-Headers                                              |
| libstdc++ / libgcc (static)          | GPL-3.0 **with GCC Runtime Library Exception** | GCC — the exception explicitly permits distributing the compiled binary in proprietary form |
| MinGW-w64 CRT / winpthreads (static) | Zlib / BSD / public domain (permissive)        | https://www.mingw-w64.org                                                                   |
| OpenAL Soft 1.25.1 (`OpenAL32.dll`)  | LGPL v2                                        | https://github.com/kcat/openal-soft                                                         |

## Bundled first-boot toolchain (Windows archive only) — mere aggregation

Shipped as separate executables under `recomp/tools/`, invoked as
subprocesses to recompile the user's disc at first boot. Not linked into
`openpete-spyro1.exe`; each is an independent program (mere aggregation).

| Component                                | License                                                          | Notes                                                      |
| ---------------------------------------- | ---------------------------------------------------------------- | ---------------------------------------------------------- |
| llvm-mingw (LLVM/Clang, lld, llvm-ar, …) | Apache-2.0 **with LLVM exception** (+ MinGW-w64 CRT, permissive) | first-boot compiler; ship LLVM `LICENSE.TXT` in the bundle |
| PSXRecomp (psxrecomp)                    | PolyForm Noncommercial License 1.0.0                             | static recompiler; build tool, not shipped-linked          |

## Trademarks

_Spyro the Dragon_ and related marks are trademarks of their respective
owners. OpenPete is not affiliated with or endorsed by Sony Interactive
Entertainment, Insomniac Games, or Activision. OpenPete distributes no game
code or assets; a user supplies their own copy of the game.
