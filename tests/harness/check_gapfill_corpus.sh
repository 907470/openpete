#!/usr/bin/env bash
# Freecam-corpus regression gate for the terrain near-LP gapfill classifier.
# Replays every recorded problematic spot (tests/freecam/gapfill-corpus.toml)
# headless and asserts the metric expectations.  Exit 0 = PASS, 1 = a check
# failed, 64 = SKIP (binary/disc/prereqs missing).
#
# Usage: tests/harness/check_gapfill_corpus.sh [--game SLUG] [--build-dir=DIR]
#        [--only=SUBSTR] [--keep=DIR]
set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run - nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

BUILD_DIR=build/linux-release
PASS=()
for a in "$@"; do
  case "$a" in
    --build-dir=*)   BUILD_DIR="${a#*=}" ;;
    --game|--game=*) ;;   # consumed by pg_parse_game above
    spyro-*)         ;;   # the two-word --game SLUG spelling
    *) PASS+=("$a") ;;
  esac
done

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64

# The corpus poses, their level ids and the metric expectations are Spyro 1's,
# so --game names the game whose binary and disc are used and every other slug
# is refused rather than replayed against poses that do not exist in it.
[[ "$PG_GAME" == "spyro-1" ]] ||
  { echo "SKIP: $PG_GAME has no gapfill corpus"; exit 64; }
[[ -x "$PG_BIN"  ]] || { echo "SKIP: no binary at $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "SKIP: no disc at $PG_DISC"; exit 64; }

exec python3 tests/freecam/run_corpus.py \
  --binary "$PG_BIN" \
  --disc "$PG_DISC" \
  --corpus tests/freecam/gapfill-corpus.toml \
  "${PASS[@]:-}"
