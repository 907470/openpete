# The Linux release build base (distribution D6.6; plan
# openpete-distribution-local-recomp).
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
# C++17 surface (glslang, imgui, cubeb, disc_library.cpp) fine, and using
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
#   docker build -f docs/linux-release.Dockerfile -t openpete-linux-release .
#   scripts/release-container.sh            # build + package inside it
FROM debian:bullseye

# Bullseye's python 3.9 has no tomllib; the build-time fences fall back
# to tomli, which bullseye does not package, so it comes from pip, as
# does mkdocs-material for the SDK docs site package-dist.sh stages.
# Bullseye ships CMake 3.18; the project requires >= 3.22, so CMake comes
# from Kitware's own apt repository rather than from a source build.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg git \
        build-essential ninja-build pkg-config python3 python3-pip glslang-tools \
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
    && apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir tomli mkdocs-material==9.7.6

# Git refuses to describe a repo owned by another uid, and the version
# stamp in every artifact name comes from `git describe`.
RUN git config --system --add safe.directory '*'

WORKDIR /src
