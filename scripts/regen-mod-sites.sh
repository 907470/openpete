#!/usr/bin/env bash
# regen-mod-sites.sh — bake enabled mods' instruction sites into the
# recompiled C (modding framework PoC rung 14, the site-hook tier).
#
# Collects [[site]] blocks from every enabled mod's mod.toml (via
# tools/gen_mod_sites.py), and — only when the collected site set differs
# from what is currently baked — re-runs the SCOPED main-EXE recomp so
# the guarded substitutions land. The site-set content is the cache key:
# re-running with an unchanged enabled list is a no-op, and disabling
# every site-hook mod regenerates byte-identical stock output (the
# inertness gate scripts/check_mods.sh asserts).
#
# Usage: scripts/regen-mod-sites.sh [--build]
#   --build  also run `cmake --build --preset linux-release` afterwards
# Env:   OPENPETE_MODS_DIR — mods directory (default: ./mods)

set -euo pipefail
cd "$(dirname "$0")/.."

MODS_DIR="${OPENPETE_MODS_DIR:-mods}"
OUT="games/spyro-1/generated"
SITES="$OUT/mod_sites.txt"
REGISTRY="$OUT/mod_sites_registry.c"

do_build=0
[[ "${1:-}" == "--build" ]] && do_build=1

mkdir -p "$OUT"
python3 tools/gen_mod_sites.py --mods-dir "$MODS_DIR" \
    --out-sites "$SITES.new" --out-registry "$REGISTRY.new"

# Cache key: the emitted site set. Treat a missing installed file as the
# empty set (stock generated C has no sites baked).
new_count=$(grep -c '^mod_' "$SITES.new" || true)
old_is_empty=1
if [[ -f "$SITES" ]] && grep -q '^mod_' "$SITES"; then old_is_empty=0; fi
if { [[ $old_is_empty == 1 && $new_count == 0 ]]; } ||
   { [[ -f "$SITES" ]] && cmp -s "$SITES.new" "$SITES"; }; then
    rm -f "$SITES.new" "$REGISTRY.new"
    echo "[mod-sites] site set unchanged ($new_count baked) — no regen needed"
    exit 0
fi

mv "$SITES.new" "$SITES"
mv "$REGISTRY.new" "$REGISTRY"
echo "[mod-sites] site set changed → scoped main-EXE regen ($new_count site(s))"
OPENPETE_REGEN_SCOPE=main_exe bash scripts/regen-recomp.sh

if [[ $do_build == 1 ]]; then
    cmake --build --preset linux-release
else
    echo "[mod-sites] done — rebuild to pick up: cmake --build --preset linux-release"
fi
