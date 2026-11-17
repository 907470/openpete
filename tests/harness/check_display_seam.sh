#!/usr/bin/env bash
# check_display_seam.sh - every device call under eager production comes
# from the thread that owns the display.
#
# Class: the display has one owner (rhi_display_claim states each transfer).
# A borrow (openpete_present_thread_borrow, then _return) takes the display
# from the present thread for a blocking device read on the sim thread and
# hands it back. A handback the present thread never observes as an
# inactive spell (the pair lands while it waits with the mutex released)
# must still make it claim the display before its next device call; a
# device call before that claim is a seam violation, which the RHI counts
# and native_render reports at exit.
#
# Windowed only: eager production needs a window, so neither the
# presentation golden nor the oracle (both headless) runs the present
# thread.
#
# Row: the Spyro 2 attract demo, --frame-limit 1500, with
# OPENPETE_IDENT_AUDIT=300, whose audit capture borrows the display once per
# canonical present (several hundred borrows over the run).
#
# Instrument: the run's own log.
#   eager engaged     the line "eager: engaged"
#   drains            "<n> drain(s)" in the present-thread report (one per
#                     set_active(0) that took the display back)
#   seam              "display-seam fence: <n> call(s)" (0 when absent) and
#                     the count of "SEAM:" lines
#
# Pass: seam 0 with eager engaged and drains >= MIN_DRAINS. VACUOUS when
# eager never engaged or drains < MIN_DRAINS (too few handbacks to reach the
# race); FAIL otherwise.
#
# Every launch passes --input-lock --vibration 0 --background 1 --no-card
# and runs on a private Xvfb display (scripts/lib/xvfb.sh).
#
# Verdict line:
#   [display-seam] game=spyro-2 eager=<0|1> drains=<n> seam=<n>
#                  verdict=PASS|FAIL|VACUOUS log=<path>
#
# --analyze=LOG skips the run and grades an existing log of the same row.
#
# Usage: tests/harness/check_display_seam.sh [--build-dir=DIR] [--out=DIR] [--analyze=LOG]
# Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (no xvfb-run, no
# binary or no disc).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
ANALYZE=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
        --analyze=*)   ANALYZE="${a#--analyze=}";;
    esac
done

GAME=spyro-2
pg_resolve "$GAME"
FRAME_LIMIT=1500
MIN_DRAINS=400
ROW_RUNTIME=600
ROW_TIMEOUT=660

if [[ -n "$ANALYZE" ]]; then
    [[ -s "$ANALYZE" ]] || { echo "display-seam: FAIL (no log at $ANALYZE)"; exit 1; }
    LOG="$ANALYZE"
else
    [[ -x "$PG_BIN" ]] || { echo "display-seam: SKIP (no binary at $PG_BIN)"; exit 64; }
    [[ -f "$PG_DISC" ]] || { echo "display-seam: SKIP (no disc at $PG_DISC)"; exit 64; }
    op_xvfb_prefix display-seam 1280x960
    [[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/display-seam"
    mkdir -p "$OUT"
    LOG="$OUT/$GAME.log"
    rm -f "$LOG" "$OUT"/a__*.png
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_IDENT_AUDIT=300 \
        "${OP_XVFB[@]}" timeout -k 10 "$ROW_TIMEOUT" "$PG_BIN" \
        --game "$GAME" --no-config --disc "$PG_DISC" --no-card --mute 1 \
        --vibration 0 --input-lock --background 1 --renderer vk \
        --frame-limit "$FRAME_LIMIT" --max-runtime-seconds "$ROW_RUNTIME" \
        --screenshot-output "$OUT/a__{n}.png" \
        </dev/null >"$LOG" 2>&1
    rc=$?
    [[ -s "$LOG" ]] || { echo "display-seam: FAIL (no log, rc=$rc)"; exit 1; }
fi

eager=0
grep -q "eager: engaged" "$LOG" && eager=1
drains=$(sed -n 's/.*present-thread: .* \([0-9][0-9]*\) drain(s).*/\1/p' "$LOG" | tail -n 1)
drains=${drains:-0}
fence=$(sed -n 's/.*display-seam fence: \([0-9][0-9]*\) call(s).*/\1/p' "$LOG" | tail -n 1)
lines=$(grep -c "SEAM: " "$LOG")
seam=${fence:-$lines}
(( lines > seam )) && seam=$lines

verdict=PASS; code=0
if (( seam > 0 )); then
    verdict=FAIL; code=1
elif (( eager == 0 || drains < MIN_DRAINS )); then
    verdict=VACUOUS; code=3
fi
echo "[display-seam] game=$GAME eager=$eager drains=$drains min_drains=$MIN_DRAINS seam=$seam verdict=$verdict log=$LOG"
exit "$code"
