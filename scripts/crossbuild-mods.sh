#!/usr/bin/env bash
# Cross-build each C mod's Windows DLL on a Linux host.
#
# The Windows exe compiles a mod on boot by spawning a compiler
# (core/mods/mod_loader.c, mods_resolve_cc + mod_compile). On a Linux
# development box that path cannot run: the exe is a Windows process and
# the host's mingw driver is an ELF binary it cannot spawn. Without a
# DLL the loader logs "no compiler and no prebuilt ... disabled" and the
# mod is inert while the run still succeeds. This script produces the
# missing artifact so mods living outside the repo tree can be tested on
# the windows-mingw exe.
#
# The compile line mirrors mod_compile()'s Windows branch and the
# cross-preset rule in games/spyro-1/CMakeLists.txt:
#
#   <cc> -shared -Wl,--export-all-symbols -O2 -g -fno-strict-aliasing \
#        -Wall -Wundef -static-libgcc \
#        -I<sdk>/games/<game> -I<sdk> \
#        -o <out>/<id>/.build/<game>/<id>.dll <mod>/src/*.c \
#        <sdk>/openpete_mod_abi.c
#
# openpete_mod_abi.c is mandatory: it emits the openpete_mod_abi
# descriptor the loader checks before calling the entry point, and a DLL
# without it is refused. With the per-game directory on the include path
# it also records the game (openpete_mod_game), which the loader matches
# against the running game. --export-all-symbols keeps every global
# exported when a mod source marks one dllexport, which would otherwise
# turn the MinGW linker's export of every global off. -static-libgcc is mandatory: the exe ships no
# mingw runtime DLLs, so the mod DLL must be self-contained.
#
# The per-mod stamps <id>.hash and <id>.fail are deleted after a
# successful build. An
# unstamped DLL is the loader's cross-handoff form: build_mod() loads it
# silently on a host with no compiler and recompiles it once on a host
# that has one, rather than reporting it as built from different
# sources.
#
# The mod id comes from mod.toml's [mod] id key, not from the directory
# name: the loader looks for .build/<game>/<id>.dll under that id.
#
# Usage:
#   scripts/crossbuild-mods.sh [--toolchain DIR] [--mods-dir DIR]
#                              [--out DIR] [--sdk DIR] [--game ID] [<mod>...]
#
#   --toolchain DIR  unpacked llvm-mingw tree (Linux-hosted) whose
#                    bin/ holds the cross driver. Default: $OPENPETE_TOOLCHAIN,
#                    then a Linux-hosted llvm-mingw* tree under
#                    <spyro>/toolchains/, then x86_64-w64-mingw32-gcc on PATH.
#   --mods-dir DIR   mods root; every subdirectory with a mod.toml is a
#                    mod. Default: <repo>/mods.
#   --out DIR        write <DIR>/<id>/.build/<game>/<id>.dll instead of
#                    building in place under the mods root. Sources are not
#                    copied.
#   --sdk DIR        staged SDK root. Default: the windows-mingw-launcher
#                    build's sdk/, else the windows-mingw build's (beside the
#                    one executable), else a static tree's games/<game>/sdk/.
#   --game ID        the game to build for: its per-game SDK directory
#                    <sdk>/games/ID/ and object directory .build/ID/.
#                    Default spyro-1.
#   <mod>...         directory names to build; default every mod with src/*.c.
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib/mingw.sh
source "$REPO/scripts/lib/mingw.sh"
SPYRO=$(mingw_spyro_root "$REPO")

TOOLCHAIN="${OPENPETE_TOOLCHAIN:-}"
MODS_DIR=""
OUT_DIR=""
SDK_DIR=""
GAME="spyro-1"
SELECT=()

usage() {
    sed -n '/^# Usage:/,/^set -euo/p' "${BASH_SOURCE[0]}" \
        | sed '$d;s/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --toolchain) TOOLCHAIN="${2:-}"; shift 2 ;;
        --mods-dir)  MODS_DIR="${2:-}";  shift 2 ;;
        --out)       OUT_DIR="${2:-}";   shift 2 ;;
        --sdk)       SDK_DIR="${2:-}";   shift 2 ;;
        --game)      GAME="${2:-}";      shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        --*)         echo "crossbuild-mods: unknown option: $1" >&2
                     usage >&2; exit 64 ;;
        *)           SELECT+=("$1"); shift ;;
    esac
done

die() { echo "crossbuild-mods: $1" >&2; exit "${2:-1}"; }

# --- mods root ------------------------------------------------------------
[[ -n "$MODS_DIR" ]] || MODS_DIR="$REPO/mods"
[[ -d "$MODS_DIR" ]] || die "mods dir not found: $MODS_DIR (pass --mods-dir DIR)" 64
MODS_DIR=$(cd -- "$MODS_DIR" && pwd)

# --- SDK ------------------------------------------------------------------
if [[ -z "$SDK_DIR" ]]; then
    for c in "$REPO/build/windows-mingw-launcher/sdk" \
             "$REPO/build/windows-mingw/sdk" \
             "$REPO/build/windows-mingw/games/$GAME/sdk"; do
        [[ -f "$c/openpete_mod_abi.c" ]] && { SDK_DIR="$c"; break; }
    done
fi
[[ -n "$SDK_DIR" ]] || die "no staged Windows SDK found under build/windows-mingw*/sdk or build/windows-mingw/games/$GAME/sdk; build a windows-mingw preset or pass --sdk DIR" 64
[[ -f "$SDK_DIR/openpete_mod_abi.c" ]] \
    || die "$SDK_DIR has no openpete_mod_abi.c - not a staged SDK" 64
[[ -f "$SDK_DIR/games/$GAME/openpete_game.h" ]] \
    || die "$SDK_DIR has no games/$GAME/openpete_game.h - not a $GAME SDK" 64
SDK_DIR=$(cd -- "$SDK_DIR" && pwd)

# --- compiler -------------------------------------------------------------
# A tree with only .exe drivers is the Windows-hosted release, which this
# host cannot run: name that case rather than fail at the first compile.
CC=""
if [[ -n "$TOOLCHAIN" ]]; then
    [[ -d "$TOOLCHAIN" ]] || die "toolchain not found: $TOOLCHAIN" 64
    CC=$(mingw_driver_in "$TOOLCHAIN") || {
        if compgen -G "$TOOLCHAIN/bin/x86_64-w64-mingw32-*.exe" >/dev/null; then
            die "$TOOLCHAIN is a Windows-hosted llvm-mingw tree (.exe drivers only); this host needs the Linux-hosted build of the same release" 64
        fi
        die "$TOOLCHAIN/bin has no x86_64-w64-mingw32-clang or -gcc" 64
    }
else
    CC=$(mingw_find_cc "$SPYRO") || CC=""
fi
[[ -n "$CC" ]] || die "no cross compiler: no Linux-hosted llvm-mingw tree under $SPYRO/toolchains and no x86_64-w64-mingw32-gcc on PATH (pass --toolchain DIR)" 64

echo "crossbuild-mods: cc       $CC"
echo "crossbuild-mods: sdk      $SDK_DIR (games/$GAME)"
echo "crossbuild-mods: mods-dir $MODS_DIR"
[[ -n "$OUT_DIR" ]] && echo "crossbuild-mods: out      $OUT_DIR"

# --- mod list -------------------------------------------------------------
mod_id() {  # mod_id <mod.toml> -> the [mod] table's id, empty if absent
    awk '
        /^[[:space:]]*\[/ { in_mod = ($0 ~ /^[[:space:]]*\[mod\]/); next }
        in_mod && /^[[:space:]]*id[[:space:]]*=/ {
            if (match($0, /"[^"]*"/)) {
                print substr($0, RSTART + 1, RLENGTH - 2); exit
            }
        }
    ' "$1"
}

CANDIDATES=()
if [[ ${#SELECT[@]} -gt 0 ]]; then
    for m in "${SELECT[@]}"; do
        [[ -f "$MODS_DIR/$m/mod.toml" ]] \
            || die "$MODS_DIR/$m has no mod.toml" 64
        CANDIDATES+=("$m")
    done
else
    for t in "$MODS_DIR"/*/mod.toml; do
        [[ -f "$t" ]] || continue
        CANDIDATES+=("$(basename -- "$(dirname -- "$t")")")
    done
fi
[[ ${#CANDIDATES[@]} -gt 0 ]] || die "no mod.toml under $MODS_DIR" 64

FLAGS=(-shared -Wl,--export-all-symbols -O2 -g -fno-strict-aliasing -Wall
       -Wundef -static-libgcc
       -I"$SDK_DIR/games/$GAME" -I"$SDK_DIR")

built=0; skipped=0; failed=0
FAILED_IDS=()
printf '\n%-32s %-10s %s\n' MOD RESULT DETAIL

for dir in "${CANDIDATES[@]}"; do
    mod_dir="$MODS_DIR/$dir"
    id=$(mod_id "$mod_dir/mod.toml")
    [[ -n "$id" ]] || { printf '%-32s %-10s %s\n' "$dir" SKIP \
        "mod.toml has no [mod] id"; skipped=$((skipped + 1)); continue; }
    [[ "$id" == "$dir" ]] || echo "crossbuild-mods: note: $dir declares id \"$id\"; the DLL is named after the id" >&2

    srcs=()
    while IFS= read -r -d '' f; do srcs+=("$f"); done \
        < <(find "$mod_dir/src" -maxdepth 1 -name '*.c' -type f -print0 \
            2>/dev/null | sort -z)
    if [[ ${#srcs[@]} -eq 0 ]]; then
        printf '%-32s %-10s %s\n' "$id" SKIP "no src/*.c (assets-only or binary-only)"
        skipped=$((skipped + 1)); continue
    fi

    if [[ -n "$OUT_DIR" ]]; then build_dir="$OUT_DIR/$id/.build/$GAME"
    else                         build_dir="$mod_dir/.build/$GAME"; fi
    mkdir -p "$build_dir"
    dll="$build_dir/$id.dll"
    log="$build_dir/compile.log"
    # Stale outputs produce false verdicts: the previous DLL and log go
    # before the compile, and the DLL is asserted to exist after it.
    rm -f -- "$dll" "$log"

    if "$CC" "${FLAGS[@]}" -o "$dll" "${srcs[@]}" \
            "$SDK_DIR/openpete_mod_abi.c" >"$log" 2>&1 && [[ -f "$dll" ]]; then
        # The stamp would mark the DLL as built from a known source+SDK
        # key; a cross-built DLL carries none, so a Windows host with a
        # compiler rebuilds it instead of trusting this one.
        rm -f -- "${build_dir:?}/$id.hash" "${build_dir:?}/$id.fail"
        printf '%-32s %-10s %s\n' "$id" OK \
            "$(du -h -- "$dll" | cut -f1) from ${#srcs[@]} source(s)"
        built=$((built + 1))
    else
        first=$(grep -m1 -E ': (error|fatal error):' "$log" || head -n1 "$log")
        printf '%-32s %-10s %s\n' "$id" FAIL "${first:-see $log}"
        echo "crossbuild-mods: $id: full log at $log" >&2
        FAILED_IDS+=("$id")
        failed=$((failed + 1))
    fi
done

printf '\ncrossbuild-mods: %d built, %d skipped, %d failed\n' \
    "$built" "$skipped" "$failed"
if [[ $failed -gt 0 ]]; then
    echo "crossbuild-mods: failed: ${FAILED_IDS[*]}" >&2
    exit 1
fi
[[ $built -gt 0 ]] || die "nothing built: every candidate was skipped" 2
