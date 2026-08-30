#!/usr/bin/env bash
# Freecam-corpus regression gate for the terrain near-LP gapfill classifier.
# Replays every recorded problematic spot (tests/freecam/gapfill-corpus.toml)
# headless and asserts the metric expectations.  Exit 0 = PASS, 1 = a check
# failed, 64 = SKIP (binary/disc/prereqs missing).
#
# Usage: scripts/check_gapfill_corpus.sh [--build-dir=DIR] [--only=SUBSTR] [--keep=DIR]
set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/.."
BUILD_DIR=build/linux-release
PASS=()
for a in "$@"; do
  case "$a" in
    --build-dir=*) BUILD_DIR="${a#*=}" ;;
    *) PASS+=("$a") ;;
  esac
done
exec python3 tests/freecam/run_corpus.py \
  --binary "$BUILD_DIR/games/spyro-1/openpete-spyro1" \
  --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
  --corpus tests/freecam/gapfill-corpus.toml \
  "${PASS[@]:-}"
