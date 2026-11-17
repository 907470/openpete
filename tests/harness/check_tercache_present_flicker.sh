#!/usr/bin/env bash
# check_tercache_present_flicker.sh - retained-terrain present-to-present
# toggle census at a static freecam pose, windowed on the real GPU.
#
# Class: with the terrain cache on, a retained face that every input keeps
# identical (camera, sector flags, chunk bytes, globals, push constants)
# drops out, or lands at a wrong position, for one present and is back on
# the next. The census measures it as screen regions that change between
# two consecutive presents while nothing in the scene moves.
#
# Windowed, not headless, and not a ctest: headless runs on llvmpipe, whose
# vertex stage computes the same output on every execution, and emits no
# intermediate presents, so the class is invisible to the deterministic
# gates. Run it by hand, or from a script that has a display and a GPU. The
# display must be the run's alone: one windowed game process on the machine.
#
# Leg: --lod extended --terrain-cache 1 --aspect 16:9 at the pose below,
# presents PRESENTS_START.. (PRESENTS_N of them) captured native-only
# (--screenshot-presents-start/-count), canonical and intermediate alike.
#
# Metric: consecutive-present pairs whose changed pixels (any channel
# differing by more than 32) hold a connected region of at least MIN_REGION
# pixels, outside the masks. A toggle shows as two adjacent pairs (a region
# appears at present p and reverts at p+1).
# Masks (853x448 capture, spyro-1 cliff_town pose):
#   x >= 780, y <= 20    the OSD corner, which redraws text between presents
#   x 113-188, y 163-314 the waterfall, whose texture animation changes it
#                        every tick
#
# Pass: 0 toggle pairs with engagement. Engagement: at least
# MIN_PRESENTS presents captured, at least MIN_INTER of them intermediate,
# and a retained-terrain world draw ([tc] ter.world, OPENPETE_TC_DBG) logged
# before every captured present. FAIL lists the pairs; VACUOUS (exit 1) when
# engagement is short.
#
# Path leg (games with a PATH_SPEC row): the camera flies a freecam path
# (--freecam-path), presents PATH_START.. (PATH_N of them) captured
# native-only. Class: a sub-tick present composes retained terrain of a
# different extraction than its CPU-fed terrain (the canonical tick's
# retained state beside the re-extract's feed), so near terrain lags the
# camera (seams open onto the clear colour) and sectors owned differently
# by the two builds draw in neither (a large hole for a present).
# Metrics, per captured present, on pixels within 2 of the clear colour
# PATH_CLEAR outside the OSD corner (x >= 690, y < 20):
#   thin   clear pixels outside 5x5-opened regions (seams), mean per kind
#   spike  total clear pixels > neighbour median (+-3 presents) + 2000 and
#          > 2 x median + 500 (a hole for a present)
#   tc_src composed presents preceded by a re-extract ([rx-census],
#          OPENPETE_RX_CENSUS) whose retained terrain came from the staged
#          canonical state (tc_src=3, OPENPETE_FREEZE_DBG): the present
#          stamp is not the re-extract's build stamp
# Pass: thin mean <= PATH_THIN_MAX for both kinds, 0 spikes, 0 tc_src=3
# presents after a re-extract. Engagement: at least PATH_MIN_INTER
# intermediate presents captured, and the re-extracts fed terrain: the
# census terrain= triangles (CPU-walked sectors) plus the secs= of each
# sub-tick retained-cache build ([tgpu-cen] sub=1,
# OPENPETE_TERRAIN_GPU_CENSUS) sum to more than 0.
#
# Usage: tests/harness/check_tercache_present_flicker.sh [--game SLUG]
#            [--build-dir=DIR] [--leg=static|path|all]
#   OPENPETE_TC_FLICKER_EXE   the exe to drive (default: the build's
#                             windows-mingw openpete.exe, one executable for
#                             every game, run with --game); a Linux binary
#                             runs windowed on a private Xvfb display
#   OPENPETE_TC_FLICKER_OUT   output directory (default: the build tree)
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (exe, disc, display or python3/PIL
# missing, or the game has no measured pose).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

PG_BUILD_DIR="build/windows-mingw"
LEG=all
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --leg=*) LEG="${a#--leg=}";;
    esac
done
case "$LEG" in static|path|all) ;; *) echo "tercache-flicker: --leg must be static, path or all"; exit 64;; esac

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: level, pose, capture window and the masks the metric applies
# (x0,y0,x1,y1 inclusive, capture pixels). A game with no row skips.
case "$GAME" in
  spyro-1)
    LEVEL="cliff_town"
    POSE="pos=(138997, 98065, 22037) yaw=1370 pitch=8"
    PRESENTS_START=1900
    PATH_START=1900
    MASKS="780,0,10000,20 113,163,188,314"
    ;;
  spyro-3)
    # Fireworks Factory at 16:10: terrain dropping out for whole presents.
    LEVEL="fireworks_factory"
    POSE="pos=(39952, 29602, 9465) yaw=999 pitch=260"
    ASPECT="16:10"
    # Terrain alone: the level's actors and effects move every present.
    PATHS="terrain"
    PRESENTS_START=2473
    PATH_START=2473
    MASKS=""
    # Path leg: an approach-and-sweep over the factory floor, terrain path
    # alone at 16:10 (waypoints x,y,z,yaw,pitch; 24 ticks per segment).
    PATH_SPEC="ticks=24; 37952,29602,9465,699,260; 39952,29602,9465,999,260; 41452,31102,10265,1299,180; 39952,29602,9465,999,320; 38952,27602,8465,800,100; 39952,29602,9465,1100,260"
    PATH_CLEAR="88,20,189"
    ;;
  *) LEVEL="";;
esac
[[ -n "$LEVEL" ]] || { echo "tercache-flicker: SKIP ($GAME has no measured pose)"; exit 64; }
PRESENTS_N=64
PATH_SPEC="${PATH_SPEC:-}"
PATH_N=300
PATH_THIN_MAX=60
PATH_MIN_INTER=100
[[ "$LEG" == path && -z "$PATH_SPEC" ]] && { echo "tercache-flicker: SKIP ($GAME has no path leg)"; exit 64; }
ASPECT="${ASPECT:-16:9}"
PATH_ARGS=(); [[ -n "${PATHS:-}" ]] && PATH_ARGS=(--render-paths "$PATHS")
MIN_PRESENTS=32
MIN_INTER=8
MIN_REGION=100

EXE="${OPENPETE_TC_FLICKER_EXE:-$PG_BUILD_DIR/openpete.exe}"
DISC_ABS="$(cd "$(dirname "$PG_DISC")" 2>/dev/null && pwd -P)/$(basename "$PG_DISC")"
[[ -x "$EXE" && -f "$DISC_ABS" ]] ||
    { echo "tercache-flicker: SKIP (exe or disc missing: $EXE)"; exit 64; }
python3 -c 'import PIL, numpy, scipy' 2>/dev/null ||
    { echo "tercache-flicker: SKIP (python3 with PIL, numpy and scipy missing)"; exit 64; }
if [[ "$EXE" == *.exe ]]; then
    [[ -e /proc/sys/fs/binfmt_misc/WSLInterop || -e /proc/sys/fs/binfmt_misc/WSLInterop-late ]] ||
        { echo "tercache-flicker: SKIP (no WSL interop to run $EXE)"; exit 64; }
fi
# A Windows .exe opens its window on the Windows desktop; a Linux binary
# opens it on a private Xvfb display (scripts/lib/xvfb.sh).
OP_XVFB=()
[[ "$EXE" == *.exe ]] || op_xvfb_prefix tercache-flicker 2048x1536

OUT="${OPENPETE_TC_FLICKER_OUT:-$PG_BUILD_DIR/tercache-flicker}"
mkdir -p "$OUT"
OUT_ABS="$(cd "$OUT" && pwd -P)"
LOG="$OUT_ABS/$GAME-present-flicker.log"

wslenv_forward OPENPETE_VIBRATION=0 OPENPETE_TC_DBG=1 OPENPETE_TC_SUBMIT_CENSUS=1

STATIC_RC=0
if [[ "$LEG" != path ]]; then
# A loaded machine walks slower, and the capture window may not be reached
# inside the first frame limit: one retry with a longer limit.
attempt=1
limit=$((PRESENTS_START + 100))
while :; do
    rm -f "$LOG" "$OUT_ABS/$GAME"__*.png
    env "${WSLENV_ARGS[@]}" "${OP_XVFB[@]}" "$EXE" --game "$GAME" \
        --vibration 0 --input-lock --no-config --mute 1 --no-card --disc "$DISC_ABS" \
        --renderer vk --aspect "$ASPECT" --lod extended --terrain-cache 1 "${PATH_ARGS[@]}" \
        --skip-to-level "$LEVEL" --freecam-pose "$POSE" \
        --screenshot-native-only --screenshot-presents-start "$PRESENTS_START" \
        --screenshot-presents-count "$PRESENTS_N" \
        --screenshot-output "$OUT_ABS/${GAME}__{n}_{p}.png" \
        --frame-limit "$limit" 2>&1 | tr -d '\r' |
        grep -E --line-buffered '^\[tc\] ter\.world|screenshot: wrote|submit census|exit: reason|panic|assert' \
            > "$LOG"
    n=$(ls "$OUT_ABS/$GAME"__*.png 2>/dev/null | wc -l)
    [[ "$n" -ge "$MIN_PRESENTS" ]] && break
    if [[ "$attempt" -ge 2 ]]; then
        break
    fi
    echo "tercache-flicker: $n presents captured, retrying once with a longer frame limit"
    attempt=2
    limit=$((PRESENTS_START + 500))
    sleep 15
done

python3 - "$OUT_ABS" "$GAME" "$LOG" "$MASKS" "$MIN_REGION" "$MIN_PRESENTS" "$MIN_INTER" <<'CENSUS_PY'
import glob, re, sys
import numpy as np
from PIL import Image
from scipy import ndimage

out, game, log, masks = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
min_region, min_presents, min_inter = int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
rx = re.compile(r"__(\d+)_(\d+)\.png$")
files = sorted((f for f in glob.glob(f"{out}/{game}__*.png") if rx.search(f)),
               key=lambda f: int(rx.search(f).group(2)))
boxes = [tuple(int(v) for v in m.split(",")) for m in masks.split()]

# Engagement from the log: each captured present's kind, and whether a
# retained world draw precedes it (since the previous capture).
kinds, drawn, census = [], [], ""
world = 0
for line in open(log, errors="replace"):
    if line.startswith("[tc] ter.world"):
        world += 1
    elif "screenshot: wrote" in line:
        m = re.search(r"kind=native-(\w+)", line)
        kinds.append(m.group(1) if m else "?")
        drawn.append(world)
        world = 0
    elif "submit census" in line:
        census = line.split("census: ", 1)[-1].strip()
inter = sum(1 for k in kinds if k == "intermediate")
undrawn = [i for i, w in enumerate(drawn) if w == 0]

hits, prev = [], None
for f in files:
    a = np.asarray(Image.open(f).convert("RGB")).astype(np.int16)
    if prev is not None and prev.shape == a.shape:
        m = np.abs(a - prev).max(axis=2) > 32
        for x0, y0, x1, y1 in boxes:
            m[y0:y1 + 1, x0:x1 + 1] = False
        lab, k = ndimage.label(m)
        if k:
            sizes = ndimage.sum(m, lab, range(1, k + 1))
            big = [int(s) for s in sizes if s >= min_region]
            if big:
                i = int(np.argmax(sizes)) + 1
                ys, xs = np.nonzero(lab == i)
                hits.append((int(rx.search(f).group(2)), max(big),
                             "x%d-%d y%d-%d" % (xs.min(), xs.max(), ys.min(), ys.max())))
    prev = a

reasons = []
if len(files) < min_presents:
    reasons.append("presents=%d<%d" % (len(files), min_presents))
if inter < min_inter:
    reasons.append("intermediate=%d<%d" % (inter, min_inter))
if len(kinds) != len(files):
    reasons.append("log names %d captures, %d files" % (len(kinds), len(files)))
if undrawn:
    reasons.append("no retained world draw before present(s) %s" % undrawn[:8])
if reasons:
    verdict, rc = "VACUOUS", 1
elif hits:
    verdict, rc = "FAIL", 1
else:
    verdict, rc = "PASS", 0
print("[tercache-flicker] game=%s presents=%d intermediate=%d toggle_pairs=%d of %d "
      "max_region=%d verdict=%s%s"
      % (game, len(files), inter, len(hits), max(len(files) - 1, 0),
         max([h[1] for h in hits], default=0), verdict,
         (" reason=" + "; ".join(reasons)) if reasons else ""))
if census:
    print("[tercache-flicker] census %s" % census)
for p, size, box in hits:
    print("[tercache-flicker]   pair ending at present %d region=%d %s" % (p, size, box))
sys.exit(rc)
CENSUS_PY
STATIC_RC=$?
fi

PATH_RC=0
if [[ "$LEG" != static && -n "$PATH_SPEC" ]]; then
PLOG="$OUT_ABS/$GAME-path.log"
wslenv_forward OPENPETE_VIBRATION=0 OPENPETE_FREEZE_DBG=1 OPENPETE_RX_CENSUS=0-1000000 \
    OPENPETE_TERRAIN_GPU_CENSUS=1
attempt=1
limit=$((PATH_START + PATH_N + 100))
while :; do
    rm -f "$PLOG" "$OUT_ABS/$GAME"-path__*.png
    env "${WSLENV_ARGS[@]}" "${OP_XVFB[@]}" "$EXE" --game "$GAME" \
        --vibration 0 --input-lock --no-config --mute 1 --no-card --disc "$DISC_ABS" \
        --renderer vk --aspect "$ASPECT" --lod extended --terrain-cache 1 "${PATH_ARGS[@]}" \
        --skip-to-level "$LEVEL" --freecam-path "$PATH_SPEC" \
        --screenshot-native-only --screenshot-presents-start "$PATH_START" \
        --screenshot-presents-count "$PATH_N" \
        --screenshot-output "$OUT_ABS/${GAME}-path__{n}_{p}.png" \
        --frame-limit "$limit" 2>&1 | tr -d '\r' |
        grep -E --line-buffered '^\[freeze f=|^\[rx-census|^\[tgpu-cen|screenshot: wrote|exit: reason|panic|assert' \
            > "$PLOG"
    n=$(ls "$OUT_ABS/$GAME"-path__*.png 2>/dev/null | wc -l)
    [[ "$n" -ge "$PATH_N" ]] && break
    if [[ "$attempt" -ge 2 ]]; then
        break
    fi
    echo "tercache-flicker: path leg $n presents captured, retrying once with a longer frame limit"
    attempt=2
    limit=$((PATH_START + PATH_N + 600))
    sleep 15
done

python3 - "$OUT_ABS" "$GAME" "$PLOG" "$PATH_CLEAR" "$PATH_THIN_MAX" "$PATH_MIN_INTER" "$PATH_N" <<'PATH_PY'
import glob, re, sys
import numpy as np
from PIL import Image
from scipy import ndimage

out, game, log, clear = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
thin_max, min_inter, want = int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
clr = np.array([int(v) for v in clear.split(",")])
rx = re.compile(r"__(\d+)_(\d+)\.png$")
files = sorted((f for f in glob.glob(f"{out}/{game}-path__*.png") if rx.search(f)),
               key=lambda f: int(rx.search(f).group(2)))

kinds, rx_terrain, rx_n, after_rx, tc3_after, pending = [], 0, 0, 0, 0, False
sub_secs = 0
for line in open(log, errors="replace"):
    if line.startswith("[tgpu-cen"):
        m = re.search(r" sub=1 secs=(\d+)", line)
        sub_secs += int(m.group(1)) if m else 0
        continue
    if line.startswith("[rx-census"):
        m = re.search(r" terrain=(\d+)", line)
        rx_terrain += int(m.group(1)) if m else 0
        rx_n += 1
        pending = True
    elif line.startswith("[freeze f="):
        if pending:
            after_rx += 1
            if re.search(r" tc_src=3 ", line):
                tc3_after += 1
        pending = False
    elif "screenshot: wrote" in line:
        m = re.search(r"kind=native-(\w+)", line)
        kinds.append(m.group(1) if m else "?")

thin = {"canonical": [], "intermediate": []}
total = []
for i, f in enumerate(files):
    a = np.asarray(Image.open(f).convert("RGB")).astype(int)
    m = np.abs(a - clr).max(axis=2) <= 2
    m[:20, 690:] = False
    big = ndimage.binary_dilation(ndimage.binary_opening(m, np.ones((5, 5))), np.ones((5, 5)))
    k = kinds[i] if i < len(kinds) else "?"
    thin.setdefault(k, []).append(int((m & ~big).sum()))
    total.append(int(m.sum()))
spikes = []
for i, n in enumerate(total):
    lo, hi = max(0, i - 3), min(len(total), i + 4)
    nb = total[lo:i] + total[i + 1:hi]
    med = int(np.median(nb)) if nb else n
    if n > med + 2000 and n > 2 * med + 500:
        spikes.append((int(rx.search(files[i]).group(2)), n, med))
mean = {k: (sum(v) / len(v) if v else 0.0) for k, v in thin.items()}
inter = len(thin.get("intermediate", []))

reasons = []
if len(files) < want:
    reasons.append("presents=%d<%d" % (len(files), want))
if inter < min_inter:
    reasons.append("intermediate=%d<%d" % (inter, min_inter))
if len(kinds) != len(files):
    reasons.append("log names %d captures, %d files" % (len(kinds), len(files)))
if rx_terrain + sub_secs <= 0:
    reasons.append("re-extracts fed no terrain (census terrain sum %d over %d, retained sub-tick sectors %d)"
                   % (rx_terrain, rx_n, sub_secs))
fails = []
for k in ("canonical", "intermediate"):
    if mean.get(k, 0.0) > thin_max:
        fails.append("thin %s mean %.1f>%d" % (k, mean[k], thin_max))
if spikes:
    fails.append("spikes=%d" % len(spikes))
if tc3_after:
    fails.append("tc_src=3 after re-extract on %d of %d presents" % (tc3_after, after_rx))
verdict, rc = ("VACUOUS", 1) if reasons else (("FAIL", 1) if fails else ("PASS", 0))
print("[tercache-flicker] path game=%s presents=%d intermediate=%d thin_mean canon=%.1f inter=%.1f "
      "spikes=%d tc_src3_after_rx=%d of %d rx=%d rx_terrain=%d rx_retained_secs=%d verdict=%s%s"
      % (game, len(files), inter, mean.get("canonical", 0.0), mean.get("intermediate", 0.0),
         len(spikes), tc3_after, after_rx, rx_n, rx_terrain, sub_secs, verdict,
         (" reason=" + "; ".join(reasons + fails)) if (reasons or fails) else ""))
for p, n, med in spikes[:16]:
    print("[tercache-flicker]   spike at present %d clear=%d median=%d" % (p, n, med))
sys.exit(rc)
PATH_PY
PATH_RC=$?
fi
[[ "$STATIC_RC" -ne 0 ]] && exit "$STATIC_RC"
exit "$PATH_RC"
