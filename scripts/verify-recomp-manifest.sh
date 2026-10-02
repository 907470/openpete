#!/usr/bin/env bash
# verify-recomp-manifest.sh — prove the committed converged manifests
# reproduce the multi-pass regen output with a SINGLE psxrecomp pass per
# binary.
#
# For each binary: run PSXRecomp once with manifest/recomp/<sku>/'s funcs
# list, apply the same dispatch-rename regen applies, and byte-diff the
# generated C against games/spyro-1/generated/. Any diff = the manifest
# under-captures discovery = the first-boot single-pass path would build
# a different game. Exit 0 only if every binary is byte-identical.
set -euo pipefail
cd "$(dirname "$0")/.."

PSXRECOMP=third_party/psxrecomp/build/PSXRecomp
OUT=games/spyro-1/generated
MAN=games/spyro-1/manifest/recomp/SCUS-94228
PSYQ=games/spyro-1/manifest/psyq.toml
EXE=games/spyro-1/spyro-1/build/PSX.EXE
TMP=$(mktemp -d "${TMPDIR:-/tmp}/openpete-manifest-verify.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

[[ -x "$PSXRECOMP" ]] || { echo "[verify-manifest] PSXRecomp not built" >&2; exit 64; }
[[ -f "$MAN/manifest.toml" ]] || { echo "[verify-manifest] no manifest — run scripts/emit-recomp-manifest.sh" >&2; exit 64; }

fail=0

check() {  # check <label> <regen_file> <singlepass_file>
    if cmp -s "$2" "$3"; then
        echo "  PASS  $1"
    else
        echo "  FAIL  $1 ($(diff "$2" "$3" | head -1))"
        fail=1
    fi
}

# --- main EXE -------------------------------------------------------------
mkdir -p "$TMP/main_exe"
"$PSXRECOMP" "$EXE" \
    --output-dir="$TMP/main_exe" \
    --external-symbols="$PSYQ" \
    --external-prefix=psyq_ \
    --extra-funcs="$MAN/main_exe.funcs.txt" \
    --channel-sites="$OUT/channel_sites.txt" \
    --skip-on-error >/dev/null
sed -i 's/\bpsx_dispatch_compiled\b/psx_dispatch_main_exe/g' "$TMP/main_exe/PSX.EXE_dispatch.c"
check "main_exe (full)"     "$OUT/PSX.EXE_full.c"     "$TMP/main_exe/PSX.EXE_full.c"
check "main_exe (dispatch)" "$OUT/PSX.EXE_dispatch.c" "$TMP/main_exe/PSX.EXE_dispatch.c"

# --- overlays ---------------------------------------------------------------
# Boot-shaped invocation: raw .ovl straight from the WAD dir + --raw-bin,
# exactly what the first-boot pipeline runs against carved segments.
WAD_DIR=games/spyro-1/spyro-1/build/wad
while IFS=$'\t' read -r name wad; do
    mkdir -p "$TMP/$name"
    "$PSXRECOMP" "$WAD_DIR/$wad" \
        --raw-bin --load-addr=0x8007AA38 --output-stem="$name.bin" \
        --output-dir="$TMP/$name" \
        --external-symbols="$PSYQ" \
        --external-prefix=psyq_ \
        --func-prefix="func_${name}_" \
        --extra-funcs="$MAN/$name.funcs.txt" \
        --skip-on-error >/dev/null
    sed -i "s/\bpsx_dispatch_compiled\b/psx_dispatch_${name}/g" \
        "$TMP/$name/$name.bin_dispatch.c"
    check "$name (full)"     "$OUT/$name/$name.bin_full.c"     "$TMP/$name/$name.bin_full.c"
    check "$name (dispatch)" "$OUT/$name/$name.bin_dispatch.c" "$TMP/$name/$name.bin_dispatch.c"
done < <(python3 - "$MAN/manifest.toml" <<'PYEOF'
import re, sys
toml = open(sys.argv[1]).read()
for m in re.finditer(r'\[\[overlay\]\]\nname = "([^"]+)"(?:.*?)wad_file = "([^"]+)"',
                     toml, re.S):
    print(f"{m.group(1)}\t{m.group(2)}")
PYEOF
)

if [[ $fail -ne 0 ]]; then
    echo "[verify-manifest] FAIL — manifest does not reproduce the regen output"
    exit 2
fi
echo "[verify-manifest] PASS — single-pass ≡ multi-pass for every binary"
