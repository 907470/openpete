#!/usr/bin/env bash
# Regenerate the spyro-1 manifest + the artifacts driven from it.
#
# Inputs:
#   spyro-1/build/psx.map         — parsed for symbol addresses
#   games/spyro-1/manifest/native.toml — hand-curated native config
#
# Outputs (all under games/spyro-1/):
#   manifest/symbols.toml         — auto from psx.map
#   manifest/globals.toml         — auto from psx.map
#   generated/native_aliases.cmake — --defsym block
#   generated/native_dispatch.c   — psx_override_dispatch_game switch
#
# Run after `make` in spyro-1/ or after editing native.toml. Equivalent
# to the regen-recomp.sh role for the recompiled-overlay pipeline.

set -euo pipefail
cd "$(dirname "$0")/.."

PSX_MAP="${PSX_MAP:-../spyro-1/build/psx.map}"
INCLUDE_DIR="${INCLUDE_DIR:-../spyro-1/include}"
PSYQ_INCLUDE_DIR="${PSYQ_INCLUDE_DIR:-third_party/PsyCross/include/psx}"
MANIFEST_DIR="games/spyro-1/manifest"
GENERATED_DIR="games/spyro-1/generated"

if [[ ! -f "$PSX_MAP" ]]; then
    echo "regen-manifest: $PSX_MAP not found — build spyro-1 first" >&2
    exit 1
fi

python3 tools/build_manifest.py "$PSX_MAP" "$MANIFEST_DIR"
python3 tools/gen_defsyms.py     "$MANIFEST_DIR" "$GENERATED_DIR/native_aliases.cmake"
python3 tools/gen_dispatch.py    "$MANIFEST_DIR" "$GENERATED_DIR/native_dispatch.c" "$INCLUDE_DIR"
python3 tools/gen_thunks.py      "$MANIFEST_DIR" "$GENERATED_DIR/native_thunks.cpp" \
                                 "$INCLUDE_DIR" "$PSYQ_INCLUDE_DIR"
python3 tools/gen_renames.py     "$MANIFEST_DIR" "$GENERATED_DIR/native_renames.h"
