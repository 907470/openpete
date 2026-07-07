#!/usr/bin/env bash
# check_carve_equivalence.sh — prove disc-carved recomp inputs == the
# decomp build outputs regen consumes (distribution D2.3 gate; plan
# openpete-distribution-local-recomp).
#
# Runs `openpete-spyro1 --extract-recomp-inputs` against the reference
# disc, then byte-compares: PSX.EXE vs the decomp's build/PSX.EXE, and
# every carved overlay vs build/wad/<wad_file> (mapping from
# manifest/recomp/SCUS-94228/manifest.toml). The whole first-boot
# pipeline silently assumes this equivalence — this makes it explicit.
# Exit 0 = every byte matches; 2 = mismatch; 64 = prerequisites missing.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MAN=games/spyro-1/manifest/recomp/SCUS-94228
DECOMP=games/spyro-1/spyro-1/build
TMP=$(mktemp -d "${TMPDIR:-/tmp}/openpete-carve.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

[[ -x "$BIN" ]] || { echo "[carve] binary missing: $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[carve] disc missing: $DISC" >&2; exit 64; }
[[ -f "$MAN/manifest.toml" ]] || { echo "[carve] manifest missing" >&2; exit 64; }

"$BIN" --extract-recomp-inputs "$TMP" --disc "$DISC" >/dev/null 2>&1 \
    || { echo "[carve] extraction failed" >&2; exit 2; }

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
