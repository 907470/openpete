#!/usr/bin/env bash
#
# timing_gate.sh - wait out a timing window, then hold it shut, before a
# launch or a build. SOURCED, not executed.
#
# op_timing_gate [LABEL] returns 0 when no timing window is open, or when
# this process runs inside the open one (its OPENPETE_TIMING_TOKEN is the
# holder's); otherwise it blocks, printing who holds the window once a
# minute on stderr, until the window closes. It never returns 0 while
# another job's window is open: with OPENPETE_TIMING_GATE_TIMEOUT=<seconds>
# set it returns 75 at the limit instead, and the caller exits non-zero.
#
# On return 0 outside a window the shell holds the window shut: a holder
# process (scripts/lib/flock_hold.py) keeps LOCK_SH on the lock until this
# shell ($$) exits, so a timing job cannot take the window between the gate
# and the launch after it. A later call in the same shell (or a subshell of
# it) keeps that hold, unless a timing job is waiting for the window: then
# the hold is released, the call waits until the window closes, and holds
# again. A container launched under the hold receives
# OPENPETE_TIMING_COVERED=1 (scripts/lib/container.sh).
#
# The window is scripts/bench/timing_lock.py's flock on
# $OPENPETE_BENCH_DIR/timing.lock (default ~/.local/share/openpete/bench),
# its want lock timing.want, plus a hand-written TIMING-WINDOW marker there.

op_timing_gate() {
    local label="${1:-${0##*/}}"
    local args=(gate --for-pid "$$" --label "$label")
    [[ -n "${OPENPETE_TIMING_GATE_TIMEOUT:-}" ]] && args+=(--timeout "$OPENPETE_TIMING_GATE_TIMEOUT")
    python3 "$(dirname "${BASH_SOURCE[0]}")/../bench/timing_lock.py" "${args[@]}"
}
