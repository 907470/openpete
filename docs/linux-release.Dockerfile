# The Linux release build base.
#
# Debian 11 (bullseye) — glibc 2.31, GLIBCXX 3.4.28 — because the symbol
# floor of a binary is decided by the machine that built it, not by any
# flag. A dev-box build (Ubuntu 22.04, glibc 2.35) requires GLIBC_2.34
# and refuses to start on anything older than Ubuntu 22.04 / Debian 12,
# with a loader error the user cannot act on. Building here covers
# Ubuntu 20.04+, Debian 11+ and RHEL 9, and matches Steam Runtime 3
# (sniper), which keeps a Steam/Flatpak path open at no extra cost.
#
# Deliberately NOT here: a newer gcc. The base's own gcc 10 compiles the
# C++17 surface (glslang, imgui, cubeb, games/spyro-1/dist/disc_library.cpp) fine, and using
# it is what keeps the GLIBCXX floor low for free — a backported compiler
# would raise the floor again. Old X11/Wayland headers are harmless:
# SDL3 dlopens those clients at runtime, but refuses to configure without
# at least one of X11 or Wayland present at build time, so the set below
# mirrors the dev box's (libdecor is absent from bullseye; SDL3 dlopens it
# too). Vulkan headers are vendored (third_party/vulkan-headers).
#
# The bundled clang the image ships is NOT built here — it is the
# llvm.org 18.1.8 ubuntu-18.04 tarball (scripts/setup_linux_clang.sh),
# whose own floor is lower still.
#
#   scripts/release-container.sh --appimage   # builds this image on first use
#                                             # (tag openpete-linux-release:<Dockerfile
#                                             # sha256, 12 hex>), then packages in it
#
# The base is pinned by digest (the debian:bullseye multi-arch index on
# Docker Hub), so a rebuild of this image starts from the same bytes; tomli
# is pinned for the same reason. Debian 11 is past its LTS window: its
# packages are leaving deb.debian.org, whose bullseye-security index names
# files its pool no longer serves (404). The apt sources therefore point
# at snapshot.debian.org at one fixed instant, so every rebuild installs
# the same package versions. SNAPSHOT is the last instant before the image
# v0.4.1 and v0.4.2 were built in (2026-09-04 19:55 UTC) whose security
# pool serves every file its index lists (at 20260904T190000Z perl,
# openssl and python3.9 404); a rebuild at it installs that image's dpkg
# set exactly. CMake comes from Kitware's repository, which keeps old
# versions, so it is pinned by version, and every pip package, the
# dependencies of mkdocs-material included, is pinned to that image's.
FROM debian:bullseye@sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9

ARG SNAPSHOT=20260903T000000Z
ARG CMAKE_VERSION=4.4.3-0kitware1ubuntu20.04.1
RUN printf '%s\n' \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} bullseye main" \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/${SNAPSHOT} bullseye-security main" \
      "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} bullseye-updates main" \
      > /etc/apt/sources.list

# Bullseye's python 3.9 has no tomllib; the build-time fences fall back
# to tomli, which bullseye does not package, so it comes from pip, as
# does mkdocs-material for the SDK docs site package-dist.sh stages.
# Bullseye ships CMake 3.18; the project requires >= 3.22, so CMake comes
# from Kitware's own apt repository rather than from a source build.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg git \
        build-essential ninja-build pkg-config python3 python3-pip glslang-tools spirv-tools \
        libopenal-dev libasound2-dev libpulse-dev libsndio-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
        libxi-dev libxss-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols \
        libdrm-dev libgbm-dev libgl-dev libegl-dev libgles-dev \
        libegl1-mesa-dev libdbus-1-dev libudev-dev libibus-1.0-dev \
        xz-utils file squashfs-tools desktop-file-utils \
    && curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
         | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
https://apt.kitware.com/ubuntu/ focal main" \
         > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends \
         "cmake=${CMAKE_VERSION}" \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir tomli==2.0.1 mkdocs-material==9.7.6 \
         babel==2.18.0 backrefs==6.2 certifi==2026.7.22 charset-normalizer==3.5.1 \
         click==8.1.8 colorama==0.4.6 ghp-import==2.1.0 idna==3.19 \
         importlib-metadata==8.7.1 jinja2==3.1.6 markdown==3.9 markupsafe==3.0.3 \
         mergedeep==1.3.4 mkdocs-get-deps==0.2.2 mkdocs-material-extensions==1.3.1 \
         mkdocs==1.6.1 packaging==26.3 paginate==0.5.7 pathspec==1.1.1 \
         platformdirs==4.4.0 pygments==2.21.0 pymdown-extensions==10.21.3 \
         python-dateutil==2.9.0.post0 pyyaml-env-tag==1.1 pyyaml==6.0.3 \
         requests==2.32.5 six==1.17.0 urllib3==2.6.3 watchdog==6.0.0 zipp==3.23.1

# Git refuses to describe a repo owned by another uid, and the version
# stamp in every artifact name comes from `git describe`.
RUN git config --system --add safe.directory '*'

WORKDIR /src
