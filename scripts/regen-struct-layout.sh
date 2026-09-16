#!/usr/bin/env bash
# Regenerate games/<game>/manifest/struct_layout.toml: the MIPS o32
# sizeof/offsetof of every decomp game struct, which gen_game_structs.py
# turns into the _Static_assert pins of openpete_game_structs.h.
#
# Needs mipsel-linux-gnu-gcc (apt: gcc-mipsel-linux-gnu). Every other
# build reads the committed toml; run this after a change to the decomp's
# headers, to tools/psyq_types.py, or to the game's row in
# tools/game_struct_config.py, then commit the toml.
#
#   bash scripts/regen-struct-layout.sh
#   GAME=spyro-1 bash scripts/regen-struct-layout.sh
#   GAME=spyro-3 bash scripts/regen-struct-layout.sh

set -euo pipefail
cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
DECOMP="games/$GAME/$GAME"
OUT="games/$GAME/manifest/struct_layout.toml"

if ! command -v mipsel-linux-gnu-gcc >/dev/null; then
    echo "regen-struct-layout: mipsel-linux-gnu-gcc not found" >&2
    exit 1
fi
if [[ ! -d "$DECOMP/include" ]]; then
    echo "regen-struct-layout: $DECOMP/include not found (submodule checked out?)" >&2
    exit 1
fi

rm -f "$OUT"
python3 tools/probe_struct_layout.py --game "$GAME" --decomp "$DECOMP" --out "$OUT"
test -s "$OUT"
