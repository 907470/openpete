#!/usr/bin/env bash
# check_tercache_vram.sh - the terrain arenas under real device-memory
# exhaustion, on a real GPU.
#
# rhi_create_vertex_buffer_device attempts each arena on every host-visible,
# host-coherent memory type the buffer accepts, DEVICE_LOCAL types first, and
# keeps the buffer on the first type that allocates. check_tercache_alloc.sh
# proves the candidate order and the init-failure path with
# OPENPETE_RHI_FAIL_MEMTYPE, which refuses a type index inside the engine and
# never reaches the driver. This harness produces the condition the retry
# exists for: OPENPETE_RHI_BALLOON holds device memory at boot until the
# driver itself refuses further allocations in a heap, so the refusal the
# arenas meet is the driver's, with the driver's code.
#
# The balloon runs after the RHI's own images, buffers and staging rings are
# resident, so the renderer comes up and only what it allocates afterwards -
# the terrain arenas at level load - meets the filled heap.
#
# Legs (--leg selects one; all run by default):
#   probe          no balloon. Dumps the device's memory table
#                  ([rhi/vk] memory type <i>: flags <f> heap <h>), from which
#                  the other legs pick their heaps, and carries the retained
#                  cache's census so the margin legs can test whether the
#                  level-edge service point (svc=) exists yet.
#   bar-full       fill the heap of the first DEVICE_LOCAL|HOST_VISIBLE|
#                  HOST_COHERENT type. Every arena's attempt on that heap's
#                  type is refused by the driver, every arena lands on a type
#                  without DEVICE_LOCAL, the cache does not report init
#                  failed, and the census reports sectors > 0: the reported
#                  failure reproduced and survived.
#   all-full       every candidate refused: the DEVICE_LOCAL host-visible
#                  heap filled by the balloon, so its type is refused by the
#                  driver, and every other host-visible host-coherent type
#                  refused by OPENPETE_RHI_FAIL_MEMTYPE inside the engine,
#                  which takes the whole list. A device carries more than one
#                  such fallback (a HOST_CACHED twin of the plain coherent
#                  type), and the retry walks all of them, so one refused type
#                  leaves the arenas allocating on the next. No candidate can
#                  allocate, the cache reports init failed, the CPU terrain
#                  walk draws, and the run reaches its frame limit with
#                  exit 0.
#
#                  The system heap is refused by injection rather than filled
#                  because filling it commits the machine's own RAM: on a WSL
#                  box the host and the guest draw from one pool, so a balloon
#                  the size of a host-visible system heap pages the host
#                  before it ever reaches a refusal. One real refusal
#                  (the driver's, on the heap the arenas prefer) and one
#                  injected refusal (the fallback type) cover the same path.
#   edge-margin    fill-256 on the DEVICE_LOCAL|HOST_VISIBLE heap: the init
#                  allocation fits in the margin, the level-edge replacement's
#                  fresh pairs do not, and they land on the system-memory type
#                  while the old pairs retire.
#   overflow-margin  fill-128 with OPENPETE_TC_INITIAL_CAP=16384: the init
#                  allocation (the union row) fits in the margin, the
#                  level-edge pairs and the overflow buffers do not and land
#                  on the system-memory type through the retry, overflow
#                  buffers install, and afail stops rising (it counts the
#                  claims made before each overflow landed, so it is flat at
#                  the end, not zero).
# The last two need the arena replacement and the overflow buffers; they
# report SKIP while the census line carries no svc= field.
#
# Engagement: every leg prints the MiB its balloon holds and FAILS
# at 0 - a leg whose balloon allocated nothing refused nothing and measured
# nothing.
#
# Usage:
#   scripts/check_tercache_vram.sh [--game SLUG] [--exe PATH]
#                                  [--build-dir=DIR] [--out DIR] [--leg NAME]
#   --exe   the executable to drive (default: the windows-mingw build; the
#           real GPU is the machine this check needs)
# Exit 0 PASS, 1 FAIL, 64 SKIP (no display, no exe, no disc, no memory table,
# or no requested leg ran with a balloon holding memory). PASS requires at
# least one engaged leg.
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

# Unattended launcher: nobody is holding the pad and the buzzing appears in no
# log, so every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

WIN_BUILD_DIR="build/windows-mingw"
EXE=""
OUT=""
LEG=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) WIN_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
        --leg=*)       LEG="${a#--leg=}";;
    esac
done
prev=""
for a in "$@"; do
    case "$prev" in
        --exe) EXE="$a";;
        --out) OUT="$a";;
        --leg) LEG="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# The level the arenas are sized against, and a frame limit past its load and
# well into the walk, so the census reports a built cache rather than an empty
# one.
LEVEL=artisans
FRAME_LIMIT=2610
PROBE_FRAME_LIMIT=400
LEG_RUNTIME=900
LEG_TIMEOUT=960

[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "tercache-vram: SKIP (no display)"; exit 64; }

[[ -n "$EXE" ]] || EXE="$WIN_BUILD_DIR/games/$GAME/$(pg_bin_name "$GAME").exe"
[[ -x "$EXE" ]] || { echo "tercache-vram: SKIP (no exe at $EXE)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-vram: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$WIN_BUILD_DIR/tercache-vram/$GAME"
mkdir -p "$OUT"

# A Windows child resolves a path against the Windows view of the filesystem
# and does not follow a link anywhere in it, so every path handed to a .exe is
# converted and the disc is resolved to its link-free absolute form first.
IS_EXE=0
case "$EXE" in *.exe) IS_EXE=1;; esac
hostpath() {
    if [[ "$IS_EXE" == 1 ]]; then wslpath -w "$1"; else echo "$1"; fi
}

DISC_DIR="$(cd "$(dirname "$PG_DISC")" && pwd -P)"
DISC_ABS="$DISC_DIR/$(basename "$PG_DISC")"
# A cue whose FILE names a bin that is not beside it loads under the Linux
# build (which resolves the sibling by the cue's own stem) but aborts part way
# through the boot under the Windows one. Stage a cue naming the sibling that
# is actually there, hard-linked so no disc image is copied.
CUE_FILE="$(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\([^"]*\)".*/\1/p' "$DISC_ABS" | head -1)"
if [[ -n "$CUE_FILE" && ! -f "$DISC_DIR/$CUE_FILE" ]]; then
    STEM="$(basename "$DISC_ABS" .cue)"
    [[ -f "$DISC_DIR/$STEM.bin" ]] ||
        { echo "tercache-vram: SKIP (cue names $CUE_FILE, absent, and no $STEM.bin beside it)"; exit 64; }
    STAGE="$OUT/disc"
    mkdir -p "$STAGE"
    [[ -f "$STAGE/$STEM.bin" ]] || ln "$DISC_DIR/$STEM.bin" "$STAGE/$STEM.bin" 2>/dev/null ||
        { echo "tercache-vram: SKIP (cannot stage $STEM.bin beside a cue)"; exit 64; }
    sed "s|\"$CUE_FILE\"|\"$STEM.bin\"|" "$DISC_ABS" > "$STAGE/$STEM.cue"
    DISC_ABS="$(cd "$STAGE" && pwd -P)/$STEM.cue"
fi
DISC_ARG="$(hostpath "$DISC_ABS")"

# One leg: a windowed run with the balloon spec its name carries. The log is
# deleted before the run and asserted after it, so a stale log from
# an earlier build can never be read as this run's result. stdin is closed so
# a child cannot consume the caller's input.
run_leg() {  # tag balloon-spec frame-limit level extra-env...
    local tag="$1" balloon="$2" limit="$3" level="$4"; shift 4
    local log="$OUT/$tag.log"
    rm -f "$log"
    local -a envs=(OPENPETE_VIBRATION=0 OPENPETE_TERRAIN_GPU_DBG=1 "$@")
    [[ -n "$balloon" ]] && envs+=("OPENPETE_RHI_BALLOON=$balloon")
    wslenv_forward "${envs[@]}"
    timeout "$LEG_TIMEOUT" env "${WSLENV_ARGS[@]}" "$EXE" \
        --disc "$DISC_ARG" --no-config --no-card --mute 1 \
        --vibration 0 --input-lock --perf-stats off --osd off \
        --renderer vk --window 664x448 --vsync 0 \
        --skip-to-level "$level" --frame-limit "$limit" \
        --max-runtime-seconds "$LEG_RUNTIME" \
        < /dev/null > "$log" 2>&1
    local rc=$?
    tr -d '\r' < "$log" > "$log.txt" && mv "$log.txt" "$log"
    [[ -s "$log" ]] || { echo "  leg $tag produced no log"; return 1; }
    return $rc
}

# Counters read off one leg's log.
n_of() { grep -c "$1" "$2" 2>/dev/null || true; }

# MiB the balloon reports holding across every spec of a leg. Zero is VACUOUS.
balloon_mib() {  # log
    awk '/balloon heap [0-9]+ type [0-9]+:/ { for (i = 1; i <= NF; i++)
             if ($(i+1) == "MiB") s += $i } END { print s + 0 }' "$1"
}

# Hex-string to integer inside awk, so the memory table's flag words parse
# under any awk on the box.
AWK_HEX='function hex(s,   i, c, v, d) { v = 0
    for (i = 1; i <= length(s); i++) { c = tolower(substr(s, i, 1))
        d = index("0123456789abcdef", c) - 1
        if (d >= 0) v = v * 16 + d }
    return v }
function bit(v, b) { return int(v / b) % 2 }
'

fail=0
ran=0        # a requested leg reached its launch
engaged=0    # a leg ran with a balloon holding more than 0 MiB
structural=0 # a leg skipped because this device cannot carry it
pending=0    # a leg skipped because the build lacks the mechanism it tests
want() { [[ -z "$LEG" || "$LEG" == "$1" ]]; }

# ---- probe: the device's memory table, and a census to read svc= from ------
run_leg probe "" "$PROBE_FRAME_LIMIT" "$LEVEL"
rc_probe=$?
[[ "$rc_probe" -eq 0 ]] || echo "[tercache-vram] leg=probe rc=$rc_probe (the memory table is still read from its log)"
MEMTAB="$(grep -o 'memory type [0-9]*: flags [0-9a-f]* heap [0-9]*' "$OUT/probe.log" | sort -u)"
[[ -n "$MEMTAB" ]] || { echo "tercache-vram: SKIP (no memory table in $OUT/probe.log)"; exit 64; }
echo "$MEMTAB" | sed 's/^/[tercache-vram]   /'

# The first type carrying DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (flags 0x7)
# and its heap: the type an arena is attempted on first, and the heap the
# balloon fills.
BAR_HEAP="$(echo "$MEMTAB" | awk "$AWK_HEX"'{ f = hex($5)
    if (bit(f, 1) && bit(f, 2) && bit(f, 4) && !done) { print $7; done = 1 } }')"
BAR_TYPE="$(echo "$MEMTAB" | awk "$AWK_HEX"'{ f = hex($5)
    if (bit(f, 1) && bit(f, 2) && bit(f, 4) && !done) { sub(":", "", $3); print $3; done = 1 } }')"
# Every fallback the arenas can land on once the BAR heap is full: each
# host-visible host-coherent type drawing from any other heap, in table order.
# A device offers more than one (a HOST_CACHED twin of the plain coherent
# type, for instance), and the retry walks all of them, so all of them are
# refused.
SYS_TYPES="$(echo "$MEMTAB" | awk -v bar="${BAR_HEAP:--1}" "$AWK_HEX"'{ f = hex($5)
    if (bit(f, 2) && bit(f, 4) && ($7 + 0) != (bar + 0)) {
        sub(":", "", $3); printf "%s%s", sep, $3; sep = "," } }')"
# Self-check on the two parses above, computed independently in shell
# arithmetic: the set of host-visible host-coherent types outside the BAR heap
# must be exactly what the parse yielded, and a table carrying a DEVICE_LOCAL
# host-visible type must yield a barType. A parse that drops one would turn a
# leg into a silent SKIP, or leave a candidate the retry can still allocate
# on, which is what this check refuses.
sys_expected=""
bar_expected=0
while read -r _ _ idx _ fl _ hp; do
    [[ -n "${fl:-}" && -n "${hp:-}" ]] || continue
    idx="${idx%:}"
    v=$((16#$fl))
    (( (v & 7) == 7 )) && bar_expected=1
    if (( (v & 6) == 6 )) && [[ -z "$BAR_HEAP" || "$hp" != "$BAR_HEAP" ]]; then
        sys_expected="${sys_expected}${sys_expected:+,}$idx"
    fi
done <<< "$MEMTAB"

parse_bad=0
if [[ "$bar_expected" -eq 1 && -z "$BAR_TYPE" ]]; then
    echo "[tercache-vram] FAIL (the memory table carries a DEVICE_LOCAL host-visible type, the parse yielded none):"
    echo "$MEMTAB" | sed 's/^/[tercache-vram]   /'
    parse_bad=1
    fail=1
fi
if [[ "$sys_expected" != "$SYS_TYPES" ]]; then
    echo "[tercache-vram] FAIL (the memory table's host-visible coherent types outside heap ${BAR_HEAP:-none} are" \
         "[${sys_expected:-none}], the parse yielded [${SYS_TYPES:-none}]):"
    echo "$MEMTAB" | sed 's/^/[tercache-vram]   /'
    parse_bad=1
    fail=1
fi

# The margin legs need the level-edge service point, which the census names.
HAS_SVC=0
grep -q '\[ter-gpu f=[0-9]*\].*svc=' "$OUT/probe.log" && HAS_SVC=1
echo "[tercache-vram] probe: barHeap=${BAR_HEAP:-none} barType=${BAR_TYPE:-none}" \
     "sysTypes=[${SYS_TYPES:-none}] svc=$HAS_SVC"

# ---- bar-full: the device-local host-visible heap is full ------------------
if want bar-full; then
    ran=1
    if [[ -z "$BAR_HEAP" ]]; then
        echo "[tercache-vram] leg=bar-full SKIP (no DEVICE_LOCAL host-visible type on this device)"
        structural=1
    else
        run_leg bar-full "$BAR_HEAP:fill" "$FRAME_LIMIT" "$LEVEL"
        rc=$?
        held=$(balloon_mib "$OUT/bar-full.log")
        [[ "$held" -ge 1 ]] && engaged=$((engaged + 1))
        n_fail=$(n_of 'device vertex buffer [0-9]* B: type [0-9]* failed' "$OUT/bar-full.log")
        n_ok=$(n_of 'device vertex buffer [0-9]* B: type [0-9]* flags' "$OUT/bar-full.log")
        # An arena that landed on a DEVICE_LOCAL type did not fall through.
        n_ok_dl=$(grep -o 'device vertex buffer [0-9]* B: type [0-9]* flags [0-9a-f]*' "$OUT/bar-full.log" |
                  awk "$AWK_HEX"'{ if (bit(hex($9), 1)) n++ } END { print n + 0 }')
        n_initfail=$(n_of '\[ter-gpu\] init failed' "$OUT/bar-full.log")
        secs=$(grep -o '\[ter-gpu f=[0-9]*\] sectors=[0-9]*' "$OUT/bar-full.log" | tail -1 | sed 's/.*sectors=//')
        echo "[tercache-vram] leg=bar-full rc=$rc balloonMiB=$held failedAttempts=$n_fail" \
             "allocated=$n_ok onDeviceLocal=$n_ok_dl initFailed=$n_initfail sectors=${secs:-0}"
        grep -m1 'balloon heap' "$OUT/bar-full.log" | sed 's/^/[tercache-vram]   /'
        grep -m2 'device vertex buffer' "$OUT/bar-full.log" | sed 's/^/[tercache-vram]   /'
        [[ "$rc" -eq 0 ]] || { echo "[tercache-vram] leg=bar-full FAIL (exit $rc)"; fail=1; }
        if [[ "$held" -lt 1 ]]; then
            echo "[tercache-vram] leg=bar-full VACUOUS (the balloon held 0 MiB)"; fail=1
        fi
        [[ "$n_fail" -ge 1 ]] ||
            { echo "[tercache-vram] leg=bar-full FAIL (no arena attempt was refused)"; fail=1; }
        [[ "$n_ok" -ge 1 ]] ||
            { echo "[tercache-vram] leg=bar-full FAIL (no arena allocated)"; fail=1; }
        [[ "$n_ok_dl" -eq 0 ]] ||
            { echo "[tercache-vram] leg=bar-full FAIL ($n_ok_dl arena(s) allocated on a DEVICE_LOCAL type)"; fail=1; }
        [[ "$n_initfail" -eq 0 ]] ||
            { echo "[tercache-vram] leg=bar-full FAIL (the cache reported init failed)"; fail=1; }
        [[ "${secs:-0}" -gt 0 ]] ||
            { echo "[tercache-vram] leg=bar-full FAIL (census reported no retained sectors)"; fail=1; }
    fi
fi

# ---- all-full: every candidate refused, one by the driver, one injected ---
if want all-full; then
    ran=1
    if [[ -z "$BAR_HEAP" || -z "$SYS_TYPES" ]]; then
        echo "[tercache-vram] leg=all-full SKIP (device has no DEVICE_LOCAL host-visible heap or no other host-visible type)"
        structural=1
    else
        run_leg all-full "$BAR_HEAP:fill" "$FRAME_LIMIT" "$LEVEL" \
            "OPENPETE_RHI_FAIL_MEMTYPE=$SYS_TYPES"
        rc=$?
        held=$(balloon_mib "$OUT/all-full.log")
        [[ "$held" -ge 1 ]] && engaged=$((engaged + 1))
        refusal=$(grep -o 'balloon heap [0-9]* type [0-9]*: .*refusal -\?[0-9]*' "$OUT/all-full.log" |
                  tail -1 | sed 's/.*refusal //')
        n_fail=$(n_of 'device vertex buffer [0-9]* B: type [0-9]* failed' "$OUT/all-full.log")
        n_fail_bar=$(n_of "device vertex buffer [0-9]* B: type $BAR_TYPE failed" "$OUT/all-full.log")
        # Every candidate the retry can still reach is refused, one type at a
        # time: a type left out is a type an arena allocates on, and the leg
        # then measures a fallback rather than exhaustion.
        sys_report=""
        sys_missing=""
        for t in ${SYS_TYPES//,/ }; do
            c=$(n_of "device vertex buffer [0-9]* B: type $t failed (-2)" "$OUT/all-full.log")
            sys_report="${sys_report}${sys_report:+,}$t:$c"
            [[ "$c" -ge 1 ]] || sys_missing="${sys_missing}${sys_missing:+,}$t"
        done
        n_ok=$(n_of 'device vertex buffer [0-9]* B: type [0-9]* flags' "$OUT/all-full.log")
        n_initfail=$(n_of '\[ter-gpu\] init failed' "$OUT/all-full.log")
        n_limit=$(n_of 'reason=frame-limit frames=' "$OUT/all-full.log")
        echo "[tercache-vram] leg=all-full rc=$rc balloonMiB=$held refusal=${refusal:-0}" \
             "failedAttempts=$n_fail onBarType=$n_fail_bar onSysTypes=[$sys_report]" \
             "allocated=$n_ok initFailed=$n_initfail frameLimit=$n_limit"
        grep -m2 'balloon heap' "$OUT/all-full.log" | sed 's/^/[tercache-vram]   /'
        grep -m2 'device vertex buffer' "$OUT/all-full.log" | sed 's/^/[tercache-vram]   /'
        grep -m1 '\[ter-gpu\] init failed' "$OUT/all-full.log" | sed 's/^/[tercache-vram]   /'
        [[ "$rc" -eq 0 ]] || { echo "[tercache-vram] leg=all-full FAIL (exit $rc)"; fail=1; }
        if [[ "$held" -lt 1 ]]; then
            echo "[tercache-vram] leg=all-full VACUOUS (the balloon held 0 MiB)"; fail=1
        fi
        if [[ "$n_fail" -lt 1 ]]; then
            echo "[tercache-vram] leg=all-full VACUOUS (no arena attempt was refused)"; fail=1
        fi
        # The balloon stopped on the driver's refusal, not on its block cap: a
        # capped balloon leaves free space and the heap is not actually full.
        [[ -n "${refusal:-}" && "$refusal" -ne 0 ]] ||
            { echo "[tercache-vram] leg=all-full FAIL (the balloon reached no driver refusal)"; fail=1; }
        [[ "$n_fail_bar" -ge 1 ]] ||
            { echo "[tercache-vram] leg=all-full FAIL (no attempt on type $BAR_TYPE was refused by the driver)"; fail=1; }
        [[ -z "$sys_missing" ]] ||
            { echo "[tercache-vram] leg=all-full FAIL (no injected refusal on type(s) $sys_missing)"; fail=1; }
        [[ "$n_ok" -eq 0 ]] ||
            { echo "[tercache-vram] leg=all-full FAIL ($n_ok arena(s) allocated with every candidate refused)"; fail=1; }
        [[ "$n_initfail" -ge 1 ]] ||
            { echo "[tercache-vram] leg=all-full FAIL (the cache did not report init failed)"; fail=1; }
        [[ "$n_limit" -ge 1 ]] ||
            { echo "[tercache-vram] leg=all-full FAIL (the run did not reach its frame limit)"; fail=1; }
    fi
fi

# ---- edge-margin: the level edge's fresh pairs do not fit ------------------
if want edge-margin; then
    ran=1
    if [[ "$HAS_SVC" -eq 0 ]]; then
        echo "[tercache-vram] leg=edge-margin SKIP (no level-edge service point: the census carries no svc=)"
        pending=1
    elif [[ -z "$BAR_HEAP" ]]; then
        echo "[tercache-vram] leg=edge-margin SKIP (no DEVICE_LOCAL host-visible type on this device)"
        structural=1
    else
        run_leg edge-margin "$BAR_HEAP:fill-256" "$FRAME_LIMIT" "$LEVEL"
        rc=$?
        held=$(balloon_mib "$OUT/edge-margin.log")
        [[ "$held" -ge 1 ]] && engaged=$((engaged + 1))
        n_initfail=$(n_of '\[ter-gpu\] init failed' "$OUT/edge-margin.log")
        n_fail=$(n_of 'device vertex buffer [0-9]* B: type [0-9]* failed' "$OUT/edge-margin.log")
        svc=$(grep -o '\[ter-gpu f=[0-9]*\].*svc=[0-9]*' "$OUT/edge-margin.log" | tail -1 | sed 's/.*svc=//')
        echo "[tercache-vram] leg=edge-margin rc=$rc balloonMiB=$held failedAttempts=$n_fail" \
             "initFailed=$n_initfail svc=${svc:-0}"
        [[ "$rc" -eq 0 ]] || { echo "[tercache-vram] leg=edge-margin FAIL (exit $rc)"; fail=1; }
        if [[ "$held" -lt 1 ]]; then
            echo "[tercache-vram] leg=edge-margin VACUOUS (the balloon held 0 MiB)"; fail=1
        fi
        # Init fits in the margin; the level edge's fresh pairs do not, and
        # fall through to the system-memory type.
        [[ "$n_initfail" -eq 0 ]] ||
            { echo "[tercache-vram] leg=edge-margin FAIL (init did not fit in the margin)"; fail=1; }
        [[ "$n_fail" -ge 1 ]] ||
            { echo "[tercache-vram] leg=edge-margin FAIL (no allocation was refused after the edge)"; fail=1; }
        [[ "${svc:-0}" -ge 1 ]] ||
            { echo "[tercache-vram] leg=edge-margin FAIL (no level edge was serviced)"; fail=1; }
    fi
fi

# ---- overflow-margin: the overflow buffers do not fit ----------------------
if want overflow-margin; then
    ran=1
    if [[ "$HAS_SVC" -eq 0 ]]; then
        echo "[tercache-vram] leg=overflow-margin SKIP (no level-edge service point: the census carries no svc=)"
        pending=1
    elif [[ -z "$BAR_HEAP" ]]; then
        echo "[tercache-vram] leg=overflow-margin SKIP (no DEVICE_LOCAL host-visible type on this device)"
        structural=1
    else
        run_leg overflow-margin "$BAR_HEAP:fill-128" "$FRAME_LIMIT" metalhead \
            OPENPETE_TC_INITIAL_CAP=16384
        rc=$?
        held=$(balloon_mib "$OUT/overflow-margin.log")
        [[ "$held" -ge 1 ]] && engaged=$((engaged + 1))
        n_bar_fail=$(n_of "device vertex buffer [0-9]* B: type $BAR_TYPE failed" "$OUT/overflow-margin.log")
        n_sys_ok=$(grep -c "device vertex buffer [0-9]* B: type [0-9]* flags" "$OUT/overflow-margin.log" \
                   | { read -r n; echo "$n"; })
        n_bar_ok=$(n_of "device vertex buffer [0-9]* B: type $BAR_TYPE flags" "$OUT/overflow-margin.log")
        n_sys_ok=$((n_sys_ok - n_bar_ok))
        # The last three census lines: afail (summed over arenas) must be flat
        # across them, and at least one overflow buffer must be installed.
        tail3=$(grep -o '\[ter-gpu f=[0-9]*\] hwm=.*' "$OUT/overflow-margin.log" | tail -3)
        afail_seq=$(echo "$tail3" | sed 's/.*afail=\([0-9,]*\).*/\1/' \
                    | awk -F, '{ s = 0; for (i = 1; i <= NF; i++) s += $i; printf "%s%d", (NR > 1 ? " " : ""), s }')
        ovf_last=$(echo "$tail3" | tail -1 | sed 's/.*ovf=\([0-9,]*\).*/\1/' \
                   | awk -F, '{ s = 0; for (i = 1; i <= NF; i++) s += $i; print s }')
        echo "[tercache-vram] leg=overflow-margin rc=$rc balloonMiB=$held" \
             "barRefused=$n_bar_fail sysAllocated=$n_sys_ok ovf=${ovf_last:-0} afailTail=[${afail_seq:-}]"
        [[ "$rc" -eq 0 ]] || { echo "[tercache-vram] leg=overflow-margin FAIL (exit $rc)"; fail=1; }
        if [[ "$held" -lt 1 ]]; then
            echo "[tercache-vram] leg=overflow-margin VACUOUS (the balloon held 0 MiB)"; fail=1
        fi
        [[ "$n_bar_fail" -ge 1 ]] ||
            { echo "[tercache-vram] leg=overflow-margin FAIL (the driver refused nothing on type $BAR_TYPE)"; fail=1; }
        [[ "$n_sys_ok" -ge 1 ]] ||
            { echo "[tercache-vram] leg=overflow-margin FAIL (nothing allocated on a system-memory type)"; fail=1; }
        [[ "${ovf_last:-0}" -ge 1 ]] ||
            { echo "[tercache-vram] leg=overflow-margin FAIL (no overflow buffer installed)"; fail=1; }
        set -- $afail_seq
        if [[ $# -lt 3 || "$1" != "$2" || "$2" != "$3" ]]; then
            echo "[tercache-vram] leg=overflow-margin FAIL (afail not flat over the last three census lines: [$afail_seq])"; fail=1
        fi
    fi
fi

[[ "$ran" -eq 1 ]] || { echo "tercache-vram: SKIP (no leg named '$LEG')"; exit 64; }

# A verdict is earned by a leg that ran with a balloon holding memory. A leg
# that skipped because this device carries no such heap or type, or because
# the build lacks the mechanism it tests, leaves the run with nothing measured:
# that is SKIP, never PASS. A leg that skipped on a device that does carry
# what it needs is a defect in the parse above, which the self-check already
# turned into a FAIL.
if [[ "$fail" -ne 0 ]]; then
    echo "[tercache-vram] game=$GAME verdict=FAIL legsEngaged=$engaged"
    exit 1
fi
if [[ "$engaged" -lt 1 ]]; then
    echo "tercache-vram: SKIP (no leg ran with an engaged balloon;" \
         "structural=$structural pending=$pending)"
    exit 64
fi
echo "[tercache-vram] game=$GAME verdict=PASS legsEngaged=$engaged"
exit 0
