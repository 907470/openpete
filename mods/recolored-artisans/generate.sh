#!/usr/bin/env bash
# Derive the recoloured Artisans blob from the player's own disc. The
# output is disc-derived and stays on the player's machine; the mod
# distributes only this script.
# Usage: ./generate.sh path/to/game.cue
set -eu
cd "$(dirname "$0")"
disc="${1:?usage: ./generate.sh <disc.cue>}"
python3 ../../tools/extract_wad_entry.py --disc "$disc" --level 10 \
    --recolor-bg 255,0,255 --out assets/wad/level10.data.bin
echo "generated: enable the mod and enter Artisans"
