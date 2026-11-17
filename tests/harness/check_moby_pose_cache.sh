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
# runs, or with --instrument=snap-listed the snap capture limited to the drawn faces near
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
# ... mismatched= fresh: sets= ... mismatched=). Under snap-listed the same
# walking rows run with OPENPETE_SNAP_LISTED_CHECK=1 and the terrain cache
# forced on (OPENPETE_TERRAIN_CACHE_FORCE=1; the software device otherwise
# keeps it off and no sector is cache-owned): every chunk runs the full walk and the listed capture and compares each moby's planes
# in order, the snap table rows, the carry rows, the snapped flags and the
# contact census ([snap-listed-check] chunks_canon= chunks_rex= ...
# mismatched_*= walks_full= walks_listed=). Under snap-rex-merge the same
# walking rows run with OPENPETE_SNAP_REX_MERGE_CHECK=1 and the terrain cache
# forced on: every sub-tick present resolves its snap queue once per walk
# pass and then once for all passes (games/common/renderer/moby_snap.c,
# ms_rm_check), and the packed snap table words and the table's
# insertion-order digest are compared ([snap-rex-merge-check] presents=
# presents_multi= ... mismatched_words= mismatched_digest=). Under
# snap-corner-eq the still-camera rows run with OPENPETE_SNAP_CORNER_EQ_CHECK=1:
# every moby whose key-depth corners the snap carry compares under a camera
# bit-equal to its endpoint's takes both the input test and the comparison
# (moby_snap.c, mc_corners_moved), and a moby the test passes that the
# comparison would refresh is a mismatch ([snap-corner-eq-check] resolves=
# mobys= inputs_equal= mismatched=). Under snap-queue-lean the walking rows
# run with OPENPETE_SNAP_QUEUE_LEAN_CHECK=1 and the terrain cache forced on:
# every queued moby's bounds are computed at the queue and again when its
# chunk resolves (ms_bounds_ensure), compared bit for bit
# ([snap-queue-lean-check] queued= resolved= compared= mismatched=), and the
# collect gathers each moby's corners into the queue and into its own buffer
# and compares them (moby.c, moby_snap_gather_to;
# [snap-queue-lean-gather] mobys= mismatched=). Under snap-min-px the
# still-camera rows run with OPENPETE_SNAP_MIN_PX=16 and the terrain cache
# forced on: the screen-size bound must leave mobys out of both the tick's
# resolve and the sub-tick resolves, and an in-between scene must inherit the
# endpoint's decision for some of its mobys rather than compute their bounds
# ([snap-min-px total] ... considered_tick= out_tick= considered_scene=
# out_scene= bounded_scene=); a run where the bound is set and nothing is
# left out fails.
# Under rank-absent the same walking rows run with
# OPENPETE_MOBY_RANK_ABSENT_SKIP=check: every sub-tick moby run whose faces
# absent from the carry table contest no face (rank_absent_contests) is also
# re-ranked in full, and the contested order the select reads is compared
# ([rank-absent-check final] skipped= reranked= compared= mismatched=).
# Under occl-held the same walking rows at eight VBLs capture the sub-tick
# present at alpha 0.95 twice, with the re-confirm of held provisional
# occlusion admissions on (OPENPETE_OCCL_HELD_CONFIRM=log) and off: every
# capture pair must match byte for byte while the captured presents dropped
# a moby ([occl-held] ... dropped=, matched to the captures by their
# "wrote intermediate ... on frame" lines).
#
# Under sim-proj-defer the still-camera rows (Spyro 2 and 3; Spyro 1 has
# no simple-model decode) run with OPENPETE_MOBY_SIM_PROJ_DEFER_CHECK=1:
# every simple-moby record whose projection moves into its job is also
# projected in the serial prefix, and the job compares every field of its
# own projection and the paint key's integer RTPS with the prefix's
# ([sim-proj-defer-check final] deferred= immediate= records= verts=
# mismatched=).
#
# Usage:
#   tests/harness/check_moby_pose_cache.sh [--game SLUG] [--build-dir=DIR] [--out DIR]
#       [--instrument=pose-cache|push-fanout|snap-fanout|rec-fanout|rank-fanout|snap-listed|snap-rex-merge
#        |snap-corner-eq|snap-queue-lean|snap-min-px
#        |rank-absent|occl-held|sim-proj-defer]
# Exit 0 PASS: engaged above zero (hits, emits compared, chunks of each leg
# compared) and no mismatch. 1 FAIL: a mismatch, a missing capture or a
# missing total. 2 VACUOUS: no hit, no emit, no chunk of a leg compared, or
# no simple record compared, no listed chunk compared on either leg or no walk saved, or no present
# compared or none where two walk passes queued, or no moby with equal
# corner inputs, or no moby resolved or none left unresolved, no kept or
# no re-ranked run, or no moby dropped on a captured present. snap-min-px
# has no VACUOUS outcome: nothing left out is a FAIL.
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
if [[ "$INSTR" == snap-fanout || "$INSTR" == rec-fanout || "$INSTR" == rank-fanout || "$INSTR" == snap-listed ||
      "$INSTR" == snap-rex-merge || "$INSTR" == snap-queue-lean ||
      "$INSTR" == rank-absent || "$INSTR" == occl-held ]]; then
    # A bk2 per game that walks the player through the captured VBLs.
    BK2=games/$GAME/bk2
    case "$GAME" in
        spyro-1) ROW=(--no-card --skip-to-level artisans --bk2 "$BK2/stone-hill-entry.bk2"); VBLS=1640,1680;;
        spyro-2) ROW=(--skip-to-level colossus --player-pose "pos=(123439, 127435, 20502) yaw=-1057"
                      --bk2 "$BK2/colossus-ice-skating.bk2"); VBLS=1460,1500;;
        spyro-3) ROW=(--skip-to-level dino_mines --bk2 "$BK2/dino-mines-walk-scan.bk2"); VBLS=1920,1960;;
    esac
    # The re-confirm drops mobys near the tick's own camera: eight VBLs from
    # the first, captured at alpha 0.95.
    if [[ "$INSTR" == occl-held ]]; then
        v0=${VBLS%,*}; VBLS=$v0
        for k in 1 2 3 4 5 6 7; do VBLS+=",$(( v0 + 10 * k ))"; done
    fi
fi

[[ -n "$OUT" ]] || OUT="$OPENPETE_SANDBOX/pose-cache"
mkdir -p "$OUT"
LOG="$OUT/$GAME-$INSTR.log"
SHOT="$OUT/$GAME-$INSTR"
OUTS="${SHOT}__${VBLS%,*}-vk.png,${SHOT}__${VBLS#*,}-vk.png"
case "$INSTR" in
    # The snap compare rows run with the screen-size bound off (OPENPETE_SNAP_MIN_PX=0):
    # each checks machinery the bound would leave with nothing to compare on a row
    # of small mobys. snap-min-px sets the bound itself.
    pose-cache)  KNOB=(OPENPETE_MOBY_POSE_CACHE=2);;
    push-fanout) KNOB=(OPENPETE_MOBY_PUSH_FANOUT_CHECK=1);;
    snap-fanout) KNOB=(OPENPETE_MOBY_SNAP_FANOUT_CHECK=1 OPENPETE_MOBY_SNAP_FANOUT_MIN=0 OPENPETE_SNAP_MIN_PX=0);;
    rec-fanout)  KNOB=(OPENPETE_MOBY_REC_FANOUT_CHECK=1 OPENPETE_MOBY_REC_FANOUT_MIN=0 OPENPETE_MOBY_POSE_CACHE=0);;
    rank-fanout) KNOB=(OPENPETE_MOBY_RANK_FANOUT_CHECK=1 OPENPETE_MOBY_RANK_FANOUT_MIN=0);;
    snap-listed)      KNOB=(OPENPETE_SNAP_LISTED_CHECK=1 OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_SNAP_MIN_PX=0);;
    snap-rex-merge)   KNOB=(OPENPETE_SNAP_REX_MERGE_CHECK=1 OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_SNAP_MIN_PX=0);;
    snap-corner-eq)   KNOB=(OPENPETE_SNAP_CORNER_EQ_CHECK=1 OPENPETE_SNAP_MIN_PX=0);;
    snap-queue-lean)  KNOB=(OPENPETE_SNAP_QUEUE_LEAN_CHECK=1 OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_SNAP_MIN_PX=0);;
    snap-min-px)      KNOB=(OPENPETE_SNAP_MIN_PX=16 OPENPETE_TERRAIN_CACHE_FORCE=1);;
    rank-absent) KNOB=(OPENPETE_MOBY_RANK_ABSENT_SKIP=check);;
    occl-held)   KNOB=(OPENPETE_OCCL_HELD_CONFIRM=log);;
    sim-proj-defer) [[ "$GAME" == spyro-1 ]] && { echo "sim-proj-defer: SKIP (no simple-model decode on $GAME)"; exit 64; }
                 KNOB=(OPENPETE_MOBY_SIM_PROJ_DEFER_CHECK=1);;
    *) echo "pose-cache: SKIP (no instrument $INSTR)"; exit 64;;
esac
ALPHA=0.5
PRE=0.06,0.12,0.18,0.25,0.31,0.37,0.43,0.56,0.62,0.68,0.75,0.81,0.87,0.93,0.97
if [[ "$INSTR" == occl-held ]]; then
    ALPHA=0.95
    PRE=0.06,0.12,0.18,0.25,0.31,0.37,0.43,0.5,0.56,0.62,0.68,0.75,0.81,0.87,0.93,0.97
    OUTS=""
    for v in ${VBLS//,/ }; do OUTS+="${OUTS:+,}${SHOT}__$v-vk.png"; done
fi
PG_CAPTURE_TIMEOUT=600
# capture LOG SHOT: one headless run of the row with KNOB at the leg's alpha.
capture() {
    local outs="${OUTS//"$SHOT"/"$2"}"
    PG_CAPTURE_ENV=("${KNOB[@]}" OPENPETE_INTERMEDIATE_ALPHA=$ALPHA OPENPETE_INTERMEDIATE_PRE_ALPHA=$PRE)
    pg_capture "$1" "$outs" -- --renderer vk --aspect 16:9 --lod extended --no-card --mute 1 \
        "${ROW[@]}" --screenshot-native-only --screenshot-intermediate-on-frame "$VBLS" \
        --screenshot-output "$2__{n}.png" --frame-limit "$(( ${VBLS##*,} + 10 ))"
}
capture "$LOG" "$SHOT" || { echo "$INSTR: FAIL (capture, see $LOG)"; exit 1; }

if [[ "$INSTR" == occl-held ]]; then
    KNOB=(OPENPETE_OCCL_HELD_CONFIRM=0)
    capture "$LOG.off" "$SHOT-off" || { echo "occl-held: FAIL (off capture, see $LOG.off)"; exit 1; }
    bad=0 drop=0
    for v in ${VBLS//,/ }; do
        f="$(sed -n "s/.*wrote intermediate alpha=${ALPHA}0 .* on frame \([0-9]*\) (target=$v,.*/\1/p" "$LOG" | head -n 1)"
        d="$(sed -n "s/^\[occl-held\] vbl=$f alpha=${ALPHA}0 .* dropped=\([0-9]*\).*/\1/p" "$LOG" | head -n 1)"
        same=1
        cmp -s "${SHOT}__$v-vk.png" "${SHOT}-off__$v-vk.png" || { same=0; bad=1; }
        echo "occl-held: $GAME vbl=$v frame=${f:-?} dropped=${d:-0} identical=$same"
        drop=$(( drop + ${d:-0} ))
    done
    grep '^\[occl-held final\]' "$LOG" | sed "s/^/occl-held: $GAME /"
    [[ "$bad" == 0 ]] || { echo "occl-held: FAIL (a capture differs with the re-confirm off)"; exit 1; }
    [[ "$drop" -gt 0 ]] || { echo "occl-held: VACUOUS (no moby dropped on a captured present)"; exit 2; }
    echo "occl-held: PASS"
    exit 0
fi
if [[ "$INSTR" == sim-proj-defer ]]; then
    line="$(grep '^\[sim-proj-defer-check final\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "sim-proj-defer: FAIL (no [sim-proj-defer-check final] total in $LOG)"; exit 1; }
    echo "sim-proj-defer: $GAME $line"
    bd="$(sed -n 's/.* mismatched=\([0-9]*\).*/\1/p' <<<"$line")"
    rc="$(sed -n 's/.* records=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${bd:-1}" == 0 ]] || { echo "sim-proj-defer: FAIL (${bd:-?} mismatched records of ${rc:-?})"; exit 1; }
    [[ "${rc:-0}" -gt 0 ]] || { echo "sim-proj-defer: VACUOUS (no deferred record compared)"; exit 2; }
    echo "sim-proj-defer: PASS"
    exit 0
fi
if [[ "$INSTR" == rank-absent ]]; then
    line="$(grep -m1 '^\[rank-absent-check final\]' "$LOG")"
    [[ -n "$line" ]] || { echo "rank-absent: FAIL (no [rank-absent-check final] total in $LOG)"; exit 1; }
    echo "rank-absent: $GAME $line"
    sk="$(sed -n 's/.* skipped=\([0-9]*\).*/\1/p' <<<"$line")"
    rr="$(sed -n 's/.* reranked=\([0-9]*\).*/\1/p' <<<"$line")"
    cm="$(sed -n 's/.* compared=\([0-9]*\).*/\1/p' <<<"$line")"
    bd="$(sed -n 's/.* mismatched=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${bd:-1}" == 0 && "${cm:-0}" == "${sk:-x}" ]] ||
        { echo "rank-absent: FAIL (${bd:-?} mismatched of ${cm:-?} compared, ${sk:-?} kept)"; exit 1; }
    [[ "${sk:-0}" -gt 0 && "${rr:-0}" -gt 0 ]] ||
        { echo "rank-absent: VACUOUS (kept runs ${sk:-0}, re-ranked runs ${rr:-0})"; exit 2; }
    echo "rank-absent: PASS"
    exit 0
fi

if [[ "$INSTR" == snap-corner-eq ]]; then
    line="$(grep '^\[snap-corner-eq-check\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "snap-corner-eq: FAIL (no [snap-corner-eq-check] total in $LOG)"; exit 1; }
    echo "snap-corner-eq: $GAME $line"
    v="$(sed -n 's/.* mismatched=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${v:-1}" == 0 ]] || { echo "snap-corner-eq: FAIL (mismatched=${v:-missing})"; exit 1; }
    eq="$(sed -n 's/.* inputs_equal=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${eq:-0}" -gt 0 ]] || { echo "snap-corner-eq: VACUOUS (no moby with equal corner inputs)"; exit 2; }
    echo "snap-corner-eq: PASS"
    exit 0
fi
if [[ "$INSTR" == snap-queue-lean ]]; then
    line="$(grep '^\[snap-queue-lean-check\]' "$LOG" | tail -n 1)"
    gline="$(grep '^\[snap-queue-lean-gather\]' "$LOG" | tail -n 1)"
    [[ -n "$line" && -n "$gline" ]] || { echo "snap-queue-lean: FAIL (no [snap-queue-lean-check] or -gather total in $LOG)"; exit 1; }
    echo "snap-queue-lean: $GAME $line"
    echo "snap-queue-lean: $GAME $gline"
    v="$(sed -n 's/.* mismatched=\([0-9]*\).*/\1/p' <<<"$line")"
    gv="$(sed -n 's/.* mismatched=\([0-9]*\).*/\1/p' <<<"$gline")"
    [[ "${v:-1}" == 0 && "${gv:-1}" == 0 ]] ||
        { echo "snap-queue-lean: FAIL (bounds mismatched=${v:-missing}, gather mismatched=${gv:-missing})"; exit 1; }
    q="$(sed -n 's/.*queued=\([0-9]*\).*/\1/p' <<<"$line")"
    c="$(sed -n 's/.* compared=\([0-9]*\).*/\1/p' <<<"$line")"
    r="$(sed -n 's/.* resolved=\([0-9]*\).*/\1/p' <<<"$line")"
    gm="$(sed -n 's/.* mobys=\([0-9]*\).*/\1/p' <<<"$gline")"
    [[ "${c:-0}" -gt 0 && "${r:-0}" -lt "${q:-0}" && "${gm:-0}" -gt 0 ]] ||
        { echo "snap-queue-lean: VACUOUS (compared ${c:-0}, resolved ${r:-0} of ${q:-0} queued, gathered ${gm:-0})"; exit 2; }
    echo "snap-queue-lean: PASS"
    exit 0
fi
if [[ "$INSTR" == snap-min-px ]]; then
    line="$(grep '^\[snap-min-px total\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "snap-min-px: FAIL (no [snap-min-px total] line in $LOG)"; exit 1; }
    echo "snap-min-px: $GAME $line"
    for f in considered_tick out_tick considered_scene out_scene bounded_scene; do
        printf -v "v_$f" '%s' "$(sed -n "s/.* $f=\([0-9]*\).*/\1/p" <<<"$line")"
    done
    [[ "${v_out_tick:-0}" -gt 0 && "${v_out_scene:-0}" -gt 0 ]] ||
        { echo "snap-min-px: FAIL (bound set, left out tick ${v_out_tick:-0} of ${v_considered_tick:-0}, scene ${v_out_scene:-0} of ${v_considered_scene:-0})"; exit 1; }
    [[ "${v_bounded_scene:-0}" -lt "${v_considered_scene:-0}" ]] ||
        { echo "snap-min-px: FAIL (no in-between moby inherited its endpoint's decision: bounded ${v_bounded_scene:-0} of ${v_considered_scene:-0})"; exit 1; }
    echo "snap-min-px: PASS"
    exit 0
fi
if [[ "$INSTR" == snap-rex-merge ]]; then
    line="$(grep '^\[snap-rex-merge-check\]' "$LOG" | tail -n 1)"
    [[ -n "$line" ]] || { echo "snap-rex-merge: FAIL (no [snap-rex-merge-check] total in $LOG)"; exit 1; }
    echo "snap-rex-merge: $GAME $line"
    bad=0
    for f in words digest; do
        v="$(sed -n "s/.* mismatched_$f=\([0-9]*\).*/\1/p" <<<"$line")"
        [[ "${v:-1}" == 0 ]] || { echo "snap-rex-merge: FAIL (mismatched_$f=${v:-missing})"; bad=1; }
    done
    [[ "$bad" == 0 ]] || exit 1
    pr="$(sed -n 's/.*presents=\([0-9]*\) .*/\1/p' <<<"$line")"
    mu="$(sed -n 's/.* presents_multi=\([0-9]*\).*/\1/p' <<<"$line")"
    [[ "${pr:-0}" -gt 0 && "${mu:-0}" -gt 0 ]] ||
        { echo "snap-rex-merge: VACUOUS (presents ${pr:-0}, presents with two queueing passes ${mu:-0})"; exit 2; }
    echo "snap-rex-merge: PASS"
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
