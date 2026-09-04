#!/usr/bin/env bash
# Derive the recoloured Artisans blob from the player's own disc. The
# output is disc-derived and stays on the player's machine; the mod
# distributes only this script.
#
# Needs bash and python3 on PATH. On Windows run it from Git Bash or WSL,
# or run the python command below by hand with `py -3`.
# Usage: ./generate.sh path/to/game.cue
set -eu
cd "$(dirname "$0")"
disc="${1:?usage: ./generate.sh <disc.cue>}"
# The tool lives at tools/ in the source tree and at sdk/tools/ in a
# distribution. Relative to this directory that is ../../tools/ when the
# mod sits in mods/ (source tree) or sdk/examples/ (archive), and
# ../../sdk/tools/ when the mod has been copied to <exe>/mods/.
tool=""
for c in ../../tools ../../sdk/tools "${OPENPETE_SDK_TOOLS:-}"; do
    if [ -n "$c" ] && [ -f "$c/extract_wad_entry.py" ]; then tool="$c/extract_wad_entry.py"; break; fi
done
[ -n "$tool" ] || { echo "extract_wad_entry.py not found; set OPENPETE_SDK_TOOLS to the sdk/tools directory" >&2; exit 64; }
python3 "$tool" --disc "$disc" --level 10 \
    --recolor-bg 255,0,255 --out assets/wad/level10.data.bin
echo "generated: enable the mod and enter Artisans"
