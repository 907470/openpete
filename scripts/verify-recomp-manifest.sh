#!/usr/bin/env bash
# verify-recomp-manifest.sh - prove the committed converged manifests
# reproduce the multi-pass regen output with a SINGLE psxrecomp pass per
# binary.
#
# For each binary: run PSXRecomp once with manifest/recomp/<sku>/'s funcs
# list, apply the same dispatch-rename regen applies, and byte-diff the
# generated C against games/<game>/generated/. Any diff = the manifest
# under-captures discovery = the first-boot single-pass path would build
# a different game. Exit 0 only if every binary is byte-identical.
#
#   GAME=spyro-2 bash scripts/verify-recomp-manifest.sh   # default spyro-1
set -euo pipefail
cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
# shellcheck source=/dev/null
source "games/$GAME/recomp.conf"
SERIAL="${EXE_FILENAME/_/-}"; SERIAL="${SERIAL/./}"
PSXRECOMP=third_party/psxrecomp/build/PSXRecomp
OUT=games/$GAME/generated
MAN=games/$GAME/manifest/recomp/$SERIAL
PSYQ=games/$GAME/manifest/psyq.toml
EXE=$PSX_EXE
TMP=$(mktemp -d "${TMPDIR:-/tmp}/openpete-manifest-verify.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

[[ -x "$PSXRECOMP" ]] || { echo "[verify-manifest] PSXRecomp not built" >&2; exit 64; }
[[ -f "$MAN/manifest.toml" ]] || { echo "[verify-manifest] no manifest - run scripts/emit-recomp-manifest.sh" >&2; exit 64; }

fail=0

# The base-redirect window applies to every binary (recomp.conf SONY_IMAGE_WINDOW,
# recorded in the manifest so the first-boot build passes it too).
window=()
win_line=$(grep -m1 '^sony_image_window = ' "$MAN/manifest.toml" || true)
if [[ -n "$win_line" ]]; then
    win=${win_line#sony_image_window = \"}; win=${win%\"}
    window=(--sony-image-window="$win")
fi

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
channel=()
if grep -qx 'channel_sites = true' "$MAN/manifest.toml"; then
    channel=(--channel-sites="$OUT/channel_sites.txt")
fi
"$PSXRECOMP" "$EXE" \
    --output-dir="$TMP/main_exe" \
    --external-symbols="$PSYQ" \
    --external-prefix=psyq_ \
    --extra-funcs="$MAN/main_exe.funcs.txt" \
    "${channel[@]}" \
    "${window[@]}" \
    --skip-on-error >/dev/null
sed -i 's/\bpsx_dispatch_compiled\b/psx_dispatch_main_exe/g' "$TMP/main_exe/PSX.EXE_dispatch.c"
check "main_exe (full)"     "$OUT/PSX.EXE_full.c"     "$TMP/main_exe/PSX.EXE_full.c"
check "main_exe (dispatch)" "$OUT/PSX.EXE_dispatch.c" "$TMP/main_exe/PSX.EXE_dispatch.c"

# --- overlays ---------------------------------------------------------------
# Boot-shaped invocation: raw .ovl straight from the WAD dir + --raw-bin and
# the single-pass main EXE's leaf summary, exactly what the first-boot
# pipeline (games/common/recomp_build.cpp) runs against carved segments.
while IFS=$'\t' read -r name wad load; do
    mkdir -p "$TMP/$name"
    "$PSXRECOMP" "$WAD_DIR/$wad" \
        --raw-bin --load-addr="$load" --output-stem="$name.bin" \
        --output-dir="$TMP/$name" \
        --external-symbols="$PSYQ" \
        --external-prefix=psyq_ \
        --func-prefix="func_${name}_" \
        --extra-funcs="$MAN/$name.funcs.txt" \
        --leaf-summary="$TMP/main_exe/PSX.EXE_leaves.tsv" \
        "${window[@]}" \
        --skip-on-error >/dev/null
    sed -i "s/\bpsx_dispatch_compiled\b/psx_dispatch_${name}/g" \
        "$TMP/$name/$name.bin_dispatch.c"
    check "$name (full)"     "$OUT/$name/$name.bin_full.c"     "$TMP/$name/$name.bin_full.c"
    check "$name (dispatch)" "$OUT/$name/$name.bin_dispatch.c" "$TMP/$name/$name.bin_dispatch.c"
done < <(python3 - "$MAN/manifest.toml" <<'PYEOF'
import re, sys
toml = open(sys.argv[1]).read()
for m in re.finditer(r'\[\[overlay\]\]\nname = "([^"]+)"(?:.*?)load_addr = (\S+)(?:.*?)wad_file = "([^"]+)"',
                     toml, re.S):
    print(f"{m.group(1)}\t{m.group(3)}\t{m.group(2)}")
PYEOF
)

if [[ $fail -ne 0 ]]; then
    echo "[verify-manifest] FAIL - manifest does not reproduce the regen output"
    exit 2
fi
echo "[verify-manifest] PASS - single-pass ≡ multi-pass for every binary"
