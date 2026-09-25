#!/usr/bin/env bash
# check_vstream_boot.sh — vertex-stream field resolution on a stock boot.
#
# A moby's animation frame word carries either a PSX address (a stock stream)
# or a handle a mod bound when it replaced the stream. The reader classifies
# each frame word it decodes: `stock` when the field names an address,
# `handle` when it names a bound handle, `unresolved` when the field is below
# the handle bound with no handle bound to it — a stream the draw would read
# from address 0.
#
# On a stock boot nothing binds a handle, so every field must classify as
# stock. An `unresolved` there is not a mod defect: it is the moby walk
# decoding a record the game never queued — a record queue read past its
# terminator, or a pointer taken from memory the game has not written — and
# the frame word is then read from an unmapped address, where the read
# returns 0 and 0 is below the handle bound. The count is the instrument for
# that whole class, and it fires at boot, before any level is up, where the
# disc load leaves the render scratch full of stale bytes.
#
# Run: one headless boot per game, 206 VBLs, with a native screenshot armed
# at VBL 200. The screenshot request is what makes the native moby walk run
# headless; without it the reader never executes and the census is empty.
#
# Instrument (stderr, end of run, printed by core/runtime/run.c):
#   [vstream] stock=<n> handle=<n> unresolved=<n> first=<addr>
#
# Pass criterion: unresolved = 0. VACUOUS (exit 1) when stock = 0 or the line
# is absent — the reader decoded no field, so a zero count proves nothing.
#
# Verdict line:
#   [vstream-boot] game=<g> stock=<n> handle=<n> unresolved=<n> first=<addr>
#                  verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_vstream_boot.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "vstream-boot: SKIP (binary or disc missing)"; exit 64; }

SHOT_VBL=200
LIMIT=206
OUT="${PG_BUILD_DIR:-build/linux-release}/vstream-boot/$GAME"
mkdir -p "$OUT"
LOG="$OUT/boot.log"
SHOT="$OUT/boot__$SHOT_VBL-vk.png"

PG_CAPTURE_TIMEOUT=600
pg_capture "$LOG" "$SHOT" -- \
    --renderer vk --mute 1 --vibration 0 \
    --screenshot-native-only --screenshot-on-frame "$SHOT_VBL" \
    --screenshot-output "$OUT/boot__{n}.png" \
    --frame-limit "$LIMIT" </dev/null
rc=$?
if (( rc )); then
    echo "vstream-boot: FAIL (game=$GAME: run exited $rc; see $LOG)"
    exit 1
fi

line="$(grep -E '^\[vstream\] stock=' "$LOG" | tail -n 1)"
if [[ -z "$line" ]]; then
    echo "[vstream-boot] game=$GAME verdict=VACUOUS: no census line (see $LOG)"
    exit 1
fi

stock="${line#*stock=}";       stock="${stock%% *}"
handle="${line#*handle=}";     handle="${handle%% *}"
unres="${line#*unresolved=}";  unres="${unres%% *}"
first="${line#*first=}";       first="${first%% *}"

head="[vstream-boot] game=$GAME stock=$stock handle=$handle"
head="$head unresolved=$unres first=$first"
if (( stock == 0 )); then
    echo "$head verdict=VACUOUS: the reader decoded no frame word"
    exit 1
fi
if (( unres != 0 )); then
    echo "$head verdict=FAIL"
    echo "[vstream-boot]   $unres field(s) below the handle bound with no" \
         "handle bound; first=$first"
    grep -n 'vstream' "$LOG" | grep -v '^\s*$' | head -n 5 |
        sed 's/^/[vstream-boot]   /'
    exit 1
fi
echo "$head verdict=PASS"
exit 0
