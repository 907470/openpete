#!/usr/bin/env bash
# check_canim_bounds.sh - terrain colour-anim `mix` block bounds, one headless
# row each on Spyro 2 and Spyro 3 (Spyro 1 has no past-bank triangle).
#
# On a sub-tick present or a model slot the walk reads a sector's colours from
# the slot's host-side `mix` block. A triangle's fourth colour offset (c3) may
# index past the banks: the flat and second-walk paths do not draw it, and the
# near path reads it as the game does, with no bound. A read past the block
# lands on whatever the heap holds there, and faults when the block abuts an
# uncommitted page.
#
# Instrument: OPENPETE_TERRAIN_CANIM_GUARD=1 ends `mix` flush against an
# inaccessible page, so such a read faults on every run, and the walk counts
# the faces of colour-anim passes ([canim-bounds] line): tri triangles read,
# lpTri / flatTri / nearTri triangles whose c3 is past the bank by path, corner
# faces with a drawn corner past it, tailDiff tail words that differ from
# guest RAM.
#
# Each row runs to a sub-tick present at alpha 0.5 of a colour-anim scene.
#
# Pass criterion, per row: exit 0, no crash report, tailDiff 0, and the row's
# counter above 0 (VACUOUS otherwise, exit 2). The counter is the sum of the
# past-bank triangle counts on the Spyro 2 and 3 rows. Spyro 1 authors no
# past-bank triangle on a colour-anim sector, so its row's counter is tri: the
# walk read that game's colour-anim triangles under the guard.
#
# --calibrate: the Spyro 2 and 3 rows at OPENPETE_TERRAIN_CANIM_GUARD=2, where
# each counted triangle reads the far word its c3 names from a block with no
# tail. Every row must fault with the counter above 0; a row that survives
# means the guard is blind and FAILs.
#
# Usage: tests/harness/check_canim_bounds.sh [--game SLUG] [--calibrate]
#        [--build-dir=DIR] [--out DIR]
# Exit 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP or bad usage.
set -u

export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT=""
CAL=0
GAMES="spyro-2 spyro-3"
prev=""
for a in "$@"; do
    case "$prev" in
        --out)  OUT="$a";;
        --game) GAMES="$a";;
    esac
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --game=*)      GAMES="${a#*=}";;
        --calibrate)   CAL=1;;
        --out|--game)  ;;
        -h|--help)     sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64;;
        *) [[ "$prev" == --out || "$prev" == --game ]] ||
               { echo "canim-bounds: unknown argument $a" >&2; exit 64; };;
    esac
    prev="$a"
done
[[ -n "$OUT" ]] || OUT="$BUILD_DIR/canim-bounds"
mkdir -p "$OUT"

GUARD=1
(( CAL )) && GUARD=2

# run_row GAME VBL ARGS...: one guarded run; sets ROW_RC, ROW_CRASH, ROW_HIT,
# ROW_DIFF and ROW_TRI.
run_row() {
    local game="$1" v="$2"; shift 2
    PG_BUILD_DIR="$BUILD_DIR"
    pg_resolve "$game" || exit 64
    [[ -x "$PG_BIN"  ]] || { echo "canim-bounds: SKIP (no binary at $PG_BIN)"; exit 64; }
    [[ -f "$PG_DISC" ]] || { echo "canim-bounds: SKIP (no disc at $PG_DISC)"; exit 64; }
    local tag="$game-$GUARD" mid=$((v + 1))
    local log="$OUT/$tag.log" dir="$OUT/$tag" outs=""
    rm -rf "${dir:?}"; mkdir -p "$dir"
    (( CAL )) || outs="$dir/$mid-vk.png"
    PG_CAPTURE_ENV=("OPENPETE_TERRAIN_CANIM_GUARD=$GUARD" OPENPETE_INTERMEDIATE_ALPHA=0.5)
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$outs" -- \
        --renderer vk --mute 1 --vibration 0 --aspect 4:3 --no-card \
        --screenshot-native-only --screenshot-intermediate-on-frame "$mid" \
        --screenshot-output "$dir/{n}.png" --frame-limit $((v + 11)) "$@" </dev/null
    ROW_RC=$?
    ROW_CRASH=0
    grep -aq '=== openpete crash' "$log" && ROW_CRASH=1
    local line
    line="$(grep -a '^\[canim-bounds f=' "$log" | tail -1)"
    echo "  $game: rc=$ROW_RC crash=$ROW_CRASH ${line:-no census line}"
    ROW_HIT="$(sed -n 's/.*lpTri=\([0-9]*\) flatTri=\([0-9]*\) nearTri=\([0-9]*\).*/\1 \2 \3/p' <<<"$line" |
               awk '{print $1+$2+$3}')"
    ROW_DIFF="$(sed -n 's/.*tailDiff=\([0-9]*\).*/\1/p' <<<"$line")"
    ROW_TRI="$(sed -n 's/.* tri=\([0-9]*\).*/\1/p' <<<"$line")"
    : "${ROW_HIT:=0}" "${ROW_DIFF:=0}" "${ROW_TRI:=0}"
    # A fault can beat the periodic line; the first-hit line is printed ahead
    # of the read.
    (( ROW_HIT == 0 )) && grep -aq '^\[canim-bounds-first' "$log" && ROW_HIT=1
}

fail=0 vacuous=0
for g in $GAMES; do
    case "$g" in
        spyro-2) run_row "$g" 2973 --skip-to-level summer_forest --lod extended;;
        spyro-3) run_row "$g" 2973 --skip-to-level evening_lake --lod original \
                         --freecam-pose "pos=(36800, 70400, 40000) yaw=0 pitch=992";;
        *) echo "canim-bounds: unknown game $g" >&2; exit 64;;
    esac
    if (( CAL )); then
        if (( ! ROW_CRASH )); then
            echo "canim-bounds: FAIL ($g survived the tail-less block, rc=$ROW_RC)"; fail=1
        elif (( ROW_HIT == 0 )); then
            echo "canim-bounds: VACUOUS ($g faulted with the counter at 0)"; vacuous=1
        fi
    elif (( ROW_RC != 0 || ROW_CRASH )); then
        echo "canim-bounds: FAIL ($g exited $ROW_RC crash=$ROW_CRASH, log $OUT/$g-$GUARD.log)"; fail=1
    elif (( ROW_DIFF != 0 )); then
        echo "canim-bounds: FAIL ($g tail differs from guest RAM in $ROW_DIFF reads)"; fail=1
    elif (( ROW_HIT == 0 )); then
        echo "canim-bounds: VACUOUS ($g read no index past a bank)"; vacuous=1
    fi
done

(( fail ))    && { echo "canim-bounds: FAIL"; exit 1; }
(( vacuous )) && { echo "canim-bounds: VACUOUS"; exit 2; }
echo "canim-bounds: PASS"
exit 0
