#!/usr/bin/env bash

# tests/harness/check_input_lock.sh - the --input-lock gate.
#
# The lock's effect is an absence: keys, pointer motion and pads that do
# nothing. An absence is only evidence when the same input demonstrably does
# something without the lock, so this runs one injection script three ways on
# the headless build and reads the end state each run prints on its
# `openpete-exit:` and `openpete-input:` lines.
#
# The script (OPENPETE_MOUSE_INJECT) presses F (free camera), moves the
# pointer, and presses Escape three times: the `menu` binding opens the
# settings menu, the pump's Escape dismiss closes it, and the third press
# opens it again. OPENPETE_MOUSE_INJECT_RAW=1 files
# the injected events and held keys with the physical devices, which is what
# the lock discards; without it they carry the injection device id, which the
# lock lets through.
#
#   leg raw+lock    raw injection, --input-lock: nothing may change. The free
#                   camera and the menu stay off, nothing is dismissed, the
#                   motion totals stay zero, and the lock's discard counter
#                   equals the number of injected events.
#   leg raw         raw injection, no lock: the negative control. The free
#                   camera engages, the motion totals move, the menu opens,
#                   the second Escape dismisses it, and the third reopens it,
#                   so the run ends with the menu open. A leg that shows none
#                   of this means the injection never engaged and the first
#                   leg proved nothing, so the verdict is VACUOUS, not PASS.
#   leg marked+lock marked injection, --input-lock: the scripted stream still
#                   flows under the lock, with the same end state as `raw`.
#
# Headless on linux-release; no window, no display server input involved. The
# injection enters at the two places a physical device does (the SDL event
# queue and the polled key snapshot), so the legs exercise the lock's own
# filters. What the legs cannot show is SDL's delivery of a real device's
# events, which is SDL's code, not this project's.
#
# Usage: tests/harness/check_input_lock.sh [--game SLUG] [--build-dir=DIR]
# Exit:  0 pass, 1 fail, 2 vacuous (negative control did not engage),
#        64 skip (no binary / no disc).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for a in "$@"; do
    case "$a" in --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;; esac
done
ARGS=()
for a in "$@"; do
    case "$a" in --build-dir=*) ;; *) ARGS+=("$a") ;; esac
done

# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game ${ARGS[@]+"${ARGS[@]}"})" || exit 64
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN" ]]  || { echo "[input-lock] SKIP: no binary $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "[input-lock] SKIP: no disc $PG_DISC"; exit 64; }

# The level name and the key bindings are Spyro 1's.
[[ "$PG_GAME" == "spyro-1" ]] ||
    { echo "[input-lock] SKIP: $PG_GAME has no input-lock scenario"; exit 64; }

OUT="${INPUT_LOCK_OUT:-${TMPDIR:-/tmp}/openpete-check-input-lock}"
mkdir -p "$OUT"

FRAME_LIMIT=2500
SCRIPT="$OUT/inject.txt"
rm -f "$SCRIPT"
cat >"$SCRIPT" <<EOF
1500 keydown F
1504 keyup F
1520 rel 40 -25
1540 keydown Escape
1544 keyup Escape
1560 keydown Escape
1564 keyup Escape
1600 keydown Escape
1604 keyup Escape
EOF
INJECTED_EVENTS=9   # eight key events and one motion event

run_leg() {
    local tag="$1"; shift
    PG_CAPTURE_ENV=(OPENPETE_MOUSE_INJECT="$SCRIPT" OPENPETE_MOUSE_PROBE=1 "$@")
    PG_CAPTURE_TIMEOUT=600
    PG_CAPTURE_STDOUT="$OUT/$tag.out"
    rm -f "$PG_CAPTURE_STDOUT"
    pg_capture "$OUT/$tag.log" "" -- --renderer vk --no-card --mute 1 \
        --skip-to-level artisans --frame-limit "$FRAME_LIMIT" ||
        { echo "[input-lock] FAIL: leg $tag exited non-zero"; return 1; }
    [[ -s "$PG_CAPTURE_STDOUT" ]] ||
        { echo "[input-lock] FAIL: leg $tag wrote no stdout"; return 1; }
}

# field LEG KEY: one `key=value` from the leg's openpete-exit / openpete-input line.
field() {
    grep -E '^openpete-(exit|input):' "$OUT/$1.out" | tr ' ' '\n' |
        grep -E "^$2=" | head -n1 | cut -d= -f2-
}
motion() { grep -E '^openpete-input:' "$OUT/$1.out" | sed -E 's/.*mouse_motion=//'; }
dismissals() { grep -c 'request close -> open=0' "$OUT/$1.log" || true; }

# Two game processes at a time.
run_leg raw_lock OPENPETE_MOUSE_INJECT_RAW=1 OPENPETE_INPUT_LOCK=1 & p1=$!
run_leg raw      OPENPETE_MOUSE_INJECT_RAW=1 & p2=$!
rc=0
wait "$p1" || rc=1
wait "$p2" || rc=1
run_leg marked_lock OPENPETE_INPUT_LOCK=1 || rc=1
[[ $rc -eq 0 ]] || exit 1

for leg in raw_lock raw marked_lock; do
    echo "[input-lock] $leg: reason=$(field $leg reason) frames=$(field $leg frames)" \
         "lock=$(field $leg lock) dropped=$(field $leg dropped)" \
         "freecam=$(field $leg freecam) menu_open=$(field $leg menu_open)" \
         "motion=$(motion $leg) dismissals=$(dismissals $leg)"
done

# Engagement of the negative control first: without it nothing below counts.
engaged() {
    [[ "$(field $1 reason)" == "frame-limit" ]] &&
    [[ "$(field $1 menu_open)" == "1" ]] &&
    [[ "$(field $1 freecam)" == "1" ]] &&
    [[ "$(motion $1)" == "(40, -25)" ]] &&
    [[ "$(dismissals $1)" -ge 1 ]]
}
if ! engaged raw; then
    echo "[input-lock] VACUOUS: raw injection without the lock changed nothing;" \
         "the injection did not engage"
    exit 2
fi

fail=0
[[ "$(field raw lock)" == "0" && "$(field raw dropped)" == "0" ]] ||
    { echo "[input-lock] FAIL: the unlocked leg discarded events"; fail=1; }

[[ "$(field raw_lock lock)" == "1" ]] ||
    { echo "[input-lock] FAIL: raw_lock did not run locked"; fail=1; }
[[ "$(field raw_lock reason)" == "frame-limit" &&
   "$(field raw_lock frames)" == "$FRAME_LIMIT" ]] ||
    { echo "[input-lock] FAIL: the locked run did not reach the frame limit"; fail=1; }
[[ "$(field raw_lock freecam)" == "0" &&
   "$(field raw_lock freecam_engages)" == "0" ]] ||
    { echo "[input-lock] FAIL: a raw F reached the free camera under the lock"; fail=1; }
[[ "$(field raw_lock menu_open)" == "0" && "$(dismissals raw_lock)" == "0" ]] ||
    { echo "[input-lock] FAIL: a raw Escape reached the menu under the lock"; fail=1; }
[[ "$(motion raw_lock)" == "(0, 0)" ]] ||
    { echo "[input-lock] FAIL: raw pointer motion reached the mouse channel under the lock"; fail=1; }
[[ "$(field raw_lock dropped)" == "$INJECTED_EVENTS" ]] ||
    { echo "[input-lock] FAIL: the lock discarded $(field raw_lock dropped) events, expected $INJECTED_EVENTS"; fail=1; }

engaged marked_lock ||
    { echo "[input-lock] FAIL: the marked injection stream did not flow under the lock"; fail=1; }
[[ "$(field marked_lock dropped)" == "0" ]] ||
    { echo "[input-lock] FAIL: the lock discarded marked injected events"; fail=1; }

if [[ $fail -ne 0 ]]; then
    echo "[input-lock] verdict: FAIL (logs in $OUT)"
    exit 1
fi
echo "[input-lock] verdict: PASS (negative control engaged: menu open, $(dismissals raw) dismissal(s)," \
     "locked run discarded $(field raw_lock dropped) events)"
