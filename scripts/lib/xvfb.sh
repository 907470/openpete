#!/usr/bin/env bash
#
# xvfb.sh - the launch prefix of a scripted windowed run: a private Xvfb
# display, never the session's own. SOURCED by shell harnesses, EXECUTED by
# Python ones (scripts/lib/hostenv.py xvfb_prefix).
#
# A windowed run on the session's display opens a window on the user's
# desktop and takes focus; under WSLg that is the Windows desktop, and a
# compositor that stops answering stalls every run connected to it. The
# prefix runs the child under `xvfb-run -a`, which exports the Xvfb server's
# DISPLAY, with WAYLAND_DISPLAY removed and SDL_VIDEODRIVER=x11. Removing
# WAYLAND_DISPLAY alone does not keep SDL off the compositor: SDL's Wayland
# backend connects to $XDG_RUNTIME_DIR/wayland-0 when the variable is unset,
# and WSLg links that socket, so the driver is named as well.
#
# A missing xvfb-run is a SKIP (exit 64), never a fallback to the session's
# display.
#
# Escape: OPENPETE_HARNESS_REAL_DISPLAY=1 runs on the session's display (an
# empty prefix), for a person who wants to watch the run or needs the real
# GPU's present path; it SKIPs when the session has no display. Off by
# default.
#
# Shell usage:
#   source scripts/lib/xvfb.sh
#   op_xvfb_prefix TAG WxH        # sets OP_XVFB, or prints SKIP and exits 64
#   op_xvfb_set WxH || ...        # sets OP_XVFB, or returns 1 (no exit)
#   op_own_run env K=V "${OP_XVFB[@]}" timeout ... "$BIN" --window ...
#
# Executed:
#   scripts/lib/xvfb.sh --check TAG WxH     exit 0, or print SKIP and exit 64
#   scripts/lib/xvfb.sh TAG WxH -- CMD...   run CMD under the prefix
#
# The prefix line `[TAG] display: ...` goes to stderr, so a log names the
# display the run used.

# op_xvfb_set WxH: set the array OP_XVFB to the prefix for a WxH screen.
# Returns 1 (OP_XVFB empty, OP_XVFB_WHY set) when no private display can be
# made and the escape is off, or when the escape is on and the session has no
# display.
op_xvfb_set() {
    local screen="$1"
    OP_XVFB=()
    OP_XVFB_WHY=""
    if [[ "${OPENPETE_HARNESS_REAL_DISPLAY:-0}" == 1 ]]; then
        if [[ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
            OP_XVFB_WHY="OPENPETE_HARNESS_REAL_DISPLAY=1 and the session has no display"
            return 1
        fi
        OP_XVFB_DESC="the session's own (OPENPETE_HARNESS_REAL_DISPLAY=1)"
        return 0
    fi
    if ! command -v xvfb-run >/dev/null 2>&1; then
        OP_XVFB_WHY="xvfb-run not installed; a scripted windowed run never uses the session's display"
        return 1
    fi
    OP_XVFB=(env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11
             xvfb-run -a -s "-screen 0 ${screen}x24")
    OP_XVFB_DESC="private Xvfb, screen ${screen}"
    return 0
}

# op_xvfb_prefix TAG WxH: op_xvfb_set, printing `TAG: SKIP (...)` and
# exiting 64 on failure, and the display line on success.
op_xvfb_prefix() {
    local tag="$1" screen="$2"
    if ! op_xvfb_set "$screen"; then
        echo "$tag: SKIP ($OP_XVFB_WHY)"
        exit 64
    fi
    echo "[$tag] display: $OP_XVFB_DESC" >&2
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    if [[ "${1:-}" == --check ]]; then
        [[ $# -eq 3 ]] || { echo "usage: $0 --check TAG WxH" >&2; exit 2; }
        op_xvfb_prefix "$2" "$3" 2>/dev/null
        exit 0
    fi
    [[ $# -ge 4 && "$3" == -- ]] || { echo "usage: $0 TAG WxH -- CMD..." >&2; exit 2; }
    op_xvfb_prefix "$1" "$2"
    shift 3
    exec "${OP_XVFB[@]}" "$@"
fi
