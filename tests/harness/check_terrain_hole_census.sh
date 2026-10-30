#!/usr/bin/env bash
# check_terrain_hole_census.sh - retained-terrain hole census: the fp32
# vertex stage's coverage against the fp64 stage's, per freecam pose, from
# face-identity captures (OPENPETE_TERRAIN_FACEID=1).
#
# A face-identity capture paints each pixel with the identity of the terrain
# face covering it (terrain_faceid.frag on the retained cache's colour
# pipelines, linked to the shipped vertex stage's pre-rasterisation library;
# walked faces recoloured on the host), and keeps nothing but terrain. Per
# pose and LOD tier it captures three arms on one build and device:
#   f64  the fp64 stage (OPENPETE_TERRAIN_FP32 unset)
#   f32  the fp32 stage (OPENPETE_TERRAIN_FP32=1)
#   ctl  the positive control (OPENPETE_TERRAIN_FP32=2, per-sector rounding)
# and tools/terrain_hole_census.py compares f32 and ctl against f64: pxdiff
# (pixels whose face identity or coverage differs; the summed count below),
# holes (terrain in f64, background in the arm), reverse holes, the change
# in isolated-identity pixels, and identity changes interior to faces.
# With --lto=both each arm also runs under OPENPETE_PIPELINE_LTO=0, the
# fast-linked pipelines; --repeat=2 captures every leg twice and reports a
# leg whose two captures differ. With --colour each pose also captures the
# stock image of f64 and f32 and splits their pixel difference into edge and
# face pixels (terrain_hole_census.py split).
#
# --jitter=J sets OPENPETE_TERRAIN_CAM_JITTER=J on every leg: the cache's
# camera moves by a sub-unit offset on each axis, so the camera-relative
# operand carries a fraction. A freecam camera is integral, and with an
# integral camera the fp32 combine and the control's per-sector combine
# are both exact, so without it the control cannot engage.
#
# Engagement: a row whose f64 capture shows no face (faces + walked == 0),
# or whose leg logged no retained sectors (secs > 0 on a [tgpu-cen] line),
# is VACUOUS. The control's summed holes must be nonzero or the run is
# VACUOUS as a whole (a blind detector, not a pass). Holes, pxdiff and the
# control are counted in pxdiff.
#
# OPENPETE_BIN=<path to openpete.exe> drives a Windows build instead (a
# hardware device: headless, --input-lock, knobs forwarded through WSLENV).
#
# Usage:
#   tests/harness/check_terrain_hole_census.sh [--game SLUG] [--filter SUBSTR] [--jitter=J]
#       [--lods "original extended"] [--lto=on|off|both] [--repeat=N]
#       [--colour] [--stride=K] [--jobs N] [--out DIR] [--build-dir=DIR]
# --stride=K keeps every K-th selected manifest row (the first, the K+1-th,
# ...), a sample of the corpus for a run on a slow device.
# Exit 0 when every engaged row reads pxdiff == 0 for f32, 1 when
# any does not or the run is VACUOUS, 64 SKIP (no binary, disc or PIL).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT="" FILTER="" LODS="original extended" LTO=on REPEAT=1 COLOUR=0 JOBS=2 JITTER="" STRIDE=1
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --lods=*)      LODS="${a#*=}";;
        --lto=*)       LTO="${a#*=}";;
        --repeat=*)    REPEAT="${a#*=}";;
        --jobs=*)      JOBS="${a#*=}";;
        --jitter=*)    JITTER="${a#*=}";;
        --stride=*)    STRIDE="${a#*=}";;
        --colour)      COLOUR=1;;
    esac
    case "$prev" in
        --out) OUT="$a";; --filter) FILTER="$a";; --lods) LODS="$a";;
        --jobs) JOBS="$a";;
    esac
    prev="$a"
done
case "$LTO" in on) LTOS="lto";; off) LTOS="fast";; both) LTOS="lto fast";;
    *) echo "hole-census: --lto must be on|off|both" >&2; exit 64;; esac

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
pg_cap_settings
MAN="$PG_FREECAM_MANIFEST"
pg_manifest "$MAN" pipe
[[ -x "$PG_BIN"  ]] || { echo "hole-census: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "hole-census: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null || { echo "hole-census: SKIP (python3 or PIL missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$BUILD_DIR/hole-census/$PG_GAME"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
DISC_ABS="$(cd "$(dirname "$PG_DISC")" && pwd -P)/$(basename "$PG_DISC")"
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh
TOOL=tools/terrain_hole_census.py

leg() {  # tag level pose start lod precision lto faceid
    local tag="$1" level="$2" pose="$3" start="$4" lod="$5" prec="$6" lto="$7" fid="$8"
    local -a PG_SKIP_ARGS=()
    pg_skip_args "$level"
    local log="$OUT/$tag.log" shot="$OUT/$tag.png"
    rm -f "$shot" "$OUT/$tag-vk.png"
    local -a env=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_TERRAIN_GPU_CENSUS=1)
    [[ "$prec" != 0 ]] && env+=("OPENPETE_TERRAIN_FP32=$prec")
    [[ "$lto" == fast ]] && env+=(OPENPETE_PIPELINE_LTO=0)
    [[ "$fid" == 1 ]] && env+=(OPENPETE_TERRAIN_FACEID=1)
    [[ -n "$JITTER" ]] && env+=("OPENPETE_TERRAIN_CAM_JITTER=$JITTER")
    local -a args=(--no-card --mute 1 --vibration 0 --mods ""
        --renderer vk --lod "$lod" --aspect 4:3 --vertex-precision subpixel
        "${PG_SKIP_ARGS[@]}" --freecam-pose "$pose"
        --screenshot-native-only --screenshot-on-frame "$start"
        --screenshot-output "$shot"
        --frame-limit $((start + 20)) --max-runtime-seconds 840)
    if [[ "$PG_BIN" == *.exe ]]; then
        # A Windows exe (OPENPETE_BIN): the knobs cross WSL only through
        # WSLENV (scripts/lib/wslenv.sh); the run is headless and takes no
        # input.
        wslenv_forward OPENPETE_VIBRATION=0 "${env[@]}"
        op_own_run env "${WSLENV_ARGS[@]}" timeout -k 10 900 "$PG_BIN" --headless --input-lock \
            --no-config --disc "$DISC_ABS" --disc-cache 0 "${args[@]}" >"$log" 2>&1
        [[ -f "$OUT/$tag-vk.png" ]]
        return
    fi
    PG_CAPTURE_ENV=("${env[@]}")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$OUT/$tag-vk.png" -- "${args[@]}"
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

# Every leg of the run, one line each, run JOBS at a time.
LEGS="$OUT/legs.txt"; rm -f "$LEGS"
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        for lto in $LTOS; do
            for prec in 0 1 2; do
                for r in $(seq 1 "$REPEAT"); do
                    echo "${SLUGS[$i]}__${lod}__${lto}__p${prec}__r$r|$i|$lod|$prec|$lto|1" >> "$LEGS"
                done
            done
        done
        if [[ $COLOUR == 1 ]]; then
            for prec in 0 1; do
                echo "${SLUGS[$i]}__${lod}__lto__p${prec}__col|$i|$lod|$prec|lto|0" >> "$LEGS"
            done
        fi
    done
done
[[ -f "$LEGS" ]] || { echo "[hole-census] game=$PG_GAME rows=0 verdict=VACUOUS"; exit 1; }

run_line() {
    local tag i lod prec lto fid
    IFS='|' read -r tag i lod prec lto fid <<<"$1"
    leg "$tag" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$prec" "$lto" "$fid" ||
        echo "[hole-census] leg=$tag NO-CAPTURE"
}
mapfile -t LINES < "$LEGS"
for ((k = 0; k < ${#LINES[@]}; k += JOBS)); do
    for ((j = k; j < k + JOBS && j < ${#LINES[@]}; j++)); do run_line "${LINES[$j]}" & done
    wait
done

secs_of() { grep -o 'secs=[0-9]*' "$1" 2>/dev/null | cut -d= -f2 | sort -n | tail -1; }

rows=0 vac=0 bad=0 ctl_px=0 ctl_iso=0 f32_px=0 f32_iso=0 nondet=0
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        for lto in $LTOS; do
            base="$OUT/${SLUGS[$i]}__${lod}__${lto}__p0__r1"
            rows=$((rows + 1))
            for r in $(seq 2 "$REPEAT"); do
                for prec in 0 1 2; do
                    p="$OUT/${SLUGS[$i]}__${lod}__${lto}__p${prec}__r"
                    if ! cmp -s "${p}1-vk.png" "${p}$r-vk.png"; then
                        echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=p$prec repeat=$r NONDETERMINISTIC"
                        nondet=$((nondet + 1))
                    fi
                done
            done
            secs="$(secs_of "$base.log")"
            for prec in 1 2; do
                arm="$OUT/${SLUGS[$i]}__${lod}__${lto}__p${prec}__r1"
                line="$(python3 "$TOOL" census "$base-vk.png" "$arm-vk.png")" || line="MISSING"
                arm_name=$([[ $prec == 1 ]] && echo f32 || echo ctl)
                echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=$arm_name secs=${secs:-0} $line"
                [[ "$line" == MISSING* ]] && { bad=$((bad + 1)); continue; }
                hs="$(sed -n 's/.* pxdiff=\([0-9]*\) .*/\1/p' <<<"$line")"
                iso="$(sed -n 's/.*iso_d=\(-\?[0-9]*\).*/\1/p' <<<"$line")"
                fc="$(sed -n 's/^faces=\([0-9]*\) walked=\([0-9]*\).*/\1 \2/p' <<<"$line")"
                if [[ $(( ${fc% *} + ${fc#* } )) -eq 0 || "${secs:-0}" -eq 0 ]]; then
                    [[ $prec == 1 ]] && vac=$((vac + 1)); continue
                fi
                if [[ $prec == 1 ]]; then
                    f32_px=$((f32_px + hs)); f32_iso=$((f32_iso + iso))
                    [[ $hs -ne 0 ]] && bad=$((bad + 1))
                else
                    ctl_px=$((ctl_px + hs)); ctl_iso=$((ctl_iso + iso))
                fi
            done
        done
        if [[ $COLOUR == 1 ]]; then
            c="$OUT/${SLUGS[$i]}__${lod}__lto"
            echo "[hole-census] row=${SLUGS[$i]} lod=$lod split: $(python3 "$TOOL" split \
                "${c}__p0__r1-vk.png" "${c}__p1__r1-vk.png" "${c}__p0__col-vk.png" \
                "${c}__p1__col-vk.png" --components 6 | tr '\n' ';')"
        fi
    done
done
verdict=PASS
[[ $bad -ne 0 ]] && verdict=FAIL
[[ $ctl_px -eq 0 || $vac -eq $rows ]] && verdict=VACUOUS
echo "[hole-census] game=$PG_GAME rows=$rows vacuous=$vac f32_px=$f32_px f32_iso_d=$f32_iso" \
     "ctl_px=$ctl_px ctl_iso_d=$ctl_iso nondeterministic=$nondet verdict=$verdict"
[[ $verdict == PASS ]]
