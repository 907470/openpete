#!/usr/bin/env bash
#
# game.sh - per-game binary/disc resolution and the headless capture helper
# shared by the check_* harnesses. SOURCED, not executed.
#
# The resolver is scripts/presentation-game.sh; this file sources it rather
# than carrying a second copy, so `--game SLUG` means the same thing in every
# harness and the disc path and per-game boot flags keep their one authority,
# games/<slug>/oracle.conf.
#
# Usage:
#   source scripts/lib/game.sh
#   GAME="$(pg_parse_game "$@")" || exit 64
#   pg_resolve "$GAME"
#   pg_capture "$log" "$shot" -- --renderer vk --screenshot-on-frame 100 ...
#
# pg_resolve exports PG_GAME, PG_BIN, PG_DISC and the PG_RUN_ARGS array (plus
# the golden/corpus paths the presentation scripts use); PG_BUILD_DIR, set
# before the call, selects the build directory.
#
# Vibration: every run pg_capture starts is silenced. Vibration ships on by
# default, which is right for a player and wrong for a machine-driven run -
# nobody is holding the pad, and the buzzing appears in no log, so it would
# never surface on its own. Setting it here rather than at each call site
# means a harness cannot miss it by appending the flag to the wrong one of
# the several argument lists it builds.
# (scripts/fences/rumble_guard.py enforces this.)

# shellcheck source=scripts/presentation-game.sh
source "$(dirname "${BASH_SOURCE[0]}")/../presentation-game.sh"

# pg_capture LOG OUTPUTS [--] <run args...>
#
# One headless run of PG_BIN. LOG collects stdout and stderr; OUTPUTS is a
# comma-separated list of files the run must produce (empty string: none).
#
# LOG and every OUTPUT are deleted before the run and asserted after it, so a
# stale file from an earlier run can never be read as this run's result
#.
#
# The invocation is hermetic: --no-config ignores every config file, so a
# developer's openpete.toml cannot move a verdict; --disc-cache 0 keeps the
# disc timing the goldens were captured under; the game's own boot flags come
# from PG_RUN_ARGS, ahead of the caller's arguments so the caller can override
# them.
#
# Optional, set by the caller before the call:
#   PG_CAPTURE_ENV      array of KEY=VALUE assignments for the run
#   PG_CAPTURE_TIMEOUT  wall-clock seconds; unset or 0 runs untimed
#   PG_CAPTURE_STDOUT   file for stdout; unset sends it to LOG with stderr
#
# Returns the run's exit status, or 1 when a named output is missing.
pg_capture() {
    local log="$1" outs="$2"; shift 2
    [[ "${1:-}" == "--" ]] && shift

    local -a out_arr=()
    [[ -n "$outs" ]] && IFS=',' read -ra out_arr <<<"$outs"
    rm -f "$log"
    [[ ${#out_arr[@]} -gt 0 ]] && rm -f "${out_arr[@]}"

    local -a pre=(env OPENPETE_VIBRATION=0)
    [[ -n "${PG_CAPTURE_ENV+set}" ]] && pre+=("${PG_CAPTURE_ENV[@]}")
    [[ -n "${PG_CAPTURE_TIMEOUT:-}" && "${PG_CAPTURE_TIMEOUT:-0}" != 0 ]] &&
        pre+=(timeout "$PG_CAPTURE_TIMEOUT")

    local rc=0
    if [[ -n "${PG_CAPTURE_STDOUT:-}" ]]; then
        "${pre[@]}" "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
            --disc-cache 0 "${PG_RUN_ARGS[@]}" "$@" \
            >"$PG_CAPTURE_STDOUT" 2>"$log"
        rc=$?
    else
        "${pre[@]}" "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
            --disc-cache 0 "${PG_RUN_ARGS[@]}" "$@" >"$log" 2>&1
        rc=$?
    fi

    local o
    for o in "${out_arr[@]}"; do
        [[ -z "$o" || -f "$o" ]] && continue
        echo "pg_capture: run produced no $o (see $log)" >&2
        return 1
    done
    return $rc
}
