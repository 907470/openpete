#!/usr/bin/env bash
# check_capture_repeat.sh - headless native captures repeat bit-for-bit
# across host load: the same run twice, the second pinned to one CPU.
#
# Both legs run the same headless capture (terrain cache forced on, since
# llvmpipe does not select it alone) and must agree on every native PNG's
# digest, the native-feed triangle total (`native-feed: ... tris=`) and the
# cache's `tercache:` exit row fields bakes and budget_claims. The second leg
# runs under one CPU (affinity of CPU 0), which slows every worker and bake;
# a presentation decision keyed on wall time (a bake budget charged in
# nanoseconds) then defers a different sector set and the totals or images
# differ. Engagement: each leg must report bakes>0 and budget_claims>0 (the
# first-touch budget deferred sectors), else the row is VACUOUS.
#
# Row per game: the level a --skip-to-level run loads and the VBLs captured.
# Spyro 3 country_speedway at VBLs 1590-1610 (the level loads near VBL 1150
# and the budget defers its first-touch bakes over the following walks).
#
# Usage:
#   tests/harness/check_capture_repeat.sh [--game SLUG] [--exe PATH] [--out DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no binary, no disc, no row for
# the game).
set -u
set -o pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

EXE=""; OUT=""
prev=""
for a in "$@"; do
    case "$prev" in
        --exe) EXE="$a";; --out) OUT="$a";;
    esac
    prev="$a"
done
# --game defaults to spyro-3, the game the row is defined on.
GAME="$(pg_parse_game --game spyro-3 "$@")" || exit 64
pg_resolve "$GAME"
[[ -n "$EXE" ]] && PG_BIN="$EXE"
[[ -x "$PG_BIN" ]] || { echo "capture-repeat: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "capture-repeat: SKIP (no disc at $PG_DISC)"; exit 64; }
command -v taskset >/dev/null || { echo "capture-repeat: SKIP (no taskset)"; exit 64; }

case "$GAME" in
    spyro-3) LEVEL=country_speedway; FIRST=1590; LAST=1610;;
    *) echo "capture-repeat: SKIP (no row for $GAME)"; exit 64;;
esac
[[ -n "$OUT" ]] || OUT="${TMPDIR:-/tmp}/capture-repeat/$GAME"
mkdir -p "$OUT"

SHOTS="$(seq -s, "$FIRST" "$LAST")"
PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_HEADLESS_NATIVE=1
                OPENPETE_TERRAIN_CANIMNEAR=1)
PG_CAPTURE_TIMEOUT=1800

# One leg: deletes its PNGs and log, captures, and asserts every PNG exists.
run_leg() {  # tag
    local tag="$1" n outs=""
    mkdir -p "$OUT/$tag"
    rm -f "${OUT:?}/${tag:?}/"shot__*-vk.png
    for ((n = FIRST; n <= LAST; n++)); do outs+="${outs:+,}$OUT/$tag/shot__$n-vk.png"; done
    pg_capture "$OUT/$tag.log" "$outs" -- \
        --renderer vk --mute 1 --vibration 0 --aspect 4:3 --no-card --lod original \
        --skip-to-level "$LEVEL" --screenshot-on-frame "$SHOTS" --screenshot-native-only \
        --screenshot-output "$OUT/$tag/shot__{n}.png" --frame-limit $((LAST + 10))
}

# A field of the leg's last `tercache:` row, or empty.
field() {  # tag name
    grep -o '^tercache: .*' "$OUT/$1.log" 2>/dev/null | tail -1 |
        tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}
# The leg's native-feed triangle total, or empty.
tris() {  # tag
    grep -o 'native-feed: .*' "$OUT/$1.log" 2>/dev/null | tail -1 |
        tr ' ' '\n' | sed -n 's/^tris=//p' | head -1
}
# The leg's PNG digests, one "name digest" line per VBL.
digests() {  # tag
    local n
    for ((n = FIRST; n <= LAST; n++)); do
        echo "shot__$n $(sha256sum < "$OUT/$1/shot__$n-vk.png" | cut -c1-16)"
    done
}

run_leg free || { echo "capture-repeat: FAIL (leg free did not complete; $OUT/free.log)"; exit 1; }
# The pinned leg: the subshell pins itself to CPU 0 and the game inherits it.
( taskset -pc 0 "$BASHPID" >/dev/null && run_leg pinned ) ||
    { echo "capture-repeat: FAIL (leg pinned did not complete; $OUT/pinned.log)"; exit 1; }

fail=0
for t in free pinned; do
    b="$(field "$t" bakes)"; c="$(field "$t" budget_claims)"
    echo "[capture-repeat] leg=$t bakes=${b:-none} budget_claims=${c:-none} tris=$(tris "$t")"
    if [[ -z "$b" || "$b" == 0 || -z "$c" || "$c" == 0 ]]; then
        echo "[capture-repeat] leg=$t VACUOUS (bakes or budget_claims is 0 or missing)"; fail=1
    fi
done
for k in bakes budget_claims; do
    [[ "$(field free "$k")" == "$(field pinned "$k")" ]] ||
        { echo "[capture-repeat] FAIL ($k differs)"; fail=1; }
done
[[ -n "$(tris free)" && "$(tris free)" == "$(tris pinned)" ]] ||
    { echo "[capture-repeat] FAIL (native-feed tris differ or missing)"; fail=1; }
digests free > "$OUT/free.digests"
digests pinned > "$OUT/pinned.digests"
nd="$(diff "$OUT/free.digests" "$OUT/pinned.digests" | grep -c '^<')"
echo "[capture-repeat] images=$((LAST - FIRST + 1)) differing=$nd"
[[ "$nd" == 0 ]] || fail=1

if [[ "$fail" == 0 ]]; then echo "capture-repeat: PASS"; exit 0; fi
echo "capture-repeat: FAIL"
exit 1
