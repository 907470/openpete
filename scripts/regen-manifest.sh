#!/usr/bin/env bash
# Regenerate one game's symbols.toml and globals.toml from its psx.map.
#
# Which game is selected by GAME= (default spyro-1):
#   GAME=spyro-2 bash scripts/regen-manifest.sh
#
# Inputs:
#   games/<game>/<game>/build/psx.map - parsed for symbol addresses
#
# Outputs (all under games/<game>/):
#   manifest/symbols.toml         - auto from psx.map
#   manifest/globals.toml         - auto from psx.map
#
# Run after `make` in the decomp submodule. Equivalent to the
# regen-recomp.sh role for the recompiled-overlay pipeline.

set -euo pipefail
cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
PSX_MAP="${PSX_MAP:-games/$GAME/$GAME/build/psx.map}"
MANIFEST_DIR="games/$GAME/manifest"

if [[ ! -f "$PSX_MAP" ]]; then
    echo "regen-manifest: $PSX_MAP not found - build the $GAME decomp first" >&2
    exit 1
fi

mkdir -p "$MANIFEST_DIR"
python3 tools/build_manifest.py "$PSX_MAP" "$MANIFEST_DIR"
