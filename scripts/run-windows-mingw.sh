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
# With no args, the script defaults to "boot Spyro to gameplay":
#   --disc <repo>/games/spyro-1/spyro-1/build/SCUS-94228.cue (or
#       $OPENPETE_DISC if set)
#   --run-recomp 0x80012204                 # Spyro main()
#   --fast-boot-until-frame 850             # skip boot intro yields
#   --press 861:return,931:return,1037:return,1073:return,1919:return
#       # walk past the title-screen prompts
#
# Each default is independent: pass an explicit value for any single
# flag and only that one is overridden. Examples:
#   # Just sanity smoke (overrides every default since --headless +
#   # --frame-limit dominate, and we pass --disc/--run-recomp/--press
#   # explicitly to neutralize them):
#   scripts/run-windows-mingw.sh --headless --frame-limit 60 \
#       --screenshot-on-frame 30 --output /tmp/shot.png
#
#   # Boot Spyro with a different runtime budget; defaults fill the
#   # rest:
#   scripts/run-windows-mingw.sh --max-runtime-seconds 30
#
# Disc resolution (when no `--disc` is in the args, first match wins):
#   1. $OPENPETE_DISC (whatever location it points at, .cue or .bin)
#   2. <repo>/games/spyro-1/spyro-1/build/SCUS-94228.cue
#      (matching-decomp built inside the vendored submodule)
#   3. <repo>/../spyro-1/build/SCUS-94228.cue
#      (matching-decomp checked out as a sibling of openpete)
#   4. <repo>/../roms/SCUS-94228/SCUS-94228.cue (per README §Disc)
#   5. (skipped — no implicit disc, .exe will run without)
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

# --- Default-flag injection ----------------------------------------------
# If any of these flags is missing from the argv, prepend the matching
# default. They make zero-arg invocation "boot to gameplay": dispatch
# main(), skip the boot-intro yields, and replay the keypress sequence
# that walks past the title-screen prompts.
has_flag() {
    local needle="$1"
    for a in "${forwarded[@]}"; do
        case "$a" in "$needle"|"$needle="*) return 0 ;; esac
    done
    return 1
}

prepend() { forwarded=("$@" "${forwarded[@]}"); }

if ! has_flag --disc; then
    DEFAULT_DISC=""
    for candidate in \
        "${OPENPETE_DISC:-}" \
        "$OPENPETE_ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue" \
        "$OPENPETE_ROOT/../spyro-1/build/SCUS-94228.cue" \
        "$OPENPETE_ROOT/../roms/SCUS-94228/SCUS-94228.cue" \
    ; do
        if [[ -n "$candidate" && -f "$candidate" ]]; then
            DEFAULT_DISC="$candidate"
            break
        fi
    done
    if [[ -n "$DEFAULT_DISC" ]]; then
        yellow "  (no --disc passed; defaulting to $DEFAULT_DISC)"
        prepend --disc "$(wslpath -w "$DEFAULT_DISC")"
    fi
fi

if ! has_flag --run-recomp; then
    yellow "  (no --run-recomp; defaulting to 0x80012204 — Spyro main)"
    prepend --run-recomp 0x80012204
fi

if ! has_flag --fast-boot-until-frame; then
    yellow "  (no --fast-boot-until-frame; defaulting to 850 — past boot intro)"
    prepend --fast-boot-until-frame 850
fi

if ! has_flag --press; then
    DEFAULT_PRESS="861:return,931:return,1037:return,1073:return,1919:return"
    yellow "  (no --press; defaulting to title-screen walkthrough)"
    prepend --press "$DEFAULT_PRESS"
fi

# --- Run -----------------------------------------------------------------
green "[2/2] launching $EXE_REL on Windows host"
yellow "  args: ${forwarded[*]:-<none>}"
exec "$EXE_ABS" "${forwarded[@]}"
