#!/usr/bin/env bash
# build-docs.sh - generate and build the documentation site (docs/mkdocs.yml).
#
# Two stages:
#   1. The generators of the sections being built: tools/gen_mod_docs.py
#      (stdlib python) writes docs/src/modding/reference/ and
#      docs/src/modding/imgui.md; tools/gen_code_docs.py (libclang over the
#      linux-release compile database) writes docs/src/developer/reference/.
#   2. mkdocs --strict builds the HTML site when mkdocs is importable
#      (pip install mkdocs-material); otherwise the script prints a notice
#      and exits 0, so builds on machines without it stay green. Packaging
#      refuses a tree with no built site.
#
# Usage:
#   scripts/build-docs.sh [--sections <a,b>] [--structs <openpete_game_structs.h>]
#                         [--sdk-dir <staged sdk>] [--no-code-docs]
#                         [--site-dir <dir>] [--serve]
#
# --sections names the top-level sections to build (user, modding,
# developer, spyro); without it every section builds. It is passed to the
# mkdocs hook tools/mkdocs_sections.py as OPENPETE_DOCS_SECTIONS.
# --no-code-docs skips gen_code_docs.py when the caller has already run it
# (the `code_docs` CMake target). --sdk-dir names a staged SDK whose
# games/<id>/ marks the games with a mod SDK (gen_mod_docs.py; default the
# first build directory's mod-sdk-test/sdk/).
#
# The `docs` CMake target runs this for the shipped sections (stamp-guarded:
# a no-op unless an input changed).

set -euo pipefail
cd "$(dirname "$0")/.."

SECTIONS=""
STRUCTS=""
SDK_DIR=""
SITE_DIR="build/docs-site"
SERVE=0
CODE_DOCS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --sections)     SECTIONS="$2"; shift 2 ;;
        --structs)      STRUCTS="$2"; shift 2 ;;
        --sdk-dir)      SDK_DIR="$2"; shift 2 ;;
        --site-dir)     SITE_DIR="$2"; shift 2 ;;
        --no-code-docs) CODE_DOCS=0; shift ;;
        --serve)        SERVE=1; shift ;;
        *) echo "build-docs: unknown arg $1" >&2; exit 2 ;;
    esac
done

# builds <section>: true when the section is part of this build.
builds() { [ -z "$SECTIONS" ] || [[ ",$SECTIONS," == *",$1,"* ]]; }

if builds modding; then
    GEN_ARGS=()
    [ -n "$STRUCTS" ] && GEN_ARGS+=(--structs "$STRUCTS")
    [ -n "$SDK_DIR" ] && GEN_ARGS+=(--sdk-dir "$SDK_DIR")
    python3 tools/gen_mod_docs.py "${GEN_ARGS[@]}"
fi
if builds developer && [ "$CODE_DOCS" = 1 ]; then
    python3 tools/gen_code_docs.py
fi

if ! python3 -c "import mkdocs" 2>/dev/null; then
    echo "[docs] mkdocs not installed - markdown generated, HTML" \
         "skipped (pip install mkdocs-material to build the site)"
    exit 0
fi

export OPENPETE_DOCS_SECTIONS="$SECTIONS"
if [ "$SERVE" = 1 ]; then
    exec python3 -m mkdocs serve -f docs/mkdocs.yml
fi

# mkdocs build empties the site directory first (its --clean default), so
# a page dropped from the source does not survive in the output.
SITE_DIR="$(realpath -m "$SITE_DIR")"
python3 -m mkdocs build --strict -f docs/mkdocs.yml -d "$SITE_DIR" --quiet
[ -f "$SITE_DIR/index.html" ] \
    || { echo "build-docs: mkdocs wrote no $SITE_DIR/index.html" >&2; exit 1; }
echo "[docs] site built: $SITE_DIR/index.html${SECTIONS:+ (sections: $SECTIONS)}"
