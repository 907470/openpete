#!/usr/bin/env bash
# Terrain fill-tag sweep: derive the LP fill-face list by measurement
# (tests/freecam/fill_sweep.py; poses in tests/freecam/fill-sweep-poses.manifest).
#
# og legs are invariant under native knobs and cached under build/fill-sweep-cache;
# delete a pose's directory to force recapture.
#
# Usage: scripts/terrain-fill-sweep.sh [--build-dir=DIR] [--only=SUBSTR] [--jobs=N]
set -euo pipefail
cd "$(dirname "$0")/.."
# The player's memory cards are never opened: runs boot on scratch cards.
source scripts/lib/scratch_cards.sh
BUILD_DIR=build/linux-release
PASS=()
for a in "$@"; do
  case "$a" in
    --build-dir=*) BUILD_DIR="${a#*=}" ;;
    *) PASS+=("$a") ;;
  esac
done
exec python3 tests/freecam/fill_sweep.py \
  --binary "$BUILD_DIR/openpete" \
  --disc games/spyro-1/spyro-1/build/SCUS-94228.cue \
  --manifest tests/freecam/fill-sweep-poses.manifest \
  --cache build/fill-sweep-cache \
  "${PASS[@]:-}"
