#!/usr/bin/env bash
# Regenerate one game's manifest + the artifacts driven from it.
#
# Which game is selected by GAME= (default spyro-1):
#   GAME=spyro-2 bash scripts/regen-manifest.sh
#
# Inputs:
#   games/<game>/<game>/build/psx.map — parsed for symbol addresses
#   games/<game>/manifest/native.toml — hand-curated native config
#
# Outputs (all under games/<game>/):
#   manifest/symbols.toml         — auto from psx.map
#   manifest/globals.toml         — auto from psx.map
#   generated/native_aliases.cmake — --defsym block
#   generated/native_dispatch.c   — psx_override_dispatch_game switch
#
# Run after `make` in the decomp submodule or after editing native.toml. Equivalent
# to the regen-recomp.sh role for the recompiled-overlay pipeline.

set -euo pipefail
cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
PSX_MAP="${PSX_MAP:-games/$GAME/$GAME/build/psx.map}"
INCLUDE_DIR="${INCLUDE_DIR:-games/$GAME/$GAME/include}"
PSYQ_INCLUDE_DIR="${PSYQ_INCLUDE_DIR:-third_party/PsyCross/include/psx}"
SPYRO_SRC_DIR="${SPYRO_SRC_DIR:-games/$GAME/$GAME/src}"
MANIFEST_DIR="games/$GAME/manifest"
GENERATED_DIR="games/$GAME/generated"
REWRITTEN_DIR="$GENERATED_DIR/rewritten"

if [[ ! -f "$PSX_MAP" ]]; then
    echo "regen-manifest: $PSX_MAP not found — build the $GAME decomp first" >&2
    exit 1
fi

mkdir -p "$MANIFEST_DIR" "$GENERATED_DIR"
python3 tools/build_manifest.py "$PSX_MAP" "$MANIFEST_DIR"

# A game with no native.toml compiles no decomp TU natively: everything routes
# through the recompiler. The AST rewriter has nothing to do, and the four
# generators below emit empty artifacts from an empty manifest.
if [[ ! -f "$MANIFEST_DIR/native.toml" ]]; then
    echo "regen-manifest: no $MANIFEST_DIR/native.toml — $GAME is fully recompiled,"
    echo "                symbols.toml and globals.toml written, nothing else to do."
    exit 0
fi

# Run the AST rewriter for every native source. Each TU emits
# <stem>.rewritten.c (compiled by host CMake instead of the original) plus
# <stem>.redirects.txt (Pass 2 bare-global slots — gen_defsyms.py merges
# them into the linker --defsym block below).
mkdir -p "$REWRITTEN_DIR"
# Wipe per-TU artifacts (discovered.toml / redirects.txt / rewritten.c)
# so a TU dropped from native_source doesn't leak stale entries into
# the merge step. Per-TU shadow include trees get wiped inside the
# loop below.
rm -f "$REWRITTEN_DIR"/*.discovered.toml \
      "$REWRITTEN_DIR"/*.redirects.txt \
      "$REWRITTEN_DIR"/*.rewritten.c \
      "$REWRITTEN_DIR"/*.spliced.txt
NATIVE_SOURCES=$(python3 -c "
import sys, tomli
with open('$MANIFEST_DIR/native.toml','rb') as f:
    cfg = tomli.load(f)
for ns in cfg.get('native_source', []):
    print(ns['file'])
")

# Pass 6 (auto-manifest discovery) needs the union of function names
# defined by *other* native TUs in this domain, so it can suppress
# same-domain native→native calls from the outbound_recomp set. Build
# that list per-TU by parsing each TU's exported FUNCTION_DECLs once;
# we approximate cheaply with a grep over `^<retty> name(` patterns.
NATIVE_FN_NAMES_FILE="$REWRITTEN_DIR/.native_fn_names.txt"
: > "$NATIVE_FN_NAMES_FILE"
for rel in $NATIVE_SOURCES; do
    grep -oE "^[A-Za-z_][A-Za-z_0-9 *]+ \**[A-Za-z_][A-Za-z_0-9]+\(" \
        "$SPYRO_SRC_DIR/$(basename "$rel")" 2>/dev/null \
        | sed -E 's/.*[ \*]([A-Za-z_][A-Za-z_0-9]+)\(.*/\1/' \
        >> "$NATIVE_FN_NAMES_FILE" || true
done
sort -u -o "$NATIVE_FN_NAMES_FILE" "$NATIVE_FN_NAMES_FILE"

for rel in $NATIVE_SOURCES; do
    stem=$(basename "$rel" .c)
    # Wipe per-TU shadow tree before each run so removed rewrites don't
    # linger; rewrite_tu.py only writes headers it touches.
    rm -rf "$REWRITTEN_DIR/$stem/include"
    python3 tools/rewrite_tu.py \
        "$SPYRO_SRC_DIR/$(basename "$rel")" \
        "$REWRITTEN_DIR/$stem.rewritten.c" \
        --domain=main_exe \
        --symbols-toml="$MANIFEST_DIR/symbols.toml" \
        --shadow-include-dir="$REWRITTEN_DIR/$stem/include" \
        --redirects-out="$REWRITTEN_DIR/$stem.redirects.txt" \
        --discovered-toml-out="$REWRITTEN_DIR/$stem.discovered.toml" \
        --native-fn-names="$NATIVE_FN_NAMES_FILE" \
        --native-toml="$MANIFEST_DIR/native.toml" \
        --globals-toml="$MANIFEST_DIR/globals.toml"
done

# Phase 3 (auto-manifest): cross-check the discovered entries against the
# hand-curated [[outbound_recomp]] / [[outbound_psyq]] / [[dispatch]] in
# native.toml. Diagnostic-only — discrepancies don't fail the build, but
# make it visible when the rewriter has caught up to (or surpassed) the
# hand-list.
python3 tools/diff_discovered_manifest.py "$MANIFEST_DIR" "$REWRITTEN_DIR" \
    || true

python3 tools/gen_defsyms.py     "$MANIFEST_DIR" "$GENERATED_DIR/native_aliases.cmake" \
                                 "$REWRITTEN_DIR"
python3 tools/gen_dispatch.py    "$MANIFEST_DIR" "$GENERATED_DIR/native_dispatch.c" \
                                 "$INCLUDE_DIR" "$REWRITTEN_DIR"
python3 tools/gen_thunks.py      "$MANIFEST_DIR" "$GENERATED_DIR/native_thunks.cpp" \
                                 "$INCLUDE_DIR" "$PSYQ_INCLUDE_DIR" "$REWRITTEN_DIR"
python3 tools/gen_renames.py     "$MANIFEST_DIR" "$GENERATED_DIR/native_renames.h" \
                                 "$REWRITTEN_DIR"
