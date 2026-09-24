#!/usr/bin/env bash
# check_terrain_walk_cache.sh — CPU terrain walk vs retained GPU terrain cache,
# per-pose differing-pixel count against a committed baseline.
#
# The walk and the cache are one scene by contract; where they draw different
# pixels at a pose (depth ties under the stored depth encoding, a blended-face
# draw order the two feeds emit differently), the count of those pixels is the
# measured residual of that pose. The freecam golden captures the walk only, so
# it cannot see a residual whose walk image is already right; this check holds
# one row per such pose and fails on any change of its count. A drop is a fix
# and is promoted with --bless, as a golden tile is.
#
# Rows: games/<game>/walk-cache.manifest, `<level> | <pose> | [lod]` (default
# lod extended). A row's id is `<level>-<first 6 hex of the pose's md5>__<lod>`.
#
# Instrument: two headless legs of one build per row, 16:9, native-only, at the
# freecam corpus's canonical VBL (START), differing only in the feed:
#   walk   the software-vk default: the terrain cache is not initialized, every
#          sector goes through the CPU walk
#   cache  OPENPETE_TERRAIN_CACHE_FORCE=1: the retained cache, forced on under
#          software vk
# The row's value is the number of pixels whose RGB differ in any channel.
#
# Engagement, read from the logs at the captured VBL:
#   walk   `[nearcen] frame=<VBL> ... terrainTris=<n>` (OPENPETE_TERRAIN_NEARCEN=1),
#          n > 0
#   cache  `[tgpu-cen f=<VBL>] sub=<n> secs=<n>` (OPENPETE_TERRAIN_GPU_CENSUS=1),
#          secs > 0; the walk leg reports no retained sector (walk secs == 0)
# The value is the line whose VBL equals START, else the last line before it.
# A missing capture or a zero count is VACUOUS and fails the row.
#
# Baseline: tests/golden/<game>/walk-cache-baseline.tsv, `<row id>\t<px>`. A row
# fails when its count differs from the baseline in either direction, or when it
# has no baseline row. --bless merges the captured counts into the baseline
# (rows outside --filter are kept) and exits 0 when every row engaged.
#
# Usage:
#   scripts/check_terrain_walk_cache.sh [--game SLUG] [--build-dir DIR]
#                                       [--filter SUBSTR] [--bless]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no binary, disc, manifest,
# Vulkan loader, or python3/PIL).
set -u

# Unattended launcher: every run this script starts is silenced. Vibration
# ships on by default, which is right for a player and wrong for a
# machine-driven run.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
FILTER=""
BLESS=0
prev=""
for a in "$@"; do
    case "$prev" in
        --build-dir) BUILD_DIR="$a";;
        --filter)    FILTER="$a";;
    esac
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --bless)       BLESS=1;;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64

MAN="games/$PG_GAME/walk-cache.manifest"
BASE="tests/golden/$PG_GAME/walk-cache-baseline.tsv"
[[ -x "$PG_BIN"  ]] || { echo "walk-cache: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "walk-cache: SKIP (no disc at $PG_DISC)"; exit 64; }
[[ -f "$MAN"     ]] || { echo "walk-cache: SKIP (no manifest at $MAN)"; exit 64; }
ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1' ||
    { echo "walk-cache: SKIP (no Vulkan loader)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "walk-cache: SKIP (python3 or PIL missing)"; exit 64; }

LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
[[ -e "$LVP_ICD" ]] && export VK_ICD_FILENAMES="$LVP_ICD"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

OUT="$BUILD_DIR/walk-cache/$PG_GAME"
mkdir -p "$OUT"

# The freecam corpus's canonical post-load VBL (scripts/freecam-corpus.sh).
START=1900

# run_leg ID LEG LOD LEVEL POSE [ENV=VAL...]: one capture; writes
# $OUT/ID__LEG.log and $OUT/ID__LEG-vk.png, both deleted first by pg_capture.
run_leg() {
    local id="$1" leg="$2" lod="$3" level="$4" pose="$5"; shift 5
    local stem="$OUT/${id}__${leg}"
    rm -f "$stem.png"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_NEARCEN=1 OPENPETE_TERRAIN_GPU_CENSUS=1 "$@")
    PG_CAPTURE_TIMEOUT=600
    pg_capture "$stem.log" "$stem-vk.png" -- \
        --renderer vk --mute 1 --aspect 16:9 --lod "$lod" --no-card \
        --skip-to-level "$level" --freecam-pose "$pose" \
        --screenshot-native-only --screenshot-on-frame "$START" \
        --screenshot-output "$stem.png" \
        --frame-limit $((START + 10)) --max-runtime-seconds 580
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
IDS=(); LEVELS=(); POSES=(); LODS=()
while IFS='|' read -r level pose lod; do
    level="$(echo "$level" | tr -d ' \t')"
    [[ -z "$level" || "${level:0:1}" == "#" ]] && continue
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    lod="$(echo "${lod:-}" | tr -d ' \t')"; lod="${lod:-extended}"
    [[ -z "$pose" ]] && continue
    id="$level-$(printf '%s' "$pose" | md5sum | cut -c1-6)__$lod"
    [[ -n "$FILTER" && "$id" != *"$FILTER"* ]] && continue
    IDS+=("$id"); LEVELS+=("$level"); POSES+=("$pose"); LODS+=("$lod")
done < "$MAN"

if [[ ${#IDS[@]} -eq 0 ]]; then
    echo "[walk-cache] game=$PG_GAME rows=0 filter='$FILTER' verdict=VACUOUS"
    exit 1
fi
echo "[walk-cache] game=$PG_GAME bin=$PG_BIN rows=${#IDS[@]} filter='$FILTER' -> $OUT"

# Two game processes per row: the walk leg and the cache leg run concurrently.
RESULTS="$OUT/results.tsv"
rm -f "$RESULTS"
for i in "${!IDS[@]}"; do
    id="${IDS[$i]}"
    run_leg "$id" walk  "${LODS[$i]}" "${LEVELS[$i]}" "${POSES[$i]}" & pw=$!
    run_leg "$id" cache "${LODS[$i]}" "${LEVELS[$i]}" "${POSES[$i]}" \
        OPENPETE_TERRAIN_CACHE_FORCE=1 & pc=$!
    wait "$pw"; rw=$?
    wait "$pc"; rc=$?
    echo "  [$id] level=${LEVELS[$i]} lod=${LODS[$i]} rc walk=$rw cache=$rc"
    printf '%s\n' "$id" >> "$RESULTS"
done

python3 - "$OUT" "$RESULTS" "$BASE" "$START" "$BLESS" "$PG_GAME" <<'WC_PY'
import os, re, sys
from PIL import Image, ImageChops

out, results, base_path, start, bless, game = sys.argv[1:7]
start, bless = int(start), bless == "1"
tri = re.compile(r"\[nearcen\] frame=(\d+) .*?terrainTris=(\d+)")
cen = re.compile(r"\[tgpu-cen f=(\d+)\] sub=\d+ secs=(\d+)")


def at_start(path, pat):
    """The count on the line of the captured VBL, else the last line before it."""
    v = None
    if not os.path.isfile(path):
        return 0
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if m and int(m.group(1)) <= start:
            v = int(m.group(2))
    return v or 0


def read_base(path):
    base = {}
    if os.path.isfile(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#"):
                k, n = line.split("\t")
                base[k] = int(n)
    return base


base = read_base(base_path)
counts, fails = {}, 0
ids = [l.strip() for l in open(results) if l.strip()]
for rid in ids:
    walk_png = os.path.join(out, f"{rid}__walk-vk.png")
    cache_png = os.path.join(out, f"{rid}__cache-vk.png")
    tris = at_start(os.path.join(out, f"{rid}__walk.log"), tri)
    secs = at_start(os.path.join(out, f"{rid}__cache.log"), cen)
    walk_secs = at_start(os.path.join(out, f"{rid}__walk.log"), cen)
    missing = [p for p in (walk_png, cache_png) if not os.path.isfile(p)]
    if missing or tris == 0 or secs == 0 or walk_secs != 0:
        print(f"[walk-cache] row={rid} verdict=VACUOUS walkTris={tris} cacheSecs={secs}"
              f" walkSecs={walk_secs}"
              + (f" missing={','.join(os.path.basename(p) for p in missing)}" if missing else ""))
        fails += 1
        continue
    A = Image.open(walk_png).convert("RGB")
    B = Image.open(cache_png).convert("RGB")
    if A.size != B.size:
        print(f"[walk-cache] row={rid} verdict=FAIL reason=size {A.size} vs {B.size}")
        fails += 1
        continue
    d = ImageChops.difference(A, B).convert("L").point(lambda v: 255 if v else 0)
    px = d.histogram()[255]
    counts[rid] = px
    want = base.get(rid)
    if bless:
        verdict = "BLESS"
    elif want is None:
        verdict = "FAIL"; fails += 1
    elif px != want:
        verdict = "FAIL"; fails += 1
    else:
        verdict = "PASS"
    print(f"[walk-cache] row={rid} verdict={verdict} px={px} baseline="
          f"{'none' if want is None else want} walkTris={tris} cacheSecs={secs}")

if bless:
    if fails:
        print(f"[walk-cache] game={game} bless refused: {fails} row(s) vacuous")
        sys.exit(1)
    merged = dict(base)
    merged.update(counts)
    os.makedirs(os.path.dirname(base_path), exist_ok=True)
    with open(base_path, "w") as f:
        f.write("# row\tpx whose RGB differ, CPU terrain walk vs retained terrain cache "
                "(scripts/check_terrain_walk_cache.sh)\n")
        for k in sorted(merged):
            f.write(f"{k}\t{merged[k]}\n")
    print(f"[walk-cache] baseline written: {base_path} ({len(counts)} rows from this run, "
          f"{len(merged) - len(counts)} kept)")
    sys.exit(0)
print(f"[walk-cache] game={game} rows={len(ids)} fail={fails} verdict={'PASS' if not fails else 'FAIL'}")
sys.exit(1 if fails else 0)
WC_PY
