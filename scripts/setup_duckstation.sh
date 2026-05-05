#!/usr/bin/env bash
# scripts/setup_duckstation.sh — bootstrap a vendored DuckStation build with
# the openpete .psxtrace oracle hooks.
#
# DuckStation is the canonical PSX oracle for openpete: real Sony BIOS
# (SCPH1001), real PSYQ MIPS code, accurate-enough renderer to run Spyro
# past the cyclorama point where PCSX-Redux chokes. We fork it here only
# to add a custom -psxtrace flag that emits per-frame v2 records via
# an execute breakpoint (no Lua scripting subsystem needed).
#
# Usage: scripts/setup_duckstation.sh
#
# Idempotent — safe to re-run. Skips steps already complete.
set -euo pipefail
cd "$(dirname "$0")/.."

DS_DIR=".tools/duckstation"
DS_REV="e4c55ad0283ff26a76991a759d8db13d6969b9ec"
DEPS_VERSION="release-20260404"
DEPS_FILE="deps-linux-x64.tar.xz"
DEPS_SHA256="8266f12d3c43e0244411908e71cf150f4d57d5e3b0782ce0a9c295905a3efe7b"
PATCHES_DIR="scripts/duckstation-patches"

if [[ ! -d "$DS_DIR/.git" ]]; then
    echo "[setup-ds] cloning DuckStation @ $DS_REV"
    git clone https://github.com/stenzek/duckstation.git "$DS_DIR"
    ( cd "$DS_DIR" && git checkout -q "$DS_REV" )
fi

if [[ ! -d "$DS_DIR/dep/prebuilt/linux-x64" ]]; then
    echo "[setup-ds] downloading prebuilt deps $DEPS_VERSION"
    mkdir -p "$DS_DIR/dep/prebuilt"
    cd "$DS_DIR/dep/prebuilt"
    curl --fail --retry 5 -LO \
        "https://github.com/duckstation/dependencies/releases/download/${DEPS_VERSION}/${DEPS_FILE}"
    echo "${DEPS_SHA256}  ${DEPS_FILE}" | sha256sum -c -
    tar xJf "$DEPS_FILE"
    rm "$DEPS_FILE"
    cd - >/dev/null
fi

# Stage our extra source file (psxtrace_capture.{cpp,h}). Idempotent — diff
# against what's already in tree and only copy when content differs.
for f in psxtrace_capture.cpp psxtrace_capture.h; do
    src="$PATCHES_DIR/$f"
    dst="$DS_DIR/src/duckstation-regtest/$f"
    if [[ ! -f "$dst" ]] || ! cmp -s "$src" "$dst"; then
        echo "[setup-ds] staging $f"
        cp "$src" "$dst"
    fi
done

# Apply the patch (CMake list, regtest host wiring, EGL
# header fix). `git apply --check` first so re-runs are no-ops.
( cd "$DS_DIR" && \
    if git apply --check "../../$PATCHES_DIR/01-regtest-psxtrace.patch" 2>/dev/null; then
        echo "[setup-ds] applying patch 01-regtest-psxtrace"
        git apply "../../$PATCHES_DIR/01-regtest-psxtrace.patch"
    else
        echo "[setup-ds] patch already applied (skip)"
    fi )

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
