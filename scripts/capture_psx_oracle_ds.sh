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
#           --cd-read-pc HEX       record every CdRead to <out>.loads (issue
#                                  and completion stamped with record index,
#                                  VBLANK count, CPU tick, scanline); refuses
#                                  a capture that recorded zero reads
#           --cd-read-cb-slot HEX  guest word holding the game's read-complete
#                                  handler (recomp.conf CD_READ_CB_ADDR)
#           --cd-control-pc HEX    log every CdControl command to <out>.cdctl
#           --vsync-pc HEX         record every waited libetc VSync call to
#                                  <out>.vsync (the reference's VBL schedule);
#                                  refuses a capture that recorded none
#           --srand-pc HEX         record every srand seed to <out>.srand
#           --no-card              reference boots with no memory card
# A golden verified with its .vsync/.loads/.srand siblings beside it runs
# openpete under the reference's timing (core/runtime/psxtrace_schedule.c).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=""
FRAMES=600
ANCHOR_PC="0x8001ED5C"
HIT=1
MAX_RUNTIME=""
CD_READ_PC=""; CD_READ_CB_SLOT=""; CD_CONTROL_PC=""; VSYNC_PC=""; SRAND_PC=""; NO_CARD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --frames) FRAMES="$2"; shift 2 ;;
        --anchor-pc) ANCHOR_PC="$2"; shift 2 ;;
        --hit) HIT="$2"; shift 2 ;;
        --max-runtime-frames) MAX_RUNTIME="$2"; shift 2 ;;
        --cd-read-pc) CD_READ_PC="$2"; shift 2 ;;
        --cd-read-cb-slot) CD_READ_CB_SLOT="$2"; shift 2 ;;
        --cd-control-pc) CD_CONTROL_PC="$2"; shift 2 ;;
        --vsync-pc) VSYNC_PC="$2"; shift 2 ;;
        --srand-pc) SRAND_PC="$2"; shift 2 ;;
        --no-card) NO_CARD=1; shift ;;
        -h|--help) sed -n 's/^# //p' "$0" | head -34; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done
[[ -n "$OUT" ]] || { echo "[capture-ds] --out required" >&2; exit 64; }

DS_BIN="third_party/duckstation/build-regtest/bin/duckstation-regtest"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MANIFEST="${OPENPETE_REGIONS_MANIFEST:-games/spyro-1/manifest/gameplay_state_regions.toml}"

if [[ ! -x "$DS_BIN" ]]; then
    echo "[capture-ds] binary missing: $DS_BIN" >&2
    echo "[capture-ds] run scripts/setup_duckstation.sh first."     >&2
    exit 1
fi
[[ -f "$DISC" ]] || { echo "[capture-ds] disc missing: $DISC" >&2; exit 1; }

# A golden is evidence only if a rebuild from the pinned fork commit
# reproduces it: the binary's build stamp must be the commit the
# submodule pointer names, and that worktree must be clean. The pointer
# is read from the index: it moves in the same openpete commit as the
# golden, so at capture time it is staged, not yet committed.
DS_DIR="third_party/duckstation"
DS_PIN="$(git ls-files -s "$DS_DIR" | awk '{print $2}')"
DS_HEAD="$(git -C "$DS_DIR" rev-parse HEAD)"
DS_BUILD="$(strings "$DS_BIN" | grep -E "^[0-9a-f]{40}$" | head -1 || true)"
if [[ -z "$DS_BUILD" ]]; then
    echo "[capture-ds] refusing: no git hash stamped in $DS_BIN" >&2; exit 1
fi
if [[ "$DS_BUILD" != "$DS_HEAD" ]]; then
    echo "[capture-ds] refusing: binary built from $DS_BUILD, fork checkout is $DS_HEAD (rebuild the fork)" >&2; exit 1
fi
if [[ "$DS_HEAD" != "$DS_PIN" ]]; then
    echo "[capture-ds] refusing: fork checkout $DS_HEAD is not the pinned submodule commit $DS_PIN" >&2; exit 1
fi
if [[ -n "$(git -C "$DS_DIR" status --porcelain)" ]]; then
    echo "[capture-ds] refusing: fork worktree is dirty; commit it and move the submodule pointer first" >&2; exit 1
fi

# The module hashes the regions the manifest lists at this commit: the
# flat table is generated immediately before the capture, never reused.
REGIONS_FLAT="${TMPDIR:-/tmp}/openpete-capture-regions.$$.flat"
trap 'rm -f "$REGIONS_FLAT"' EXIT
python3 tools/gen_state_regions.py --manifest "$MANIFEST" --out-flat "$REGIONS_FLAT" || {
    echo "[capture-ds] refusing to capture: region table generation failed" >&2
    exit 1
}

REC_ARGS=()
[[ -n "$CD_READ_PC" ]] && REC_ARGS+=(-psxtrace-cd-read-pc "$CD_READ_PC")
[[ -n "$VSYNC_PC" ]] && REC_ARGS+=(-psxtrace-vsync-pc "$VSYNC_PC")
[[ -n "$SRAND_PC" ]] && REC_ARGS+=(-psxtrace-srand-pc "$SRAND_PC")
[[ -n "$CD_READ_CB_SLOT" ]] && REC_ARGS+=(-psxtrace-cd-read-cb-slot "$CD_READ_CB_SLOT")
[[ -n "$CD_CONTROL_PC" ]] && REC_ARGS+=(-psxtrace-cd-control-pc "$CD_CONTROL_PC")
[[ "$NO_CARD" == 1 ]] && REC_ARGS+=(-card1 none)

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT" "$OUT.loads" "$OUT.cdctl" "$OUT.vsync" "$OUT.srand"
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
    -psxtrace-regions "$REGIONS_FLAT" \
    "${REC_ARGS[@]}" \
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
python3 -m tools.psxtrace_format info "$OUT" | head -4
python3 -m tools.psxtrace_format info "$OUT" | grep -q "^scm_hash=$DS_BUILD\$" || {
    echo "[capture-ds] FAIL: trace does not carry the build hash $DS_BUILD" >&2; exit 1
}
if [[ -n "$CD_READ_PC" ]]; then
    [[ -f "$OUT.loads" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.loads" >&2; exit 1; }
    READS="$(sed -n 's/^# reads issued=\([0-9]*\).*/\1/p' "$OUT.loads")"
    if [[ -z "$READS" || "$READS" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero CdReads at $CD_READ_PC (wrong PC, or the run never loaded)" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $READS reads -> $OUT.loads"
fi
if [[ -n "$VSYNC_PC" ]]; then
    [[ -f "$OUT.vsync" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.vsync" >&2; exit 1; }
    WAITS="$(sed -n 's/^# calls=\([0-9]*\).*/\1/p' "$OUT.vsync")"
    if [[ -z "$WAITS" || "$WAITS" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero VSync calls at $VSYNC_PC (wrong PC)" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $WAITS VSync calls -> $OUT.vsync"
fi
if [[ -n "$SRAND_PC" ]]; then
    [[ -f "$OUT.srand" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.srand" >&2; exit 1; }
    echo "[capture-ds] recorded $(sed -n 's/^# calls=\([0-9]*\).*/\1/p' "$OUT.srand") srand calls -> $OUT.srand"
fi
