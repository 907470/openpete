#!/usr/bin/env bash
# check_carry_subtick.sh — identity of the 2D-UI carry across sub-tick presents.
#
# The 2D-UI layer (dialogue panels, HUD glyphs, the fade overlay) is not
# re-derivable by the sub-tick re-extract; it is captured at each canonical
# present and re-applied on the intermediate ones, with each element's screen
# position interpolated between the two most recent canonical captures. That
# interpolation is only meaningful when the two entries it reads are the same
# element. When they are not, the intermediate present draws the element
# somewhere neither canonical tick drew it, for the length of one tick, and the
# next tick snaps it back: a flicker that exists only with interpolation on.
#
# The defect class this measures is a crossed pair: the previous capture's
# entry that the current entry was paired with is not its predecessor, because
# another entry of the previous capture already sits exactly where the current
# one is. That other entry is the real predecessor, so the lerp sweeps the
# element between two different sprites. A dialogue box's top and bottom corner
# quads are the same 12x8 quad 58 px apart, so a crossed pair there slides a
# corner the full 58 px.
#
# The crossing test is what separates the defect from a fast but correct slide:
# a genuinely sliding sprite's new position was occupied by nothing last tick,
# so no twin exists and nothing fires, however far it moved.
#
# Input: games/<slug>/carry-subtick-corpus.manifest, rows
#   <bk2> | <lo>-<hi> | <key>=<value> ...
# The span is the intermediate-capture VBLs, one apart, inclusive. Keys:
#   lerps=<n>   floor on the interpolated carry triangles measured over the row
#   segs=<n>    floor on the segments the run pass carried by their own
#               translation, summed over the row: the engagement proof of the
#               segmentation path, which fires only on a run whose entries
#               displace by more than one amount
#   disagree=<n>  cap on runs_snapped_disagree over the row: the runs that
#               found a same-multiset partner and no translation at all, so
#               every entry of them snapped
#   nearer=<n>  cap on nearer_twin over the row: the moved pairs whose partner
#               lies farther than another previous entry of the same shape,
#               the mispair of a body that moves less than its own period
#   far=<n>     cap on far_both over the row: lerped tris more than 2 px from
#               both brackets, for a window in which nothing moves more than
#               4 px a tick, so any such tri was slid between two sprites
# A row whose first field does not end in .bk2 is the plain boot — the title
# screen and the attract demo, which need no input — and replays with no movie,
# the same movie-less row the presentation corpus takes.
#
# Instrument (stderr), OPENPETE_CARRY_CENSUS=1, one line per present:
#   [carrycensus vbl=<v> present=<i> alpha=<a>] carried=<n> paired=<n>
#     lerped=<n> far_both=<n> uv_mismatch=<n> crossed=<n> static_moved=<n>
#     runs=<n> runs_lerped=<n> runs_snapped_disagree=<n> runs_segmented=<n>
#     segments_lerped=<n> ambiguous_lerp=<n> nearer_twin=<n> ...
# A present whose carry interpolated nothing prints NO-ENGAGEMENT rather than
# a clean row, so an empty window cannot read as a pass.
#
# Pass criterion, per row: crossed = 0 summed over every present of every
# alpha, runs_snapped_disagree at or below the row's `disagree` cap and
# nearer_twin and far_both at or below its `nearer` and `far` caps when it
# sets them. VACUOUS (exit 1) when the row's summed `lerped` or `segments_lerped`
# is below its floor. far_both, static_moved, ambiguous_lerp and the remaining
# run counts are reported, not judged.
#
# Verdict line per row:
#   [carry-subtick] game=<g> bk2=<b> presents=<n> lerped=<n> crossed=<n>
#                   far_both=<n> static_moved=<n> runs=<n> runs_lerped=<n>
#                   runs_snapped_disagree=<n> runs_segmented=<n>
#                   segments_lerped=<n> ambiguous_lerp=<n> verdict=PASS|FAIL
#
# Usage: scripts/check_carry_subtick.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc, bk2, manifest or
# python3 missing).
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

CHUNK=60
ALPHAS=(0.25 0.5 0.75)
MANIFEST="games/$GAME/carry-subtick-corpus.manifest"
[[ -f "$MANIFEST" ]] || { echo "carry-subtick: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "carry-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "carry-subtick: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/carry-subtick/$GAME"
mkdir -p "$OUT"

# One replay of a row at one alpha, as runs of at most CHUNK captures whose
# logs append to one file. The screenshots are the capture requests that make
# the intermediate presents exist headless; each run's last one is asserted.
run_alpha() {
    local alpha="$1" tag="$2" movie="$3"; shift 3
    local dir="$OUT/$tag-a$alpha" log="$OUT/$tag-a$alpha.log"
    local -a vbls=("$@") bk2_arg=()
    [[ -n "$movie" ]] && bk2_arg=(--bk2 "$movie")
    mkdir -p "$dir"
    find "$dir" -maxdepth 1 -name '*.png' -delete
    rm -f "$log" "$log.part"
    PG_CAPTURE_ENV=(OPENPETE_CARRY_CENSUS=1 OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=600
    local i n=${#vbls[@]}
    for ((i = 0; i < n; i += CHUNK)); do
        local -a part=("${vbls[@]:i:CHUNK}")
        local last="${part[${#part[@]}-1]}" csv
        csv="$(IFS=,; echo "${part[*]}")"
        pg_capture "$log.part" "$dir/i__$last-vk.png" -- \
            --renderer vk --mute 1 --aspect 4:3 --lod original \
            "${bk2_arg[@]}" --screenshot-native-only \
            --screenshot-intermediate-on-frame "$csv" \
            --screenshot-output "$dir/i__{n}.png" \
            --frame-limit $((last + 10)) || return 1
        cat "$log.part" >>"$log"
        rm -f "$log.part"
    done
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r bk2 span keys <<<"$line"
    bk2="${bk2// /}"; span="${span// /}"
    if [[ "$bk2" == *.bk2 ]]; then
        movie="$PG_BK2_DIR/$bk2"
        [[ -f "$movie" ]] || { echo "carry-subtick: SKIP ($movie missing)"; exit 64; }
    else
        movie=""    # plain boot: title screen and attract demo, no input
    fi
    tag="${bk2%.bk2}"
    rows=$((rows + 1))

    lo="${span%-*}"; hi="${span#*-}"
    if (( hi <= lo )); then
        echo "carry-subtick: FAIL (row $bk2: span $span is not lo-hi with hi > lo)"; exit 1
    fi
    vbls=()
    for ((f = lo; f <= hi; f++)); do vbls+=("$f"); done

    logs=() rc=0
    for alpha in "${ALPHAS[@]}"; do
        run_alpha "$alpha" "$tag" "$movie" "${vbls[@]}" </dev/null || rc=$?
        logs+=("$OUT/$tag-a$alpha.log")
    done
    missing=0
    for l in "${logs[@]}"; do [[ -s "$l" ]] || missing=1; done
    if (( missing || rc )); then
        echo "carry-subtick: FAIL (row $bk2: no instrument output, rc=$rc; see $OUT)"
        fail=1; continue
    fi

    python3 - "$GAME" "$bk2" "$keys" "${logs[@]}" <<'PYEOF' || fail=1
import re, sys

game, bk2, keys = sys.argv[1:4]
opts = dict(tok.split('=', 1) for tok in keys.split())
min_lerps = int(opts.get('lerps', '1'))
min_segs = int(opts.get('segs', '0'))
max_dis = opts.get('disagree')
max_dis = None if max_dis is None else int(max_dis)
max_near = opts.get('nearer')
max_near = None if max_near is None else int(max_near)
max_far = opts.get('far')
max_far = None if max_far is None else int(max_far)

# The census fields are read BY NAME, not by position: a line from a build that
# predates a counter still parses and the absent counter reads 0, instead of the
# whole present dropping out of the row and an old build reading as no
# instrument output at all.
head_re = re.compile(r'\[carrycensus ')
kv_re = re.compile(r'(\w+)=(-?[\d.]+)')
tail_re = re.compile(r'\]\s*(.*)')
FIELDS = ('lerped', 'far_both', 'crossed', 'static_moved', 'runs', 'runs_lerped',
          'runs_snapped_disagree', 'runs_segmented', 'segments_lerped',
          'ambiguous_lerp', 'nearer_twin')
tot = dict((f, 0) for f in FIELDS)
presents = 0
bad = []
for path in sys.argv[4:]:
    for line in open(path, errors='replace'):
        if not head_re.search(line):
            continue
        kv = dict(kv_re.findall(line))
        if 'carried' not in kv or 'lerped' not in kv:
            continue
        presents += 1
        for f in FIELDS:
            tot[f] += int(float(kv.get(f, 0)))
        if int(kv['crossed']):
            t = tail_re.search(line)
            bad.append("vbl=%s present=%s alpha=%s crossed=%s of %s lerped: %s"
                       % (kv.get('vbl'), kv.get('present'), kv.get('alpha'),
                          kv['crossed'], kv['lerped'], (t and t[1].strip()) or ''))

head = ("[carry-subtick] game=%s bk2=%s presents=%d lerped=%d crossed=%d far_both=%d"
        " static_moved=%d runs=%d runs_lerped=%d runs_snapped_disagree=%d"
        " runs_segmented=%d segments_lerped=%d ambiguous_lerp=%d nearer_twin=%d"
        % (game, bk2, presents, tot['lerped'], tot['crossed'], tot['far_both'],
           tot['static_moved'], tot['runs'], tot['runs_lerped'],
           tot['runs_snapped_disagree'], tot['runs_segmented'],
           tot['segments_lerped'], tot['ambiguous_lerp'], tot['nearer_twin']))
if tot['lerped'] < min_lerps:
    print(head + " VACUOUS: floor lerps>=%d" % min_lerps)
    sys.exit(1)
if tot['segments_lerped'] < min_segs:
    print(head + " VACUOUS: floor segments_lerped>=%d" % min_segs)
    sys.exit(1)
if max_dis is not None and tot['runs_snapped_disagree'] > max_dis:
    print(head + " verdict=FAIL: runs_snapped_disagree=%d exceeds disagree<=%d"
          % (tot['runs_snapped_disagree'], max_dis))
    sys.exit(1)
if max_far is not None and tot['far_both'] > max_far:
    print(head + " verdict=FAIL: far_both=%d exceeds far<=%d" % (tot['far_both'], max_far))
    sys.exit(1)
if max_near is not None and tot['nearer_twin'] > max_near:
    print(head + " verdict=FAIL: nearer_twin=%d exceeds nearer<=%d"
          % (tot['nearer_twin'], max_near))
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad[:40]:
    print("[carry-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "carry-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
