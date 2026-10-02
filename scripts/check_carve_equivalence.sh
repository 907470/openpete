#!/usr/bin/env bash
# check_carve_equivalence.sh — prove disc-carved recomp inputs == the
# decomp build outputs regen consumes.
#
# Runs the resolved game binary with --extract-recomp-inputs against the
# reference disc, then byte-compares: PSX.EXE vs the decomp's build/PSX.EXE, and
# every carved overlay vs build/wad/<wad_file> (mapping from
# manifest/recomp/SCUS-94228/manifest.toml). The whole first-boot
# pipeline silently assumes this equivalence — this makes it explicit.
# Usage: scripts/check_carve_equivalence.sh [--game SLUG]
#
# Exit 0 = every byte matches; 2 = mismatch; 64 = prerequisites missing.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME" || exit 64
BIN="$PG_BIN"
DISC="$PG_DISC"
MAN="games/$PG_GAME/manifest/recomp/SCUS-94228"
DECOMP="games/$PG_GAME/$PG_GAME/build"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/openpete-carve.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# The manifest directory is named after Spyro 1's EXE id and the reference
# outputs are its decomp build tree, so --game names the game whose binary and
# disc are carved and every other slug is refused rather than compared against
# the wrong reference.
[[ "$PG_GAME" == "spyro-1" ]] ||
    { echo "[carve] SKIP: $PG_GAME has no recomp manifest" >&2; exit 64; }
[[ -x "$BIN" ]] || { echo "[carve] binary missing: $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[carve] disc missing: $DISC" >&2; exit 64; }
[[ -f "$MAN/manifest.toml" ]] || { echo "[carve] manifest missing" >&2; exit 64; }

# pg_capture deletes the log and the carved PSX.EXE first and asserts the
# latter after, so a failed extraction can never be compared as a pass
#.
pg_capture "$TMP/extract.log" "$TMP/PSX.EXE" -- \
    --extract-recomp-inputs "$TMP" \
    || { echo "[carve] extraction failed" >&2; tail -5 "$TMP/extract.log" >&2; exit 2; }

fail=0
check() {  # check <label> <carved> <reference>
    if cmp -s "$2" "$3"; then
        echo "  PASS  $1"
    else
        echo "  FAIL  $1"
        fail=1
    fi
}

check "PSX.EXE" "$TMP/PSX.EXE" "$DECOMP/PSX.EXE"

while IFS=$'\t' read -r name wad; do
    check "$name" "$TMP/$name.ovl" "$DECOMP/wad/$wad"
done < <(python3 - "$MAN/manifest.toml" <<'PYEOF'
import re, sys
toml = open(sys.argv[1]).read()
for m in re.finditer(r'\[\[overlay\]\]\nname = "([^"]+)"(?:.*?)wad_file = "([^"]+)"',
                     toml, re.S):
    print(f"{m.group(1)}\t{m.group(2)}")
PYEOF
)

if [[ $fail -ne 0 ]]; then
    echo "[carve] FAIL — carved bytes differ from the decomp build outputs"
    exit 2
fi
echo "[carve] PASS — disc-carved PSX.EXE + all overlays byte-match the decomp build"
