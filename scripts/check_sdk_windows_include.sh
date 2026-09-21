#!/usr/bin/env bash
# check_sdk_windows_include.sh — the mod SDK compiles ahead of <windows.h>.
#
# A Windows mod that pulls in <windows.h> gets windef.h's RECT, which the
# PSYQ types the SDK carries would collide with. The SDK spells them with a
# PSX_ prefix; this cross-compiles tests/unit/sdk_windows_include_test.c,
# which includes the staged SDK headers and then <windows.h>, and fails on a
# collision.
#
# Usage: scripts/check_sdk_windows_include.sh [--sdk DIR] [--toolchain DIR]
#
#   --sdk DIR        staged SDK headers. Default: the linux-release build's
#                    games/spyro-1/sdk, else a windows-mingw build's.
#   --toolchain DIR  unpacked Linux-hosted llvm-mingw tree. Default:
#                    $OPENPETE_TOOLCHAIN, then a llvm-mingw* tree under
#                    <spyro>/toolchains, then x86_64-w64-mingw32-gcc on PATH.
#
# Exit: 0 PASS, 1 the unit failed to compile, 64 SKIP (no cross compiler, no
# staged SDK).
set -euo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib/mingw.sh
source "$REPO/scripts/lib/mingw.sh"

TOOLCHAIN="${OPENPETE_TOOLCHAIN:-}"
SDK_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk)       SDK_DIR="${2:-}";   shift 2 ;;
        --toolchain) TOOLCHAIN="${2:-}"; shift 2 ;;
        -h|--help)   sed -n '/^# Usage:/,/^set -euo/p' "${BASH_SOURCE[0]}" \
                         | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *)           echo "check-sdk-windows-include: unknown option: $1" >&2
                     exit 64 ;;
    esac
done

if [[ -z "$SDK_DIR" ]]; then
    for c in "$REPO/build/linux-release/games/spyro-1/sdk" \
             "$REPO/build/windows-mingw-shared/games/spyro-1/sdk" \
             "$REPO/build/windows-mingw/games/spyro-1/sdk"; do
        [[ -f "$c/openpete_game_structs.h" ]] && { SDK_DIR="$c"; break; }
    done
fi
if [[ -z "$SDK_DIR" || ! -f "$SDK_DIR/openpete_game_structs.h" ]]; then
    echo "check-sdk-windows-include: SKIP — no staged SDK (build a game target" \
         "or pass --sdk DIR)" >&2
    exit 64
fi

if [[ -n "$TOOLCHAIN" ]]; then
    CC=$(mingw_driver_in "$TOOLCHAIN") || {
        echo "check-sdk-windows-include: SKIP — $TOOLCHAIN/bin has no" \
             "x86_64-w64-mingw32-clang or -gcc" >&2
        exit 64
    }
else
    CC=$(mingw_find_cc "$(mingw_spyro_root "$REPO")") || {
        echo "check-sdk-windows-include: SKIP — no mingw cross compiler" >&2
        exit 64
    }
fi

SRC="$REPO/tests/unit/sdk_windows_include_test.c"
OUT="${TMPDIR:-/tmp}/sdk_windows_include_test.$$.exe"
rm -f "$OUT"
echo "check-sdk-windows-include: cc  $CC"
echo "check-sdk-windows-include: sdk $SDK_DIR"
if ! "$CC" -std=c11 -Wall -Wextra -Werror -I "$SDK_DIR" -o "$OUT" "$SRC"; then
    echo "check-sdk-windows-include: FAIL — the unit did not compile"
    exit 1
fi
[[ -f "$OUT" ]] || { echo "check-sdk-windows-include: FAIL — no output"; exit 1; }
rm -f "$OUT"
echo "check-sdk-windows-include: PASS"
