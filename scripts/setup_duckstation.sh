#!/usr/bin/env bash
# scripts/setup_duckstation.sh — bootstrap the vendored DuckStation regtest
# build that powers the openpete .psxtrace oracle.
#
# DuckStation lives as a submodule at third_party/duckstation, tracking
# our fork (https://github.com/tyscorp/duckstation, branch openpete-psxtrace).
# That branch carries two commits on top of upstream stenzek/duckstation:
#   1. build: make the EGL context available unconditionally for surfaceless EGL
#   2. regtest: add -psxtrace flag for openpete oracle capture
#
# Usage: scripts/setup_duckstation.sh
#
# Idempotent — safe to re-run. Skips steps already complete.
set -euo pipefail
cd "$(dirname "$0")/.."

DS_DIR="third_party/duckstation"
DEPS_VERSION="release-20260404"
DEPS_FILE="deps-linux-x64.tar.xz"
DEPS_SHA256="8266f12d3c43e0244411908e71cf150f4d57d5e3b0782ce0a9c295905a3efe7b"

if [[ ! -f "$DS_DIR/CMakeLists.txt" ]]; then
    echo "[setup-ds] initialising submodule"
    git submodule update --init --recursive "$DS_DIR"
fi

if [[ ! -d "$DS_DIR/dep/prebuilt/linux-x64" ]]; then
    echo "[setup-ds] downloading prebuilt deps $DEPS_VERSION"
    mkdir -p "$DS_DIR/dep/prebuilt"
    ( cd "$DS_DIR/dep/prebuilt" && \
        curl --fail --retry 5 -LO \
            "https://github.com/duckstation/dependencies/releases/download/${DEPS_VERSION}/${DEPS_FILE}" && \
        echo "${DEPS_SHA256}  ${DEPS_FILE}" | sha256sum -c - && \
        tar xJf "$DEPS_FILE" && \
        rm "$DEPS_FILE" )
fi

# Stage the BIOS DuckStation expects.
mkdir -p "$HOME/.local/share/duckstation/bios"
if [[ ! -f "$HOME/.local/share/duckstation/bios/scph1001.bin" ]]; then
    if [[ -f ".tools/pcsx-redux/scph1001.bin" ]]; then
        echo "[setup-ds] copying SCPH1001 BIOS"
        cp ".tools/pcsx-redux/scph1001.bin" "$HOME/.local/share/duckstation/bios/"
    else
        echo "[setup-ds] WARN: scph1001.bin not found at .tools/pcsx-redux/" >&2
    fi
fi

if [[ ! -x "$DS_DIR/build-regtest/bin/duckstation-regtest" ]]; then
    echo "[setup-ds] configuring + building duckstation-regtest"
    ( cd "$DS_DIR" && \
        CC=clang-19 CXX=clang++-19 cmake -B build-regtest \
            -DBUILD_REGTEST=ON \
            -DENABLE_OPENGL=ON -DENABLE_VULKAN=OFF \
            -DENABLE_WAYLAND=OFF -DENABLE_X11=OFF \
            -DCMAKE_BUILD_TYPE=Release -G Ninja
        cmake --build build-regtest --target duckstation-regtest )
fi

echo "[setup-ds] done: $DS_DIR/build-regtest/bin/duckstation-regtest"
