#!/usr/bin/env bash
# check_nearhq_swing.sh — terrain triangle-swing census under --lod extended.
#
# The near/HQ subdivide routes a foreground quad through a 2x2 split, six
# extra triangles per face. Where the classifier has no distance band, quad
# membership changes as a corner crosses the camera plane, so the emitted
# triangle count of the scene oscillates by hundreds between neighbouring
# walks and the terrain visibly boils. This harness replays a camera-swing
# bk2 headless and reads the triangle count of every canonical terrain walk
# over a fixed VBL window. Headless runs one walk per tick and emits no
# intermediate presents, so a walk line is that tick's triangle count.
#
# Instrument (stderr, one line per walk), OPENPETE_TERRAIN_NEARCEN=1:
#   [nearcen] frame=<VBL> nearFaces=<n> subTris=<n> terrainTris=<n> sectors=<n>
#
# Verdict line:
#   [swing] game=<g> walks=<n> jumps=<n> maxJump=<n> spikes=<n> maxSpike=<n>
#           meanTris=<f> nearFaces=<f> subTris=<f> verdict=PASS|FAIL
#
# jumps  adjacent-walk terrain-triangle deltas of SWING_LIMIT or more.
# spikes one walk SWING_LIMIT or more away from BOTH of its neighbours.
#
# Pass criterion: jumps == 0.
# Engagement: at least MIN_WALKS walk lines inside the window, so a
# run that booted somewhere else, or that never reached the swing, refuses
# instead of reporting a vacuous zero.
#
# The census reads the classifier through OPENPETE_TERRAIN_HQSCALE, the
# foreground-band scale. Shrinking it drives every front-or-straddling quad
# below the band and restores the distance-gate-free routing the swing comes
# from, which is the harness's own negative control:
#
#   OPENPETE_TERRAIN_HQSCALE=0.001 scripts/check_nearhq_swing.sh --game spyro-3
#
# Only games whose draw bins the HP list by distance (game_layout.terrain
# .hp_far != 0) can swing here; the others keep the canonical classification
# at every distance. Spyro 3 is the one such game with a camera-swing bk2.
#
# Usage: scripts/check_nearhq_swing.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary, disc, bk2 or python3 missing, or the
# game has no swing corpus).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: the bk2, the VBL window it swings over, and the walk-count floor.
# A game with no row has no measured swing corpus and the check skips.
case "$GAME" in
  spyro-3) BK2="sunrise-spring-camera-swing.bk2"; VBL_LO=1500; VBL_HI=2300; MIN_WALKS=300;;
  *)       BK2="";;
esac
[[ -n "$BK2" ]] || { echo "nearhq-swing: SKIP ($GAME has no camera-swing corpus)"; exit 64; }

SWING_LIMIT=1000
MOVIE="$PG_BK2_DIR/$BK2"
[[ -x "$PG_BIN" && -f "$PG_DISC" && -f "$MOVIE" ]] ||
    { echo "nearhq-swing: SKIP (binary, disc or bk2 missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "nearhq-swing: SKIP (python3 missing)"; exit 64; }

# Outputs live in the build tree next to the binary and are deleted before the
# run by pg_capture, so a stale log from an earlier build cannot be read as
# this run's result.
OUT="${PG_BUILD_DIR:-build/linux-release}/nearhq-swing"
mkdir -p "$OUT"
LOG="$OUT/$GAME.log"
# --screenshot-native-only names the file after the render path, so the image
# pg_capture asserts carries the -vk suffix the run writes.
SHOT="$OUT/$GAME.png"
SHOT_VK="$OUT/$GAME-vk.png"

# --terrain-cache 0 keeps every tick walking the sectors, so each tick
# contributes a census line rather than replaying a cached build.
PG_CAPTURE_ENV=(OPENPETE_TERRAIN_NEARCEN=1)
PG_CAPTURE_TIMEOUT=900
# A headless run composes the native scene only for a tick something reads,
# so the census needs a screenshot request to make the terrain walk run; the
# image itself is the harness's proof that the run reached the window.
pg_capture "$LOG" "$SHOT_VK" -- \
    --renderer vk --mute 1 --aspect 4:3 --lod extended \
    --bk2 "$MOVIE" --terrain-cache 0 --vsync 0 \
    --screenshot-native-only --screenshot-on-frame "$VBL_HI" \
    --screenshot-output "$SHOT" \
    --frame-limit $((VBL_HI + 20))
rc=$?
[[ -s "$LOG" ]] || { echo "nearhq-swing: FAIL (no instrument output, rc=$rc; see $LOG)"; exit 1; }

python3 - "$LOG" "$GAME" "$VBL_LO" "$VBL_HI" "$SWING_LIMIT" "$MIN_WALKS" <<'PYEOF'
import re, sys

log, game = sys.argv[1], sys.argv[2]
lo, hi, limit, floor = (int(x) for x in sys.argv[3:7])

pat = re.compile(r'\[nearcen\] frame=(\d+) nearFaces=(\d+) subTris=(\d+) terrainTris=(\d+)')
rows = []
for line in open(log, errors='replace'):
    m = pat.search(line)
    if m:
        f, nf, st, tt = (int(x) for x in m.groups())
        if lo <= f <= hi:
            rows.append((f, nf, st, tt))

if len(rows) < floor:
    print("[swing] game=%s walks=%d VACUOUS: fewer than %d walks in VBL %d-%d"
          % (game, len(rows), floor, lo, hi))
    sys.exit(1)

tris = [r[3] for r in rows]
jumps = [(rows[i][0], abs(tris[i] - tris[i - 1]))
         for i in range(1, len(tris)) if abs(tris[i] - tris[i - 1]) >= limit]
spikes = []
for i in range(1, len(tris) - 1):
    a, b, c = tris[i - 1], tris[i], tris[i + 1]
    if (b - a >= limit and b - c >= limit) or (a - b >= limit and c - b >= limit):
        spikes.append((rows[i][0], min(abs(b - a), abs(b - c))))

ok = not jumps
print("[swing] game=%s walks=%d jumps=%d maxJump=%d spikes=%d maxSpike=%d "
      "meanTris=%.1f nearFaces=%.1f subTris=%.1f verdict=%s"
      % (game, len(rows), len(jumps), max((j[1] for j in jumps), default=0),
         len(spikes), max((s[1] for s in spikes), default=0),
         sum(tris) / len(tris),
         sum(r[1] for r in rows) / len(rows),
         sum(r[2] for r in rows) / len(rows),
         "PASS" if ok else "FAIL"))
for f, d in jumps[:12]:
    print("[swing]   jump frame=%d delta=%d" % (f, d))
sys.exit(0 if ok else 1)
PYEOF
