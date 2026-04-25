#!/usr/bin/env bash
# scripts/run-windows-mingw.sh — rebuild the windows-mingw preset and
# launch the resulting .exe on the WSL2 Windows host.
#
# WSL2 launches .exe files natively on Windows via interop. Linux-form
# paths in args (e.g. `--disc /home/.../foo.cue`) are NOT understood by
# the Windows-side .exe; this script auto-translates the `--disc`,
# `--exe-path`, and `--output` values via `wslpath -w` if they look like
# Linux paths.
#
# Usage:
#   scripts/run-windows-mingw.sh [args passed to openpete-spyro1.exe...]
#
# Examples:
#   # Sanity smoke-run, headless, screenshot at frame 30
#   scripts/run-windows-mingw.sh --headless --frame-limit 60 \
#       --screenshot-on-frame 30 --output /tmp/shot.png
#
#   # Boot the matching-decomp's main on the disc (windowed)
#   scripts/run-windows-mingw.sh --run-recomp 0x80012204 \
#       --fast-boot-until-frame 850 --max-runtime-seconds 60
#
# Disc resolution (when no `--disc` is in the args):
#   1. $OPENPETE_DISC (whatever location it points at, .cue or .bin)
#   2. <repo>/games/spyro-1/spyro-1/build/SCUS-94228.cue (matching-decomp)
#   3. (skipped — no implicit disc, .exe will run without)
#
# Returns the exit code of the .exe (or the build step on build failure).

set -euo pipefail

OPENPETE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE_REL="build/windows-mingw/games/spyro-1/openpete-spyro1.exe"
EXE_ABS="$OPENPETE_ROOT/$EXE_REL"

red()    { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green()  { printf '\033[32m%s\033[0m\n' "$*" >&2; }
yellow() { printf '\033[33m%s\033[0m\n' "$*" >&2; }

# --- Sanity --------------------------------------------------------------
if ! command -v wslpath >/dev/null 2>&1; then
    red "wslpath not found — this script is meant for WSL2 hosts."
    exit 2
fi
if ! command -v cmake >/dev/null 2>&1; then
    red "cmake not found — install cmake (>= 3.22)."
    exit 2
fi

# --- Build ---------------------------------------------------------------
yellow "[1/2] cmake --build --preset windows-mingw"
cd "$OPENPETE_ROOT"
if ! cmake --build --preset windows-mingw; then
    red "build failed"
    exit 1
fi

if [[ ! -x "$EXE_ABS" ]]; then
    red "expected exe missing: $EXE_REL"
    exit 1
fi

# --- Args translation ----------------------------------------------------
# WSL2 interop hands argv through verbatim; Linux paths in --disc /
# --exe-path / --output won't resolve on the Windows side. Translate any
# value that starts with '/' (treated as a Linux absolute path).
translate_if_linux() {
    local v="$1"
    if [[ "$v" == /* ]]; then
        wslpath -w "$v"
    else
        printf '%s' "$v"
    fi
}

forwarded=()
saw_disc=0
i=1
while (( $# )); do
    case "$1" in
        --disc|--exe-path|--output)
            saw_disc=1  # only --disc actually means "saw disc", but cheap
            forwarded+=("$1")
            if [[ $# -ge 2 ]]; then
                forwarded+=("$(translate_if_linux "$2")")
                shift
            fi
            ;;
        --disc=*|--exe-path=*|--output=*)
            key="${1%%=*}"
            val="${1#*=}"
            forwarded+=("$key=$(translate_if_linux "$val")")
            ;;
        *)
            forwarded+=("$1")
            ;;
    esac
    shift
done

# Reset saw_disc to actually-saw-disc (only --disc counts).
saw_disc=0
for a in "${forwarded[@]}"; do
    case "$a" in --disc|--disc=*) saw_disc=1; break ;; esac
done

# --- Default disc when none was passed -----------------------------------
if [[ $saw_disc -eq 0 ]]; then
    DEFAULT_DISC=""
    if [[ -n "${OPENPETE_DISC:-}" && -f "$OPENPETE_DISC" ]]; then
        DEFAULT_DISC="$OPENPETE_DISC"
    elif [[ -f "$OPENPETE_ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue" ]]; then
        DEFAULT_DISC="$OPENPETE_ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue"
    fi
    if [[ -n "$DEFAULT_DISC" ]]; then
        DISC_W="$(wslpath -w "$DEFAULT_DISC")"
        yellow "  (no --disc passed; defaulting to $DEFAULT_DISC)"
        forwarded=(--disc "$DISC_W" "${forwarded[@]}")
    fi
fi

# --- Run -----------------------------------------------------------------
green "[2/2] launching $EXE_REL on Windows host"
yellow "  args: ${forwarded[*]:-<none>}"
exec "$EXE_ABS" "${forwarded[@]}"
