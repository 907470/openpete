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
# --with-mod-sdk additionally stages the mod SDK headers + a mods/ drop
# folder, which the boot-time mod compiler needs. Releases ship without
# it; the dist gate passes it to keep that path exercised.
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
WITH_MOD_SDK=0
# --sign '<command>' — Authenticode hook, Windows platform only. Invoked
# once per first-party PE (openpete-spyro1.exe, PSXRecomp.exe) with the
# file path appended; a nonzero exit fails the package. Evaluated per
# file so embedded $(...) (e.g. a fresh az access token) re-runs each
# time. The bundled llvm-mingw binaries are NOT signed (SmartScreen
# only screens the shell-executed download; ~400 upstream tools would
# burn signatures attesting binaries we didn't build). jsign example:
#   --sign 'jsign --storetype TRUSTEDSIGNING
#           --keystore eus.codesigning.azure.net
#           --storepass "$(az account get-access-token
#               --resource https://codesigning.azure.net
#               --query accessToken -o tsv)"
#           --alias <account>/<profile>
#           --tsaurl http://timestamp.acs.microsoft.com'
# The RFC3161 timestamp is NOT optional with Artifact Signing: its
# certs live ~3 days; only the countersignature keeps releases valid.
SIGN_CMD=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --toolchain) TOOLCHAIN="$2"; shift 2 ;;
        --sign) SIGN_CMD="$2"; shift 2 ;;
        --with-mod-sdk) WITH_MOD_SDK=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

sign_pe() {  # sign_pe <file> — no-op unless --sign was given
    [[ -n "$SIGN_CMD" ]] || return 0
    echo "[package] signing $(basename "$1")"
    eval "$SIGN_CMD \"\$1\"" || { echo "[package] signing FAILED: $1" >&2; exit 1; }
}

VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo dev)
NAME="openpete-spyro1-${VERSION}-${PLATFORM}-x86_64"
STAGE="$OUT/$NAME"

stage_common() {  # stage_common <build-game-dir>
    local B="$1"
    mkdir -p "$STAGE/recomp"
    cp -a "$B/recomp/manifest" "$STAGE/recomp/"
    cp -a "$B/recomp/src"      "$STAGE/recomp/"
    # The kit is compiled on the user's machine, so it ships as readable
    # C — but its comments are engineering notes about the host tree.
    scripts/strip-comments.py "$STAGE"/recomp/src/*
    if [[ $WITH_MOD_SDK -eq 1 ]]; then
        # Mod SDK + drop-in mod dir (openpete-mod-compile-on-boot): mods
        # ship mod.toml + src/ and compile at boot against <exe>/sdk with
        # the bundled toolchain. The SDK is a handful of headers.
        cp -a "$B/sdk" "$STAGE/sdk"
        mkdir -p "$STAGE/mods"
        printf '%s\n' \
            "Drop mods here: mods/<id>/ (or mods/<id>.zip) containing" \
            "mod.toml, plus src/*.c (compiled automatically at boot) and/or" \
            "assets/. mods/enabled.txt (one id per line) fixes the load" \
            "order; without it every mod loads, alphabetically." \
            > "$STAGE/mods/README.txt"
    fi
    cp gamecontrollerdb.txt "$STAGE/" 2>/dev/null || true
    cp LICENSE NOTICE.md "$STAGE/"
    # Preconfigured settings, so a first run looks the way we intend
    # rather than falling back to bare defaults. Both in-game menus
    # rewrite this file, so anything here is only a starting point.
    cp docs/openpete-dist.toml "$STAGE/openpete.toml"
    # The README template carries both platforms' wording; keep only the
    # blocks for the one being packaged.
    awk -v want="@$PLATFORM" '
        /^<!-- @(windows|linux) -->$/ { skip = ($2 != want); next }
        /^<!-- @end -->$/             { skip = 0; next }
        !skip
    ' docs/README-dist.md.in \
        | sed -e "s/@VERSION@/$VERSION/g" -e "s/@PLATFORM@/$PLATFORM/g" \
        > "$STAGE/README.md"
}

case "$PLATFORM" in
linux)
    B=build/linux-release-shared/games/spyro-1
    [[ -x "$B/openpete-spyro1" ]] || { echo "build linux-release-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1" "$STAGE/"
    # Strip debug info from the SHIPPED copy only (RelWithDebInfo embeds
    # thousands of absolute build paths — a username/layout leak). The
    # build-tree exe keeps its symbols for dev debugging. Exports live in
    # .dynsym, which strip preserves, so the first-boot module still binds.
    strip "$STAGE/openpete-spyro1"
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
    [[ -n "$SIGN_CMD" ]] && echo "[package] NOTE: --sign is Windows-only" \
        "(Authenticode); ignored for linux" >&2
    tar cJf "$OUT/$NAME.tar.xz" -C "$OUT" "$NAME"
    ( cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
    ;;
windows)
    B=build/windows-mingw-shared/games/spyro-1
    [[ -f "$B/openpete-spyro1.exe" ]] || { echo "build windows-mingw-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1.exe" "$STAGE/"
    # Strip debug info from the SHIPPED copy only (same rationale as the
    # Linux branch). PE exports live in the export table data directory,
    # not the COFF symbol table, and the first-boot module links against
    # the staged implib — stripping cannot break either.
    x86_64-w64-mingw32-strip "$STAGE/openpete-spyro1.exe"
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
        # Upstream llvm-mingw's bin/ is FLAT — every entry is a driver binary,
        # wrapper script or config. Anything that is a directory got there by
        # accident (a build run with its cwd inside the toolchain leaves a whole
        # tree behind, which then rides into the archive: 372MB of stale objects
        # and a recursive copy of the toolchain itself shipped this way once).
        # Prune them so a polluted source tree can't quietly bloat a release.
        find "$T/bin" -mindepth 1 -maxdepth 1 -type d -print -exec rm -rf {} + \
            | sed 's|^|[package] dropped stray dir from toolchain bin: |'
        # x86_64 + C-focused trim (see plan D3.1 measurements)
        ( cd "$T/bin" && ls | grep -E "^(aarch64|armv7|i686)" | xargs -r rm -f \
          && rm -f lldb* clang-tidy* clang-format* clang-scan-deps* \
                   llvm-pdbutil* llvm-cov* llvm-profdata* liblldb* 2>/dev/null || true )
        rm -rf "$T"/lib/liblldb* "$T"/lib/libscanbuild "$T"/lib/libear
    else
        echo "[package] NOTE: no --toolchain given — archive will need" \
             "\$OPENPETE_CC or a later toolchain drop-in for first boot" >&2
    fi
    # Sign after strip and staging, before archiving. First-party PEs
    # only (see --sign notes above).
    sign_pe "$STAGE/openpete-spyro1.exe"
    sign_pe "$STAGE/recomp/tools/PSXRecomp.exe"
    # Zipped from INSIDE the stage dir, so the archive has no wrapping
    # folder: Windows' "Extract All" already creates one named after the
    # zip, and a second level leaves users an install nested one deep.
    ( cd "$STAGE" && python3 -m zipfile -c "../$NAME.zip" . )
    ( cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" )
    ;;
*)
    echo "usage: scripts/package-dist.sh {linux|windows} [--out DIR] [--toolchain DIR]" >&2
    exit 64
    ;;
esac

echo "[package] $OUT/$NAME ready"
ls -la "$OUT" | grep "$NAME"
