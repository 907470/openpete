#!/usr/bin/env bash
# check_player_subtick.sh — sub-tick continuity of the player model.
#
# An intermediate present at VBL F shows the player between the previous
# canonical tick's pose (alpha 0) and the current one's (alpha 1). Two
# identities follow, and both are exact in a correct build:
#
#   continuity  the alpha-0 present at F equals the alpha-1 present at F-2:
#               one interval starts on the pose the interval before ended on.
#   identity    the alpha-1 present at F equals the tick present at F-1: the
#               interval ends on the canonical pose.
#
# A part placed from the wrong tick's data breaks continuity by the distance
# that part moves in a tick, while identity still holds.
#
# Input: games/<slug>/corpus/player-subtick-corpus.manifest, rows
#   <bk2> | <lo>-<hi>[,<lo>-<hi>...]
# Each range is a span of intermediate-present VBLs, two apart. Which VBL
# parity the intermediates fall on is the movie's own (the tick phase at the
# level's first tick), so the row states it through lo; the lerp=1 requirement
# below refuses a span laid on the tick presents. Every VBL of a range is
# captured; every one but the range's first is a leg, measured against the
# capture two VBLs earlier.
#
# Instrument (stderr), OPENPETE_PLAYER_POSTRACE=1 and OPENPETE_PLAYER_RECDBG=1,
# one run at OPENPETE_INTERMEDIATE_ALPHA=0 and one at 1 per row:
#   [player-screen f=<VBL>] ... c=(x,y) bb=(x0,y0,x1,y1) mirror=<0|1>
#   [recdbg present f=<VBL> k=<n>] ... lerp=<0|1> ...
# The mirrored copy and the avatar are measured separately.
#
# Pass criterion, every leg, every copy: continuity under LIMIT px on the
# centroid and on every bounding-box edge; identity exact on the printed
# values; the present at the leg bracketed (lerp=1) in both runs.
# Engagement: a row with no bracketed leg, or a leg with no trace line, fails;
# the verdict line carries the bracketed-present count.
#
# Verdict line per row:
#   [player-subtick] game=<g> bk2=<b> legs=<n> bracketed=<n> maxLagC=<px>
#                    maxLagBB=<px> maxIdent=<px> verdict=PASS|FAIL
#
# Usage: scripts/check_player_subtick.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or a missing corpus manifest (the message names the
# path), 64 SKIP (binary, disc, bk2 or python3 missing).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

LIMIT=0.01
MANIFEST="$(pg_corpus_manifest "$GAME" player-subtick-corpus.manifest)"
pg_manifest "$MANIFEST" line
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "player-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "player-subtick: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/player-subtick/$GAME"
mkdir -p "$OUT"

# One run of a row at one alpha. The screenshots are the capture requests that
# make the intermediate presents exist headless; the last one is asserted.
run_alpha() {
    local alpha="$1" tag="$2" movie="$3" vbls="$4" last="$5"
    local dir="$OUT/$tag-a$alpha"
    mkdir -p "$dir"
    find "$dir" -maxdepth 1 -name '*.png' -delete
    PG_CAPTURE_ENV=(OPENPETE_PLAYER_POSTRACE=1 OPENPETE_PLAYER_RECDBG=1
                    OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=600
    pg_capture "$OUT/$tag-a$alpha.log" "$dir/i__$last-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original \
        --bk2 "$movie" --screenshot-native-only \
        --screenshot-intermediate-on-frame "$vbls" \
        --screenshot-output "$dir/i__{n}.png" \
        --frame-limit $((last + 10))
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r bk2 ranges <<<"$line"
    bk2="${bk2// /}"; ranges="${ranges// /}"
    movie="$PG_BK2_DIR/$bk2"
    [[ -f "$movie" ]] || { echo "player-subtick: SKIP ($movie missing)"; exit 64; }
    tag="${bk2%.bk2}"
    rows=$((rows + 1))

    vbls="" last=0
    IFS=',' read -ra spans <<<"$ranges"
    for s in "${spans[@]}"; do
        lo="${s%-*}"; hi="${s#*-}"
        if (( (hi - lo) % 2 != 0 || hi < lo + 2 )); then
            echo "player-subtick: FAIL (row $bk2: span $s is not lo-hi with hi = lo + 2k)"; exit 1
        fi
        for ((f = lo; f <= hi; f += 2)); do vbls+="${vbls:+,}$f"; done
        (( hi > last )) && last=$hi
    done

    rm -f "$OUT/$tag-a0.log" "$OUT/$tag-a1.log"
    run_alpha 0 "$tag" "$movie" "$vbls" "$last" </dev/null & p0=$!
    run_alpha 1 "$tag" "$movie" "$vbls" "$last" </dev/null & p1=$!
    wait $p0; rc0=$?
    wait $p1; rc1=$?
    if [[ ! -s "$OUT/$tag-a0.log" || ! -s "$OUT/$tag-a1.log" ]]; then
        echo "player-subtick: FAIL (row $bk2: no instrument output, rc=$rc0/$rc1; see $OUT)"
        fail=1; continue
    fi

    python3 - "$OUT/$tag-a0.log" "$OUT/$tag-a1.log" "$GAME" "$bk2" "$ranges" "$LIMIT" <<'PYEOF' || fail=1
import re, sys

log0, log1, game, bk2, ranges, limit = sys.argv[1:7]
limit = float(limit)

scr = re.compile(r'\[player-screen f=(\d+)\] leg=\w+ slot=\d+ ntri=(\d+) nv=\d+ '
                 r'c=\(([-\d.]+),([-\d.]+)\) '
                 r'bb=\(([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)\) mirror=(\d)')
rec = re.compile(r'\[recdbg present f=(\d+) k=\d+\].* lerp=1 ')

def load(path):
    shape, lerp = {}, {}
    for line in open(path, errors='replace'):
        m = scr.search(line)
        if m:
            key = (int(m[1]), int(m[9]))
            shape.setdefault(key, tuple(float(m[i]) for i in range(3, 9)))
            continue
        m = rec.search(line)
        if m:
            lerp[int(m[1])] = lerp.get(int(m[1]), 0) + 1
    return shape, lerp

def dist(a, b):
    c = ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5
    return c, max(abs(x - y) for x, y in zip(a[2:], b[2:]))

s0, l0 = load(log0)
s1, l1 = load(log1)

legs = []
for span in ranges.split(','):
    lo, hi = (int(x) for x in span.split('-'))
    legs += list(range(lo + 2, hi + 1, 2))

bad, bracketed = [], 0
max_c = max_bb = max_id = 0.0
for F in legs:
    if l0.get(F, 0) and l1.get(F, 0):
        bracketed += 1
    else:
        bad.append("leg=%d unbracketed (lerp=1 presents a0=%d a1=%d)"
                   % (F, l0.get(F, 0), l1.get(F, 0)))
        continue
    copies = [mi for mi in (0, 1) if (F, mi) in s0 or (F, mi) in s1]
    if not copies:
        bad.append("leg=%d no trace line" % F)
        continue
    for mi in copies:
        a0, a1 = s0.get((F, mi)), s1.get((F, mi))
        prev, tick = s1.get((F - 2, mi)), s0.get((F - 1, mi))
        if None in (a0, a1, prev, tick):
            bad.append("leg=%d mirror=%d missing a comparand" % (F, mi))
            continue
        c, bb = dist(a0, prev)
        ic, ibb = dist(a1, tick)
        max_c, max_bb, max_id = max(max_c, c), max(max_bb, bb), max(max_id, ic, ibb)
        if c >= limit or bb >= limit:
            bad.append("leg=%d mirror=%d lagC=%.3f lagBB=%.3f" % (F, mi, c, bb))
        if a1 != tick:
            bad.append("leg=%d mirror=%d identC=%.3f identBB=%.3f" % (F, mi, ic, ibb))

if bracketed == 0:
    print("[player-subtick] game=%s bk2=%s legs=%d bracketed=0 VACUOUS: no leg is a "
          "bracketed present" % (game, bk2, len(legs)))
    sys.exit(1)
print("[player-subtick] game=%s bk2=%s legs=%d bracketed=%d maxLagC=%.3f maxLagBB=%.3f "
      "maxIdent=%.3f verdict=%s"
      % (game, bk2, len(legs), bracketed, max_c, max_bb, max_id, "FAIL" if bad else "PASS"))
for b in bad[:40]:
    print("[player-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "player-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
