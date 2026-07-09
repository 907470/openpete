#!/usr/bin/env bash
# Freecam-corpus regression gate for the terrain near-LP gapfill classifier.
# Replays every recorded problematic spot (tests/freecam/gapfill-corpus.toml)
# headless and asserts the metric expectations.  Exit 0 = PASS, 1 = a check
# failed, 64 = SKIP (binary/disc/prereqs missing).
#
# Usage: scripts/check_gapfill_corpus.sh [--build-dir=DIR] [--only=SUBSTR] [--keep=DIR]
set -euo pipefail
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
