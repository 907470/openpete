#!/usr/bin/env bash
# check_tercache_gpu_holes.sh - retained-terrain GPU cache hole census on a
# real GPU, windowed.
#
# The retained cache draws a static sector's faces from a persistent vertex
# buffer and resolves their PSX ordering-table order in a fragment key
# contest: each face writes its OT key into the depth attachment through the
# rasteriser's perspective divide, and the fragment stage re-encodes the same
# key to compare against what is stored. The two encodings are computed by
# different stages of the device, so a key that sits on a bucket boundary can
# decode one ulp below its own stored value, fall into the previous bucket,
# and lose the contest to itself. The face then draws nothing and the pixel
# shows the framebuffer clear: a hole in the terrain, the shape of one face.
#
# The class is device-dependent by construction (an exact-bit tie on one
# device is not a tie on another), so a software rasteriser cannot see it and
# the headless presentation golden, which runs llvmpipe with the cache off,
# cannot separate a build that carries it from a build that does not. This
# census therefore runs windowed on the machine's own GPU.
#
# Instrument: an in-binary A/B on one build, two legs per corpus row on the
# same device, differing by exactly one lever:
#   A  --terrain-cache 0   the CPU terrain walk feed; the reference
#   B  (default)           the retained GPU cache
# Both legs render the same pose on the same device, so the compare is not a
# cross-device diff and needs no software leg.
#
# Metric: pixels the two legs render differently where the cached leg shows
# the framebuffer clear and the reference leg does not. The clear
# colour is not hard-coded - it is read from the engine's own draw-env census
# line (OPENPETE_LOAD_CENSUS=1, `[census f=<VBL> CLEAR] rgb=<rgba>`), which
# reports the value openpete_native_set_clear_color latched from the PSX
# DRAWENV struct the game presents the scene under. Hole pixels are grouped
# into 8-connected components; every component of MIN_COMPONENT pixels or
# more is listed with its bounding box.
#
# Pass criterion: no component of MIN_COMPONENT pixels or more on every
# engaged row. A missing face is a contiguous region, the shape of the face;
# isolated hole pixels along a silhouette are edge grain of the two feeds'
# rasterisation and vary between runs of one build on one device. The raw
# hole-pixel count and the largest component are printed per row.
# Engagement: leg B must report at least MIN_SECS retained sectors on
# its [tgpu-cen] line (the cache actually served the frame) and leg A at least
# MIN_TRIS terrain triangles on its [nearcen] line (the reference frame
# actually carries terrain). A row that misses either floor proves nothing
# about the cache and is reported SKIP-ENGAGEMENT, never PASS.
#
# The glitch mark's reader is not an instrument for this class: its swing rule
# keys on the present record's terrain triangle count, which is zero whenever
# the retained cache serves every sector of the frame.
#
# Usage:
#   tests/harness/check_tercache_gpu_holes.sh [--game SLUG] [--filter=SUBSTR]
#                                       [--exe PATH] [--build-dir=DIR]
#                                       [--out DIR]
#   --filter  substring matched against the row's level name and pose slug
#   --exe     the executable to drive (default: the windows-mingw build)
# Exit 0 PASS, 1 FAIL or a missing corpus manifest (the message names the
# path), 64 SKIP (no display, no exe, no disc, no python3/PIL, or every row
# skipped for engagement).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

# Unattended launcher: no one is holding the pad and the buzzing appears in no
# log, so every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

WIN_BUILD_DIR="build/windows-mingw"
EXE=""
FILTER=""
OUT=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) WIN_BUILD_DIR="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --out=*)       OUT="${a#--out=}";;
    esac
done
# --exe and --filter also accept the space-separated spelling.
prev=""
for a in "$@"; do
    case "$prev" in
        --exe)    EXE="$a";;
        --filter) FILTER="$a";;
        --out)    OUT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Capture geometry and pinned VBL. 664x448 is the window the class was first
# measured in; the pinned frame is the corpus's own post-load canonical frame.
WIDTH=664
HEIGHT=448
START_DEFAULT="${OPENPETE_TERCACHE_HOLES_START:-1900}"
# Per-channel tolerance when matching a pixel against the engine's clear
# colour: the native path tone-maps and dithers, so the backdrop is not a
# single exact triple.
TOL=24
MIN_COMPONENT=8
# Per-leg wall-clock budget. A level that loads slowly reaches the pinned VBL
# and starts its screenshot readback several minutes in; the outer timeout
# stays above the engine's own so the engine exits on its budget first and
# leaves a readable log.
LEG_RUNTIME=600
LEG_TIMEOUT=660
MIN_SECS=50
MIN_TRIS=1000

pg_manifest "$PG_FREECAM_MANIFEST" pipe

# A live window on a real GPU is the whole point of the check; a headless
# invocation (ctest on a build box, a session with no display) has nothing to
# measure and skips rather than reporting a verdict it did not earn.
[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "tercache-holes: SKIP (no display)"; exit 64; }

[[ -n "$EXE" ]] || EXE="$WIN_BUILD_DIR/games/$GAME/$(pg_bin_name "$GAME").exe"
[[ -x "$EXE" ]] || { echo "tercache-holes: SKIP (no exe at $EXE)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-holes: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "tercache-holes: SKIP (python3 or PIL missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$WIN_BUILD_DIR/tercache-holes/$GAME"
mkdir -p "$OUT"

# A Windows child resolves a path against the Windows view of the filesystem
# and does not follow a link anywhere in it, so every path handed to a .exe is
# converted and the disc is resolved to its link-free absolute form first.
IS_EXE=0
case "$EXE" in *.exe) IS_EXE=1;; esac
hostpath() {
    if [[ "$IS_EXE" == 1 ]]; then wslpath -w "$1"; else echo "$1"; fi
}

DISC_DIR="$(cd "$(dirname "$PG_DISC")" && pwd -P)"
DISC_ABS="$DISC_DIR/$(basename "$PG_DISC")"
# A cue whose FILE names a bin that is not beside it loads under the Linux
# build (which resolves the sibling by the cue's own stem) but aborts part way
# through the boot under the Windows one. Stage a cue naming the sibling that
# is actually there, hard-linked so no disc image is copied.
CUE_FILE="$(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\([^"]*\)".*/\1/p' "$DISC_ABS" | head -1)"
if [[ -n "$CUE_FILE" && ! -f "$DISC_DIR/$CUE_FILE" ]]; then
    STEM="$(basename "$DISC_ABS" .cue)"
    [[ -f "$DISC_DIR/$STEM.bin" ]] ||
        { echo "tercache-holes: SKIP (cue names $CUE_FILE, absent, and no $STEM.bin beside it)"; exit 64; }
    STAGE="$OUT/disc"
    mkdir -p "$STAGE"
    [[ -f "$STAGE/$STEM.bin" ]] || ln "$DISC_DIR/$STEM.bin" "$STAGE/$STEM.bin" 2>/dev/null ||
        { echo "tercache-holes: SKIP (cannot stage $STEM.bin beside a cue)"; exit 64; }
    sed "s|\"$CUE_FILE\"|\"$STEM.bin\"|" "$DISC_ABS" > "$STAGE/$STEM.cue"
    DISC_ABS="$(cd "$STAGE" && pwd -P)/$STEM.cue"
fi

DISC_ARG="$(hostpath "$DISC_ABS")"

# One leg. Its log and its PNG are deleted before the run and asserted after
# it, so a stale file from an earlier build can never be read as this run's
# result. The capture is a readback of the presented frame, chrome included,
# so the on-screen stats block and the notifications are switched off: the
# FPS digits differ between the two legs and between runs, and text over the
# clear reads as a hole in one leg and as the reverse case in the other. stdin is closed: the row loop reads the manifest on its own
# stdin, and a child that inherits it consumes the rows that have not run yet.
run_leg() {  # tag level pose start extra-args...
    local tag="$1" level="$2" pose="$3" start="$4"; shift 4
    local log="$OUT/$tag.log"
    local shot="$OUT/$tag.png"
    local shot_vk="$OUT/$tag-vk.png"
    local attempt=1
    while :; do
        rm -f "$log" "$shot" "$shot_vk"
        wslenv_forward OPENPETE_VIBRATION=0 \
                       OPENPETE_TERRAIN_GPU_CENSUS=1 \
                       OPENPETE_TERRAIN_NEARCEN=1 \
                       OPENPETE_LOAD_CENSUS=1
        timeout "$LEG_TIMEOUT" env "${WSLENV_ARGS[@]}" "$EXE" \
            --disc "$DISC_ARG" --no-config --no-card --mute 1 \
            --vibration 0 --mouse 0 --input-lock --perf-stats off --osd off \
            --renderer vk --lod extended --window "${WIDTH}x${HEIGHT}" --vsync 0 \
            --skip-to-level "$level" --freecam-pose "$pose" \
            --screenshot-native-only --screenshot-on-frame "$start" \
            --screenshot-output "$(hostpath "$shot")" \
            --frame-limit $((start + 20)) --max-runtime-seconds "$LEG_RUNTIME" \
            "$@" < /dev/null > "$log" 2>&1
        local rc=$?
        tr -d '\r' < "$log" > "$log.txt" && mv "$log.txt" "$log"
        [[ -f "$shot_vk" ]] && return 0
        # A windowed launch started while the previous run is still releasing
        # the display sometimes never brings its window up: WSL interop fails
        # the vsock accept and the process writes nothing. That is a launch
        # failure, not a reading, so it is retried once after the display has
        # settled. A leg that produced an image is never retried, so a real
        # divergence cannot be retried away.
        if [[ "$attempt" -ge 2 ]]; then
            echo "  no capture for leg $tag in 2 attempts (rc=$rc)"
            return 1
        fi
        attempt=2
        sleep 20
    done
}

pose_slug() {  # level pose
    local h; h="$(printf '%s' "$2" | md5sum | cut -c1-6)"
    echo "${1}-${h}"
}

rows=0; engaged=0; skipped=0; failed=0; missing=0
while IFS='|' read -r level pose start _launch; do
    level="$(echo "$level" | tr -d ' \t')"
    [[ -z "$level" || "${level:0:1}" == "#" ]] && continue
    # A variant row (`<level>@<variant>`) re-captures a base pose under its
    # own launch field; the cache-vs-walk comparison belongs to the base row.
    [[ "$level" == *@* ]] && continue
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    start="$(echo "$start" | tr -d ' \t')"; start="${start:-$START_DEFAULT}"
    [[ -z "$pose" ]] && continue
    slug="$(pose_slug "$level" "$pose")"
    [[ -n "$FILTER" && "$level" != *"$FILTER"* && "$slug" != *"$FILTER"* ]] && continue
    rows=$((rows + 1))

    if ! run_leg "$slug-a" "$level" "$pose" "$start" --terrain-cache 0; then
        echo "[tercache-holes] row=$slug verdict=NO-CAPTURE leg=a"
        missing=$((missing + 1)); continue
    fi
    sleep 5
    if ! run_leg "$slug-b" "$level" "$pose" "$start"; then
        echo "[tercache-holes] row=$slug verdict=NO-CAPTURE leg=b"
        missing=$((missing + 1)); continue
    fi

    python3 - "$OUT/$slug-a-vk.png" "$OUT/$slug-b-vk.png" \
             "$OUT/$slug-a.log" "$OUT/$slug-b.log" \
             "$slug" "$TOL" "$MIN_COMPONENT" "$MIN_SECS" "$MIN_TRIS" <<'HOLES_PY'
import re, sys
from collections import deque
from PIL import Image

a_png, b_png, a_log, b_log, row = sys.argv[1:6]
tol, min_comp, min_secs, min_tris = (int(x) for x in sys.argv[6:10])

# The engine's own draw-env clear, as openpete_native_set_clear_color latched
# it from the PSX DRAWENV the presented scene is drawn under. rgba packs
# 0xFF000000 | r | g<<8 | b<<16; a zero value is the "no auto-clear" case,
# where the background comes from the scene feed instead and this census has
# no colour to match against.
clear_re = re.compile(r"\[census f=\d+ CLEAR\] isbg=\d+ rgb=([0-9a-f]{8})")
cen_re = re.compile(r"\[tgpu-cen f=\d+\] sub=\d+ secs=(\d+)")
tris_re = re.compile(r"\[nearcen\] frame=\d+ nearFaces=\d+ subTris=\d+ terrainTris=(\d+)")

def scan(path, pat):
    out = []
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if m:
            out.append(m.group(1))
    return out

clears = scan(b_log, clear_re)
secs = [int(x) for x in scan(b_log, cen_re)]
tris = [int(x) for x in scan(a_log, tris_re)]

reasons = []
if not clears or int(clears[-1], 16) == 0:
    reasons.append("the engine reported no draw-env clear colour")
if max(secs, default=0) < min_secs:
    reasons.append("cached leg retained secs=%d<%d" % (max(secs, default=0), min_secs))
if max(tris, default=0) < min_tris:
    reasons.append("reference leg terrainTris=%d<%d" % (max(tris, default=0), min_tris))

if reasons:
    print("[tercache-holes] row=%s secs=%d tris=%d verdict=SKIP-ENGAGEMENT reason=%s"
          % (row, max(secs, default=0), max(tris, default=0), "; ".join(reasons)))
    sys.exit(64)

v = int(clears[-1], 16)
clear = (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)

a = Image.open(a_png).convert("RGB")
b = Image.open(b_png).convert("RGB")
if a.size != b.size:
    print("[tercache-holes] row=%s verdict=FAIL reason=leg sizes differ %s vs %s"
          % (row, a.size, b.size))
    sys.exit(1)
w, h = a.size
pa, pb = a.load(), b.load()

def is_clear(px):
    return (abs(px[0] - clear[0]) <= tol and abs(px[1] - clear[1]) <= tol
            and abs(px[2] - clear[2]) <= tol)

def differs(p, q):
    return max(abs(p[i] - q[i]) for i in range(3)) > tol

# A hole: the cached leg shows the framebuffer clear where the reference leg
# drew terrain. Both conditions are required. Matching the clear within a
# tolerance alone is not evidence: a dithered sky gradient sits within a few
# steps of the clear, and the two feeds quantize it differently, so the
# tolerance alone classifies the same rendered colour as clear in one leg and
# not the other. A pixel the two legs render to the same colour is not a hole
# whatever that colour is. The reverse case (the reference leg clear, the
# cached leg drawn) is not this class and is counted only as a diagnostic.
mask = bytearray(w * h)
holes = 0
extra = 0
for y in range(h):
    for x in range(w):
        qa, qb = pa[x, y], pb[x, y]
        if not differs(qa, qb):
            continue
        ca, cb = is_clear(qa), is_clear(qb)
        if cb and not ca:
            mask[y * w + x] = 1
            holes += 1
        elif ca and not cb:
            extra += 1

# 8-connected components of the hole mask, largest first.
comps = []
largest = 0
seen = bytearray(w * h)
for i in range(w * h):
    if not mask[i] or seen[i]:
        continue
    seen[i] = 1
    q = deque([i])
    x0 = x1 = i % w
    y0 = y1 = i // w
    n = 0
    while q:
        j = q.popleft()
        n += 1
        jx, jy = j % w, j // w
        x0, x1 = min(x0, jx), max(x1, jx)
        y0, y1 = min(y0, jy), max(y1, jy)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = jx + dx, jy + dy
                if 0 <= nx < w and 0 <= ny < h:
                    k = ny * w + nx
                    if mask[k] and not seen[k]:
                        seen[k] = 1
                        q.append(k)
    largest = max(largest, n)
    if n >= min_comp:
        comps.append((n, x0, x1, y0, y1))
comps.sort(reverse=True)

verdict = "PASS" if not comps else "FAIL"
print("[tercache-holes] row=%s clear=(%d,%d,%d) secs=%d tris=%d holePx=%d "
      "largestComponent=%d components=%d reverseClearPx=%d verdict=%s"
      % (row, clear[0], clear[1], clear[2], max(secs), max(tris), holes,
         largest, len(comps), extra, verdict))
for n, x0, x1, y0, y1 in comps[:12]:
    print("[tercache-holes]   component px=%d x %d-%d y %d-%d" % (n, x0, x1, y0, y1))
sys.exit(0 if not comps else 1)
HOLES_PY
    rc=$?
    case "$rc" in
        0)  engaged=$((engaged + 1));;
        64) skipped=$((skipped + 1));;
        *)  engaged=$((engaged + 1)); failed=$((failed + 1));;
    esac
    sleep 5
done < "$PG_FREECAM_MANIFEST"

echo "[tercache-holes] game=$GAME rows=$rows engaged=$engaged skipEngagement=$skipped noCapture=$missing failed=$failed"
[[ "$missing" -gt 0 ]] && exit 1
[[ "$failed" -gt 0 ]] && exit 1
# A run whose every row missed its engagement floor measured nothing about the
# cache; it neither passes nor fails.
[[ "$engaged" -eq 0 ]] && exit 64
exit 0
