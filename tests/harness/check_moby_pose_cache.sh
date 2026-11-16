#!/usr/bin/env bash
# check_moby_pose_cache.sh - the sub-tick pose cache (games/common/renderer/
# moby.c, pc_lookup) against a fresh decode of the same records, with
# --instrument=push-fanout the moby emit fan-out (moby_emit_fanout) against
# the serial emit of the same walk, or with --instrument=snap-fanout the snap
# contact pass's fan-out (games/common/renderer/moby_snap.c, ms_fo_parallel)
# against the serial pass of the same chunk, or with --instrument=rec-fanout
# the sub-tick record pass's decode fan-out (moby.c, rex_fanout_pass) against
# a serial decode of the same records, or with --instrument=rank-fanout the
# paint rank's fan-out (moby.c, rkf_run) against the serial rank of the same
# runs, or with --instrument=snap-owned-box the snap capture's owned-sector box
# test (games/common/renderer/terrain.c, tn_snap_owned_clear) against walking
# every reached owned sector for the same capture, or with
# --instrument=snap-listed the snap capture limited to the drawn faces near
# each moby's contact box (games/common/renderer/moby_snap.c, ms_lc_capture)
# against the full walk of the same chunk.
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
# mismatched_tris= mismatched_state=). Under snap-fanout each game replays a
# bk2 that walks the player (a moving camera, so the sub-tick resolves run
# the carry's moved-camera path and resolve fresh mobys), with
# OPENPETE_MOBY_SNAP_FANOUT_CHECK=1 and both fan-out bounds at 0: every chunk
# of both legs runs serially, then fanned out, and each moby's planes, the
# snap table, the carry table, the snapped flags, the census and the corner
# boxes are compared ([snap-fanout-check final] canon: chunks= ... rex:
# chunks= ... mismatched_*=). Under rec-fanout the same walking rows run with
# OPENPETE_MOBY_REC_FANOUT_CHECK=1, its bound at 0 and the pose cache off:
# every fanned-out decode is decoded again on the calling thread and its
# vertex count and every projection field compared, and every eighth record
# takes the deferred serial path ([rec-fanout-check final] sets= ...
# mismatched_counts= mismatched_verts= ... deferred=). Under rank-fanout the
# same walking rows run with OPENPETE_MOBY_RANK_FANOUT_CHECK=1 and its bound
# at 0: every set of moby runs whose paint-rank census and rank (the
# canonical leg) or sub-tick re-rank (the fresh leg) fans out runs again on
# the calling thread from the same inputs, and each face's contest flag,
# rank and decal base are compared ([rank-fanout-check final] canon: sets=
# ... mismatched= fresh: sets= ... mismatched=). Under snap-owned-box the same
# walking rows run with OPENPETE_SNAP_OWNED_BOX_CHECK=1 and the terrain
# cache forced on (OPENPETE_TERRAIN_CACHE_FORCE=1; the software device
# otherwise keeps it off and no sector is cache-owned): every capture takes
# the owned sectors' triangles both ways and compares the two results field
# by field and their census counts ([snap-owned-box-check] captures=
# owned_tris= mismatched_counts= mismatched_tris= mismatched_census= tested=
# skipped=). Under snap-listed the same walking rows run with
# OPENPETE_SNAP_LISTED_CHECK=1 and the terrain cache forced on: every chunk
# runs the full walk and the listed capture and compares each moby's planes
# in order, the snap table rows, the carry rows, the snapped flags and the
# contact census ([snap-listed-check] chunks_canon= chunks_rex= ...
# mismatched_*= walks_full= walks_listed=).
#
# Usage:
#   tests/harness/check_moby_pose_cache.sh [--game SLUG] [--build-dir=DIR] [--out DIR]
#       [--instrument=pose-cache|push-fanout|snap-fanout|rec-fanout|rank-fanout|snap-owned-box|snap-listed]
# Exit 0 PASS: engaged above zero (hits, emits compared, chunks of each leg
# compared) and no mismatch. 1 FAIL: a mismatch, a missing capture or a
# missing total. 2 VACUOUS: no hit, no emit, no chunk of a leg compared, or
# no owned triangle captured or no owned sector walk skipped, or no listed
# chunk compared on either leg or no walk saved.
# 64 SKIP (no binary or disc).
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
if [[ "$INSTR" == snap-fanout || "$INSTR" == rec-fanout || "$INSTR" == rank-fanout || "$INSTR" == snap-owned-box ||
      "$INSTR" == snap-listed ]]; then
    # A bk2 per game that walks the player through the captured VBLs.
    BK2=games/$GAME/bk2
    case "$GAME" in
        spyro-1) ROW=(--no-card --skip-to-level artisans --bk2 "$BK2/stone-hill-entry.bk2"); VBLS=1640,1680;;
        spyro-2) ROW=(--skip-to-level colossus --player-pose "pos=(123439, 127435, 20502) yaw=-1057"
                      --bk2 "$BK2/colossus-ice-skating.bk2"); VBLS=1460,1500;;
        spyro-3) ROW=(--skip-to-level dino_mines --bk2 "$BK2/dino-mines-walk-scan.bk2"); VBLS=1920,1960;;
    esac
fi

[[ -n "$OUT" ]] || OUT="$OPENPETE_SANDBOX/pose-cache"
mkdir -p "$OUT"
LOG="$OUT/$GAME-$INSTR.log"
SHOT="$OUT/$GAME-$INSTR"
OUTS="${SHOT}__${VBLS%,*}-vk.png,${SHOT}__${VBLS#*,}-vk.png"
case "$INSTR" in
    pose-cache)  KNOB=(OPENPETE_MOBY_POSE_CACHE=2);;
    push-fanout) KNOB=(OPENPETE_MOBY_PUSH_FANOUT_CHECK=1);;
    snap-fanout) KNOB=(OPENPETE_MOBY_SNAP_FANOUT_CHECK=1 OPENPETE_MOBY_SNAP_FANOUT_MIN=0);;
    rec-fanout)  KNOB=(OPENPETE_MOBY_REC_FANOUT_CHECK=1 OPENPETE_MOBY_REC_FANOUT_MIN=0 OPENPETE_MOBY_POSE_CACHE=0);;
    rank-fanout) KNOB=(OPENPETE_MOBY_RANK_FANOUT_CHECK=1 OPENPETE_MOBY_RANK_FANOUT_MIN=0);;
    snap-owned-box)   KNOB=(OPENPETE_SNAP_OWNED_BOX_CHECK=1 OPENPETE_TERRAIN_CACHE_FORCE=1);;
    snap-listed)      KNOB=(OPENPETE_SNAP_LISTED_CHECK=1 OPENPETE_TERRAIN_CACHE_FORCE=1);;
    *) echo "pose-cache: SKIP (no instrument $INSTR)"; exit 64;;
esac
PG_CAPTURE_ENV=("${KNOB[@]}" OPENPETE_INTERMEDIATE_ALPHA=0.5
    OPENPETE_INTERMEDIATE_PRE_ALPHA=0.06,0.12,0.18,0.25,0.31,0.37,0.43,0.56,0.62,0.68,0.75,0.81,0.87,0.93,0.97)
PG_CAPTURE_TIMEOUT=600
pg_capture "$LOG" "$OUTS" -- --renderer vk --aspect 16:9 --lod extended --no-card --mute 1 \
    "${ROW[@]}" --screenshot-native-only --screenshot-intermediate-on-frame "$VBLS" \
    --screenshot-output "${SHOT}__{n}.png" --frame-limit "$(( ${VBLS#*,} + 10 ))" ||
    { echo "$INSTR: FAIL (capture, see $LOG)"; exit 1; }

if [[ "$INSTR" == snap-owned-box ]]; then
    line="$(grep '^\[snap-owned-box-check\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "snap-owned-box: FAIL (no [snap-owned-box-check] total in $LOG)"; exit 1; }
    echo "snap-owned-box: $GAME $line"
    bad=0
    for f in counts tris census; do
        v="$(sed -n "s/.* mismatched_$f=\([0-9]*\).*/\1/p" <<<"$line")"
        [[ "${v:-1}" == 0 ]] || { echo "snap-owned-box: FAIL (mismatched_$f=${v:-missing})"; bad=1; }
    done
    [[ "$bad" == 0 ]] || exit 1
    tris="$(sed -n 's/.* owned_tris=\([0-9]*\).*/\1/p' <<<"$line")"
    skip="$(sed -n 's/.* skipped=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${tris:-0}" -gt 0 && "${skip:-0}" -gt 0 ]] ||
        { echo "snap-owned-box: VACUOUS (owned triangles ${tris:-0}, walks skipped ${skip:-0})"; exit 2; }
    echo "snap-owned-box: PASS"
    exit 0
fi
if [[ "$INSTR" == snap-listed ]]; then
    line="$(grep '^\[snap-listed-check\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "snap-listed: FAIL (no [snap-listed-check] total in $LOG)"; exit 1; }
    echo "snap-listed: $GAME $line"
    bad=0
    for f in results snap snapped census; do
        v="$(sed -n "s/.* mismatched_$f=\([0-9]*\).*/\1/p" <<<"$line")"
        [[ "${v:-1}" == 0 ]] || { echo "snap-listed: FAIL (mismatched_$f=${v:-missing})"; bad=1; }
    done
    [[ "$bad" == 0 ]] || exit 1
    can="$(sed -n 's/.*chunks_canon=\([0-9]*\).*/\1/p' <<<"$line")"
    rex="$(sed -n 's/.*chunks_rex=\([0-9]*\).*/\1/p' <<<"$line")"
    wl="$(sed -n 's/.* walks_listed=\([0-9]*\).*/\1/p' <<<"$line")"
    wf="$(sed -n 's/.* walks_full=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${can:-0}" -gt 0 && "${rex:-0}" -gt 0 && "${wl:-0}" -lt "${wf:-0}" ]] ||
        { echo "snap-listed: VACUOUS (canonical chunks ${can:-0}, sub-tick chunks ${rex:-0}, walks ${wl:-0} of ${wf:-0})"; exit 2; }
    echo "snap-listed: PASS"
    exit 0
fi
if [[ "$INSTR" == rank-fanout ]]; then
    line="$(grep -m1 '^\[rank-fanout-check final\]' "$LOG")"
    [[ -n "$line" ]] || { echo "rank-fanout: FAIL (no [rank-fanout-check final] total in $LOG)"; exit 1; }
    echo "rank-fanout: $GAME $line"
    canon="${line#*canon: }"; canon="${canon%% fresh:*}"; fresh="${line#* fresh: }"; fresh="${fresh%% |*}"
    for leg in canon fresh; do
        v="${!leg}"
        bad="$(sed -n 's/.*mismatched=\([0-9]*\).*/\1/p' <<<"$v")"
        sets="$(sed -n 's/^sets=\([0-9]*\).*/\1/p' <<<"$v")"
        [[ "${bad:-1}" == 0 ]] || { echo "rank-fanout: FAIL ($leg: ${bad:-?} mismatched faces)"; exit 1; }
        [[ "${sets:-0}" -gt 0 ]] || { echo "rank-fanout: VACUOUS ($leg: no set compared)"; exit 2; }
    done
    echo "rank-fanout: PASS"
    exit 0
fi
if [[ "$INSTR" == rec-fanout ]]; then
    line="$(grep -m1 '^\[rec-fanout-check final\]' "$LOG")"
    [[ -n "$line" ]] || { echo "rec-fanout: FAIL (no [rec-fanout-check final] total in $LOG)"; exit 1; }
    echo "rec-fanout: $GAME $line"
    bn="$(sed -n 's/.* mismatched_counts=\([0-9]*\).*/\1/p' <<<"$line")"
    bv="$(sed -n 's/.* mismatched_verts=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${bn:-1}" == 0 && "${bv:-1}" == 0 ]] ||
        { echo "rec-fanout: FAIL ($bn mismatched counts, $bv mismatched vertices)"; exit 1; }
    sets="$(sed -n 's/.*sets=\([0-9]*\).*/\1/p' <<<"$line")"
    dfr="$(sed -n 's/.* deferred=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${sets:-0}" -gt 0 && "${dfr:-0}" -gt 0 ]] ||
        { echo "rec-fanout: VACUOUS (decode sets ${sets:-0}, deferred records ${dfr:-0})"; exit 2; }
    echo "rec-fanout: PASS"
    exit 0
fi
if [[ "$INSTR" == snap-fanout ]]; then
    line="$(grep -m1 '^\[snap-fanout-check final\]' "$LOG")"
    [[ -n "$line" ]] || { echo "snap-fanout: FAIL (no [snap-fanout-check final] total in $LOG)"; exit 1; }
    echo "snap-fanout: $GAME $line"
    bad=0
    for f in results snap carry snapped census boxes; do
        v="$(sed -n "s/.* mismatched_$f=\([0-9]*\).*/\1/p" <<<"$line")"
        [[ "${v:-1}" == 0 ]] || { echo "snap-fanout: FAIL (mismatched_$f=${v:-missing})"; bad=1; }
    done
    [[ "$bad" == 0 ]] || exit 1
    can="$(sed -n 's/.*canon: chunks=\([0-9]*\).*/\1/p' <<<"$line")"
    rex="$(sed -n 's/.*rex: chunks=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${can:-0}" -gt 0 && "${rex:-0}" -gt 0 ]] ||
        { echo "snap-fanout: VACUOUS (canonical chunks ${can:-0}, sub-tick chunks ${rex:-0})"; exit 2; }
    echo "snap-fanout: PASS"
    exit 0
fi
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
