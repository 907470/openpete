#!/usr/bin/env bash
# check_tercache_alloc.sh — the terrain arenas' memory-type allocation legs.
#
# rhi_create_vertex_buffer_device allocates each retained-terrain arena on the
# first host-visible, host-coherent memory type that accepts it, DEVICE_LOCAL
# types first. A machine whose device-local host-visible heap is small refuses
# an arena part way through the cache's init loop; the loop stops at the first
# NULL buffer and openpete_tercache_init shuts the cache down for the session,
# leaving the CPU terrain walk in place.
#
# A software rasteriser advertises one memory type, so no run here can execute
# a second attempt: the candidate order itself is pinned by the unit test
# rhi_memtype_candidates_test. What this harness proves is the leg either side
# of the order — that a refused memory type is reported per attempt and ends
# in the cache's own init-failure path, and that the unrefused run names the
# type it allocated on and does not take that path.
#
# Two legs of the same binary, one lever between them:
#   inject   OPENPETE_RHI_FAIL_MEMTYPE=0 — every attempt on memory type 0
#            (llvmpipe's only type) reports VK_ERROR_OUT_OF_DEVICE_MEMORY
#            without reaching the driver
#   plain    no injection
# Both run headless with OPENPETE_TERRAIN_CACHE_FORCE=1, which waives the
# software-GPU exclusion so the cache initialises at all, and both take a
# screenshot, without which a headless run never brings the native renderer up.
#
# Assertions:
#   inject  at least one `[rhi/vk] device vertex buffer <n> B: type 0 failed`
#           line (engagement: the count is printed, and a zero count
#           is VACUOUS, never PASS), the cache's `[ter-gpu] init failed` line,
#           exit 0, and the screenshot written — the run continues on the CPU
#           terrain walk.
#   plain   at least one `device vertex buffer <n> B: type 0 flags <x>` line,
#           no failed attempt, no `init failed`, exit 0, screenshot written.
#
# The injected leg's failed-attempt count is one, not one per arena: the init
# loop is `for (a = 0; ok && a < OPTC_ARENA_COUNT; a++)`, so the first NULL
# buffer ends the loop and no later arena is attempted.
#
# Usage:
#   scripts/check_tercache_alloc.sh [--game SLUG] [--build-dir=DIR] [--out DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: nobody is holding the pad and the buzzing appears in no
# log, so every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
    esac
done
prev=""
for a in "$@"; do
    case "$prev" in
        --out) OUT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

[[ -x "$PG_BIN" ]] || { echo "tercache-alloc: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-alloc: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/tercache-alloc/$GAME"
mkdir -p "$OUT"

# The pinned VBL is far enough into the level for the cache to have built
# sectors and early enough that a software rasteriser reaches it in under a
# minute. The screenshot is what keeps the native renderer alive headless.
SHOT_VBL=400
LEG_TIMEOUT=900

# One leg. pg_capture deletes the log and the asserted screenshot before the
# run and reports a missing output.
run_leg() {  # tag [env...]
    local tag="$1"; shift
    local log="$OUT/$tag.log"
    local shot="$OUT/$tag.png"
    local shot_vk="$OUT/$tag-vk.png"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_VIBRATION=0 "$@")
    PG_CAPTURE_TIMEOUT="$LEG_TIMEOUT"
    pg_capture "$log" "$shot_vk" -- \
        --renderer vk --no-card --mute 1 --vibration 0 \
        --skip-to-level artisans \
        --screenshot-native-only --screenshot-on-frame "$SHOT_VBL" \
        --screenshot-output "$shot" --frame-limit $((SHOT_VBL + 1))
}

fail=0

# Leg 1: memory type 0 refused. Every candidate fails, so the arena is NULL
# and the cache shuts itself down.
run_leg inject OPENPETE_RHI_FAIL_MEMTYPE=0
rc_inject=$?
n_fail_inject=$(grep -c 'device vertex buffer [0-9]* B: type 0 failed' "$OUT/inject.log" || true)
n_ok_inject=$(grep -c 'device vertex buffer [0-9]* B: type 0 flags' "$OUT/inject.log" || true)
n_initfail_inject=$(grep -c '\[ter-gpu\] init failed' "$OUT/inject.log" || true)
echo "[tercache-alloc] leg=inject rc=$rc_inject failedAttempts=$n_fail_inject" \
     "allocated=$n_ok_inject initFailed=$n_initfail_inject"
grep -m2 'device vertex buffer' "$OUT/inject.log" | sed 's/^/[tercache-alloc]   /'
grep -m1 '\[ter-gpu\] init failed' "$OUT/inject.log" | sed 's/^/[tercache-alloc]   /'
[[ "$rc_inject" -eq 0 ]] || { echo "[tercache-alloc] leg=inject FAIL (exit $rc_inject)"; fail=1; }
# Engagement: a leg whose injection never fired measured nothing.
if [[ "$n_fail_inject" -lt 1 ]]; then
    echo "[tercache-alloc] leg=inject VACUOUS (no injected attempt in the log)"
    fail=1
fi
[[ "$n_initfail_inject" -ge 1 ]] ||
    { echo "[tercache-alloc] leg=inject FAIL (cache did not report init failed)"; fail=1; }
[[ "$n_ok_inject" -eq 0 ]] ||
    { echo "[tercache-alloc] leg=inject FAIL (an allocation succeeded on the refused type)"; fail=1; }

# Leg 2: no injection. The one memory type allocates every arena.
run_leg plain
rc_plain=$?
n_fail_plain=$(grep -c 'device vertex buffer [0-9]* B: type [0-9]* failed' "$OUT/plain.log" || true)
n_ok_plain=$(grep -c 'device vertex buffer [0-9]* B: type 0 flags' "$OUT/plain.log" || true)
n_initfail_plain=$(grep -c '\[ter-gpu\] init failed' "$OUT/plain.log" || true)
echo "[tercache-alloc] leg=plain rc=$rc_plain failedAttempts=$n_fail_plain" \
     "allocated=$n_ok_plain initFailed=$n_initfail_plain"
grep -m1 'device vertex buffer' "$OUT/plain.log" | sed 's/^/[tercache-alloc]   /'
[[ "$rc_plain" -eq 0 ]] || { echo "[tercache-alloc] leg=plain FAIL (exit $rc_plain)"; fail=1; }
[[ "$n_ok_plain" -ge 1 ]] ||
    { echo "[tercache-alloc] leg=plain VACUOUS (no arena allocation in the log)"; fail=1; }
[[ "$n_fail_plain" -eq 0 ]] ||
    { echo "[tercache-alloc] leg=plain FAIL (an allocation failed with no injection)"; fail=1; }
[[ "$n_initfail_plain" -eq 0 ]] ||
    { echo "[tercache-alloc] leg=plain FAIL (cache init failed with no injection)"; fail=1; }

echo "[tercache-alloc] game=$GAME verdict=$([[ "$fail" -eq 0 ]] && echo PASS || echo FAIL)"
exit "$fail"
