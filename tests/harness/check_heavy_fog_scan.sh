#!/usr/bin/env bash
# check_heavy_fog_scan.sh - the heavy-fog scan across a level load.
#
# The heavy-fog scan (terrain_heavy_fog_refresh) walks the guest sector table
# from the moby draw hooks at any tick, including the ticks of a level load in
# which the level id already names the new level while the table pointer and
# count still name the old one and the load has overwritten the records under
# them. A table entry is then any word. A scan that follows such a word as a
# guest address makes a bus access that reaches no device, which the bus
# logs as a guest access and the hang trace counts as one. The moby
# trailing-edge reveal on the first canonical pass after the load is the
# other host reader on this route: it tests the previous level's queued
# mobys, whose slots the load refilled.
#
# One headless run per game: boot, --skip-to-level through the level load to
# the level, the moby draw path alone (--render-paths moby) so the hooks that
# call the scan run.
#
# Engagement, required, else VACUOUS exit 3: a `terrain: heavy fog: level <id>`
# line whose far-colour count is above 0, so the scan ran on the loaded level.
#
# Criterion: `reached no device` lines = 0. Any is a FAIL, the first three
# quoted.
#
# Verdict line per game:
#   [heavy-fog-scan] game=<g> level=<l> vbl=<n> scans=<n> verdict_line=<text>
#                    bus=<n> verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_heavy_fog_scan.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# The level each game loads and the last VBL. Spyro 1's load into artisans
# overwrites the previous level's sector records with overlay code at VBL
# 1636 while the moby hooks still run; the run ends once the level draws.
case "$GAME" in
    spyro-1) LEVEL=artisans;       VBL=1700;;
    spyro-2) LEVEL=summer_forest;  VBL=2480;;
    spyro-3) LEVEL=sunrise_spring; VBL=2480;;
    *) echo "heavy-fog-scan: SKIP ($GAME has no level row)"; exit 64;;
esac

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "heavy-fog-scan: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/heavy-fog-scan/$GAME"
mkdir -p "$OUT"
LOG="$OUT/$GAME.log"

PG_CAPTURE_TIMEOUT=600
rc=0
pg_capture "$LOG" "" -- \
    --no-card --mute 1 --aspect 4:3 --lod original --render-paths moby \
    --skip-to-level "$LEVEL" --frame-limit "$VBL" </dev/null || rc=$?

if [[ ! -s "$LOG" ]]; then
    echo "[heavy-fog-scan] game=$GAME level=$LEVEL vbl=$VBL verdict=VACUOUS: the run wrote no log (rc=$rc)"
    exit 3
fi

scans=$(grep -c 'terrain: heavy fog: level ' "$LOG")
last=$(grep -E 'terrain: heavy fog: level [0-9]+, [0-9]+ sectors, [1-9][0-9]* far colours' "$LOG" |
       tail -1 | sed 's/.*terrain: heavy fog: //')
bus=$(grep -c 'reached no device' "$LOG")

head="[heavy-fog-scan] game=$GAME level=$LEVEL vbl=$VBL scans=$scans bus=$bus"

if [[ -z "$last" ]]; then
    echo "$head verdict=VACUOUS: no scan found a far colour, so the scan never ran on a level (rc=$rc); see $LOG"
    exit 3
fi
echo "[heavy-fog-scan]   last verdict: $last"
echo "$head verdict=$( ((bus)) && echo FAIL || echo PASS)"
if ((bus)); then
    grep 'reached no device' "$LOG" | head -3 | sed 's/^/[heavy-fog-scan]   /'
    echo "[heavy-fog-scan]   full log: $LOG"
    exit 1
fi
exit 0
