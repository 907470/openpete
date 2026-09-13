#!/usr/bin/env bash
#
# psycross-level-corpus.sh — one PsyCross reference frame per level, per game.
#
# Spyro 2 and Spyro 3 have no presentation golden and no oracle golden: the
# whole-game corpus under tests/golden/ is Spyro 1's, and it captures the
# native Vulkan renderer, so it cannot see a change to the PsyCross rasterizer
# at all. This is the image-side instrument for those two games. It boots each
# level with --skip-to-level, runs to a fixed canonical draw tick, and writes
# the PsyCross frame ("-og") for it.
#
# Keyed on --screenshot-on-tick, never --screenshot-on-frame: the VBL-to-tick
# mapping drifts with host timing, so a frame-keyed A/B can compare two
# different sim moments and report the difference as a rendering change.
#
# Usage:
#   scripts/psycross-level-corpus.sh <game> <outdir> [--tick N] [--filter SUB]
#     game      spyro-2 | spyro-3 | spyro-1
#     --tick N  canonical draw tick to capture. The default is per-game,
#               because the three reach a live level by different routes:
#               Spyro 2 and 3 hand off to the loader by frame ~850, so tick 560
#               (~frame 1105) is settled; Spyro 1 induces a demo transition at
#               frame ~1197 and is still on the loading card at tick 900, so it
#               defaults to 1400 (~frame 2416).
#     --filter  only levels whose name contains SUB
#
# CORPUS_JOBS caps concurrency (see scripts/corpus-jobs.sh).
#
# Exit: 0 = every requested tile written, 64 = SKIP (binary or disc missing),
#       1 = a level failed to load or produced no tile.
set -u

# Unattended launcher: nobody is holding the pad (scripts/fences/rumble_guard.py).
export OPENPETE_VIBRATION=0
# PsyCross rasterizes through software GL headless, so the capture needs no GPU
# and does not race whatever the user has on screen.
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

cd "$(dirname "$0")/.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh

GAME="${1:-}"; OUT="${2:-}"
[[ -z "$GAME" || -z "$OUT" ]] && {
  echo "usage: $0 <game> <outdir> [--tick N] [--filter SUB]" >&2; exit 64; }
shift 2

TICK=""
FILTER=""
while [[ $# -gt 0 ]]; do case "$1" in
  --tick)   TICK="$2"; shift 2;;
  --tick=*) TICK="${1#--tick=}"; shift;;
  --filter) FILTER="$2"; shift 2;;
  --filter=*) FILTER="${1#--filter=}"; shift;;
  *) echo "unknown argument: $1" >&2; exit 64;;
esac; done

case "$GAME" in
  spyro-1) BIN="build/linux-release/games/spyro-1/openpete-spyro1"
           DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue";;
  spyro-2) BIN="build/linux-release/games/spyro-2/openpete-spyro2"
           DISC="games/spyro-2/spyro-2/build/SCUS-94425.cue";;
  spyro-3) BIN="build/linux-release/games/spyro-3/openpete-spyro3"
           DISC="games/spyro-3/spyro-3/build/SCUS-94467.cue";;
  *) echo "unknown game: $GAME" >&2; exit 64;;
esac
# Per-game default; --tick still wins.
if [[ -z "$TICK" ]]; then
  case "$GAME" in spyro-1) TICK=1400;; *) TICK=560;; esac
fi

[[ -x "$BIN"  ]] || { echo "[psycross-corpus] SKIP: no $BIN" >&2;  exit 64; }
[[ -f "$DISC" ]] || { echo "[psycross-corpus] SKIP: no $DISC" >&2; exit 64; }

# Stale tiles read as a clean A/B leg, so the output is emptied before the run
# and every expected tile is asserted afterwards.
rm -rf "$OUT"; mkdir -p "$OUT"

mapfile -t LEVELS < <("$BIN" --skip-to-level 2>&1 |
                      awk '/^ +[a-z0-9_]+ +\([0-9]+\)$/ {print $1}')
[[ ${#LEVELS[@]} -gt 0 ]] || { echo "[psycross-corpus] SKIP: no levels" >&2; exit 64; }
if [[ -n "$FILTER" ]]; then
  mapfile -t LEVELS < <(printf '%s\n' "${LEVELS[@]}" | grep -- "$FILTER")
fi

# A tick key sits well past the level load; the frame limit only bounds a run
# that never reaches it.
FRAME_LIMIT=$(( TICK * 2 + 700 ))

capture_level() {
  local lvl="$1" log="$OUT/$1.log"
  "$BIN" --headless --disc "$DISC" --skip-to-level "$lvl" \
         --no-card --mute 1 --vibration 0 \
         --screenshot-on-tick "$TICK" \
         --screenshot-output "$OUT/$lvl-{n}.png" \
         --frame-limit "$FRAME_LIMIT" --max-runtime-seconds 300 \
         > "$log" 2>&1
  # Spyro 2 and 3 report the loader handoff; Spyro 1 induces a demo
  # transition instead and never prints the former.
  grep -qE "loader took level|induced demo-transition" "$log" ||
      { echo "  $lvl: NEVER LOADED" >&2; return 1; }
  [[ -f "$OUT/$lvl-$TICK-og.png" ]] || { echo "  $lvl: no tile" >&2; return 1; }
  return 0
}

echo "[psycross-corpus] $GAME: ${#LEVELS[@]} level(s) at tick $TICK, ${CORPUS_JOBS} job(s)"
fail=0
for lvl in "${LEVELS[@]}"; do cj_run capture_level "$lvl"; done
cj_wait || fail=1

# Engagement counter: a run that wrote nothing is a failure, not a clean pass.
n=$(find "$OUT" -name '*-og.png' | wc -l)
echo "[psycross-corpus] $GAME: $n/${#LEVELS[@]} tile(s) in $OUT"
[[ "$n" -eq 0 ]] && { echo "[psycross-corpus] FAIL: no tiles written" >&2; exit 1; }
[[ "$n" -ne "${#LEVELS[@]}" ]] && fail=1
exit "$fail"
