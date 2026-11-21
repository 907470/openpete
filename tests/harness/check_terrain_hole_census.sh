#!/usr/bin/env bash
# check_terrain_hole_census.sh - retained-terrain hole census over
# face-identity captures (OPENPETE_TERRAIN_FACEID=1), per freecam pose.
#
# A face-identity capture paints each pixel with the identity of the terrain
# face covering it (terrain_faceid.frag on the retained cache's colour
# pipelines, linked to the shipped vertex stage's pre-rasterisation library;
# walked faces recoloured on the host), and keeps nothing but terrain. Per
# pose and LOD tier the default mode captures, on one build and device:
#   p0   the shipped vertex stage, under each --lto arm
#   p1   the positive control (OPENPETE_TERRAIN_SECDELTA=1, per-sector
#        rounding of the camera operand), under each --lto arm
# and tools/terrain_hole_census.py compares, per --lto arm, the control
# against the shipped stage (ctl_px, expected nonzero: the proof the
# census sees a shared-vertex disagreement) and, with --lto=both, the
# fast-linked shipped stage against the LTO one (fast_cov, its holes plus
# reverse holes, expected 0: the position chain's operation order is
# identical in every pipeline; its pxdiff is reported, not judged, since
# the face-identity words of one face differ between the two link modes
# while its coverage does not). pxdiff
# counts pixels whose face identity or coverage differs; the tool's line
# also gives holes (terrain in the base, background in the arm), reverse
# holes, the change in isolated-identity pixels, and identity changes
# interior to faces. --repeat=2 captures every leg twice and fails on a leg
# whose two captures differ. Every cache leg runs with the first-touch bake
# budget off (OPENPETE_TC_BAKE_BUDGET_US=off), so the arena placement, and
# with it each face's identity, is the same in every leg of a pose.
#
# --jitter=J sets OPENPETE_TERRAIN_CAM_JITTER=J on every leg: the terrain
# camera (the CPU walk's, its triangle dump's and the cache's alike) moves
# by a sub-unit offset on each axis, so the camera-relative operand carries
# a fraction. A freecam camera is integral, and with an integral camera the
# shipped combine and the control's per-sector combine are both exact, so
# without it the control cannot engage.
#
# Engagement: a row whose base capture shows no face (faces + walked == 0),
# or whose leg logged no retained sectors (secs > 0 on a [tgpu-cen] line),
# is VACUOUS; its line ends VACUOUS(<reading>). The control's summed pxdiff
# must be nonzero or the run is VACUOUS as a whole (a blind detector, not a
# pass). A leg that ends without its capture is LEG-FAILED (after one retry
# when the launch itself failed), its pairs print MISSING and are not
# compared, and a run with a failed leg reads INCOMPLETE, never PASS.
#
# OPENPETE_BIN=<path to openpete.exe> drives a Windows build instead (a
# hardware device: headless, --input-lock, knobs forwarded through WSLENV).
#
# Usage:
#   tests/harness/check_terrain_hole_census.sh [--game SLUG] [--filter SUBSTR] [--jitter=J]
#       [--lods "original extended"] [--lto=on|off|both] [--repeat=N]
#       [--stride=K] [--jobs N] [--out DIR] [--build-dir=DIR]
#       [--holes [--control] [--classify] [--max-holes=N]] [--resume]
#       [--vprec=subpixel|integer] [--texmap=perspective|affine] [--fog=MODE] [--start=N]
# --stride=K keeps every K-th selected manifest row (the first, the K+1-th,
# ...), a sample of the corpus for a run on a slow device.
#
# --holes counts holes instead of comparing arms: one capture per pose and
# LOD (the shipped stage, or with --control the per-sector control; LTO;
# --jitter as given), with --render-paths terrain,portal,transition so no
# moby, sky, particle or HUD path draws (the terrain feed and the cache's
# claims are those of a full capture). `terrain_hole_census.py holes`
# counts the clear pixels enclosed by terrain: pins (1 px) and holes
# (components up to 16 px). A row whose image holds a pixel that is
# neither the clear colour nor a face identity (foreign > 0) is IMPURE and
# fails the run. With --classify, every row with a pin also runs a walk leg
# on the Linux build (--terrain-cache 0, OPENPETE_TERRAIN_TRIDUMP at the
# capture key), and tools/terrain_hole_classify.py attributes each pin to a
# geometric class (T-junction, crack, walk-covered, shared edge, open edge,
# unattributed). --max-holes=N passes a run with at most N holes (default
# 0).
# --vprec, --texmap (affine sets the AFFINE_UV stages) and --fog set the
# run's configuration (defaults: subpixel, perspective, the game's fog);
# --start=N captures every leg at frame N in place of the manifest's start.
# --resume keeps a leg whose output exists and is newer than the binary
# (and the walk dump's newer than the build's), so an interrupted run
# continues where it stopped; without it every leg's outputs are removed
# before it runs.
#
# Exit 0 when every leg captured and every engaged row reads fast_cov == 0
# (--holes: the summed holes are at most --max-holes and no row is IMPURE),
# 1 when any does not or the run is VACUOUS or INCOMPLETE, 64 SKIP (no
# binary, disc or PIL).
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT="" FILTER="" LODS="original extended" LTO=on REPEAT=1 JOBS=2 JITTER="" STRIDE=1
HOLES=0 HPREC=0 CLASSIFY=0 MAXHOLES=0 RESUME=0 VPREC=subpixel TEXMAP=perspective FOGM="" START=""
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
        --holes)       HOLES=1;;
        --control)     HPREC=1;;
        --classify)    CLASSIFY=1;;
        --max-holes=*) MAXHOLES="${a#*=}";;
        --resume)      RESUME=1;;
        --vprec=*)     VPREC="${a#*=}";;
        --texmap=*)    TEXMAP="${a#*=}";;
        --fog=*)       FOGM="${a#*=}";;
        --start=*)     START="${a#*=}";;
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

leg() {  # tag level pose start lod control(0|1) lto faceid [kind: holes|walk]
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
        # OPENPETE_TC_BAKE_BUDGET_US=off: the face identity hashes a face's
        # element index in its arena buffer, which follows the order sectors
        # are baked; the first-touch budget spreads bakes over walks in an
        # order keyed on view depth, so an arm that moves the camera would
        # place a sector at different elements and repaint every face. With
        # the budget off every sector bakes on its first walk in walk order.
        env+=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_TC_BAKE_BUDGET_US=off)
    fi
    [[ "$prec" != 0 ]] && env+=(OPENPETE_TERRAIN_SECDELTA=1)
    [[ "$lto" == fast ]] && env+=(OPENPETE_PIPELINE_LTO=0)
    [[ "$fid" == 1 ]] && env+=(OPENPETE_TERRAIN_FACEID=1)
    [[ -n "$JITTER" ]] && env+=("OPENPETE_TERRAIN_CAM_JITTER=$JITTER")
    local -a args=(--no-card --mute 1 --vibration 0 --mods ""
        --renderer vk --lod "$lod" --aspect 4:3 --vertex-precision "$VPREC"
        --texture-mapping "$TEXMAP" ${FOGM:+--fog "$FOGM"}
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
        local rc=0
        op_own_run env "${WSLENV_ARGS[@]}" timeout -k 10 900 "$PG_BIN" --headless --input-lock \
            --no-config --disc "$DISC_ABS" --disc-cache 0 "${args[@]}" >"$log" 2>&1 || rc=$?
        [[ -f "$OUT/$tag-vk.png" ]] || return $(( rc ? rc : 1 ))
        return 0
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
    STARTS+=("${START:-${start:-$PG_FREECAM_START}}")
done < "$MAN"

# A leg whose run ends without its capture is retried once when the launch
# itself failed: the WSL interop accept timeout that stops a Windows exe
# before it starts (UtilAcceptVsock ... accept4 failed), or a process that
# exited without writing a log line. A leg still without its capture is
# LEG-FAILED, with the run's exit status and the log's last line; the
# compare step counts it as failed, never as a difference.
leg_log_unlaunched() {  # log
    [[ ! -s "$1" ]] || grep -qE 'UtilAcceptVsock|accept4 failed' "$1"
}
run_line() {
    local tag i lod prec lto fid kind rc=0 retry=""
    IFS='|' read -r tag i lod prec lto fid kind <<<"$1"
    leg "$tag" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$prec" "$lto" "$fid" "$kind" || rc=$?
    if [[ $rc -ne 0 ]] && leg_log_unlaunched "$OUT/$tag.log"; then
        echo "[hole-census] leg=$tag RETRY rc=$rc (launch failed: $(leg_last_line "$OUT/$tag.log"))"
        retry=" retried=1"; rc=0
        leg "$tag" "${LEVELS[$i]}" "${POSES[$i]}" "${STARTS[$i]}" "$lod" "$prec" "$lto" "$fid" "$kind" || rc=$?
    fi
    [[ $rc -eq 0 ]] && return 0
    echo "[hole-census] leg=$tag LEG-FAILED rc=$rc$retry last: $(leg_last_line "$OUT/$tag.log")"
}
leg_last_line() {  # log: its last non-empty line, or a note when there is none
    local l
    l="$(grep -v '^[[:space:]]*$' "$1" 2>/dev/null | tail -1)"
    echo "${l:-<empty log>}"
}
# The output a leg line names, for the failed-leg count after run_legs.
leg_output() {  # leg line
    local tag kind
    IFS='|' read -r tag _ _ _ _ _ kind <<<"$1"
    if [[ "$kind" == walk ]]; then echo "$OUT/$tag.tri"; else echo "$OUT/$tag-vk.png"; fi
}
count_failed() {  # file of leg lines: legs without their output
    local n=0 line
    while IFS= read -r line; do [[ -s "$(leg_output "$line")" ]] || n=$((n + 1)); done < "$1"
    echo "$n"
}
RESUME_NOTE="(--resume reruns only the legs without a capture)"
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
    failed="$(count_failed "$LEGS")"
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
                echo "[hole-census] row=${SLUGS[$i]} lod=$lod MISSING (leg failed)"
                continue
            fi
            fc=$(( $(kv faces) + $(kv walked) ))
            note=""
            if [[ $fc -eq 0 ]]; then
                vac=$((vac + 1)); note=" VACUOUS(faces+walked=0)"
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
    [[ $t_holes -gt $MAXHOLES || $impure -ne 0 ]] && verdict=FAIL
    [[ $vac -eq $rows ]] && verdict=VACUOUS
    [[ $missing -ne 0 && $verdict != FAIL ]] && verdict=INCOMPLETE
    resume=""; [[ $missing -ne 0 ]] && resume=" $RESUME_NOTE"
    summary=""
    for c in $(printf '%s\n' "${!CLS[@]}" | sort); do summary+=" $c=${CLS[$c]}"; done
    echo "[hole-census] game=$PG_GAME mode=holes control=$HPREC rows=$rows vacuous=$vac impure=$impure" \
         "missing=$missing failed=$failed pins=$t_pins holes=$t_holes hole_px=$t_px${summary:+ classes:$summary}" \
         "verdict=$verdict${resume}"
    [[ $verdict == PASS ]]
    exit
fi

# Every leg of the run, one line each, run JOBS at a time.
LEGS="$OUT/legs.txt"; rm -f "${OUT:?}/legs.txt"
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        for lto in $LTOS; do
            for prec in 0 1; do
                for r in $(seq 1 "$REPEAT"); do
                    echo "${SLUGS[$i]}__${lod}__${lto}__p${prec}__r$r|$i|$lod|$prec|$lto|1" >> "$LEGS"
                done
            done
        done
    done
done
[[ -f "$LEGS" ]] || { echo "[hole-census] game=$PG_GAME rows=0 verdict=VACUOUS"; exit 1; }

run_legs "$LEGS"
failed="$(count_failed "$LEGS")"

rows=0 vac=0 bad=0 ctl_px=0 ctl_iso=0 fast_px=0 fast_cov=0 nondet=0
for i in "${!SLUGS[@]}"; do
    for lod in $LODS; do
        lto_base="$OUT/${SLUGS[$i]}__${lod}__lto__p0__r1"
        for lto in $LTOS; do
            base="$OUT/${SLUGS[$i]}__${lod}__${lto}__p0__r1"
            rows=$((rows + 1))
            for r in $(seq 2 "$REPEAT"); do
                for prec in 0 1; do
                    p="$OUT/${SLUGS[$i]}__${lod}__${lto}__p${prec}__r"
                    # A missing capture is a failed leg (LEG-FAILED above),
                    # not a difference between two captures.
                    [[ -f "${p}1-vk.png" && -f "${p}$r-vk.png" ]] || continue
                    if ! cmp -s "${p}1-vk.png" "${p}$r-vk.png"; then
                        echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=p$prec repeat=$r NONDETERMINISTIC"
                        nondet=$((nondet + 1))
                    fi
                done
            done
            secs="$(secs_of "$base.log")"
            # ctl: the control against the shipped stage of the same link
            # mode; fast: the fast-linked shipped stage against the LTO one.
            pairs=("ctl|$base|$OUT/${SLUGS[$i]}__${lod}__${lto}__p1__r1")
            [[ $lto == fast && " $LTOS " == *" lto "* ]] && pairs+=("fast|$lto_base|$base")
            for pr in "${pairs[@]}"; do
                IFS='|' read -r arm_name b a <<<"$pr"
                if [[ ! -f "$b-vk.png" || ! -f "$a-vk.png" ]]; then
                    echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=$arm_name MISSING (leg failed)"
                    continue
                fi
                line="$(python3 "$TOOL" census "$b-vk.png" "$a-vk.png")" || line="TOOL-ERROR"
                [[ "$line" == TOOL-ERROR ]] && { bad=$((bad + 1)); echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=$arm_name TOOL-ERROR"; continue; }
                hs="$(sed -n 's/.* pxdiff=\([0-9]*\) .*/\1/p' <<<"$line")"
                iso="$(sed -n 's/.*iso_d=\(-\?[0-9]*\).*/\1/p' <<<"$line")"
                fc="$(sed -n 's/^faces=\([0-9]*\) walked=\([0-9]*\).*/\1 \2/p' <<<"$line")"
                why=""
                [[ $(( ${fc% *} + ${fc#* } )) -eq 0 ]] && why="faces+walked=0"
                [[ "${secs:-0}" -eq 0 ]] && why="${why:+$why,}secs=0"
                echo "[hole-census] row=${SLUGS[$i]} lod=$lod lto=$lto arm=$arm_name secs=${secs:-0} $line${why:+ VACUOUS($why)}"
                if [[ -n "$why" ]]; then
                    [[ $arm_name == ctl ]] && vac=$((vac + 1)); continue
                fi
                if [[ $arm_name == fast ]]; then
                    cv="$(sed -n 's/.* holes=\([0-9]*\) rholes=\([0-9]*\).*/\1 \2/p' <<<"$line")"
                    cv=$(( ${cv% *} + ${cv#* } ))
                    fast_px=$((fast_px + hs)); fast_cov=$((fast_cov + cv))
                    [[ $cv -ne 0 ]] && bad=$((bad + 1))
                else
                    ctl_px=$((ctl_px + hs)); ctl_iso=$((ctl_iso + iso))
                fi
            done
        done
    done
done
verdict=PASS
[[ $bad -ne 0 || $nondet -ne 0 ]] && verdict=FAIL
[[ $ctl_px -eq 0 || $vac -eq $rows ]] && verdict=VACUOUS
[[ $failed -ne 0 && $verdict != FAIL ]] && verdict=INCOMPLETE
resume=""; [[ $failed -ne 0 ]] && resume=" $RESUME_NOTE"
echo "[hole-census] game=$PG_GAME rows=$rows vacuous=$vac failed=$failed fast_cov=$fast_cov fast_px=$fast_px" \
     "ctl_px=$ctl_px ctl_iso_d=$ctl_iso nondeterministic=$nondet verdict=$verdict$resume"
[[ $verdict == PASS ]]
