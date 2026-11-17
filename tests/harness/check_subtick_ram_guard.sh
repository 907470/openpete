#!/usr/bin/env bash

# tests/harness/check_subtick_ram_guard.sh - the sub-tick RAM-write row: the
# windowed oracle (check_psx_oracle.sh with OPENPETE_ORACLE_WINDOWED=1) over
# the full golden, with OPENPETE_SUBTICK_RAM_GUARD=1 mapping guest RAM
# read-only for every present the sub-tick view brackets.
#
# The guard logs `GUEST RAM WRITTEN` for a host-pointer store into guest RAM
# during a present and then lets the store through, so a store that writes
# back the canonical value (a write-and-restore) leaves every hashed region
# intact and the oracle passes. The oracle verdict alone therefore cannot
# fail such a write; this row reads the guard's lines as well.
#
# PASS requires all of:
#   - check_psx_oracle.sh exits 0 (windowed oracle PASS over every record);
#   - a nonzero `sub-tick: N re-extract(s) presented` count;
#   - a `RAM guard: armed_presents=N hits=M` exit row with N > 0;
#   - zero `GUEST RAM WRITTEN` lines and M == 0.
#
# The run is the native renderer under lavapipe in a window on a private
# Xvfb display (scripts/lib/xvfb.sh, applied by check_psx_oracle.sh), with
# --render-fps 60 above the 29.913 Hz tick rate so non-drawing VBLs plan
# sub-tick presents. It paces to the display:
# about 12 minutes for the 20k-record Spyro 1 golden.
#
# Usage:
#   tests/harness/check_subtick_ram_guard.sh [--game SLUG] [-- <extra openpete flags>]
#
# Negative control: OPENPETE_SUBTICK_FAULT_STORE=1 makes every bracketed
# present rewrite one guest word through a host pointer (the value already
# there, so no hashed byte changes); the row must FAIL on the guard lines
# while the oracle itself passes.
#
# Env knobs:
#   OPENPETE_BIN, OPENPETE_DISC, OPENPETE_FRAME_LIMIT  forwarded to the oracle
#   OPENPETE_RAMGUARD_LOG=PATH  keep the run's log at PATH (else deleted)
#
# Exit codes:
#   0   PASS
#   2   a guard line, a guard hit, or the oracle's own divergence/failure
#   3   VACUOUS: no re-extract presented, or the guard never armed
#   64  prerequisites missing (xvfb-run, golden, binary, disc, lvp ICD)
#   *   the oracle's own exit code otherwise
set -euo pipefail

export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

GAME="spyro-1"
PASSTHRU=()
_sep=0
_game_next=0
for _a in "$@"; do
    if   (( _game_next ));      then GAME="$_a"; _game_next=0
    elif (( _sep ));            then PASSTHRU+=("$_a")
    elif [[ "$_a" == "--" ]];   then _sep=1
    elif [[ "$_a" == "--game" ]]; then _game_next=1
    else echo "[ram-guard] unknown argument: $_a" >&2; exit 64
    fi
done

op_xvfb_set 1280x960 || { echo "[ram-guard] SKIP: $OP_XVFB_WHY" >&2; exit 64; }

if [[ -n "${OPENPETE_RAMGUARD_LOG:-}" ]]; then
    LOG="$OPENPETE_RAMGUARD_LOG"
else
    LOG="$(mktemp "${TMPDIR:-/tmp}/subtick-ram-guard.XXXXXX.log")"
    trap 'rm -f "${LOG:?}"' EXIT
fi
rm -f "$LOG"

# The oracle's windowed row puts its window on the private display.
export OPENPETE_ORACLE_WINDOWED=1
export OPENPETE_SUBTICK_RAM_GUARD=1
export OPENPETE_ORACLE_LOG="$LOG"
if [[ -n "${OPENPETE_SUBTICK_FAULT_STORE:-}" ]]; then
    echo "[ram-guard] OPENPETE_SUBTICK_FAULT_STORE=$OPENPETE_SUBTICK_FAULT_STORE: negative control, expect FAIL"
fi

# --input-lock: the window ignores host input (it sits on a private display,
# but the run must not depend on that). --render-fps 60 first so a caller's
# flag overrides it (the options parser takes the last occurrence).
set +e
tests/harness/check_psx_oracle.sh --game "$GAME" -- \
    --input-lock --render-fps 60 "${PASSTHRU[@]}"
rc=$?
set -e

if [[ ! -s "$LOG" ]]; then
    echo "[ram-guard] the oracle wrote no log at $LOG (rc=$rc)" >&2
    (( rc != 0 )) && exit "$rc"
    exit 2
fi

REX=$(sed -n 's/.*sub-tick: \([0-9]*\) re-extract(s) presented.*/\1/p' "$LOG" | tail -1)
GUARD_ROW=$(grep -o 'RAM guard: armed_presents=.*' "$LOG" | tail -1 || true)
ARMED=$(sed -n 's/.*armed_presents=\([0-9]*\).*/\1/p' <<<"$GUARD_ROW")
HITS=$(sed -n 's/.* hits=\([0-9]*\).*/\1/p' <<<"$GUARD_ROW")
LINES=$(grep -c 'GUEST RAM WRITTEN' "$LOG" || true)

echo "[ram-guard] oracle exit=$rc re_extracts=${REX:-0} armed_presents=${ARMED:-none} hits=${HITS:-none} guard_lines=$LINES"
if (( LINES > 0 )); then
    echo "[ram-guard] first guard lines:" >&2
    grep -m 5 'GUEST RAM WRITTEN' "$LOG" | sed 's/^/    /' >&2
fi

verdict=0
if (( rc != 0 )); then
    verdict=$rc
fi
if (( LINES > 0 )) || [[ -n "$HITS" && "$HITS" != 0 ]]; then
    verdict=2
elif (( verdict == 0 )) && [[ "${REX:-0}" == 0 || -z "$GUARD_ROW" || "${ARMED:-0}" == 0 ]]; then
    verdict=3
fi
case "$verdict" in
    0) echo "[ram-guard] verdict: PASS" ;;
    3) echo "[ram-guard] verdict: VACUOUS (re-extracts=${REX:-0}, guard row: ${GUARD_ROW:-missing})" >&2 ;;
    *) echo "[ram-guard] verdict: FAIL (exit $verdict)" >&2 ;;
esac
exit "$verdict"
