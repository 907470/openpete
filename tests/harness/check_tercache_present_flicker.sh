#!/usr/bin/env bash
# check_tercache_present_flicker.sh — retained-terrain present-to-present
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
# Usage: tests/harness/check_tercache_present_flicker.sh [--game SLUG]
#            [--build-dir=DIR]
#   OPENPETE_TC_FLICKER_EXE   the exe to drive (default: the build's
#                             windows-mingw exe of the game); a Linux binary
#                             runs windowed on $DISPLAY or $WAYLAND_DISPLAY
#   OPENPETE_TC_FLICKER_OUT   output directory (default: the build tree)
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (exe, disc, display or python3/PIL
# missing, or the game has no measured pose).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

PG_BUILD_DIR="build/windows-mingw"
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: level, pose, capture window and the masks the metric applies
# (x0,y0,x1,y1 inclusive, capture pixels). A game with no row skips.
case "$GAME" in
  spyro-1)
    LEVEL="cliff_town"
    POSE="pos=(138997, 98065, 22037) yaw=1370 pitch=8"
    PRESENTS_START=1900
    MASKS="780,0,10000,20 113,163,188,314"
    ;;
  *) LEVEL="";;
esac
[[ -n "$LEVEL" ]] || { echo "tercache-flicker: SKIP ($GAME has no measured pose)"; exit 64; }
PRESENTS_N=64
MIN_PRESENTS=32
MIN_INTER=8
MIN_REGION=100

EXE="${OPENPETE_TC_FLICKER_EXE:-$PG_BUILD_DIR/games/$GAME/$(pg_bin_name "$GAME").exe}"
DISC_ABS="$(cd "$(dirname "$PG_DISC")" 2>/dev/null && pwd -P)/$(basename "$PG_DISC")"
[[ -x "$EXE" && -f "$DISC_ABS" ]] ||
    { echo "tercache-flicker: SKIP (exe or disc missing: $EXE)"; exit 64; }
python3 -c 'import PIL, numpy, scipy' 2>/dev/null ||
    { echo "tercache-flicker: SKIP (python3 with PIL, numpy and scipy missing)"; exit 64; }
if [[ "$EXE" == *.exe ]]; then
    [[ -e /proc/sys/fs/binfmt_misc/WSLInterop || -e /proc/sys/fs/binfmt_misc/WSLInterop-late ]] ||
        { echo "tercache-flicker: SKIP (no WSL interop to run $EXE)"; exit 64; }
else
    [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
        { echo "tercache-flicker: SKIP (no display)"; exit 64; }
fi

OUT="${OPENPETE_TC_FLICKER_OUT:-$PG_BUILD_DIR/tercache-flicker}"
mkdir -p "$OUT"
OUT_ABS="$(cd "$OUT" && pwd -P)"
LOG="$OUT_ABS/$GAME-present-flicker.log"

wslenv_forward OPENPETE_VIBRATION=0 OPENPETE_TC_DBG=1 OPENPETE_TC_SUBMIT_CENSUS=1

# A loaded machine walks slower, and the capture window may not be reached
# inside the first frame limit: one retry with a longer limit.
attempt=1
limit=$((PRESENTS_START + 100))
while :; do
    rm -f "$LOG" "$OUT_ABS/$GAME"__*.png
    env "${WSLENV_ARGS[@]}" "$EXE" \
        --vibration 0 --input-lock --no-config --mute 1 --no-card --disc "$DISC_ABS" \
        --renderer vk --aspect 16:9 --lod extended --terrain-cache 1 \
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
