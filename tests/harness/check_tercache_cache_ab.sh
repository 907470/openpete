#!/usr/bin/env bash
# check_tercache_cache_ab.sh - retained-terrain GPU cache vs CPU terrain walk,
# whole-frame A/B over a bk2 camera swing, windowed on a real GPU.
#
# The retained cache draws a static sector from a persistent vertex buffer
# and re-derives per-face decisions (LOD tier, near/HQ subdivision, far cull)
# in the vertex stage from sector parameters; the CPU walk takes the same
# decisions per face on the host. The two must compose the same scene. Where
# the vertex stage takes a decision the walk does not, the two feeds draw a
# sector differently, and a sector that changes feed between presents changes
# appearance in place.
#
# Software vk never draws the retained path, so the headless gates cannot
# execute it; this check runs windowed on the machine's own GPU.
#
# Instrument: four legs of one build on one device over the same bk2 replay,
# --render-fps 0 (one present per tick, no sub-tick interpolation: a
# free-running pair differs by sub-tick alpha alone), a screenshot at every
# sampled VBL of the window:
#   ext-on    --lod extended  (default cache)
#   ext-off   --lod extended  --terrain-cache 0
#   org-on    --lod original  (default cache)
#   org-off   --lod original  --terrain-cache 0
# Per sampled VBL, D_ext = pixels ext-on and ext-off render differently and
# D_org = the same for the original-mode pair. A pixel differs when any
# channel differs by more than TOL (0: any difference; both legs of a pair run
# the same fragment path on the same device, so equal inputs give equal
# bytes).
#
# The original-mode pair is the in-run control. It carries every difference
# between the two feeds that is not LOD-mode-dependent (edge rasterisation,
# replay-to-VBL phase), measured on the same build, device and VBLs, so the
# extended-mode criterion is relative to it and needs no absolute floor.
#
# Pass criterion: D_ext <= D_org + MARGIN at every sampled VBL.
# Engagement: within the window, each cache-on leg must report retained
# sectors (secs > 0 on its [tgpu-cen] lines), each cache-off leg must report
# none (no [tgpu-cen] line with secs > 0) and must report terrain triangles
# on its [nearcen] lines, and the two cache-off legs must differ at one
# sampled VBL or more (the LOD mode reached the renderer). A run that misses
# any of these compared nothing
# about the cache and is reported VACUOUS, never PASS.
#
# Usage:
#   tests/harness/check_tercache_cache_ab.sh [--game SLUG] [--exe PATH]
#                                      [--build-dir=DIR] [--out DIR]
#   --exe     the executable to drive (default: the windows-mingw build)
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no display, no exe, no disc, no
# bk2, no python3/PIL, or the game has no measured swing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

WIN_BUILD_DIR="build/windows-mingw"
EXE=""
OUT=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) WIN_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
        --exe=*)       EXE="${a#--exe=}";;
    esac
done
prev=""
for a in "$@"; do
    case "$prev" in
        --exe) EXE="$a";;
        --out) OUT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: the bk2 and the VBL window its camera swings terrain across the
# near/HQ band in. A tick is two VBLs, so the window is sampled every second
# VBL.
case "$GAME" in
  spyro-3) BK2="sunrise-spring-camera-swing.bk2"; VBL_LO=2473; VBL_HI=2553; VBL_STEP=2;;
  *) echo "tercache-ab: SKIP ($GAME has no measured camera swing)"; exit 64;;
esac

WIDTH=664
HEIGHT=448
# Per-channel tolerance of the pixel compare and the margin of the extended
# pair over the control pair.
TOL=0
MARGIN=16
LEG_RUNTIME=600
LEG_TIMEOUT=660

[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "tercache-ab: SKIP (no display)"; exit 64; }
[[ -n "$EXE" ]] || EXE="$WIN_BUILD_DIR/games/$GAME/$(pg_bin_name "$GAME").exe"
[[ -x "$EXE" ]] || { echo "tercache-ab: SKIP (no exe at $EXE)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-ab: SKIP (no disc at $PG_DISC)"; exit 64; }
MOVIE="games/$GAME/bk2/$BK2"
[[ -f "$MOVIE" ]] || { echo "tercache-ab: SKIP (no bk2 at $MOVIE)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "tercache-ab: SKIP (python3 or PIL missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$WIN_BUILD_DIR/tercache-ab/$GAME"
mkdir -p "$OUT"

# A Windows child resolves a path against the Windows view of the filesystem
# and follows no link in it, so every path handed to a .exe is converted and
# resolved to its link-free absolute form first.
IS_EXE=0
case "$EXE" in *.exe) IS_EXE=1;; esac
hostpath() {
    if [[ "$IS_EXE" == 1 ]]; then wslpath -w "$1"; else echo "$1"; fi
}
abspath() { echo "$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"; }

DISC_ARG="$(hostpath "$(abspath "$PG_DISC")")"
MOVIE_ARG="$(hostpath "$(abspath "$MOVIE")")"
OUT_ABS="$(cd "$OUT" && pwd -P)"

# The bk2's recorded launch arguments (its level), one word per token.
MOVIE_ARGS=()
if [[ -f "$MOVIE.args" ]]; then
    while read -r line; do
        [[ -z "$line" || "${line:0:1}" == "#" ]] && continue
        # shellcheck disable=SC2206
        MOVIE_ARGS+=($line)
    done < "$MOVIE.args"
fi

VBLS=()
for ((v = VBL_LO; v <= VBL_HI; v += VBL_STEP)); do VBLS+=("$v"); done
VBL_LIST="$(IFS=,; echo "${VBLS[*]}")"

# One leg. Its log and every PNG it can write are deleted before the run and
# asserted after it, so a file from an earlier build is never read as this
# run's result. The capture is a readback of the presented frame, chrome
# included, so the stats block and the notifications are switched off.
run_leg() {  # tag extra-args...
    local tag="$1"; shift
    local log="$OUT_ABS/$tag.log"
    local attempt=1 rc v ok
    while :; do
        rm -f "$log"
        for v in "${VBLS[@]}"; do rm -f "$OUT_ABS/${tag}__$v.png" "$OUT_ABS/${tag}__$v-vk.png"; done
        wslenv_forward OPENPETE_VIBRATION=0 \
                       OPENPETE_TERRAIN_GPU_CENSUS=1 \
                       OPENPETE_TERRAIN_NEARCEN=1
        timeout "$LEG_TIMEOUT" env "${WSLENV_ARGS[@]}" "$EXE" \
            --disc "$DISC_ARG" --no-config --no-card --mute 1 \
            --vibration 0 --mouse 0 --input-lock --perf-stats off --osd off \
            --renderer vk --window "${WIDTH}x${HEIGHT}" --vsync 0 --render-fps 0 \
            "${MOVIE_ARGS[@]}" --bk2 "$MOVIE_ARG" \
            --screenshot-native-only --screenshot-on-frame "$VBL_LIST" \
            --screenshot-output "$(hostpath "$OUT_ABS/${tag}__{n}.png")" \
            --frame-limit $((VBL_HI + 20)) --max-runtime-seconds "$LEG_RUNTIME" \
            "$@" < /dev/null > "$log" 2>&1
        rc=$?
        tr -d '\r' < "$log" > "$log.txt" && mv "$log.txt" "$log"
        ok=1
        for v in "${VBLS[@]}"; do [[ -f "$OUT_ABS/${tag}__$v-vk.png" ]] || ok=0; done
        [[ "$ok" == 1 ]] && return 0
        # A windowed launch started while the previous run is still releasing
        # the display sometimes never brings its window up and writes nothing.
        # That is a launch failure, not a reading, so it is retried once. A
        # leg that produced its images is never retried.
        if [[ "$attempt" -ge 2 ]]; then
            echo "tercache-ab: leg $tag wrote an incomplete capture set in 2 attempts (rc=$rc)"
            return 1
        fi
        attempt=2
        sleep 20
    done
}

rm -f "$OUT_ABS/verdict.txt"
run_leg ext-on  --lod extended                    || exit 1; sleep 5
run_leg ext-off --lod extended --terrain-cache 0  || exit 1; sleep 5
run_leg org-on  --lod original                    || exit 1; sleep 5
run_leg org-off --lod original --terrain-cache 0  || exit 1

python3 - "$OUT_ABS" "$TOL" "$MARGIN" "$VBL_LO" "$VBL_HI" "${VBLS[@]}" > "$OUT_ABS/verdict.txt" <<'AB_PY'
import re, sys
from PIL import Image, ImageChops

out = sys.argv[1]
tol, margin, lo, hi = (int(x) for x in sys.argv[2:6])
vbls = [int(x) for x in sys.argv[6:]]

cen_re = re.compile(r"\[tgpu-cen f=(\d+)\] sub=\d+ secs=(\d+)")
tri_re = re.compile(r"\[nearcen\] frame=(\d+) nearFaces=\d+ subTris=\d+ terrainTris=(\d+)")

def window_max(tag, pat):
    best = 0
    for line in open("%s/%s.log" % (out, tag), errors="replace"):
        m = pat.search(line)
        if m and lo <= int(m.group(1)) <= hi:
            best = max(best, int(m.group(2)))
    return best

def diff_px(a_tag, b_tag, v):
    a = Image.open("%s/%s__%d-vk.png" % (out, a_tag, v)).convert("RGB")
    b = Image.open("%s/%s__%d-vk.png" % (out, b_tag, v)).convert("RGB")
    if a.size != b.size:
        return -1
    d = ImageChops.difference(a, b)
    r, g, bl = d.split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), bl).point(lambda p: 255 if p > tol else 0)
    return m.histogram()[255]

reasons = []
for tag in ("ext-on", "ext-off", "org-on", "org-off"):
    secs = window_max(tag, cen_re)
    tris = window_max(tag, tri_re)
    print("[tercache-ab] engagement leg=%s retainedSecs=%d terrainTris=%d" % (tag, secs, tris))
    if tag.endswith("-on"):
        if secs == 0:
            reasons.append("%s retained secs=0" % tag)
    else:
        if secs != 0:
            reasons.append("%s retained secs=%d, expected 0" % (tag, secs))
        if tris == 0:
            reasons.append("%s terrainTris=0" % tag)
if reasons:
    print("[tercache-ab] verdict=VACUOUS reason=%s" % "; ".join(reasons))
    sys.exit(3)

# The two LOD modes must render differently somewhere in the window;
# otherwise --lod did not reach the renderer and the control pair is the
# extended pair under another name.
mode_px = max(diff_px("ext-off", "org-off", v) for v in vbls)
print("[tercache-ab] engagement lodModeMaxPx=%d" % mode_px)
if mode_px <= 0:
    print("[tercache-ab] verdict=VACUOUS reason=extended and original walk legs render identically at every sampled VBL")
    sys.exit(3)

failed = 0
worst = (0, 0, 0)
for v in vbls:
    de = diff_px("ext-on", "ext-off", v)
    do = diff_px("org-on", "org-off", v)
    bad = de < 0 or do < 0 or de > do + margin
    failed += bad
    if de - do > worst[1] - worst[2] or worst[0] == 0:
        worst = (v, de, do)
    print("[tercache-ab] vbl=%d extPx=%d orgPx=%d %s" % (v, de, do, "FAIL" if bad else "ok"))
print("[tercache-ab] samples=%d failed=%d worst vbl=%d extPx=%d orgPx=%d margin=%d verdict=%s"
      % (len(vbls), failed, worst[0], worst[1], worst[2], margin, "FAIL" if failed else "PASS"))
sys.exit(1 if failed else 0)
AB_PY
rc=$?
[[ -s "$OUT_ABS/verdict.txt" ]] || { echo "tercache-ab: the compare wrote no verdict"; exit 1; }
cat "$OUT_ABS/verdict.txt"
[[ "$rc" -eq 0 ]] && exit 0
exit 1
