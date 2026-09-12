#!/usr/bin/env bash
# scripts/capture_psx_oracle.sh — produce an aligned .psxtrace + sibling
# seed/bk2 artifacts for openpete's PCSX-Redux oracle gate.
#
# Two-stage capture (one Redux process per stage — can't run both BPs in
# the same Redux instance, see pcsx-redux-trace.lua header):
#
#   Stage 1: pcsx-redux-ram-dump.lua → <out>.seed.ram + <out>.seed.cpu
#            One-shot RAM/CPU dump on the Nth hit of the anchor PC.
#   Stage 2: pcsx-redux-trace.lua    → <out> (the .psxtrace)
#            Streaming per-frame hash records starting at the same anchor.
#
# Both stages are deterministic given the same seed input (disc + BIOS +
# input), so stage-1's t=0 state and stage-2's frame-0 record are at the
# same logical instant.
#
# Usage:
#   scripts/capture_psx_oracle.sh \
#       --out      tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin \
#       --frames   600 \
#       [--anchor-pc 0x8001ED5C] [--hit 1] [--bk2 path/to/movie.bk2]
#
# Required env / inferred:
#   OPENPETE_DISC    disc image (default games/spyro-1/spyro-1/build/SCUS-94228.cue)
#   OPENPETE_BIN     openpete built binary (only used to locate gen/regions.lua)
#   OPENPETE_BUILD   openpete CMake build dir (default build/linux-release)
#
# Stage 2 input replay: if --bk2 is given, the file is converted to a
# padstream and pcsx-redux-bk2-replay.lua is co-loaded so both engines
# can run from the same input. The .bk2 itself is copied to <out>.bk2 so
# scripts/check_psx_oracle.sh forwards it to openpete.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=""
FRAMES=600
ANCHOR_PC="0x8001ED5C"
HIT=1
BK2=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --frames) FRAMES="$2"; shift 2 ;;
        --anchor-pc) ANCHOR_PC="$2"; shift 2 ;;
        --hit) HIT="$2"; shift 2 ;;
        --bk2) BK2="$2"; shift 2 ;;
        -h|--help)
            sed -n 's/^# //p' "$0" | head -40; exit 0 ;;
        *)  echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

if [[ -z "$OUT" ]]; then
    echo "[capture] --out required" >&2; exit 64
fi

DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
BUILD="${OPENPETE_BUILD:-build/linux-release}"
REGIONS="$BUILD/games/spyro-1/gen/openpete_state_regions.gen.lua"
PCSXR=".tools/pcsx-redux/pcsx-redux"
BIOS=".tools/pcsx-redux/openbios.bin"

[[ -x "$PCSXR" ]] || { echo "[capture] missing $PCSXR" >&2; exit 1; }
[[ -f "$BIOS"  ]] || { echo "[capture] missing $BIOS"  >&2; exit 1; }
[[ -f "$DISC"  ]] || { echo "[capture] missing $DISC"  >&2; exit 1; }
[[ -f "$REGIONS" ]] || {
    echo "[capture] regions file missing: $REGIONS" >&2
    echo "          run cmake --build --preset linux-release first" >&2
    exit 1; }

mkdir -p "$(dirname "$OUT")"

PADSTREAM=""
if [[ -n "$BK2" ]]; then
    [[ -f "$BK2" ]] || { echo "[capture] bk2 missing: $BK2" >&2; exit 1; }
    PADSTREAM="${OUT}.padstream"
    echo "[capture] converting $BK2 → $PADSTREAM"
    python3 tools/bk2_to_padstream.py "$BK2" -o "$PADSTREAM" --summary
    cp "$BK2" "${OUT}.bk2"
fi

echo "[capture] stage 1: seed RAM+CPU dump @ PC=$ANCHOR_PC hit=$HIT"
SEED_RAM="${OUT}.seed.ram"
SEED_CPU="${OUT}.seed.cpu"
PCSXR_DUMP_PC="$ANCHOR_PC" \
PCSXR_DUMP_HITS="$HIT" \
PCSXR_DUMP_OUT="$SEED_RAM" \
PCSXR_DUMP_CPU="$SEED_CPU" \
${PADSTREAM:+PCSXR_PADSTREAM="$PADSTREAM"} \
${PADSTREAM:+PCSXR_PADSTREAM_PC="$ANCHOR_PC"} \
    timeout 180 "$PCSXR" --no-ui --interpreter --debugger \
        --bios "$BIOS" --iso "$DISC" \
        ${PADSTREAM:+--dofile scripts/pcsx-redux-bk2-replay.lua} \
        --run --dofile scripts/pcsx-redux-ram-dump.lua \
        > /dev/null 2>&1
[[ -f "$SEED_RAM" && -f "$SEED_CPU" ]] || {
    echo "[capture] stage 1 failed: seed files not produced" >&2
    exit 1; }
echo "[capture] stage 1 OK: $SEED_RAM ($(stat -c%s "$SEED_RAM") B), $SEED_CPU"

echo "[capture] stage 2: $FRAMES-frame trace @ PC=$ANCHOR_PC hit=$HIT"
PCSXR_TRACE_OUT="$OUT" \
PCSXR_TRACE_REGIONS="$(realpath "$REGIONS")" \
PCSXR_TRACE_FRAMES="$FRAMES" \
PCSXR_TRACE_PC="$ANCHOR_PC" \
PCSXR_TRACE_HIT="$HIT" \
${PADSTREAM:+PCSXR_PADSTREAM="$PADSTREAM"} \
${PADSTREAM:+PCSXR_PADSTREAM_PC="$ANCHOR_PC"} \
    timeout 600 "$PCSXR" --no-ui --interpreter --debugger \
        --bios "$BIOS" --iso "$DISC" \
        ${PADSTREAM:+--dofile scripts/pcsx-redux-bk2-replay.lua} \
        --run --dofile scripts/pcsx-redux-trace.lua
[[ -f "$OUT" ]] || { echo "[capture] stage 2 failed: trace not produced" >&2; exit 1; }
echo "[capture] done: $OUT ($(stat -c%s "$OUT") B)"
python3 -m tools.psxtrace_format info "$OUT" | head -3
