#!/usr/bin/env bash
# scripts/setup.sh — first-time setup gate for OpenPete.
#
# Runs three checks before allowing the project to build:
#   1. Disc image exists at the expected location and matches the known SHA256.
#   2. The Spyro 1 matching-decomp submodule has been built at least once
#      (psx.map produced) — psx.map drives the recompiler manifest in Phase 1a.
#   3. Submodules are initialized.
#
# Returns exit code 3 (OPENPETE_EXIT_SETUP_ERROR) on any failure.

set -uo pipefail

OPENPETE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISC_BIN_DEFAULT="<spyro>/roms/SCUS-94228/SCUS-94228.bin"
EXPECTED_SHA256="95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f"

DISC_BIN="${OPENPETE_DISC:-$DISC_BIN_DEFAULT}"

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

errs=0

echo "==> Disc image"
if [[ ! -f "$DISC_BIN" ]]; then
    red "  missing: $DISC_BIN"
    red "  override with OPENPETE_DISC=/path/to/SCUS-94228.bin"
    errs=$((errs+1))
else
    actual=$(sha256sum "$DISC_BIN" | awk '{print $1}')
    if [[ "$actual" == "$EXPECTED_SHA256" ]]; then
        green "  ok: $DISC_BIN ($actual)"
    else
        red   "  sha256 mismatch:"
        red   "    expected: $EXPECTED_SHA256"
        red   "    actual:   $actual"
        errs=$((errs+1))
    fi
fi

echo
echo "==> Submodules"
required=(
    games/spyro-1/spyro-1
    third_party/PsyCross
    third_party/SDL
    third_party/cubeb
)
for path in "${required[@]}"; do
    if [[ -d "$OPENPETE_ROOT/$path" ]] && [[ -n "$(ls -A "$OPENPETE_ROOT/$path" 2>/dev/null)" ]]; then
        green "  ok: $path"
    else
        red "  missing or empty: $path  (run: git submodule update --init --recursive)"
        errs=$((errs+1))
    fi
done

echo
echo "==> Spyro 1 matching-decomp build artifacts"
SPYRO1_MAP="$OPENPETE_ROOT/games/spyro-1/spyro-1/build/psx.map"
SPYRO1_EXE="$OPENPETE_ROOT/games/spyro-1/spyro-1/build/PSX.EXE"
if [[ -f "$SPYRO1_MAP" ]]; then
    green "  ok: $SPYRO1_MAP"
else
    yellow "  missing: $SPYRO1_MAP"
    yellow "  Build the matching decomp at least once:"
    yellow "    cd games/spyro-1/spyro-1 && make"
    yellow "  (Phase 1a needs the map; Phase 0b builds without it.)"
fi
if [[ -f "$SPYRO1_EXE" ]]; then
    green "  ok: $SPYRO1_EXE"
else
    yellow "  missing: $SPYRO1_EXE  (Phase 1b will need this)"
fi

echo
if [[ $errs -gt 0 ]]; then
    red "setup: $errs hard error(s)"
    exit 3
fi
green "setup: ok"
