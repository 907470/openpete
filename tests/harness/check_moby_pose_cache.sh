#!/usr/bin/env bash
# check_moby_pose_cache.sh - the sub-tick pose cache (games/common/renderer/
# moby.c, pc_lookup) against a fresh decode of the same records, or with
# --instrument=push-fanout the moby emit fan-out (moby_emit_fanout) against
# the serial emit of the same walk.
#
# One headless capture per game at extended LOD with sixteen sub-tick
# re-extracts at each of two VBLs (fifteen uncaptured phases from
# OPENPETE_INTERMEDIATE_PRE_ALPHA and the captured one), under
# OPENPETE_MOBY_POSE_CACHE=2: every sub-tick decode whose key words the guest
# generation already filed decodes again and compares each vertex's current
# and previous pose with the filed copy, bit for bit. The game prints the
# totals at exit ([pose-cache] ... hits= mismatches=). Under push-fanout the
# same capture runs with OPENPETE_MOBY_PUSH_FANOUT_CHECK=1: every emit that
# fans out runs serially first, and every stored triangle and the tie,
# ordinal and scene counts are compared ([push-fanout-check final] emits=
# mismatched_tris= mismatched_state=).
#
# Usage:
#   tests/harness/check_moby_pose_cache.sh [--game SLUG] [--build-dir=DIR] [--out DIR]
#       [--instrument=pose-cache|push-fanout]
# Exit 0 PASS: engaged above zero (hits, emits compared) and no mismatch.
# 1 FAIL: a mismatch, a missing capture or a missing total. 2 VACUOUS: no
# hit or no emit compared. 64 SKIP (no binary or disc).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT=""
INSTR=pose-cache
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --instrument=*) INSTR="${a#*=}";;
    esac
    [[ "$prev" == --out ]] && OUT="$a"
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN"  ]] || { echo "pose-cache: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "pose-cache: SKIP (no disc at $PG_DISC)"; exit 64; }

# One row per game: a level and pose with animated mobys in view, and the
# two VBLs the re-extracts run at.
case "$GAME" in
    spyro-1) ROW=(--skip-to-level stone_hill --freecam-pose topdown); VBLS=3000,3040;;
    spyro-2) ROW=(--skip-to-level skelos_badlands --freecam-pose "pos=(116034, 2704, 95746) yaw=1112 pitch=541")
             VBLS=3000,3040;;
    spyro-3) ROW=(--skip-to-level sunrise_spring --freecam-pose topdown); VBLS=3000,3040;;
    *) echo "pose-cache: SKIP (no row for $GAME)"; exit 64;;
esac

[[ -n "$OUT" ]] || OUT="$OPENPETE_SANDBOX/pose-cache"
mkdir -p "$OUT"
LOG="$OUT/$GAME-$INSTR.log"
SHOT="$OUT/$GAME-$INSTR"
OUTS="${SHOT}__${VBLS%,*}-vk.png,${SHOT}__${VBLS#*,}-vk.png"
case "$INSTR" in
    pose-cache)  KNOB=OPENPETE_MOBY_POSE_CACHE=2;;
    push-fanout) KNOB=OPENPETE_MOBY_PUSH_FANOUT_CHECK=1;;
    *) echo "pose-cache: SKIP (no instrument $INSTR)"; exit 64;;
esac
PG_CAPTURE_ENV=("$KNOB" OPENPETE_INTERMEDIATE_ALPHA=0.5
    OPENPETE_INTERMEDIATE_PRE_ALPHA=0.06,0.12,0.18,0.25,0.31,0.37,0.43,0.56,0.62,0.68,0.75,0.81,0.87,0.93,0.97)
PG_CAPTURE_TIMEOUT=600
pg_capture "$LOG" "$OUTS" -- --renderer vk --aspect 16:9 --lod extended --no-card --mute 1 \
    "${ROW[@]}" --screenshot-native-only --screenshot-intermediate-on-frame "$VBLS" \
    --screenshot-output "${SHOT}__{n}.png" --frame-limit "$(( ${VBLS#*,} + 10 ))" ||
    { echo "$INSTR: FAIL (capture, see $LOG)"; exit 1; }

if [[ "$INSTR" == push-fanout ]]; then
    line="$(grep -m1 '^\[push-fanout-check final\]' "$LOG")"
    [[ -n "$line" ]] || { echo "push-fanout: FAIL (no [push-fanout-check final] total in $LOG)"; exit 1; }
    emits="$(sed -n 's/.* emits=\([0-9]*\).*/\1/p' <<<"$line")"
    bt="$(sed -n 's/.* mismatched_tris=\([0-9]*\).*/\1/p' <<<"$line")"
    bs="$(sed -n 's/.* mismatched_state=\([0-9]*\).*/\1/p' <<<"$line")"
    echo "push-fanout: $GAME $line"
    [[ "${bt:-1}" == 0 && "${bs:-1}" == 0 ]] ||
        { echo "push-fanout: FAIL ($bt mismatched triangles, $bs mismatched end states)"; exit 1; }
    [[ "${emits:-0}" -gt 0 ]] || { echo "push-fanout: VACUOUS (no emit fanned out)"; exit 2; }
    echo "push-fanout: PASS"
    exit 0
fi
line="$(grep -m1 '^\[pose-cache\]' "$LOG")"
[[ -n "$line" ]] || { echo "pose-cache: FAIL (no [pose-cache] total in $LOG)"; exit 1; }
hits="$(sed -n 's/.* hits=\([0-9]*\).*/\1/p' <<<"$line")"
bad="$(sed -n 's/.* mismatches=\([0-9]*\).*/\1/p' <<<"$line")"
echo "pose-cache: $GAME $line"
[[ "${bad:-1}" == 0 ]] || { echo "pose-cache: FAIL ($bad mismatched vertices)"; exit 1; }
[[ "${hits:-0}" -gt 0 ]] || { echo "pose-cache: VACUOUS (no sub-tick decode hit the cache)"; exit 2; }
echo "pose-cache: PASS"
exit 0
