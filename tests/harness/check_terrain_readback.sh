#!/usr/bin/env bash
# check_terrain_readback.sh - retained-terrain shared-vertex position
# readback: do vertices built by the same operations on the same integer
# corners land on bit-identical clip positions, whichever sector and arena
# family draws them?
#
# Per freecam pose and LOD tier, two arms on one build and device, each
# captured twice (OPENPETE_TERRAIN_READBACK=1 writes <shot>-readback.bin
# beside the screenshot; native_terrain_cache.c
# openpete_tercache_readback_record):
#   f32  the fp32 stages (OPENPETE_TERRAIN_FP32=1): expected 0 mismatches
#   ctl  the per-sector rounding control (OPENPETE_TERRAIN_FP32=2): its
#        camera operand is rounded per sector, so a vertex two sectors share
#        can round apart; expected nonzero, the proof the instrument sees a
#        disagreement
# tools/terrain_readback.py pairs the written vertices by readback key and
# counts, per family pair (plain/plain, plain/anim, plain/near, anim/near,
# cell/near, cell/anim, cell/cell, cut/cut, then any other pair present),
# the pairs compared and the pairs whose gl_Position bits differ, and of
# each the pairs drawn by two different sectors (xsec); `same` compares a
# leg's two captures.
#
# --jitter=J sets OPENPETE_TERRAIN_CAM_JITTER=J (world/16 units) on every
# leg: a freecam camera is integral, and with an integral camera both
# combines are exact, so the control cannot engage without a fraction. The
# fraction must also make the control's double rounding visible: the
# control rounds the sector's camera operand at ulp(origin - camera), then
# the sum at ulp(v), and the two roundings differ from one only when the
# first lands on a tie of the second that round-to-even resolves the other
# way. With the camera fraction f = 4J in x4 units, f = 2^-9 + 2^-14 + 2^-19
# (the default, J = 2^-11 + 2^-16 + 2^-21) puts such a tie at ulp(v) =
# 2^-8 and 2^-13 for every sector whose origin lies far enough from the
# camera to drop the next set bit. A fraction such as 0.3711 x 4 = 0.4844
# rounds to 31/64, a tie whose even neighbour is the value direct rounding
# gives, so the control reads no mismatch.
# The run uses subpixel vertex precision and perspective texturing, where
# the emitted screen point is the projection of the camera-space position;
# the integer snap and the affine nc4 morph move it by screen-space rules
# the key does not name.
#
# Usage:
#   tests/harness/check_terrain_readback.sh [--game SLUG] [--filter SUBSTR]
#       [--stride=K] [--manifest=PATH] [--lods "original extended"]
#       [--jitter=J] [--carve-out=PAIR[,PAIR...]] [--out DIR] [--build-dir=DIR]
# --manifest names a freecam manifest (name | pose | start) in place of the
# game's corpus; --stride=K keeps every K-th selected row.
#
# Exit 0 PASS: every f32 pair agrees, every leg's two captures agree, the
# control disagrees somewhere, and every named family pair was compared at
# least once over the run (a pair named by --carve-out excepted). 1 FAIL:
# an f32 mismatch, a nondeterministic leg, or a missing capture. 2 VACUOUS:
# no row, the control found no mismatch, or a named pair was never compared.
# 64 SKIP (no binary, disc or numpy).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT="" FILTER="" LODS="original extended" JITTER=0.000504016876220703125 STRIDE=1 MAN="" CARVE=""
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --lods=*)      LODS="${a#*=}";;
        --jitter=*)    JITTER="${a#*=}";;
        --stride=*)    STRIDE="${a#*=}";;
        --manifest=*)  MAN="${a#*=}";;
        --carve-out=*) CARVE="${a#*=}";;
    esac
    case "$prev" in
        --out) OUT="$a";; --filter) FILTER="$a";; --lods) LODS="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
pg_cap_settings
[[ -n "$MAN" ]] || MAN="$PG_FREECAM_MANIFEST"
[[ -x "$PG_BIN"  ]] || { echo "readback: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "readback: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import numpy' 2>/dev/null || { echo "readback: SKIP (python3 or numpy missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$BUILD_DIR/terrain-readback/$PG_GAME"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
TOOL=tools/terrain_readback.py

leg() {  # tag level pose start lod precision
    local tag="$1" level="$2" pose="$3" start="$4" lod="$5" prec="$6"
    local -a PG_SKIP_ARGS=()
    pg_skip_args "$level"
    rm -f "${OUT:?}/$tag.png" "${OUT:?}/$tag-vk.png" "${OUT:?}/$tag-readback.bin"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_TERRAIN_READBACK=1
                    "OPENPETE_TERRAIN_FP32=$prec" "OPENPETE_TERRAIN_CAM_JITTER=$JITTER")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$OUT/$tag.log" "$OUT/$tag-readback.bin" -- \
        --no-card --mute 1 --vibration 0 --mods "" --renderer vk --lod "$lod" --aspect 4:3 \
        --vertex-precision subpixel --texture-mapping perspective \
        "${PG_SKIP_ARGS[@]}" --freecam-pose "$pose" \
        --screenshot-native-only --screenshot-on-frame "$start" \
        --screenshot-output "$OUT/$tag.png" \
        --frame-limit $((start + 20)) --max-runtime-seconds 840
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
LEVELS=(); POSES=(); SLUGS=(); STARTS=(); nrow=0
while IFS='|' read -r name pose start _; do
    name="$(echo "$name" | tr -d ' \t')"
    [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [[ -z "$pose" ]] && continue
    start="$(echo "$start" | tr -d ' \t')"
    slug="$(pg_pose_slug "$name" "$pose")"
    [[ -n "$FILTER" && "$name $slug" != *"$FILTER"* ]] && continue
    nrow=$((nrow + 1))
    (( (nrow - 1) % STRIDE == 0 )) || continue
    LEVELS+=("${name%%@*}"); POSES+=("$pose"); SLUGS+=("$slug")
    STARTS+=("${start:-$PG_FREECAM_START}")
done < "$MAN"
echo "[readback] game=$PG_GAME rows=${#SLUGS[@]} manifest=$MAN jitter=$JITTER"
[[ ${#SLUGS[@]} -gt 0 ]] || { echo "[readback] game=$PG_GAME rows=0 verdict=VACUOUS"; exit 2; }

declare -A F32_C=() F32_M=() CTL_C=() CTL_M=() F32_X=() CTL_X=() CTL_XM=()
NAMED="plain/plain plain/anim plain/near anim/near cell/near cell/anim cell/cell cut/cut"
missing=0 nondet=0 rows=0
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        rows=$((rows + 1))
        for arm in f32 ctl; do
            prec=1; [[ $arm == ctl ]] && prec=2
            base="${SLUGS[$i]}__${lod}__${arm}"
            ok=1
            for r in r1 r2; do
                leg "${base}__$r" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$prec" || ok=0
            done
            if [[ $ok == 0 ]]; then
                echo "[readback] row=${SLUGS[$i]} lod=$lod arm=$arm NO-CAPTURE"
                missing=$((missing + 1)); continue
            fi
            same="$(python3 "$TOOL" same "$OUT/${base}__r1-readback.bin" "$OUT/${base}__r2-readback.bin")"
            [[ "$same" == same=1* ]] || nondet=$((nondet + 1))
            python3 "$TOOL" pairs "$OUT/${base}__r1-readback.bin" --detail 3 > "$OUT/$base.pairs"
            head -1 "$OUT/$base.pairs" | sed "s/^/[readback] row=${SLUGS[$i]} lod=$lod arm=$arm $same /"
            while read -r p c m x xm; do
                p="${p#pair=}"; c="${c#compared=}"; m="${m#mismatch=}"
                x="${x#xsec=}"; xm="${xm#xsec_mismatch=}"
                echo "[readback] row=${SLUGS[$i]} lod=$lod arm=$arm pair=$p compared=$c" \
                     "mismatch=$m xsec=$x xsec_mismatch=$xm"
                if [[ $arm == f32 ]]; then
                    F32_C[$p]=$(( ${F32_C[$p]:-0} + c )); F32_M[$p]=$(( ${F32_M[$p]:-0} + m ))
                    F32_X[$p]=$(( ${F32_X[$p]:-0} + x ))
                else
                    CTL_C[$p]=$(( ${CTL_C[$p]:-0} + c )); CTL_M[$p]=$(( ${CTL_M[$p]:-0} + m ))
                    CTL_X[$p]=$(( ${CTL_X[$p]:-0} + x )); CTL_XM[$p]=$(( ${CTL_XM[$p]:-0} + xm ))
                fi
            done < <(grep '^pair=' "$OUT/$base.pairs")
        done
    done
done

f32_mis=0 ctl_mis=0 unengaged="" seen=""
for p in $NAMED "${!F32_C[@]}"; do
    [[ " $seen " == *" $p "* ]] && continue
    seen="$seen $p"
    echo "[readback] game=$PG_GAME total pair=$p f32_compared=${F32_C[$p]:-0}" \
         "f32_xsec=${F32_X[$p]:-0} f32_mismatch=${F32_M[$p]:-0} ctl_compared=${CTL_C[$p]:-0}" \
         "ctl_mismatch=${CTL_M[$p]:-0} ctl_xsec_mismatch=${CTL_XM[$p]:-0}"
    f32_mis=$((f32_mis + ${F32_M[$p]:-0})); ctl_mis=$((ctl_mis + ${CTL_M[$p]:-0}))
    if [[ " $NAMED " == *" $p "* && ${F32_C[$p]:-0} -eq 0 && ",$CARVE," != *",$p,"* ]]; then
        unengaged="$unengaged $p"
    fi
done
verdict=PASS rc=0
if [[ $f32_mis -ne 0 || $nondet -ne 0 || $missing -ne 0 ]]; then
    verdict=FAIL rc=1
elif [[ $ctl_mis -eq 0 || -n "$unengaged" ]]; then
    verdict=VACUOUS rc=2
fi
echo "[readback] game=$PG_GAME rows=$rows f32_mismatch=$f32_mis ctl_mismatch=$ctl_mis" \
     "nondeterministic=$nondet missing=$missing unengaged=${unengaged:- none}" \
     "carve_out=${CARVE:-none} verdict=$verdict"
exit $rc
