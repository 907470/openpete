#!/usr/bin/env bash
# check_eager_ring_pressure.sh - eager canonical presents keep their scene's
# content while the scene ring is full.
#
# Class: under eager production the producer publishes scenes into the scene
# ring and the present thread composes them. A present thread slower than the
# producer fills the ring; a canonical publish is then refused and the scene
# id is rebuilt by the next eager scene. Each scene-id build takes a retained
# terrain (terrain cache) state. A ring whose every state belongs to an
# unretired scene overwrites a pending scene's state, and that scene's present
# selects no state: it draws no cache-owned terrain (tc=0). The same pressure
# on any other per-scene state would show as a present composing fewer
# triangles of a render path than its scene was published with.
#
# Windowed only. Eager production needs a window, so neither the presentation
# golden nor any headless capture reaches the path.
#
# Driver: OPENPETE_PRESENT_STALL_MS=N (a harness env var, not a registered
# knob) makes the present thread sleep N ms before each scene render, which
# fills the ring without loading the machine.
#
# Instruments:
#   OPENPETE_PRESENT_TRACE=1, one line per present:
#     [present] #<n> kind=<k> shows=<s> frame=<n> stamp=<hex> n=<n> tc=<n> ...
#   and the exit lines
#     ring-audit: <n> canonical present(s) compared, <m> drew a path short ...
#     scene-ring: ... <n> refused (ring full)
#     [tercache] ring: <n> scene build(s), <n> rebuild(s) of a held id,
#                <n> overwrite(s) of a live state, <n> duplicate-stamp select(s) ...
#   The ring audit compares each canonical present's per-family triangle
#   counts (provenance tag bits 31:28, the --render-paths families) and its
#   retained-terrain sector count with the counts its scene was published
#   with; a path the scene had and the present composes with fewer is short.
#   OPENPETE_TERRAIN_CACHE_FORCE=1 brings the cache up on a software
#   rasteriser.
#
# Pass, per row: 0 canonical presents with tc=0, 0 overwrites of a live
# state, 0 short canonical presents. Engagement: "eager: engaged", refused
# publishes > 0, at least MIN_CANON canonical presents audited; else VACUOUS.
#
# Rows (one per game, level 10 of each):
#   spyro-1 artisans, spyro-2 summer_forest, spyro-3 sunrise_spring
#
# Every launch passes --input-lock --vibration 0 --background 1: physical input
# cannot reach the run and the window takes no focus.
#
# Verdict line per row:
#   [ring-pressure] game=<g> canon=<n> tc0=<n> refused=<n> overwrites=<n>
#                   audited=<n> short=<n> dup_selects=<n> eager=<0|1>
#                   verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_eager_ring_pressure.sh [--game SLUG]
#            [--build-dir=DIR] [--out=DIR] [--stall-ms=N]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no display, binary or disc).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
STALL_MS=2000
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
        --stall-ms=*)  STALL_MS="${a#--stall-ms=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

case "$GAME" in
  spyro-1) LEVEL=artisans;       FRAME_LIMIT=5400;;
  spyro-2) LEVEL=summer_forest;  FRAME_LIMIT=5973;;
  spyro-3) LEVEL=sunrise_spring; FRAME_LIMIT=5973;;
  *) echo "ring-pressure: SKIP ($GAME has no row)"; exit 64;;
esac
MIN_CANON=20
ROW_RUNTIME=900
ROW_TIMEOUT=960

[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "ring-pressure: SKIP (no display)"; exit 64; }
[[ -x "$PG_BIN" ]] || { echo "ring-pressure: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "ring-pressure: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/ring-pressure"
mkdir -p "$OUT"
LOG="$OUT/$GAME.log"
rm -f "$LOG"

env OPENPETE_VIBRATION=0 OPENPETE_PRESENT_TRACE=1 OPENPETE_TERRAIN_CACHE_FORCE=1 \
    OPENPETE_PRESENT_STALL_MS="$STALL_MS" \
    timeout "$ROW_TIMEOUT" "$PG_BIN" \
    --no-config --disc "$PG_DISC" --disc-cache 0 --no-card --mute 1 \
    --vibration 0 --input-lock --background 1 \
    --renderer vk --window 640x448 \
    "${PG_RUN_ARGS[@]}" --skip-to-level "$LEVEL" \
    --frame-limit "$FRAME_LIMIT" --max-runtime-seconds "$ROW_RUNTIME" \
    </dev/null >"$LOG" 2>&1
rc=$?
[[ -s "$LOG" ]] || { echo "ring-pressure: FAIL (no log, rc=$rc)"; exit 1; }

read -r canon tc0 refused overwrites audited short dups eager <<<"$(awk '
    BEGIN { overwrites = -1; audited = 0; short = -1; dups = -1 }
    /eager: engaged/ { eager = 1 }
    /^\[present\] / && / kind=canonical / {
        canon++
        for (i = 1; i <= NF; i++) if ($i == "tc=0") tc0++
    }
    /scene-ring: .* refused \(ring full\)/ {
        for (i = 2; i <= NF; i++) if ($i == "refused") refused = $(i - 1)
    }
    /\[tercache\] ring: / {
        for (i = 2; i <= NF; i++) {
            if ($i == "overwrite(s)") overwrites = $(i - 1)
            if ($i == "duplicate-stamp") dups = $(i - 1)
        }
    }
    /ring-audit: [0-9]+ canonical present\(s\) compared/ {
        for (i = 2; i <= NF; i++) {
            if ($i == "canonical") audited = $(i - 1)
            if ($i == "drew") short = $(i - 1)
        }
    }
    END { printf "%d %d %d %d %d %d %d %d\n", canon, tc0, refused, overwrites, audited, short, dups, eager }
' "$LOG")"

verdict=PASS
if (( eager == 0 || refused == 0 || audited < MIN_CANON || overwrites < 0 || short < 0 )); then
    verdict=VACUOUS
elif (( tc0 > 0 || overwrites > 0 || short > 0 )); then
    verdict=FAIL
fi
echo "[ring-pressure] game=$GAME stall_ms=$STALL_MS canon=$canon tc0=$tc0 refused=$refused overwrites=$overwrites audited=$audited short=$short dup_selects=$dups eager=$eager verdict=$verdict log=$LOG"
[[ "$verdict" == PASS ]]
