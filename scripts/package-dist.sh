#!/usr/bin/env bash
# package-dist.sh — assemble end-user distribution archives
# (distribution D3; plan openpete-distribution-local-recomp).
#
# Ships ONLY our own code + tools + facts-metadata. No game bytes: no
# recomp module (users first-boot-compile it), no disc data, ROM-free
# icon (the dist presets pin OPENPETE_ROM_FREE_ICON=ON).
#
#   scripts/package-dist.sh linux   [--out DIR]
#   scripts/package-dist.sh windows [--out DIR] [--toolchain DIR]
#
# Inputs: the corresponding *-shared preset build (linux-release-shared /
# windows-mingw-shared) with its staged recomp/ directory. For Windows,
# --toolchain points at an UNPACKED llvm-mingw (ucrt x86_64) to bundle;
# it is trimmed (x86_64-only, C-only) into the archive. Linux v1 uses
# the system compiler (documented in README-dist; bundled-clang for
# Linux is release-engineering follow-up).
set -euo pipefail
cd "$(dirname "$0")/.."

PLATFORM="${1:-}"
shift || true
OUT=dist
TOOLCHAIN=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --toolchain) TOOLCHAIN="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo dev)
NAME="openpete-spyro1-${VERSION}-${PLATFORM}-x86_64"
STAGE="$OUT/$NAME"

stage_common() {  # stage_common <build-game-dir>
    local B="$1"
    mkdir -p "$STAGE/recomp"
    cp -a "$B/recomp/manifest" "$STAGE/recomp/"
    cp -a "$B/recomp/src"      "$STAGE/recomp/"
    mkdir -p "$STAGE/assets/music"
    cp games/spyro-1/assets/music/manifest.txt "$STAGE/assets/music/" 2>/dev/null || true
    cp gamecontrollerdb.txt "$STAGE/" 2>/dev/null || true
    cp LICENSE NOTICE.md "$STAGE/"
    sed -e "s/@VERSION@/$VERSION/g" -e "s/@PLATFORM@/$PLATFORM/g" \
        docs/README-dist.md.in > "$STAGE/README.md"
}

case "$PLATFORM" in
linux)
    B=build/linux-release-shared/games/spyro-1
    [[ -x "$B/openpete-spyro1" ]] || { echo "build linux-release-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1" "$STAGE/"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    cp third_party/psxrecomp/build/PSXRecomp "$STAGE/recomp/tools/"
    if [[ -n "$TOOLCHAIN" ]]; then
        # Bundle a Linux clang tree. find_cc (recomp_build.cpp) prefers
        # recomp/tools/clang/bin/clang over the system cc. A real
        # self-contained clang tarball trims like the Windows one
        # (plan D3.1); this stages whatever tree is pointed at.
        echo "[package] bundling clang from $TOOLCHAIN"
        mkdir -p "$STAGE/recomp/tools/clang"
        cp -a "$TOOLCHAIN"/. "$STAGE/recomp/tools/clang/"
    else
        echo "[package] NOTE: no --toolchain — Linux archive uses the" \
             "system cc (README-dist documents this)." >&2
    fi
    tar cJf "$OUT/$NAME.tar.xz" -C "$OUT" "$NAME"
    ;;
windows)
    B=build/windows-mingw-shared/games/spyro-1
    [[ -f "$B/openpete-spyro1.exe" ]] || { echo "build windows-mingw-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1.exe" "$STAGE/"
    cp "$B/OpenAL32.dll" "$STAGE/" 2>/dev/null \
        || cp third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll "$STAGE/"
    # OpenAL Soft is LGPL: its license text must ride with the DLL (NOTICE.md).
    cp third_party/OpenAL-mingw/COPYING "$STAGE/OpenAL-COPYING.txt"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    cp third_party/psxrecomp/build-mingw/PSXRecomp.exe "$STAGE/recomp/tools/"
    cp "$B/recomp/tools/libopenpete-exe-exports.a" "$STAGE/recomp/tools/"
    if [[ -n "$TOOLCHAIN" ]]; then
        echo "[package] trimming toolchain from $TOOLCHAIN"
        T="$STAGE/recomp/tools/llvm-mingw"
        mkdir -p "$T"
        cp -a "$TOOLCHAIN"/bin "$TOOLCHAIN"/lib "$TOOLCHAIN"/include \
              "$TOOLCHAIN"/x86_64-w64-mingw32 "$T/" 2>/dev/null || true
        # x86_64 + C-focused trim (see plan D3.1 measurements)
        ( cd "$T/bin" && ls | grep -E "^(aarch64|armv7|i686)" | xargs -r rm -f \
          && rm -f lldb* clang-tidy* clang-format* clang-scan-deps* \
                   llvm-pdbutil* llvm-cov* llvm-profdata* liblldb* 2>/dev/null || true )
        rm -rf "$T"/lib/liblldb* "$T"/lib/libscanbuild "$T"/lib/libear
    else
        echo "[package] NOTE: no --toolchain given — archive will need" \
             "\$OPENPETE_CC or a later toolchain drop-in for first boot" >&2
    fi
    ( cd "$OUT" && python3 -m zipfile -c "$NAME.zip" "$NAME" )
    ;;
*)
    echo "usage: scripts/package-dist.sh {linux|windows} [--out DIR] [--toolchain DIR]" >&2
    exit 64
    ;;
esac

echo "[package] $OUT/$NAME ready"
ls -la "$OUT" | grep "$NAME"
