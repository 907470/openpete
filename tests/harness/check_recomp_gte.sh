#!/usr/bin/env bash
# tests/harness/check_recomp_gte.sh - recompiler regression test for the GTE
# data-register side effects (mtc2/lwc2 to $30 recompute LZCR; to $28
# unpack IR1-3). Assembles tests/recomp/gte_sideeffects/asm_probe.py into
# a raw binary, recompiles it with PSXRecomp, compiles the emitted C
# against the harness with the host C compiler, and runs it.
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (PSXRecomp not built).
#
# Usage: tests/harness/check_recomp_gte.sh [--build-dir=DIR] [--cc=COMPILER]
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD_DIR="build/linux-release"
CC_BIN="${CC:-cc}"
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#--build-dir=}" ;;
        --cc=*)        CC_BIN="${a#--cc=}" ;;
        -h|--help)     sed -n 's/^# //p' "$0" | head -12; exit 0 ;;
        *) echo "[recomp-gte] unknown arg: $a" >&2; exit 64 ;;
    esac
done

PSXRECOMP="third_party/psxrecomp/build/PSXRecomp"
if [[ ! -x "$PSXRECOMP" ]]; then
    echo "[recomp-gte] SKIP: $PSXRECOMP not built"
    exit 64
fi

SRC="tests/recomp/gte_sideeffects"
OUT="$BUILD_DIR/tests/recomp_gte"
rm -rf "$OUT"
mkdir -p "$OUT"

python3 "$SRC/asm_probe.py" "$OUT/probe.bin"
"$PSXRECOMP" "$OUT/probe.bin" --raw-bin --load-addr=0x80010000 \
    --output-dir="$OUT" --output-stem=gteprobe --func-prefix=gteprobe_ \
    --forced-entry=0x80010000 --forced-entry=0x80010040 \
    --forced-entry=0x80010080 --forced-entry=0x800100C0 \
    --forced-entry=0x80010100 > "$OUT/recomp.log" 2>&1 || {
    echo "[recomp-gte] FAIL: PSXRecomp failed; see $OUT/recomp.log" >&2
    exit 1
}
[[ -f "$OUT/gteprobe_full.c" ]] || { echo "[recomp-gte] FAIL: no gteprobe_full.c emitted" >&2; exit 1; }

"$CC_BIN" -std=c11 -O1 -Wall -Wno-unused-label -Wno-unused-function \
    -I core/include/psxrt \
    "$SRC/harness.c" "$OUT/gteprobe_full.c" -o "$OUT/gte_test"
"$OUT/gte_test"
