#!/usr/bin/env bash
# check_sigterm_exit.sh - SIGTERM ends a headless run in every phase it can
# arrive in, and the escapes bound a shutdown that stops making progress.
#
# Run: one headless boot per leg, signalled by this script.
#   a  OPENPETE_FAST_BOOT_HOLD=1 keeps the run in the fast-boot VBL path
#      (the host loop never runs there); SIGTERM once the hold logs.
#   b  no hold; SIGTERM 1 s after `fast-boot ended`.
#   c  OPENPETE_TEARDOWN_STALL_MS=60000; SIGTERM, then a second SIGTERM once
#      the stall logs.
#   d  OPENPETE_TEARDOWN_STALL_MS=60000; one SIGTERM, nothing else: the
#      stalled teardown is a state nothing observes but the quit backstop.
#   e  OPENPETE_FAST_BOOT_HOLD=1 and --max-runtime-seconds 6; no signal.
#
# Instrument: the run's exit status, the wall time from the first signal
# (leg e: from launch) to exit, and the log lines below.
#
# Pass criterion, per leg, with its engagement line (a leg whose engagement
# line is missing fails: it did not exercise the path):
#   a  engaged `OPENPETE_FAST_BOOT_HOLD: fast boot held`, no `fast-boot
#      ended`; `openpete-exit: reason=user-quit`, rc 0, within 5 s.
#   b  engaged `fast-boot ended`; reason=user-quit, rc 0, within 5 s.
#   c  engaged `OPENPETE_TEARDOWN_STALL_MS: stalling teardown`; rc 130
#      within 5 s of the second signal.
#   d  engaged the stall line; `quit-backstop:` logged, rc 1 (panic),
#      between 29 and 35 s after the signal.
#   e  engaged the hold line; reason=max-runtime-seconds, rc 0, within 11 s
#      of launch.
#
# Verdict line per leg:
#   [sigterm-exit] leg=<a..e> rc=<n> secs=<t> verdict=PASS|FAIL: <why>
#
# Runtime: about 55 s (leg d waits out the 30 s backstop).
#
# Usage: tests/harness/check_sigterm_exit.sh [--build-dir=DIR] [--game=G]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME=spyro-1
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --game=*) GAME="${a#--game=}";;
    esac
done
pg_resolve "$GAME"

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "sigterm-exit: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/sigterm-exit/$GAME"
mkdir -p "$OUT"
rm -f "${OUT:?}"/leg-a.log "${OUT:?}"/leg-b.log "${OUT:?}"/leg-c.log \
    "${OUT:?}"/leg-d.log "${OUT:?}"/leg-e.log

HOLD_LINE='OPENPETE_FAST_BOOT_HOLD: fast boot held'
STALL_LINE='OPENPETE_TEARDOWN_STALL_MS: stalling teardown'
fail=0
RUN_PID=""

# now: wall seconds with fractions.
now() { date +%s.%N; }
# since T0: seconds elapsed since T0, two decimals.
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.2f", b - a }'; }

# launch LOG ENV... -- ARGS...: start one headless run in the background;
# RUN_PID is its PID (env execs the binary).
launch() {
    local log="$1"; shift
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    env OPENPETE_VIBRATION=0 "${envs[@]}" "$PG_BIN" --headless --no-config \
        --disc "$PG_DISC" --disc-cache 0 "${PG_RUN_ARGS[@]}" --renderer vk \
        --mute 1 --no-card --vibration 0 --frame-limit 1000000 "$@" \
        >"$log" 2>&1 </dev/null &
    RUN_PID=$!
}

# wait_line LOG PATTERN SECS: status 0 once LOG holds PATTERN, 1 when the
# run exits first or SECS pass.
wait_line() {
    local log="$1" pat="$2" t=0 limit=$(( $3 * 10 ))
    while (( t < limit )); do
        grep -qF -- "$pat" "$log" 2>/dev/null && return 0
        _op_own_alive "$RUN_PID" || { grep -qF -- "$pat" "$log"; return; }
        sleep 0.1
        t=$(( t + 1 ))
    done
    return 1
}

# wait_exit SECS: reap RUN_PID; RC is its status, or -1 (and the run is
# killed) when it outlives SECS.
wait_exit() {
    local t=0 limit=$(( $1 * 10 ))
    while (( t < limit )) && _op_own_alive "$RUN_PID"; do
        sleep 0.1
        t=$(( t + 1 ))
    done
    if _op_own_alive "$RUN_PID"; then
        kill -KILL "$RUN_PID" 2>/dev/null
        wait "$RUN_PID" 2>/dev/null
        RC=-1
        return
    fi
    wait "$RUN_PID"
    RC=$?
}

# verdict LEG SECS WHY: print the leg's line; an empty WHY is a PASS.
verdict() {
    if [[ -z "$3" ]]; then
        echo "[sigterm-exit] leg=$1 rc=$RC secs=$2 verdict=PASS"
    else
        echo "[sigterm-exit] leg=$1 rc=$RC secs=$2 verdict=FAIL: $3"
        fail=1
    fi
}

# --- a: SIGTERM while fast boot holds -----------------------------------
log="$OUT/leg-a.log"; RC=""
launch "$log" OPENPETE_FAST_BOOT_HOLD=1 --
if wait_line "$log" "$HOLD_LINE" 60; then
    sleep 1
    t0="$(now)"; kill -TERM "$RUN_PID"; wait_exit 5; s="$(since "$t0")"
    why=""
    grep -qF 'fast-boot ended' "$log" && why="fast boot ended despite the hold"
    [[ -z "$why" && "$RC" != 0 ]] && why="rc $RC, want 0 within 5 s"
    [[ -z "$why" ]] && ! grep -q 'openpete-exit: reason=user-quit' "$log" && why="no user-quit exit line"
    verdict a "$s" "$why"
else
    wait_exit 1; verdict a - "no hold engagement line (see $log)"
fi

# --- b: SIGTERM after fast boot -----------------------------------------
log="$OUT/leg-b.log"; RC=""
launch "$log" --
if wait_line "$log" 'fast-boot ended' 60; then
    sleep 1
    t0="$(now)"; kill -TERM "$RUN_PID"; wait_exit 5; s="$(since "$t0")"
    why=""
    [[ "$RC" != 0 ]] && why="rc $RC, want 0 within 5 s"
    [[ -z "$why" ]] && ! grep -q 'openpete-exit: reason=user-quit' "$log" && why="no user-quit exit line"
    verdict b "$s" "$why"
else
    wait_exit 1; verdict b - "no fast-boot ended line (see $log)"
fi

# --- c: a second signal escapes a stalled teardown ----------------------
log="$OUT/leg-c.log"; RC=""
launch "$log" OPENPETE_TEARDOWN_STALL_MS=60000 --
if wait_line "$log" 'fast-boot ended' 60; then
    kill -TERM "$RUN_PID"
    if wait_line "$log" "$STALL_LINE" 10; then
        t0="$(now)"; kill -TERM "$RUN_PID"; wait_exit 5; s="$(since "$t0")"
        why=""
        [[ "$RC" != 130 ]] && why="rc $RC, want 130 within 5 s"
        verdict c "$s" "$why"
    else
        wait_exit 1; verdict c - "no teardown stall line (see $log)"
    fi
else
    wait_exit 1; verdict c - "no fast-boot ended line (see $log)"
fi

# --- d: the backstop ends a stalled teardown nothing else observes ------
log="$OUT/leg-d.log"; RC=""
launch "$log" OPENPETE_TEARDOWN_STALL_MS=60000 --
if wait_line "$log" 'fast-boot ended' 60; then
    t0="$(now)"; kill -TERM "$RUN_PID"; wait_exit 35; s="$(since "$t0")"
    why=""
    grep -qF "$STALL_LINE" "$log" || why="no teardown stall line"
    [[ -z "$why" && "$RC" != 1 ]] && why="rc $RC, want 1 within 35 s"
    [[ -z "$why" ]] && ! grep -q 'quit-backstop:' "$log" && why="no quit-backstop line"
    [[ -z "$why" ]] && awk -v s="$s" 'BEGIN { exit !(s < 29) }' && why="exited at $s s, before the 30 s bound"
    verdict d "$s" "$why"
else
    wait_exit 1; verdict d - "no fast-boot ended line (see $log)"
fi

# --- e: --max-runtime-seconds while fast boot holds ---------------------
log="$OUT/leg-e.log"; RC=""
t0="$(now)"
launch "$log" OPENPETE_FAST_BOOT_HOLD=1 -- --max-runtime-seconds 6
wait_exit 11; s="$(since "$t0")"
why=""
grep -qF "$HOLD_LINE" "$log" || why="no hold engagement line"
[[ -z "$why" ]] && grep -qF 'fast-boot ended' "$log" && why="fast boot ended despite the hold"
[[ -z "$why" && "$RC" != 0 ]] && why="rc $RC, want 0 within 11 s"
[[ -z "$why" ]] && ! grep -q 'openpete-exit: reason=max-runtime-seconds' "$log" && why="no max-runtime-seconds exit line"
verdict e "$s" "$why"

for l in a b c d e; do
    [[ -f "$OUT/leg-$l.log" ]] || { echo "sigterm-exit: leg $l wrote no log"; fail=1; }
done
(( fail )) && { echo "sigterm-exit: FAIL"; exit 1; }
echo "sigterm-exit: PASS"
exit 0
