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
# --jitter=J sets OPENPETE_TERRAIN_CAM_JITTER=J on every leg: the terrain
# camera (the CPU walk's, its triangle dump's and the cache's alike) moves
# by a sub-unit offset on each axis, so the camera-relative operand carries
# a fraction. A freecam camera is integral, and with an integral camera the
# fp32 combine and the control's per-sector combine are both exact, so
# without it the control cannot engage.
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
#       [--holes [--prec=N] [--classify] [--max-holes=N]] [--resume]
# --stride=K keeps every K-th selected manifest row (the first, the K+1-th,
# ...), a sample of the corpus for a run on a slow device.
#
# --holes counts holes instead of comparing arms: one capture per pose and
# LOD (the fp64 stage, or the precision --prec=N names; LTO; --jitter as
# given), with --render-paths terrain,portal,transition so no moby, sky,
# particle or HUD path draws (the terrain feed and the cache's claims are
# those of a full capture). `terrain_hole_census.py holes` counts the clear
# pixels enclosed by terrain: pins (1 px) and holes (components up to
# 16 px). A row whose image holds a pixel that is neither the clear colour
# nor a face identity (foreign > 0) is IMPURE and fails the run. With
# --classify, every row with a pin also runs a walk leg on the Linux build
# (--terrain-cache 0, OPENPETE_TERRAIN_TRIDUMP at the capture key), and
# tools/terrain_hole_classify.py attributes each pin to a geometric class
# (T-junction, crack, walk-covered, shared edge, open edge, unattributed).
# --max-holes=N passes a run with at most N holes (default 0).
# --resume keeps a leg whose output exists and is newer than the binary
# (and the walk dump's newer than the build's), so an interrupted run
# continues where it stopped; without it every leg's outputs are removed
# before it runs.
#
# Exit 0 when every engaged row reads pxdiff == 0 for f32 (--holes: the
# summed holes are at most --max-holes and no row is IMPURE), 1 when any
# does not or the run is VACUOUS, 64 SKIP (no binary, disc or PIL).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT="" FILTER="" LODS="original extended" LTO=on REPEAT=1 COLOUR=0 JOBS=2 JITTER="" STRIDE=1
HOLES=0 HPREC=0 CLASSIFY=0 MAXHOLES=0 RESUME=0
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
        --holes)       HOLES=1;;
        --prec=*)      HPREC="${a#*=}";;
        --classify)    CLASSIFY=1;;
        --max-holes=*) MAXHOLES="${a#*=}";;
        --resume)      RESUME=1;;
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

# The walk's triangle dump is CPU geometry, the same on every device: the
# walk legs of --classify run the Linux build even when OPENPETE_BIN names a
# Windows exe.
LINUX_BIN="$(pg_tree_bin "$BUILD_DIR" "$PG_GAME")"

leg() {  # tag level pose start lod precision lto faceid [kind: holes|walk]
    local tag="$1" level="$2" pose="$3" start="$4" lod="$5" prec="$6" lto="$7" fid="$8"
    local kind="${9:-}"
    local -a PG_SKIP_ARGS=()
    pg_skip_args "$level"
    local log="$OUT/$tag.log" shot="$OUT/$tag.png" out="$OUT/$tag-vk.png" bin="$PG_BIN"
    [[ "$kind" == walk ]] && out="$OUT/$tag.tri" bin="$LINUX_BIN"
    [[ $RESUME == 1 && -s "$out" && "$out" -nt "$bin" ]] && return 0
    rm -f "${OUT:?}/$tag.png" "${OUT:?}/$tag-vk.png" "${OUT:?}/$tag.tri"
    local -a env=(OPENPETE_TERRAIN_GPU_CENSUS=1)
    if [[ "$kind" == walk ]]; then
        env+=("OPENPETE_TERRAIN_TRIDUMP=$start,$out")
    else
        env+=(OPENPETE_TERRAIN_CACHE_FORCE=1)
    fi
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
    [[ "$kind" == holes ]] && args+=(--render-paths terrain,portal,transition)
    [[ "$kind" == walk ]] && args+=(--terrain-cache 0)
    if [[ "$kind" == walk ]]; then
        local -a PG_CAPTURE_ENV=("${env[@]}")
        local PG_CAPTURE_TIMEOUT=900 PG_BIN="$LINUX_BIN"
        pg_capture "$log" "$out" -- "${args[@]}"
        return
    fi
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

run_line() {
    local tag i lod prec lto fid kind
    IFS='|' read -r tag i lod prec lto fid kind <<<"$1"
    leg "$tag" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$prec" "$lto" "$fid" "$kind" ||
        echo "[hole-census] leg=$tag NO-CAPTURE"
}
run_legs() {  # file of leg lines, run JOBS at a time
    local -a lines
    mapfile -t lines < "$1"
    local k j
    for ((k = 0; k < ${#lines[@]}; k += JOBS)); do
        for ((j = k; j < k + JOBS && j < ${#lines[@]}; j++)); do run_line "${lines[$j]}" & done
        wait
    done
}
secs_of() { grep -o 'secs=[0-9]*' "$1" 2>/dev/null | cut -d= -f2 | sort -n | tail -1; }

if [[ $HOLES == 1 ]]; then
    LEGS="$OUT/legs-holes.txt"; rm -f "${OUT:?}/legs-holes.txt"
    for i in "${!SLUGS[@]}"; do
        for lod in $LODS; do
            echo "${SLUGS[$i]}__${lod}__p${HPREC}__holes|$i|$lod|$HPREC|lto|1|holes" >> "$LEGS"
        done
    done
    [[ -f "$LEGS" ]] || { echo "[hole-census] game=$PG_GAME rows=0 verdict=VACUOUS"; exit 1; }
    run_legs "$LEGS"
    WALKS="$OUT/legs-walk.txt"; rm -f "${OUT:?}/legs-walk.txt"
    rows=0 vac=0 impure=0 missing=0 t_pins=0 t_holes=0 t_px=0
    declare -A ROWLINE=()
    kv() { sed -n "s/.*\b$1=\([0-9]*\).*/\1/p" <<<"$line"; }   # a count of $line
    for i in "${!SLUGS[@]}"; do
        for lod in $LODS; do
            tag="${SLUGS[$i]}__${lod}__p${HPREC}__holes"
            rows=$((rows + 1))
            line="$(python3 "$TOOL" holes "$OUT/$tag-vk.png" --list "$OUT/$tag.holes")" || line="MISSING"
            secs="$(secs_of "$OUT/$tag.log")"
            if [[ "$line" == MISSING* ]]; then
                missing=$((missing + 1))
                echo "[hole-census] row=${SLUGS[$i]} lod=$lod MISSING"
                continue
            fi
            fc=$(( $(kv faces) + $(kv walked) ))
            note=""
            if [[ $fc -eq 0 ]]; then
                vac=$((vac + 1)); note=" VACUOUS"
            elif [[ $(kv foreign) -ne 0 ]]; then
                impure=$((impure + 1)); note=" IMPURE"
            else
                t_pins=$((t_pins + $(kv pins))); t_holes=$((t_holes + $(kv holes)))
                t_px=$((t_px + $(kv hole_px)))
            fi
            ROWLINE["$tag"]="row=${SLUGS[$i]} lod=$lod secs=${secs:-0} $line$note"
            [[ $CLASSIFY == 1 && -z "$note" && $(kv pins) -gt 0 ]] &&
                echo "${SLUGS[$i]}__${lod}__walk|$i|$lod|0|lto|0|walk" >> "$WALKS"
        done
    done
    declare -A CLS=()
    if [[ $CLASSIFY == 1 && -f "$WALKS" ]]; then
        if [[ -x "$LINUX_BIN" ]]; then
            run_legs "$WALKS"
        else
            echo "[hole-census] classify: no Linux build at $LINUX_BIN; pins stay unclassified"
        fi
    fi
    for i in "${!SLUGS[@]}"; do
        for lod in $LODS; do
            tag="${SLUGS[$i]}__${lod}__p${HPREC}__holes"
            [[ -n "${ROWLINE[$tag]+set}" ]] || continue
            cls=""
            tri="$OUT/${SLUGS[$i]}__${lod}__walk.tri"
            if [[ $CLASSIFY == 1 && -s "$tri" ]]; then
                cls="$(python3 tools/terrain_hole_classify.py "$tri" "$OUT/$tag.holes" \
                       --out "$OUT/$tag.classes" | tail -1)"
                [[ "$cls" == T=* ]] || cls="UNCLASSIFIED($cls)"
                [[ "$cls" == T=* ]] && for kvp in $cls; do
                    CLS["${kvp%%=*}"]=$(( ${CLS["${kvp%%=*}"]:-0} + ${kvp#*=} ))
                done
                cls=" classes: $cls"
            fi
            echo "[hole-census] ${ROWLINE[$tag]}$cls"
        done
    done
    verdict=PASS
    [[ $t_holes -gt $MAXHOLES || $impure -ne 0 || $missing -ne 0 ]] && verdict=FAIL
    [[ $vac -eq $rows ]] && verdict=VACUOUS
    summary=""
    for c in $(printf '%s\n' "${!CLS[@]}" | sort); do summary+=" $c=${CLS[$c]}"; done
    echo "[hole-census] game=$PG_GAME mode=holes prec=$HPREC rows=$rows vacuous=$vac impure=$impure" \
         "missing=$missing pins=$t_pins holes=$t_holes hole_px=$t_px${summary:+ classes:$summary}" \
         "verdict=$verdict"
    [[ $verdict == PASS ]]
    exit
fi

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

run_legs "$LEGS"

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
