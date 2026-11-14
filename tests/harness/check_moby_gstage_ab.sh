#!/usr/bin/env bash
# check_moby_gstage_ab.sh - the moby vertex stage (core/rhi/shaders/
# moby_stage.glsl) against the CPU moby projection, on the same present.
#
# Per freecam pose, LOD tier and phase leg, one headless capture under
# OPENPETE_MOBY_GSTAGE_AB=1 writes <shot>-gstage.bin beside the screenshot
# (core/renderer/native_moby_gstage.c): the records of the scene the dump
# drew, the CPU projection's outputs of every vertex, and two arms of the
# GPU stage drawn through its readback variant (moby_rb.vert) on the same
# inputs, the stage and its negative control (the translation moved 1/64
# output px and 2^-18 in depth at the record's centre depth, and at least
# 2^-19 of its largest component on each axis). tools/moby_gstage_ab.py compare pairs
# them per vertex and tools/moby_gstage_ab.py grade grades the run per class
# (divide-overflow regime, near, saturation, lifted, GPF, animated, lerp,
# mirrored, shell, the shaded pass's records: shaded, billboard, drawscale,
# and the simple models': simple, simplescreen):
# engaged when the control mismatched on the class, blind
# when the class was compared and the control never mismatched, unseen when
# no leg compared it; a manifest row `carve-out | <class> | <reason>`
# excuses a blind or unseen class.
#
# Legs: `canon` captures the canonical present at the row's VBL
# (--screenshot-on-frame); a number A captures the sub-tick re-extract at
# phase A of the same tick (--screenshot-intermediate-on-frame,
# OPENPETE_INTERMEDIATE_ALPHA=A), where the pose lerp runs.
#
# Usage:
#   tests/harness/check_moby_gstage_ab.sh [--game SLUG] [--filter SUBSTR]
#       [--lods "original extended"] [--legs "canon 0.5"] [--rows=R]
#       [--negctl] [--manifest=PATH] [--out DIR] [--build-dir=DIR]
# The rows are games/<game>/corpus/moby-gstage.manifest (level | pose |
# [start VBL] | [LOD tiers, default --lods] | [launch tokens], and carve-out
# rows). A row whose level is bk2:<name> replays games/<game>/bk2/<name>.bk2
# (its .args sidecar names the level) with no pose; a launch token spelled
# KEY=VALUE is an environment entry, any other a command-line argument.
#
# OPENPETE_BIN=<path to openpete.exe> drives a Windows build instead (a
# hardware device: headless, --input-lock, knobs forwarded through WSLENV,
# run from the exe's directory); OPENPETE_DISC_WIN names the disc as the exe
# opens it, else the repo disc's Linux path through the WSL share. --rows=R sets the internal resolution the
# screen deltas are measured at (R rows; default 8640, 2160 rows at 4x
# supersampling).
#
# --negctl draws the control in the stage arm too
# (OPENPETE_MOBY_GSTAGE_AB_NEGCTL=1): the run then passes only when every
# leg's compare fails, the proof that the comparison sees a 1/64 px offset.
#
# Exit 0 PASS: every leg's stage arm agrees within the bounds, and every
# class is engaged or carved out. 1 FAIL: a stage mismatch or a missing
# capture (under --negctl: a leg the control did not fail). 2 VACUOUS: no
# row, a leg that compared nothing, or a blind or unseen class without a
# carve-out. 64 SKIP (no binary, disc, manifest or numpy).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT="" FILTER="" LODS="original extended" LEGS_SPEC="canon 0.5" ROWS=8640 MAN="" NEGCTL=0
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --lods=*)      LODS="${a#*=}";;
        --legs=*)      LEGS_SPEC="${a#*=}";;
        --rows=*)      ROWS="${a#*=}";;
        --manifest=*)  MAN="${a#*=}";;
        --negctl)      NEGCTL=1;;
    esac
    case "$prev" in
        --out) OUT="$a";; --filter) FILTER="$a";; --lods) LODS="$a";; --legs) LEGS_SPEC="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
pg_cap_settings
[[ -n "$MAN" ]] || MAN="$PG_CORPUS_DIR/moby-gstage.manifest"
[[ -f "$MAN" ]] || { echo "gstage: SKIP (no manifest at $MAN)"; exit 64; }
[[ -x "$PG_BIN"  ]] || { echo "gstage: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "gstage: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import numpy' 2>/dev/null || { echo "gstage: SKIP (python3 or numpy missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$BUILD_DIR/moby-gstage/$PG_GAME"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
PG_BK2_ABS="$(pwd -P)/games/$PG_GAME/bk2"
DISC_ABS="$(cd "$(dirname "$PG_DISC")" && pwd -P)/$(basename "$PG_DISC")"
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh
TOOL=tools/moby_gstage_ab.py

leg() {  # tag level pose start lod phase(canon|A) launch
    local tag="$1" level="$2" pose="$3" start="$4" lod="$5" phase="$6" launch="$7"
    local -a PG_SKIP_ARGS=() src=() extra_env=() extra_args=() tok
    if [[ "$level" == bk2:* ]]; then
        # A recording row: the movie's .args sidecar carries its level.
        src=(--bk2 "$PG_BK2_ABS/${level#bk2:}.bk2")
    else
        pg_skip_args "$level"
        src=("${PG_SKIP_ARGS[@]}" --freecam-pose "$pose")
    fi
    for tok in $launch; do
        if [[ "$tok" =~ ^[A-Z_][A-Z0-9_]*= ]]; then extra_env+=("$tok"); else extra_args+=("$tok"); fi
    done
    rm -f "${OUT:?}/$tag.png" "${OUT:?}/$tag-vk.png" "${OUT:?}/$tag-gstage.bin"
    local -a env=(OPENPETE_MOBY_GSTAGE_AB=1 "OPENPETE_MOBY_GSTAGE_AB_NEGCTL=$NEGCTL" "${extra_env[@]}")
    local -a shot=(--screenshot-on-frame "$start")
    if [[ "$phase" != canon ]]; then
        env+=("OPENPETE_INTERMEDIATE_ALPHA=$phase")
        shot=(--screenshot-intermediate-on-frame "$start")
    fi
    local -a args=(--no-card --mute 1 --vibration 0 --mods "" --renderer vk --lod "$lod" --aspect 4:3
        --vertex-precision subpixel --texture-mapping perspective
        "${src[@]}" "${extra_args[@]}"
        --screenshot-native-only "${shot[@]}"
        --screenshot-output "$OUT/$tag.png"
        --frame-limit $((start + 20)) --max-runtime-seconds 840)
    if [[ "$PG_BIN" == *.exe ]]; then
        # A Windows exe (OPENPETE_BIN): headless on the real GPU, knobs
        # forwarded through WSLENV (scripts/lib/wslenv.sh), the disc from
        # OPENPETE_DISC_WIN when set.
        local disc="${OPENPETE_DISC_WIN:-$DISC_ABS}"
        rm -f "${OUT:?}/$tag.log"
        wslenv_forward OPENPETE_VIBRATION=0 "${env[@]}"
        (cd "$(dirname "$PG_BIN")" && op_own_run env "${WSLENV_ARGS[@]}" timeout -k 10 900 \
            "./$(basename "$PG_BIN")" --headless --input-lock --no-config --disc "$disc" \
            --disc-cache 0 "${PG_RUN_ARGS[@]}" "${args[@]}") >"$OUT/$tag.log" 2>&1 </dev/null
        [[ -f "$OUT/$tag-gstage.bin" ]]
        return
    fi
    PG_CAPTURE_ENV=("${env[@]}")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$OUT/$tag.log" "$OUT/$tag-gstage.bin" -- "${args[@]}"
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
LEVELS=(); POSES=(); SLUGS=(); STARTS=(); ROWLODS=(); LAUNCH=(); CARVES=()
while IFS='|' read -r name pose start rlods launch _; do
    name="$(echo "$name" | tr -d ' \t')"
    [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
    if [[ "$name" == carve-out ]]; then
        CARVES+=(--carve "$(echo "$pose" | tr -d ' \t')=$(echo "$start" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')")
        continue
    fi
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [[ -z "$pose" && "$name" != bk2:* ]] && continue
    start="$(echo "$start" | tr -d ' \t')"
    slug="$(pg_pose_slug "${name/:/-}" "$pose")"
    pg_row_selected "$FILTER" "$name" "$slug" || continue
    LEVELS+=("${name%%@*}"); POSES+=("$pose"); SLUGS+=("$slug")
    STARTS+=("${start:-$PG_FREECAM_START}")
    rlods="$(echo "${rlods:-}" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    ROWLODS+=("${rlods:-$LODS}")
    LAUNCH+=("$(echo "${launch:-}" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')")
done < "$MAN"
echo "[gstage] game=$PG_GAME rows=${#SLUGS[@]} manifest=$MAN negctl=$NEGCTL out=$OUT"
[[ ${#SLUGS[@]} -gt 0 ]] || { echo "[gstage] game=$PG_GAME rows=0 verdict=VACUOUS"; exit 2; }

CMPS=()
missing=0 legs=0 failed=0 vacuous=0
for i in "${!SLUGS[@]}"; do
    for lod in ${ROWLODS[$i]}; do
        for phase in $LEGS_SPEC; do
            legs=$((legs + 1))
            tag="${SLUGS[$i]}__${lod}__${phase}"
            if ! leg "$tag" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$phase" "${LAUNCH[$i]}"; then
                echo "[gstage] leg=$tag NO-CAPTURE (log $OUT/$tag.log)"
                missing=$((missing + 1)); continue
            fi
            rm -f "${OUT:?}/$tag.cmp"
            python3 "$TOOL" compare "$OUT/$tag-gstage.bin" --rows "$ROWS" --tag "$tag" > "$OUT/$tag.cmp"
            rc=$?
            [[ $rc -eq 1 ]] && failed=$((failed + 1))
            [[ $rc -eq 2 ]] && vacuous=$((vacuous + 1))
            sed "s/^/[gstage] /" "$OUT/$tag.cmp" | head -1
            CMPS+=("$OUT/$tag.cmp")
        done
    done
done

if [[ $NEGCTL == 1 ]]; then
    # Every leg must fail: the control's 1/64 px is visible to the compare.
    verdict=PASS rc=0
    if [[ $missing -ne 0 || $failed -ne $legs ]]; then verdict=FAIL rc=1; fi
    echo "[gstage] game=$PG_GAME negctl legs=$legs failed=$failed vacuous=$vacuous missing=$missing verdict=$verdict"
    exit $rc
fi

grade="$(python3 "$TOOL" grade "${CARVES[@]}" "${CMPS[@]}")"
grc=$?
echo "$grade"
verdict=PASS rc=0
if [[ $grc -eq 1 || $missing -ne 0 || $failed -ne 0 ]]; then
    verdict=FAIL rc=1
elif [[ $grc -ne 0 || $vacuous -ne 0 ]]; then
    verdict=VACUOUS rc=2
fi
echo "[gstage] game=$PG_GAME legs=$legs failed=$failed vacuous=$vacuous missing=$missing verdict=$verdict"
exit $rc
