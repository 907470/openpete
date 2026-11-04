#!/usr/bin/env bash
# check_late_switch_subtick.sh - sub-tick membership of the particle and moby
# record sets: which records a present draws, by identity, and the state each
# shows.
#
# A sub-tick present between ticks K-1 and K shows tick K-1's set before alpha
# 1: a record both ticks hold lerps between its two states, a record tick K
# freed or reused holds its K-1 state, a record tick K spawned is absent. At
# alpha 1 it shows tick K's set exactly. The defect class is a present that
# draws tick K's set at alpha 0: records dead at K vanish one interval early
# and records spawned at K appear at their K state.
#
# Instrument (stderr), OPENPETE_LATE_TRACE=<lo>:<hi>, one line per drawn
# record per present whose feed key lies in the window:
#   [late-part vbl=<v> leg=canon|inter a=<alpha> ep=<set> new=<newest>]
#       id=<slot>:<serial> cls=<c> st=<x>,<y>,<z>,<size>,<angle> c=<rgb>,<rgb> v=...
#   [late-moby vbl=<v> leg=inter a=<alpha> src=curr|ghost|held] m=<moby>
#       cls=<class> n=<verts> c=(<x>,<y>) w=(<x>,<y>,<z>)
# A particle's identity is its pool slot and allocation serial; a moby's is
# its address. One run per row presents every VBL of the span at alphas 0 and
# 0.5 (OPENPETE_INTERMEDIATE_PRE_ALPHA) and 1, intermediate captures at
# lo - 2 .. hi step 2; a second run captures the canonical presents of the
# same VBLs for the identity leg. The trace VBL of an intermediate capture at
# V is V + 1.
#
# Reference states. Particles: R0 and R1 are the canonical feeds of the sets
# the newest set's epoch and the one before name (the inter line's new= and
# new - 1). Mobys: R0 is the alpha-1 present of the interval before (trace
# VBL v - 2), R1 the alpha-1 present of the interval itself.
#
# Pass criterion per interval:
#   alpha 0    the drawn identities equal R0's; each record's state equals
#              R0's (particles: st and colour exactly, vertices within vtol;
#              mobys: w exactly, n equal, centroid within ctol)
#   alpha 1    the drawn identities and states equal R1's (vertices within
#              vtol), and the intermediate capture equals the canonical
#              capture pixel for pixel
#   alpha 0.5  the drawn identities equal R0's; a record R1 also holds shows
#              a state on the segment from its R0 state to its R1 state
#              (within stol per component, colour within 1 level); a record
#              R1 does not hold shows its R0 state
#   every alpha  no particle slot and no moby drawn twice in one present
# Engagement, summed over the row's intervals: held (identities of R0 absent
# from R1), deferred (identities of R1 absent from R0) and reused (held
# particles whose slot R1 holds under another serial). A row whose count is
# below its floor is VACUOUS.
#
# Input: games/<slug>/corpus/late-switch-subtick-corpus.manifest, rows
#   <bk2> | <lo>-<hi> | kind=part|moby [cls=<hex>] [held=<n>] [deferred=<n>]
#                       [reused=<n>] [vtol=<px>] [ctol=<px>] [stol=<u>]
# where the span is the intermediate-capture VBLs, two apart, and cls=
# restricts a moby row to one class.
#
# Usage: tests/harness/check_late_switch_subtick.sh [--game SLUG]
#            [--build-dir=DIR] [--filter=SUBSTR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing manifest, 64 SKIP (binary,
# disc, bk2 or python3 missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

FILTER=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --filter=*) FILTER="${a#--filter=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

MANIFEST="$(pg_corpus_manifest "$GAME" late-switch-subtick-corpus.manifest)"
pg_manifest "$MANIFEST" line
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "late-switch: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import numpy, PIL' 2>/dev/null ||
    { echo "late-switch: SKIP (python3 with numpy and PIL missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/late-switch/$GAME"
mkdir -p "$OUT"

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r src span keys <<<"$line"
    src="${src// /}"; span="${span// /}"
    [[ -z "$FILTER" || "$src" == *"$FILTER"* ]] || continue
    movie="$PG_BK2_DIR/$src"
    [[ -f "$movie" ]] || { echo "late-switch: SKIP ($movie missing)"; exit 64; }
    lo="${span%-*}"; hi="${span#*-}"
    if (( (hi - lo) % 2 != 0 || hi < lo )); then
        echo "late-switch: FAIL (row $src $span: not lo-hi with hi = lo + 2k)"; exit 1
    fi
    rows=$((rows + 1))
    tag="${src%.bk2}-$lo-$hi"
    vbls=()
    for ((f = lo - 2; f <= hi; f += 2)); do vbls+=("$f"); done
    csv="$(IFS=,; echo "${vbls[*]}")"
    last="${vbls[${#vbls[@]}-1]}"
    di="$OUT/$tag-inter" dc="$OUT/$tag-canon"
    mkdir -p "$di" "$dc"
    find "$di" "$dc" -maxdepth 1 -name '*.png' -delete
    PG_CAPTURE_TIMEOUT=900
    PG_CAPTURE_ENV=(OPENPETE_LATE_TRACE="$((lo - 6)):$((hi + 4))"
                    OPENPETE_INTERMEDIATE_PRE_ALPHA=0,0.5 OPENPETE_INTERMEDIATE_ALPHA=1)
    rci=0 rcc=0
    pg_capture "$OUT/$tag-inter.log" "$di/i__$last-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --bk2 "$movie" \
        --screenshot-native-only --screenshot-intermediate-on-frame "$csv" \
        --screenshot-output "$di/i__{n}.png" --frame-limit $((last + 10)) </dev/null || rci=$?
    PG_CAPTURE_ENV=()
    pg_capture "$OUT/$tag-canon.log" "$dc/c__$last-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --bk2 "$movie" \
        --screenshot-native-only --screenshot-on-frame "$csv" \
        --screenshot-output "$dc/c__{n}.png" --frame-limit $((last + 10)) </dev/null || rcc=$?
    if (( rci || rcc )) || [[ ! -s "$OUT/$tag-inter.log" ]]; then
        echo "late-switch: FAIL (row $src $span: run rc=$rci/$rcc; see $OUT)"
        fail=1; continue
    fi
    python3 tests/harness/late_switch_check.py "$OUT/$tag-inter.log" "$di" "$dc" \
        "$GAME" "$src" "$lo" "$hi" "$keys" || fail=1
done <"$MANIFEST"

(( rows > 0 )) || { echo "late-switch: FAIL (no rows in $MANIFEST)"; exit 1; }
exit "$fail"
