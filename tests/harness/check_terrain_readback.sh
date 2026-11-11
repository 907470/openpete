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
#   f32  the shipped fp32 stages: expected 0 mismatches
#   ctl  the control (OPENPETE_TERRAIN_SECDELTA=1, terrain.vert): the camera
#        operand of cam_rel and cam_rel_lerp is rounded per sector, so a
#        vertex two sectors share can round apart; a near-plane cut takes
#        its endpoints in reverse order on every other triangle, and a cell
#        node of such a triangle takes its means as a + (b - a) * 0.5, so a
#        cut or node two triangles of one sector share can round apart;
#        expected nonzero on every family pair, the proof the comparison of
#        that pair can fail
# tools/terrain_readback.py pairs the written vertices by readback key and
# counts, per family pair (plain/plain, plain/anim, plain/near, anim/near,
# cell/near, cell/anim, cell/cell, cut/cut, then any other pair present:
# anim/anim, near/near, plain/cell),
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
# The poses are the game's games/<game>/corpus/terrain-readback.manifest
# (name | pose | start, and carve-out rows): rows chosen so that every named
# family pair meets on screen; tools/terrain_readback_poses.py enumerates
# anim/near candidates from a sector dump. --manifest names another freecam manifest; --stride=K keeps
# every K-th selected row.
#
# OPENPETE_BIN=<path to openpete.exe> drives a Windows build instead (a
# hardware device: headless, --input-lock, knobs forwarded through WSLENV;
# the disc and output paths are absolute Linux paths, which the exe opens
# through the WSL share it is started from).
#
# Grading is per family pair (tools/terrain_readback.py grade): a pair is
# engaged when the f32 arm compared it and the control mismatched on it;
# compared under a control that never mismatched it is blind, and a named
# pair no leg compared is unseen. A manifest row
#   carve-out | <pair> | <reason>
# (or --carve-out=PAIR) excuses a blind or unseen pair; the reason is printed
# with the pair's verdict and names the code that makes the control blind.
# The per-pair lines give the poses that compared the pair and those whose
# control mismatched on it.
#
# Exit 0 PASS: every f32 pair agrees, every leg's two captures agree, and
# every pair is engaged or carved out. 1 FAIL: an f32 mismatch, a
# nondeterministic leg, or a missing capture. 2 VACUOUS: no row, or a pair
# blind or unseen without a carve-out. 64 SKIP (no binary, disc or numpy).
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
[[ -n "$MAN" ]] || MAN="$PG_CORPUS_DIR/terrain-readback.manifest"
[[ -f "$MAN" ]] || { echo "readback: SKIP (no manifest at $MAN)"; exit 64; }
[[ -x "$PG_BIN"  ]] || { echo "readback: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "readback: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import numpy' 2>/dev/null || { echo "readback: SKIP (python3 or numpy missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$BUILD_DIR/terrain-readback/$PG_GAME"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
DISC_ABS="$(cd "$(dirname "$PG_DISC")" && pwd -P)/$(basename "$PG_DISC")"
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh
TOOL=tools/terrain_readback.py

leg() {  # tag level pose start lod control(0|1)
    local tag="$1" level="$2" pose="$3" start="$4" lod="$5" prec="$6"
    local -a PG_SKIP_ARGS=()
    pg_skip_args "$level"
    rm -f "${OUT:?}/$tag.png" "${OUT:?}/$tag-vk.png" "${OUT:?}/$tag-readback.bin"
    local -a env=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_TERRAIN_READBACK=1
                  "OPENPETE_TERRAIN_SECDELTA=$prec" "OPENPETE_TERRAIN_CAM_JITTER=$JITTER")
    local -a args=(--no-card --mute 1 --vibration 0 --mods "" --renderer vk --lod "$lod" --aspect 4:3
        --vertex-precision subpixel --texture-mapping perspective
        "${PG_SKIP_ARGS[@]}" --freecam-pose "$pose"
        --screenshot-native-only --screenshot-on-frame "$start"
        --screenshot-output "$OUT/$tag.png"
        --frame-limit $((start + 20)) --max-runtime-seconds 840)
    if [[ "$PG_BIN" == *.exe ]]; then
        # A Windows exe (OPENPETE_BIN): the knobs cross WSL only through
        # WSLENV (scripts/lib/wslenv.sh); the run is headless and takes no
        # input.
        rm -f "${OUT:?}/$tag.log"
        wslenv_forward OPENPETE_VIBRATION=0 "${env[@]}"
        op_own_run env "${WSLENV_ARGS[@]}" timeout -k 10 900 "$PG_BIN" --headless --input-lock \
            --no-config --disc "$DISC_ABS" --disc-cache 0 "${PG_RUN_ARGS[@]}" "${args[@]}" \
            >"$OUT/$tag.log" 2>&1 </dev/null
        [[ -f "$OUT/$tag-readback.bin" ]]
        return
    fi
    PG_CAPTURE_ENV=("${env[@]}")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$OUT/$tag.log" "$OUT/$tag-readback.bin" -- "${args[@]}"
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
LEVELS=(); POSES=(); SLUGS=(); STARTS=(); CARVE_PAIRS=(); CARVE_WHY=(); nrow=0
while IFS='|' read -r name pose start _; do
    name="$(echo "$name" | tr -d ' \t')"
    [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
    if [[ "$name" == carve-out ]]; then
        CARVE_PAIRS+=("$(echo "$pose" | tr -d ' \t')")
        CARVE_WHY+=("$(echo "$start" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')")
        continue
    fi
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

CARVES=()
for c in ${CARVE//,/ }; do CARVES+=(--carve "$c=named by --carve-out"); done
for i in "${!CARVE_PAIRS[@]}"; do CARVES+=(--carve "${CARVE_PAIRS[$i]}=${CARVE_WHY[$i]}"); done
LEGS=()
missing=0 nondet=0 rows=0
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        rows=$((rows + 1))
        for arm in f32 ctl; do
            prec=0; [[ $arm == ctl ]] && prec=1
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
            rm -f "${OUT:?}/$base.pairs"
            python3 "$TOOL" pairs "$OUT/${base}__r1-readback.bin" --detail 3 > "$OUT/$base.pairs"
            head -1 "$OUT/$base.pairs" | sed "s/^/[readback] row=${SLUGS[$i]} lod=$lod arm=$arm $same /"
            grep '^pair=' "$OUT/$base.pairs" | sed "s/^/[readback] row=${SLUGS[$i]} lod=$lod arm=$arm /"
            LEGS+=("${SLUGS[$i]}:$lod:$arm:$OUT/$base.pairs")
        done
    done
done

# Per family pair: a pair is engaged only when the control mismatched on it;
# compared with a control that never mismatched it is blind (VACUOUS) unless
# the manifest carves it out with its reason.
grade="$(python3 "$TOOL" grade --game "$PG_GAME" "${CARVES[@]}" "${LEGS[@]}")"
grc=$?
echo "$grade"
verdict=PASS rc=0
if [[ $grc -eq 1 || $nondet -ne 0 || $missing -ne 0 ]]; then
    verdict=FAIL rc=1
elif [[ $grc -ne 0 ]]; then
    verdict=VACUOUS rc=2
fi
summary="$(tail -n 1 <<<"$grade")"
echo "[readback] game=$PG_GAME rows=$rows nondeterministic=$nondet missing=$missing" \
     "pairs=${summary#*pairs=} verdict=$verdict"
exit $rc
