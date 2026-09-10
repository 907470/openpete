#!/usr/bin/env bash
# build-mod-docs.sh — regenerate the mod-author docs.
#
# Two stages, degrading gracefully:
#   1. tools/gen_mod_docs.py (stdlib python) extracts the reference
#      pages into docs/modding/pages/reference/ and the ImGui page into
#      docs/modding/pages/ — ALWAYS runs.
#   2. mkdocs builds the HTML site — runs when mkdocs is importable
#      (pip install mkdocs-material), otherwise prints a notice and
#      exits 0 so builds on minimal machines stay green (the markdown
#      is still perfectly readable).
#
# Usage:
#   scripts/build-mod-docs.sh [--structs <openpete_game_structs.h>]
#                             [--site-dir <dir>] [--serve]
#
# Wired into the build as the `mod_docs` CMake target (stamp-guarded:
# a no-op unless the API header, manifests, examples or pages changed).

set -euo pipefail
cd "$(dirname "$0")/.."

STRUCTS=""
SITE_DIR="build/moddocs/site"
SERVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --structs)  STRUCTS="$2"; shift 2 ;;
        --site-dir) SITE_DIR="$2"; shift 2 ;;
        --serve)    SERVE=1; shift ;;
        *) echo "build-mod-docs: unknown arg $1" >&2; exit 2 ;;
    esac
done

GEN_ARGS=()
[ -n "$STRUCTS" ] && GEN_ARGS+=(--structs "$STRUCTS")
python3 tools/gen_mod_docs.py "${GEN_ARGS[@]}"

if ! python3 -c "import mkdocs" 2>/dev/null; then
    echo "[mod-docs] mkdocs not installed — markdown generated, HTML" \
         "skipped (pip install mkdocs-material to build the site)"
    exit 0
fi

if [ "$SERVE" = 1 ]; then
    exec python3 -m mkdocs serve -f docs/modding/mkdocs.yml
fi

mkdir -p "$SITE_DIR"
python3 -m mkdocs build -f docs/modding/mkdocs.yml \
    -d "$(cd "$(dirname "$SITE_DIR")" && pwd)/$(basename "$SITE_DIR")" \
    --quiet
echo "[mod-docs] site built: $SITE_DIR/index.html"
