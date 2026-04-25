# NOTICE — third-party attributions

OpenPete bundles or depends on the following projects. Each retains its
upstream license; this file is a directory of where to look.

## Submodules

| Path                          | Project       | License                  | Upstream                                   |
|-------------------------------|---------------|--------------------------|--------------------------------------------|
| `games/spyro-1/spyro-1/`      | spyro-1 decomp| see submodule's `LICENSE`| <https://github.com/TheMobyCollective/spyro-1>|
| `third_party/SDL/`            | SDL2          | Zlib                     | <https://github.com/libsdl-org/SDL>        |
| `third_party/PsyCross/`       | PsyCross      | see submodule's license  | <https://github.com/OpenDriver2/PsyCross>  |
| `third_party/cubeb/`          | Cubeb         | ISC                      | <https://github.com/mozilla/cubeb>         |

## Vendored

| Path                          | Project       | License                  | Upstream                                   |
|-------------------------------|---------------|--------------------------|--------------------------------------------|
| `third_party/psxrecomp/`      | psxrecomp     | see `third_party/psxrecomp/LICENSE` | <https://github.com/mstan/psxrecomp> |
| `third_party/libco/`          | libco         | ISC                      | <https://github.com/higan-emu/libco>       |
| `third_party/stb/`            | stb_image_write | public domain / MIT    | <https://github.com/nothings/stb>          |
| `third_party/glad/`           | GLAD (deferred)| MIT-equivalent          | <https://glad.dav1d.de>                    |
| `third_party/SDL2-mingw/`     | SDL2 prebuilt MinGW | Zlib                | <https://github.com/libsdl-org/SDL/releases> |

Each vendored directory carries an `ORIGIN.txt` recording the upstream URL,
commit SHA (where applicable), and vendor date. Local patches against
psxrecomp will live under `third_party/psxrecomp/PATCHES/` as standalone
`.patch` files for upstream-sync hygiene.

## Trademarks

*Spyro the Dragon* and related marks are trademarks of their respective
owners. OpenPete is not affiliated with or endorsed by Sony Computer
Entertainment, Insomniac Games, or Activision.
