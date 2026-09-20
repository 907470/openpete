#!/usr/bin/env bash
# check_glint_subtick.sh — sub-tick interpolation of the gem-glint stars.
#
# The gem-glint routine links its own primitives into the 3D ordering table and
# the feeder reads them back out of the polybuf, so what a sub-tick present has
# to work with is the guest's own int16 screen corners, built under the tick's
# camera from the tick's pool record. A replay that re-emits them verbatim
# draws the star at the pixels the tick drew for the whole interior of the
# tick, and the next tick moves it the whole way at once: the star sits still
# while the world around it slides, then jumps.
#
# The defect class is a sub-tick producer that holds its tick pose. The
# measurement is where the star is on screen, per present.
#
# Instrument: leave-one-out, not colour. Every leg is captured twice
# from one binary, OPENPETE_GLINT=2 (the feed on) against OPENPETE_GLINT=0 (the
# feed suppressed, the guest draw unchanged), and the pixels that differ are
# exactly the ones this feeder painted. Those pixels are clustered at a
# CLUSTER_R Chebyshev radius into components, one per star or cross on screen,
# and each component's centroid is its position. The component pixel count is
# the engagement counter: a leg whose mask is empty measured nothing.
#
# A sub-tick present replays the set the most recent canonical tick captured
# and shows it between the pose of the tick before (alpha 0) and the pose that
# tick drew (alpha 1). The present at VBL V+1 therefore covers the interval the
# two ticks at V-2 and V drew, so three captures per row, all from the same
# replay of one movie:
#   canon(V-2), canon(V)   the two canonical ticks, the interval's ends
#   inter(V+1, alpha)      one intermediate present over that interval
# A gem's expected position there is (1-alpha)*canon(V-2) + alpha*canon(V): the
# star is on the world's own straight line between the two poses the ticks
# drew, because the camera transform the rest of the pass draws under is itself
# lerped by alpha.
#
# alpha 1 is the identity leg, where the expected position is canon(V)
# exactly: every term of a correct rebase is zero at the end of the tick's own
# interval, so the replay must reproduce the canonical present. alpha 0 is the
# leg that prices the defect: a producer that holds its tick pose reads canon(V)
# there, one whole tick step from where it belongs.
#
# alpha 0 is captured and reported, not judged. The replay is a screen-space
# rebase of the tick's own primitives, so at alpha 0 it shows the tick's star —
# its size ramp and its arm length, both of which the routine derives from the
# record's remaining lifetime and depth — standing where the tick before drew
# its own, slightly different, star. The two masks differ by a pixel or two and
# the centroid follows, which measures the size ramp rather than the rebase.
# Reproducing the previous tick's star outright needs a second copy of the
# routine's geometry, which this feeder deliberately does not carry.
#
# Input: games/<slug>/glint-subtick-corpus.manifest, rows
#   <source> | <V> | <key>=<value> ...
# <source> is a movie under games/<slug>/bk2, or any name not ending in .bk2,
# which is the plain boot — the title screen and the attract demo, which need
# no input — replayed with no movie, the same movie-less row form the
# presentation corpus takes.
# V is the canonical VBL whose primitives the intermediate present replays;
# V-2 opens the interval and the intermediate present is captured at V+1. Keys:
#   gem=<x>,<y>  a gem to judge, named by its mask component's centroid in the
#                canonical capture at V. Repeatable; at least one required.
#                A row names the gems whose star is unoccluded over the
#                interval: a partly covered star's visible centroid is not the
#                interpolation of its two visible centroids, so judging it
#                would measure the depth test, not the rebase.
#   tol=<px>     centroid bound, default 0.5
#   judge=<a>[,<a>...]
#                the alphas the bound applies to, default 0.5,1; every
#                captured alpha is reported either way
#   step=<px>    floor on the gem's canonical motion over the interval. A gem
#                that does not move between the two ticks passes every alpha
#                whatever the producer does, so a row below this floor is
#                VACUOUS rather than a pass. Default 4.
#
# Pass criterion, per row: every named gem, at every judged alpha, within `tol`
# of its expected centroid. VACUOUS (exit 1) when a leg's mask is empty, when a named
# gem resolves to no component or to an ambiguous one in some leg, or when a
# gem's canonical step is below the floor.
#
# Verdict line per row:
#   [glint-subtick] game=<g> bk2=<b> V=<v> gems=<n> legs=<n> maskpx=<n>
#                   worst=<px> verdict=PASS|FAIL
#
# Usage: scripts/check_glint_subtick.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc, bk2, manifest, python3
# or PIL missing).
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

ALPHAS=(0 0.5 1)
MANIFEST="games/$GAME/glint-subtick-corpus.manifest"
[[ -f "$MANIFEST" ]] || { echo "glint-subtick: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "glint-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "glint-subtick: SKIP (python3 or PIL missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/glint-subtick/$GAME"
mkdir -p "$OUT"

# One replay of a row's movie with one glint setting, capturing either the two
# canonical ticks or one intermediate present. The screenshot request is what
# makes the native renderer run and the intermediate present exist at all under
# --headless, so every leg carries one.
run_leg() {
    local tag="$1" glint="$2" alpha="$3" out="$4"; shift 4
    local log="$OUT/$tag.log"
    PG_CAPTURE_ENV=(OPENPETE_GLINT="$glint")
    [[ -n "$alpha" ]] && PG_CAPTURE_ENV+=(OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$out" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original \
        "${MOVIE_ARG[@]}" --screenshot-native-only \
        "$@" --screenshot-output "$OUT/$tag/{n}.png" \
        --frame-limit $((V + 11))
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r bk2 vfield keys <<<"$line"
    bk2="${bk2// /}"; V="${vfield// /}"
    # A row whose first field does not end in .bk2 is the plain boot, which
    # needs no input and replays with no movie.
    if [[ "$bk2" == *.bk2 ]]; then
        MOVIE="$PG_BK2_DIR/$bk2"
        [[ -f "$MOVIE" ]] || { echo "glint-subtick: SKIP ($MOVIE missing)"; exit 64; }
        MOVIE_ARG=(--bk2 "$MOVIE")
    else
        MOVIE_ARG=()
    fi
    rows=$((rows + 1))
    tag="${bk2%.bk2}"

    if (( V < 4 )); then
        echo "glint-subtick: FAIL (row $bk2: V=$V has no interval below it)"; exit 1
    fi
    prev=$((V - 2)) mid=$((V + 1))

    # Every capture directory and log is cleared before the run and every
    # output asserted after it, so a stale PNG cannot be read as this run's
    # result.
    rc=0 shots=()
    for lbl in on off; do
        case "$lbl" in on) g=2;; off) g=0;; esac
        d="$tag-c-$lbl"
        rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
        run_leg "$d" "$g" "" "$OUT/$d/$prev-vk.png,$OUT/$d/$V-vk.png" \
                --screenshot-on-frame "$prev,$V" </dev/null || rc=$?
        shots+=("$OUT/$d/$prev-vk.png" "$OUT/$d/$V-vk.png")
        for alpha in "${ALPHAS[@]}"; do
            d="$tag-i$alpha-$lbl"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" "$g" "$alpha" "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png")
        done
    done
    missing=0
    for f in "${shots[@]}"; do [[ -s "$f" ]] || missing=1; done
    if (( missing || rc )); then
        echo "glint-subtick: FAIL (row $bk2: capture incomplete, rc=$rc; see $OUT)"
        fail=1; continue
    fi

    python3 - "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "${ALPHAS[@]}" <<'PYEOF' || fail=1
import sys
from PIL import Image

game, bk2, V, keys, base, prev, mid = sys.argv[1:8]
alphas = sys.argv[8:]
V, prev, mid = int(V), int(prev), int(mid)

opts, gems = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k == 'gem':
        x, _, y = v.partition(',')
        gems.append((float(x), float(y)))
    else:
        opts[k] = v
tol = float(opts.get('tol', '0.5'))
judged = set(opts.get('judge', '0.5,1').split(','))
step_floor = float(opts.get('step', '4'))

# Two components of one mask are the same star when their pixels come within
# this Chebyshev distance: a four-point star's arms are contiguous, and the
# nearest two stars of either corpus row stand far further apart than this.
CLUSTER_R = 6
# A mask this large is not one feeder's primitives; the A/B isolated something
# else and its centroids would be meaningless.
MASK_CAP = 20000
# A named gem must resolve to one component: the runner-up must sit at least
# this many pixels further from the target than the winner.
AMBIG_MARGIN = 6.0
# The radius a named gem's component is looked for in, around the position the
# row names and around each tracked position after it.
FIND_R = 12.0


def mask(a_path, b_path):
    """The pixels the glint feed painted: the leave-one-out difference of the
    feed-on and feed-off captures of one present."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        raise SystemExit("[glint-subtick] %s and %s differ in size" % (a_path, b_path))
    pa, pb = a.load(), b.load()
    w, h = a.size
    px = [(x, y) for y in range(h) for x in range(w) if pa[x, y] != pb[x, y]]
    return px


def components(px):
    """Cluster mask pixels at CLUSTER_R, returning (cx, cy, count) per star."""
    seen = set(px)
    out = []
    while seen:
        stack = [seen.pop()]
        comp = [stack[0]]
        while stack:
            x, y = stack.pop()
            near = [q for q in seen
                    if abs(q[0] - x) <= CLUSTER_R and abs(q[1] - y) <= CLUSTER_R]
            for q in near:
                seen.discard(q)
                stack.append(q)
                comp.append(q)
        n = len(comp)
        out.append((sum(p[0] for p in comp) / n, sum(p[1] for p in comp) / n, n))
    return sorted(out, key=lambda c: (-c[2], c[0]))


def seg_dist(c, a, b):
    """Distance from a component's centroid to the segment between the two
    canonical positions — the line the star's own motion lies on."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / n
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return ((c[0] - (a[0] + dx * t)) ** 2 + (c[1] - (a[1] + dy * t)) ** 2) ** 0.5


def find(comps, tx, ty, radius):
    """The one component near (tx, ty), or None with the reason."""
    d = sorted(((((c[0] - tx) ** 2 + (c[1] - ty) ** 2) ** 0.5), c) for c in comps)
    if not d or d[0][0] > radius:
        return None, "no component within %.1f px of (%.1f,%.1f)" % (radius, tx, ty)
    if len(d) > 1 and d[1][0] - d[0][0] < AMBIG_MARGIN:
        return None, ("two components within %.1f px of each other at (%.1f,%.1f)"
                      % (AMBIG_MARGIN, tx, ty))
    return d[0][1], ""


legs = {}
maskpx = 0
vacuous, bad, notes = [], [], []
for name, on, off, vbl in (
        [("canon%d" % prev, "%s-c-on/%d-vk.png" % (base, prev),
          "%s-c-off/%d-vk.png" % (base, prev), prev),
         ("canon%d" % V, "%s-c-on/%d-vk.png" % (base, V),
          "%s-c-off/%d-vk.png" % (base, V), V)] +
        [("inter%s" % a, "%s-i%s-on/%d-vk.png" % (base, a, mid),
          "%s-i%s-off/%d-vk.png" % (base, a, mid), mid) for a in alphas]):
    px = mask(on, off)
    if len(px) > MASK_CAP:
        raise SystemExit("[glint-subtick] leg %s masks %d px, above the %d cap: the "
                         "leave-one-out is not isolating this feeder" % (name, len(px), MASK_CAP))
    legs[name] = components(px)
    maskpx += len(px)
    if not px:
        vacuous.append("leg %s masks no pixel: the feed painted nothing" % name)

worst = 0.0
rows_out = []
if not gems:
    vacuous.append("the row names no gem")
for gx, gy in gems:
    # The row names the gem where the replayed tick drew it; its partner one
    # tick back opens the interval.
    c1, why = find(legs["canon%d" % V], gx, gy, FIND_R)
    if c1 is None:
        vacuous.append("gem (%.1f,%.1f) at canon%d: %s" % (gx, gy, V, why))
        continue
    c0, why = find(legs["canon%d" % prev], c1[0], c1[1], 3 * FIND_R)
    if c0 is None:
        vacuous.append("gem (%.1f,%.1f) at canon%d: %s" % (gx, gy, prev, why))
        continue
    step = ((c1[0] - c0[0]) ** 2 + (c1[1] - c0[1]) ** 2) ** 0.5
    rows_out.append("gem (%.1f,%.1f) canon%d=(%.1f,%.1f) %dpx canon%d=(%.1f,%.1f) %dpx step=%.2f"
                    % (gx, gy, prev, c0[0], c0[1], c0[2], V, c1[0], c1[1], c1[2], step))
    if step < step_floor:
        vacuous.append("gem (%.1f,%.1f) steps %.2f px over the interval, below the %.2f floor"
                       % (gx, gy, step, step_floor))
        continue
    for a in alphas:
        av = float(a)
        ex = (c0[0] + (c1[0] - c0[0]) * av, c0[1] + (c1[1] - c0[1]) * av)
        # The component is identified by the interval it must lie on, not by
        # the expected point: a producer that holds its tick pose sits at the
        # interval's closing end, which is on the same segment, so one rule
        # names the same star before and after the fix.
        cand = sorted(((seg_dist(c, c0, c1), c) for c in legs["inter%s" % a]))
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("gem (%.1f,%.1f) at inter alpha=%s: no component on the interval"
                           % (gx, gy, a))
            continue
        if len(cand) > 1 and cand[1][0] - cand[0][0] < AMBIG_MARGIN:
            vacuous.append("gem (%.1f,%.1f) at inter alpha=%s: two components on the interval"
                           % (gx, gy, a))
            continue
        ci = cand[0][1]
        d = ((ci[0] - ex[0]) ** 2 + (ci[1] - ex[1]) ** 2) ** 0.5
        if a in judged:
            worst = max(worst, d)
        rows_out.append("  alpha=%s%s shown=(%.1f,%.1f) %dpx expected=(%.1f,%.1f) D=%.2f"
                        % (a, "" if a in judged else " (reported)",
                           ci[0], ci[1], ci[2], ex[0], ex[1], d))
        if a in judged and d > tol:
            bad.append("gem (%.1f,%.1f) alpha=%s D=%.2f above the %.2f bound"
                       % (gx, gy, a, d, tol))

head = ("[glint-subtick] game=%s bk2=%s V=%d gems=%d legs=%d maskpx=%d worst=%.2f"
        % (game, bk2, V, len(gems), len(legs), maskpx, worst))
for r in rows_out:
    print("[glint-subtick]   " + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[glint-subtick]   " + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[glint-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "glint-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
