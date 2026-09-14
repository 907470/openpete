#!/usr/bin/env bash
# scripts/capture_psx_oracle_ds.sh — capture a .psxtrace from DuckStation.
#
# DuckStation supersedes PCSX-Redux as the openpete oracle: real Sony
# SCPH1001 BIOS, runs Spyro past the cyclorama (where PCSX-Redux chokes
# at frame ~120), captures at 6000+ FPS headless. The PCSX-Redux
# wrapper (scripts/capture_psx_oracle.sh) is kept for forensic single-
# shot RAM dumps.
#
# Usage:
#   scripts/capture_psx_oracle_ds.sh \
#       --out tests/golden/spyro-1/psxtrace-titlescreen-baseline.bin \
#       --frames 600
#
# Optional: --anchor-pc HEX (default 0x8001ED5C, Spyro 1 GamestateDraw)
#           --hit N (default 1)
#           --max-runtime-frames N (DuckStation -frames cap; default
#                                    --frames + 1000 safety pad)
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=""
FRAMES=600
ANCHOR_PC="0x8001ED5C"
HIT=1
MAX_RUNTIME=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --frames) FRAMES="$2"; shift 2 ;;
        --anchor-pc) ANCHOR_PC="$2"; shift 2 ;;
        --hit) HIT="$2"; shift 2 ;;
        --max-runtime-frames) MAX_RUNTIME="$2"; shift 2 ;;
        -h|--help) sed -n 's/^# //p' "$0" | head -25; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done
[[ -n "$OUT" ]] || { echo "[capture-ds] --out required" >&2; exit 64; }

DS_BIN="third_party/duckstation/build-regtest/bin/duckstation-regtest"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"

if [[ ! -x "$DS_BIN" ]]; then
    echo "[capture-ds] binary missing: $DS_BIN" >&2
    echo "[capture-ds] run scripts/setup_duckstation.sh first."     >&2
    exit 1
fi
[[ -f "$DISC" ]] || { echo "[capture-ds] disc missing: $DISC" >&2; exit 1; }

# The capture module's region table is a hand-written copy of the
# manifest; a golden captured from a drifted copy would hash different
# bytes than openpete and fail every run against it.
python3 tools/check_ds_region_mirror.py || {
    echo "[capture-ds] refusing to capture: DuckStation region mirror differs from the manifest" >&2
    exit 1
}

mkdir -p "$(dirname "$OUT")"
# DuckStation's -frames is *presentation* frames (not GamestateDraw hits).
# Pad generously: ~1000 startup + 4× the requested record count handles
# the worst case (4-vbl-per-game-frame splash logos before the title-
# screen demo loop, which is 1:1 game-to-present).
[[ -n "$MAX_RUNTIME" ]] || MAX_RUNTIME="$((1000 + FRAMES * 4))"

echo "[capture-ds] capturing $FRAMES records @ PC=$ANCHOR_PC hit=$HIT -> $OUT"
# DuckStation's Mesa GL TLS-destructor SIGABRTs on thread shutdown after
# we've already exited cleanly — psxtrace records are written before the
# crash. Run inside a wrapper sh that swallows the abort entirely so
# bash's job-control "Aborted" line doesn't leak into the wrapper's
# output.
ulimit -c 0
LIBGL_ALWAYS_SOFTWARE=1 \
MESA_LOADER_DRIVER_OVERRIDE=llvmpipe \
sh -c '"$@" 2>&1; exit 0' _ "$DS_BIN" \
    -frames "$MAX_RUNTIME" \
    -psxtrace "$OUT" \
    -psxtrace-anchor-pc "$ANCHOR_PC" \
    -psxtrace-anchor-hit "$HIT" \
    -psxtrace-frames "$FRAMES" \
    -console \
    "$DISC" \
  | grep -E "psxtrace|FPS|Exiting" \
  | tail -5
echo "[capture-ds] done"
[[ -f "$OUT" ]] || { echo "[capture-ds] FAIL: trace not produced" >&2; exit 1; }
# A golden is hashes only. A trace carrying RAM payloads (FLAG_HAS_RAM) or
# any sibling dump under tests/golden/ is deleted, not left for a later
# `git add` to find.
if [[ "$OUT" == tests/golden/* ]] && ! python3 scripts/fences/no-ram-dump.py check "$OUT"; then
    rm -f "$OUT"
    echo "[capture-ds] FAIL: refusing a RAM-carrying trace under tests/golden/ (deleted $OUT)" >&2
    exit 1
fi
python3 -m tools.psxtrace_format info "$OUT" | head -3
