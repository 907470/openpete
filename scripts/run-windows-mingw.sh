#!/usr/bin/env bash
# scripts/run-windows-mingw.sh — rebuild the windows-mingw preset and
# launch the resulting .exe on the WSL2 Windows host.
#
# WSL2 launches .exe files natively on Windows via interop. Linux-form
# paths in args (e.g. `--disc /home/.../foo.cue`) are NOT understood by
# the Windows-side .exe; this script auto-translates the `--disc`,
# `--exe-path`, and `--screenshot-output` values via `wslpath -w` if they look like
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
#   --bk2 games/spyro-1/bk2/skip-to-artisans.bk2 --bk2-frame-offset 0
#       # canned input that walks past the title-screen prompts. Live
#       # keyboard takes over once the movie ends (frame 1950+).
#
# Convenience flags (handled by this script, not forwarded as-is):
#   --record NAME       expands to --bk2-record games/spyro-1/bk2/NAME.bk2.
#                       Combine with the default --bk2 prefix to capture
#                       a session that starts with skip-to-artisans and
#                       then records whatever you do with the keyboard /
#                       gamepad. The recorded movie includes the prefix
#                       frames so it replays standalone.
#   --idle              suppresses the default --bk2 prefix (no scripted
#                       input). Useful for screenshot runs that want the
#                       title screen idle.
#
# Each default is independent: pass an explicit value for any single
# flag and only that one is overridden. Examples:
#   # Smoke test, no scripted input:
#   scripts/run-windows-mingw.sh --idle --headless --frame-limit 60 \
#       --screenshot-on-frame 30 --screenshot-output /tmp/shot.bmp
#
#   # Boot Spyro with a different runtime budget; defaults fill the
#   # rest:
#   scripts/run-windows-mingw.sh --max-runtime-seconds 30
#
#   # Record a play session, prefixed by skip-to-artisans:
#   scripts/run-windows-mingw.sh --record portal-attempt-1
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

# Caller CWD — needed for resolving user-supplied relative paths (e.g.
# `--bk2 openpete/games/...`) before we `cd` into OPENPETE_ROOT below.
CALLER_CWD="$(pwd)"

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
# --exe-path / --screenshot-output / --bk2 won't resolve on the Windows side.
# Translate any value that starts with '/' (treated as a Linux absolute
# path). Relative paths fall back to abspath-then-translate so a value
# like `openpete/games/spyro-1/bk2/tas.bk2` (relative to CWD) still
# reaches the .exe as a Windows-form absolute path.
translate_if_linux() {
    local v="$1"
    if [[ "$v" == /* ]]; then
        wslpath -w "$v"
    elif [[ -e "$CALLER_CWD/$v" ]]; then
        # Relative path that exists from the user's invocation CWD —
        # resolve and translate so the .exe sees a Windows path.
        wslpath -w "$(realpath "$CALLER_CWD/$v")"
    else
        printf '%s' "$v"
    fi
}

forwarded=()
idle=0  # `--idle` suppresses the default --bk2 prefix
while (( $# )); do
    case "$1" in
        --disc|--exe-path|--screenshot-output|--bk2|--bk2-record)
            forwarded+=("$1")
            if [[ $# -ge 2 ]]; then
                forwarded+=("$(translate_if_linux "$2")")
                shift
            fi
            ;;
        --disc=*|--exe-path=*|--screenshot-output=*|--bk2=*|--bk2-record=*)
            key="${1%%=*}"
            val="${1#*=}"
            forwarded+=("$key=$(translate_if_linux "$val")")
            ;;
        --record)
            # Expand to --bk2-record games/spyro-1/bk2/<name>.bk2 (path is
            # auto-translated to a Windows form). Plain name only — pass an
            # explicit --bk2-record if you want a path elsewhere on disk.
            if [[ $# -ge 2 ]]; then
                forwarded+=(--bk2-record
                    "$(translate_if_linux "$OPENPETE_ROOT/games/spyro-1/bk2/$2.bk2")")
                shift
            else
                red "  --record requires a name argument"
                exit 2
            fi
            ;;
        --record=*)
            val="${1#*=}"
            forwarded+=(--bk2-record
                "$(translate_if_linux "$OPENPETE_ROOT/games/spyro-1/bk2/$val.bk2")")
            ;;
        --idle)
            idle=1
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

if (( idle )); then
    yellow "  (--idle: no scripted input)"
else
    if ! has_flag --bk2; then
        DEFAULT_BK2="$OPENPETE_ROOT/games/spyro-1/bk2/skip-to-artisans.bk2"
        if [[ -f "$DEFAULT_BK2" ]]; then
            yellow "  (no --bk2; defaulting to skip-to-artisans.bk2)"
            prepend --bk2 "$(wslpath -w "$DEFAULT_BK2")"
        else
            yellow "  (skip-to-artisans.bk2 missing; running with no scripted input)"
        fi
    fi
    # bk2 frame 0 → openpete_frame 0. The C-side auto-pick of
    # -first_input_frame misaligns against --fast-boot-until-frame, so pin to
    # 0 unless the caller overrode it.
    if has_flag --bk2 && ! has_flag --bk2-frame-offset; then
        yellow "  (no --bk2-frame-offset; defaulting to 0)"
        prepend --bk2-frame-offset 0
    fi
fi

# --- Run -----------------------------------------------------------------
green "[2/2] launching $EXE_REL on Windows host"
yellow "  args: ${forwarded[*]:-<none>}"
exec "$EXE_ABS" "${forwarded[@]}"
