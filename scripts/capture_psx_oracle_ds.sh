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
#   scripts/capture_psx_oracle_ds.sh --game spyro-3
#       (the recipe in games/spyro-3/oracle.conf: golden path, frame count,
#        disc, manifest, anchor and recorder options; any option given on the
#        command line overrides the recipe's)
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
#           --vsync-cb-pc HEX      record every call of a callback registrar
#                                  (libetc VSyncCallback, the game's slot
#                                  registrar) as a mode -2 row of <out>.vsync;
#                                  repeatable, up to 4
#           --srand-pc HEX         record every srand seed to <out>.srand
#           --watch-word HEX       record every store to one guest word to
#                                  <out>.watch (a loader's state word gives
#                                  the load windows the verifier re-aligns at)
#           --cd-irq               record every CD-ROM controller command and
#                                  interrupt to <out>.cdirq (a game that drives
#                                  the controller's registers itself replays it)
#           --cd-read-pc-min HEX   with --cd-irq: also record every register read
#                                  by an instruction at or above HEX (the game's
#                                  own controller code) with the value it saw
#           --pad-cmds             record every command byte the controllers
#                                  accept to <out>.padcmd (a libpad command
#                                  sequence's length in vsyncs, read off the port)
#           --cdsync-pc HEX        record every CdSync call (mode, return
#                                  value, call site) to <out>.cdsync: the
#                                  answers to a game's drive polls
#           --vsync-query-skip LO:HI
#                                  do not record VSync(-1) calls returning
#                                  into [LO, HI): a library's own timeout
#                                  polls (libcd's CD_sync..CD_cw), which
#                                  openpete's thunks never make; the row count
#                                  falls by two orders of magnitude
#           --sio0-word HEX        record the vblanks the kernel's controller/
#                                  card handler served (its stores to the port-
#                                  flip word HEX; 0x80007264 on the SCPH-1001
#                                  kernel) to <out>.sio0
#           --no-card              reference boots with no memory card
#           --disc PATH            (default $OPENPETE_DISC, else Spyro 1's)
#           --manifest PATH        gameplay_state_regions.toml whose rows
#                                  are hashed (default
#                                  $OPENPETE_REGIONS_MANIFEST, else Spyro 1's)
# A golden verified with its .vsync/.loads/.srand siblings beside it runs
# openpete under the reference's timing (core/diag/psxtrace_schedule.c).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=""
FRAMES=600
ANCHOR_PC="0x8001ED5C"
HIT=1
MAX_RUNTIME=""
CD_READ_PC=""; CD_READ_CB_SLOT=""; CD_CONTROL_PC=""; VSYNC_PC=""; SRAND_PC=""; WATCH_WORD=""; NO_CARD=0; CD_IRQ=0; CD_READ_PC_MIN=""; PAD_CMDS=0; CDSYNC_PC=""; SIO0_WORD=""; VSYNC_QUERY_SKIP=""
VSYNC_CB_PCS=()
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MANIFEST="${OPENPETE_REGIONS_MANIFEST:-games/spyro-1/manifest/gameplay_state_regions.toml}"

# --game SLUG: the recipe's values are the defaults; its recorder options
# are parsed first so the command line's override them (a later
# occurrence wins for every single-valued option).
GAME=""
_args=("$@")
for ((_i = 0; _i < ${#_args[@]} - 1; _i++)); do
    [[ "${_args[$_i]}" == "--game" ]] && GAME="${_args[$((_i + 1))]}"
done
if [[ -n "$GAME" ]]; then
    CONF="games/$GAME/oracle.conf"
    [[ -f "$CONF" ]] || { echo "[capture-ds] no recipe: $CONF" >&2; exit 64; }
    GOLDEN=""; CAPTURE_ARGS=()
    # shellcheck disable=SC1090
    source "$CONF"
    OUT="$GOLDEN"
    set -- "${CAPTURE_ARGS[@]}" "$@"
fi
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game) shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --disc) DISC="$2"; shift 2 ;;
        --manifest) MANIFEST="$2"; shift 2 ;;
        --frames) FRAMES="$2"; shift 2 ;;
        --anchor-pc) ANCHOR_PC="$2"; shift 2 ;;
        --hit) HIT="$2"; shift 2 ;;
        --max-runtime-frames) MAX_RUNTIME="$2"; shift 2 ;;
        --cd-read-pc) CD_READ_PC="$2"; shift 2 ;;
        --cd-read-cb-slot) CD_READ_CB_SLOT="$2"; shift 2 ;;
        --cd-control-pc) CD_CONTROL_PC="$2"; shift 2 ;;
        --vsync-pc) VSYNC_PC="$2"; shift 2 ;;
        --vsync-cb-pc) VSYNC_CB_PCS+=("$2"); shift 2 ;;
        --srand-pc) SRAND_PC="$2"; shift 2 ;;
        --watch-word) WATCH_WORD="$2"; shift 2 ;;
        --cd-irq) CD_IRQ=1; shift ;;
        --pad-cmds) PAD_CMDS=1; shift ;;
        --cdsync-pc) CDSYNC_PC="$2"; shift 2 ;;
        --sio0-word) SIO0_WORD="$2"; shift 2 ;;
        --vsync-query-skip) VSYNC_QUERY_SKIP="$2"; shift 2 ;;
        --cd-read-pc-min) CD_READ_PC_MIN="$2"; shift 2 ;;
        --no-card) NO_CARD=1; shift ;;
        -h|--help) sed -n 's/^# //p' "$0" | head -70; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done
[[ -n "$OUT" ]] || { echo "[capture-ds] --out required" >&2; exit 64; }

DS_BIN="third_party/duckstation/build-regtest/bin/duckstation-regtest"

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
for pc in "${VSYNC_CB_PCS[@]}"; do REC_ARGS+=(-psxtrace-vsync-cb-pc "$pc"); done
[[ -n "$SRAND_PC" ]] && REC_ARGS+=(-psxtrace-srand-pc "$SRAND_PC")
[[ -n "$WATCH_WORD" ]] && REC_ARGS+=(-psxtrace-watch-word "$WATCH_WORD")
[[ -n "$CD_READ_CB_SLOT" ]] && REC_ARGS+=(-psxtrace-cd-read-cb-slot "$CD_READ_CB_SLOT")
[[ -n "$CD_CONTROL_PC" ]] && REC_ARGS+=(-psxtrace-cd-control-pc "$CD_CONTROL_PC")
[[ "$CD_IRQ" == 1 ]] && REC_ARGS+=(-psxtrace-cd-irq)
[[ "$PAD_CMDS" == 1 ]] && REC_ARGS+=(-psxtrace-pad-cmds)
[[ -n "$CDSYNC_PC" ]] && REC_ARGS+=(-psxtrace-cdsync-pc "$CDSYNC_PC")
[[ -n "$SIO0_WORD" ]] && REC_ARGS+=(-psxtrace-sio0-word "$SIO0_WORD")
[[ -n "$VSYNC_QUERY_SKIP" ]] && REC_ARGS+=(-psxtrace-vsync-query-skip "$VSYNC_QUERY_SKIP")
[[ -n "$CD_READ_PC_MIN" ]] && REC_ARGS+=(-psxtrace-cd-read-pc-min "$CD_READ_PC_MIN")
[[ "$NO_CARD" == 1 ]] && REC_ARGS+=(-card1 none)

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT" "$OUT.loads" "$OUT.cdctl" "$OUT.vsync" "$OUT.srand" "$OUT.watch" "$OUT.cdirq" "$OUT.padcmd" "$OUT.cdsync" "$OUT.sio0"
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
if [[ -n "$WATCH_WORD" ]]; then
    [[ -f "$OUT.watch" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.watch" >&2; exit 1; }
    WRITES="$(sed -n 's/^# writes=\([0-9]*\).*/\1/p' "$OUT.watch")"
    if [[ -z "$WRITES" || "$WRITES" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero stores to $WATCH_WORD" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $WRITES stores to $WATCH_WORD -> $OUT.watch"
fi
if [[ "$CD_IRQ" == 1 ]]; then
    [[ -f "$OUT.cdirq" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.cdirq" >&2; exit 1; }
    CMDS="$(sed -n 's/^# commands=\([0-9]*\).*/\1/p' "$OUT.cdirq")"
    if [[ -z "$CMDS" || "$CMDS" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero CD-ROM commands" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $CMDS CD-ROM commands -> $OUT.cdirq"
fi
if [[ "$PAD_CMDS" == 1 ]]; then
    [[ -f "$OUT.padcmd" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.padcmd" >&2; exit 1; }
    CMDS="$(sed -n 's/^# commands=\([0-9]*\).*/\1/p' "$OUT.padcmd")"
    if [[ -z "$CMDS" || "$CMDS" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero controller commands" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $CMDS controller commands -> $OUT.padcmd"
fi
if [[ -n "$CDSYNC_PC" ]]; then
    [[ -f "$OUT.cdsync" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.cdsync" >&2; exit 1; }
    CALLS="$(sed -n 's/^# calls=\([0-9]*\).*/\1/p' "$OUT.cdsync")"
    if [[ -z "$CALLS" || "$CALLS" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw zero CdSync calls" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $CALLS CdSync calls -> $OUT.cdsync"
fi
if [[ -n "$SIO0_WORD" ]]; then
    [[ -f "$OUT.sio0" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.sio0" >&2; exit 1; }
    SERVED="$(sed -n 's/^# served=\([0-9]*\).*/\1/p' "$OUT.sio0")"
    if [[ -z "$SERVED" || "$SERVED" == 0 ]]; then
        echo "[capture-ds] FAIL: recorder saw no store to the SIO0 handler word $SIO0_WORD" >&2
        exit 1
    fi
    echo "[capture-ds] recorded $SERVED served vblanks -> $OUT.sio0"
fi
if [[ -n "$SRAND_PC" ]]; then
    [[ -f "$OUT.srand" ]] || { echo "[capture-ds] FAIL: recorder produced no $OUT.srand" >&2; exit 1; }
    echo "[capture-ds] recorded $(sed -n 's/^# calls=\([0-9]*\).*/\1/p' "$OUT.srand") srand calls -> $OUT.srand"
fi
