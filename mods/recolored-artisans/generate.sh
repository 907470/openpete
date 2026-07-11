#!/usr/bin/env bash
# Derive the recoloured Artisans blob from YOUR disc (never distributed).
# Usage: ./generate.sh path/to/game.cue
set -eu
cd "$(dirname "$0")"
disc="${1:?usage: ./generate.sh <disc.cue>}"
python3 ../../tools/extract_wad_entry.py --disc "$disc" --level 10 \
    --recolor-bg 255,0,255 --out assets/wad/level10.data.bin
echo "generated — enable the mod and enter Artisans"
