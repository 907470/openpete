#!/usr/bin/env bash
#
# timing_gate.sh - wait out a timing window before a launch or a build.
# SOURCED, not executed.
#
# op_timing_gate [LABEL] returns 0 when no timing window is open, or when
# this process runs inside the open one (its OPENPETE_TIMING_TOKEN is the
# holder's); otherwise it blocks, printing who holds the window once a
# minute on stderr, until the window closes. It never returns 0 while
# another job's window is open: with OPENPETE_TIMING_GATE_TIMEOUT=<seconds>
# set it returns 75 at the limit instead, and the caller exits non-zero.
#
# The window is scripts/bench/timing_lock.py's flock on
# $OPENPETE_BENCH_DIR/timing.lock (default ~/.local/share/openpete/bench),
# plus a hand-written TIMING-WINDOW marker there. The free case costs one
# flock(1) probe and a file test; Python runs only when the window is open.

_op_timing_dir() {
    echo "${OPENPETE_BENCH_DIR:-$HOME/.local/share/openpete/bench}"
}

op_timing_gate() {
    local label="${1:-${0##*/}}" d
    d="$(_op_timing_dir)"
    if [[ ! -e "$d/TIMING-WINDOW" ]] && { [[ ! -e "$d/timing.lock" ]] ||
            flock -n -s "$d/timing.lock" true 2>/dev/null; }; then
        return 0
    fi
    local args=(wait --label "$label")
    [[ -n "${OPENPETE_TIMING_GATE_TIMEOUT:-}" ]] && args+=(--timeout "$OPENPETE_TIMING_GATE_TIMEOUT")
    python3 "$(dirname "${BASH_SOURCE[0]}")/../bench/timing_lock.py" "${args[@]}"
}
