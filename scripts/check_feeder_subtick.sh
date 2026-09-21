#!/usr/bin/env bash
# check_feeder_subtick.sh — sub-tick interpolation of a self-linking feeder,
# measured as the screen position of the pixels it paints. Two render paths use
# it: the gem glint (check_glint_subtick.sh) and the glow ring
# (check_glow_subtick.sh), which is the form each wrapper names.
#
# Each of these routines links its own primitives into the 3D ordering table
# and the feeder reads them back out of the polybuf, so what a sub-tick present
# has to work with is the guest's own int16 screen corners, built under the
# tick's camera from the tick's pool record. A replay that re-emits them
# verbatim draws the blob at the pixels the tick drew for the whole interior of
# the tick, and the next tick moves it the whole way at once: the blob sits
# still while the world around it slides, then jumps.
#
# The defect class is a sub-tick producer that holds its tick pose. The
# measurement is where the blob is on screen, per present. The instrument fits
# a compact blob that translates; a producer whose painted extent changes with
# its own state needs its own harness, which is why check_zap_subtick.sh is a
# sibling rather than a path here.
#
# A path is named by --path and carries: what suppresses its feed for the
# leave-one-out, the manifest it reads and the alphas it judges.
#   glint   OPENPETE_GLINT=2 against =0, games/<slug>/glint-subtick-corpus.manifest
#   glow    OPENPETE_GLOW=1  against =0, games/<slug>/glow-subtick-corpus.manifest
#   shadow  --render-paths terrain,moby,shadow against terrain,moby,
#           games/<slug>/shadow-subtick-corpus.manifest
#   portal  OPENPETE_PORTAL3_DBG=1, one leg per alpha, no leave-one-out,
#           games/<slug>/portal-subtick-corpus.manifest
#
# The portal path is the homeworld portal preview of Spyro 2 and Spyro 3, and
# it takes the other statistic this script carries. What moves there is the
# aperture the preview is seen through: a screen polygon spanning most of the
# frame, not a compact blob, so the pixel mask below does not model it and the
# MASK_CAP would refuse it. Its statistic is instead the aperture polygon's
# vertices, which the feeder emits exactly under OPENPETE_PORTAL3_DBG, one
# `[portal3-ap]` line per fed preview per present, with the canonical pass and
# the sub-tick replay distinguished by `src=`. The vertices are the feeder's
# own floats, so no leave-one-out and no clustering are needed: one run per
# alpha carries the two canonical ticks and the intermediate present together.
# `--render-paths` cannot isolate this path on its own — the ordering-table
# filter drops the portal prims before the drawing-primitive count, so fast
# boot never ends and no present is reached — and the vertex list is the
# isolation instead. The engagement counter is the fed triangle count per leg.
#
# The shadow path is the moby ground shadow, which no env knob switches off on
# its own, so its two legs are a render-path selection with and without the
# shadow path. Both legs run under a selection, so everything else on screen is
# identical between them, and the terrain and moby paths stay in because the
# shadow is a depth-aware decal tested against the world depth pre-pass.
# OPENPETE_SHADOW_PLAYER=0 on both legs drops the player's own fan, leaving the
# moby shadows as the only thing the difference can contain. The shadow is not
# a self-linking feeder replayed out of the polybuf: it is re-derived from
# gamestate every present, and what this harness measures there is whether that
# derivation reads the pose the present shows or the pose the newest tick left.
#
# Instrument: leave-one-out, not colour. Every leg is captured twice
# from one binary, the path's knob at its on value against 0 (the feed
# suppressed, the guest draw unchanged), and the pixels that differ are exactly
# the ones this feeder painted. Those pixels are clustered at a CLUSTER_R
# Chebyshev radius into components, one per blob on screen, and each
# component's centroid is its position. The component pixel count is the
# engagement counter: a leg whose mask is empty measured nothing.
#
# A sub-tick present replays the set the most recent canonical tick captured
# and shows it between the pose of the tick before (alpha 0) and the pose that
# tick drew (alpha 1). The present at VBL V+1 therefore covers the interval the
# two ticks at V-2 and V drew, so three captures per row, all from the same
# replay of one movie:
#   canon(V-2), canon(V)   the two canonical ticks, the interval's ends
#   inter(V+1, alpha)      one intermediate present over that interval
# A blob's expected position there is (1-alpha)*canon(V-2) + alpha*canon(V): it
# is on the world's own straight line between the two poses the ticks drew,
# because the camera transform the rest of the pass draws under is itself
# lerped by alpha.
#
# alpha 1 is the identity leg, where the expected position is canon(V)
# exactly: every term of a correct rebase is zero at the end of the tick's own
# interval, so the replay must reproduce the canonical present. alpha 0 is the
# leg that prices the defect: a producer that holds its tick pose reads canon(V)
# there, one whole tick step from where it belongs.
#
# Three statistics per named blob per row, all in screen pixels:
#   resid  the judged one: the distance from the shown centroid to the expected
#          (1-alpha)*canon(V-2) + alpha*canon(V), at each judged alpha
#   ident  resid at alpha 1, where the expected position is canon(V) exactly
#   own    |c(alpha 0) - canon(V-2)|, how far the alpha-0 leg stands from the
#          pose the previous tick drew: a producer that carries its own motion
#          reads near zero there, one that holds its tick pose reads the whole
#          canonical step
# The component's pixel count is printed beside every centroid. A producer
# whose painted extent changes with its own state ramps that count across the
# interval and moves the visible centroid with it, which is the residual's
# floor and the reason a row sets `tol` above the instrument's own noise.
#
# A path with a suppress knob also runs an engagement leg: the alpha-0
# present captured again with the knob at 0, where the producer must hold its
# tick pose. At least one of an `engage=1` row's blobs must read `own` of at
# least its canonical step there; with none the knob reached nothing this row
# can see, the normal legs prove nothing, and the row is VACUOUS. One blob
# suffices because a row may name a control blob the knob cannot move — a
# producer already at rest in the world, whose whole step is the camera's.
#
# alpha 0 is captured and reported, not judged. The replay is a screen-space
# rebase of the tick's own primitives, so at alpha 0 it shows the tick's blob —
# its size and its outline, which each routine derives from its record's own
# state — standing where the tick before drew its own, slightly different,
# blob. The two masks differ by a pixel or two and the centroid follows, which
# measures that state's ramp rather than the rebase. Reproducing the previous
# tick's blob outright needs a second copy of the routine's geometry, which
# these feeders deliberately do not carry.
#
# Input: the path's manifest, rows
#   <source> | <V> | <key>=<value> ...
# <source> is a movie under games/<slug>/bk2, or any name not ending in .bk2,
# which is the plain boot — the title screen and the attract demo, which need
# no input — replayed with no movie, the same movie-less row form the
# presentation corpus takes.
# V is the canonical VBL whose primitives the intermediate present replays;
# V-2 opens the interval and the intermediate present is captured at V+1. A
# tick spans two VBLs, so V must be the first VBL of its tick: V+1 otherwise
# falls in the next tick, where the present replays that tick's set over the
# interval above V and the alpha-1 identity leg misses canon(V) by a whole tick
# step. The alpha-1 leg is what catches a row of the wrong parity.  Keys:
#   blob=<x>,<y> a blob to judge, named by its mask component's centroid in the
#                canonical capture at V (`gem=` is the same key, the glint
#                manifests' spelling; `vertex=` the portal manifests', where
#                the position is an aperture polygon vertex rather than a
#                centroid). Repeatable; at least one required.
#                A row names the blobs unoccluded over the interval: a partly
#                covered blob's visible centroid is not the interpolation of
#                its two visible centroids, so judging it would measure the
#                depth test, not the rebase.
#   tol=<px>     centroid bound, default 0.5
#   judge=<a>[,<a>...]
#                the alphas the bound applies to, default the path's; every
#                captured alpha is reported either way
#   step=<px>    floor on the blob's canonical motion over the interval. A blob
#                that does not move between the two ticks passes every alpha
#                whatever the producer does, so a row below this floor is
#                VACUOUS rather than a pass. Default 4.
#   engage=0     this row runs no code the path's suppress knob reaches, so the
#                engagement leg below is captured and reported but asserts
#                nothing. It is how a control row on a game that takes another
#                code path for the same producer is declared. Default 1.
#
# Pass criterion, per row: every named blob, at every judged alpha, within
# `tol` of its expected centroid. VACUOUS (exit 1) when a leg's mask is empty,
# when a named blob resolves to no component or to an ambiguous one in some
# leg, when a blob's canonical step is below the floor, or when the path has a
# suppress knob and the engagement leg's `own` is below that step.
#
# Verdict line per row:
#   [<path>-subtick] game=<g> bk2=<b> V=<v> blobs=<n> legs=<n> maskpx=<n>
#                    worst=<px> verdict=PASS|FAIL
#
# Usage: scripts/check_feeder_subtick.sh --path glint|glow|shadow|portal
#        [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc, bk2, manifest, python3
# or PIL missing).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

PATHNAME=glint
prev_arg=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --path=*)      PATHNAME="${a#--path=}";;
    esac
    [[ "$prev_arg" == "--path" ]] && PATHNAME="$a"
    prev_arg="$a"
done

# Per render path: the knob that suppresses the feed, the value that turns it
# on, and the alphas whose residual the bound applies to. alpha 1 is judged on
# every path: it is the identity leg.
# LEG_ENV_ON/OFF and LEG_ARGS_ON/OFF are what the two leave-one-out legs
# differ by, plus whatever both of them need; SUPPRESS names the knob the
# engagement leg sets to 0, empty on a path that has none.
# STAT names the statistic: `mask` is the leave-one-out mask's per-blob
# centroid, `vertex` the feeder's own emitted polygon vertices.
LEG_ENV_ON=(); LEG_ENV_OFF=(); LEG_ARGS_ON=(); LEG_ARGS_OFF=(); SUPPRESS=; STAT=mask
case "$PATHNAME" in
    glint)  LEG_ENV_ON=(OPENPETE_GLINT=2); LEG_ENV_OFF=(OPENPETE_GLINT=0)
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    glow)   LEG_ENV_ON=(OPENPETE_GLOW=1); LEG_ENV_OFF=(OPENPETE_GLOW=0)
            ALPHAS=(0 0.25 0.5 0.75 1); JUDGE_DEF=0.25,0.5,0.75,1;;
    portal) LEG_ENV_ON=(OPENPETE_PORTAL3_DBG=1)
            SUPPRESS=OPENPETE_PORTAL3_INTERP; STAT=vertex
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    shadow) LEG_ENV_ON=(OPENPETE_SHADOW_PLAYER=0)
            LEG_ENV_OFF=(OPENPETE_SHADOW_PLAYER=0)
            LEG_ARGS_ON=(--render-paths terrain,moby,shadow)
            LEG_ARGS_OFF=(--render-paths terrain,moby)
            SUPPRESS=OPENPETE_SHADOW_INTERP
            ALPHAS=(0 0.5 1); JUDGE_DEF=0.5,1;;
    *)      echo "feeder-subtick: FAIL (unknown --path $PATHNAME)"; exit 1;;
esac
TAG="$PATHNAME-subtick"

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

MANIFEST="games/$GAME/$PATHNAME-subtick-corpus.manifest"
[[ -f "$MANIFEST" ]] || { echo "$TAG: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "$TAG: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "$TAG: SKIP (python3 or PIL missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/$TAG/$GAME"
mkdir -p "$OUT"

# One replay of a row's movie with one glint setting, capturing either the two
# canonical ticks or one intermediate present. The screenshot request is what
# makes the native renderer run and the intermediate present exist at all under
# --headless, so every leg carries one.
run_leg() {
    local tag="$1" leg="$2" alpha="$3" out="$4"; shift 4
    local log="$OUT/$tag.log"
    local -a legargs=()
    if [[ "$leg" == on ]]; then
        PG_CAPTURE_ENV=(${LEG_ENV_ON[@]+"${LEG_ENV_ON[@]}"})
        legargs=(${LEG_ARGS_ON[@]+"${LEG_ARGS_ON[@]}"})
    else
        PG_CAPTURE_ENV=(${LEG_ENV_OFF[@]+"${LEG_ENV_OFF[@]}"})
        legargs=(${LEG_ARGS_OFF[@]+"${LEG_ARGS_OFF[@]}"})
    fi
    [[ -n "$SUPPRESSED" ]] && PG_CAPTURE_ENV+=("$SUPPRESS=0")
    # The census reports from the interval's opening tick on, so a leg's log
    # holds canon(V-2), canon(V) and the present between them.
    [[ "$STAT" == vertex ]] && PG_CAPTURE_ENV+=(OPENPETE_PORTAL3_DBG_FROM="$prev")
    [[ -n "$alpha" ]] && PG_CAPTURE_ENV+=(OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$out" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original \
        "${MOVIE_ARG[@]}" --screenshot-native-only \
        ${legargs[@]+"${legargs[@]}"} \
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
        [[ -f "$MOVIE" ]] || { echo "$TAG: SKIP ($MOVIE missing)"; exit 64; }
        MOVIE_ARG=(--bk2 "$MOVIE")
    else
        MOVIE_ARG=()
    fi
    rows=$((rows + 1))
    # The capture directories carry V as well as the movie: a manifest may hold
    # two rows on one bk2, and a shared directory would leave the first row's
    # captures for the second row to clear.
    tag="${bk2%.bk2}-$V"

    if (( V < 4 )); then
        echo "$TAG: FAIL (row $bk2: V=$V has no interval below it)"; exit 1
    fi
    prev=$((V - 2)) mid=$((V + 1))

    # Every capture directory and log is cleared before the run and every
    # output asserted after it, so a stale PNG cannot be read as this run's
    # result.
    rc=0 shots=()
    SUPPRESSED=
    if [[ "$STAT" == vertex ]]; then
        # One run per alpha. The feeder's census in that run's log already
        # carries canon(V-2), canon(V) and the intermediate present, so the
        # separate canonical legs and the leave-one-out partner of the mask
        # statistic are not captured at all.
        for alpha in "${ALPHAS[@]}"; do
            d="$tag-i$alpha"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" on "$alpha" "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png" "$OUT/$d.log")
        done
        SUPPRESSED=1
        d="$tag-s"
        rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
        run_leg "$d" on 0 "$OUT/$d/$mid-vk.png" \
                --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
        shots+=("$OUT/$d/$mid-vk.png" "$OUT/$d.log")
        SUPPRESSED=
    else
    for lbl in on off; do
        d="$tag-c-$lbl"
        rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
        run_leg "$d" "$lbl" "" "$OUT/$d/$prev-vk.png,$OUT/$d/$V-vk.png" \
                --screenshot-on-frame "$prev,$V" </dev/null || rc=$?
        shots+=("$OUT/$d/$prev-vk.png" "$OUT/$d/$V-vk.png")
        for alpha in "${ALPHAS[@]}"; do
            d="$tag-i$alpha-$lbl"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" "$lbl" "$alpha" "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png")
        done
    done
    # The engagement leg: the alpha-0 present again with the path's own
    # interpolation suppressed, where the producer must hold its tick pose.
    if [[ -n "$SUPPRESS" ]]; then
        SUPPRESSED=1
        for lbl in on off; do
            d="$tag-s-$lbl"
            rm -rf "${OUT:?}/$d"; mkdir -p "$OUT/$d"
            run_leg "$d" "$lbl" 0 "$OUT/$d/$mid-vk.png" \
                    --screenshot-intermediate-on-frame "$mid" </dev/null || rc=$?
            shots+=("$OUT/$d/$mid-vk.png")
        done
        SUPPRESSED=
    fi
    fi
    missing=0
    for f in "${shots[@]}"; do [[ -s "$f" ]] || missing=1; done
    if (( missing || rc )); then
        echo "$TAG: FAIL (row $bk2: capture incomplete, rc=$rc; see $OUT)"
        fail=1; continue
    fi

    if [[ "$STAT" == vertex ]]; then
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" "${ALPHAS[@]}" <<'PYEOF' || fail=1
import re
import sys

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
alphas = sys.argv[11:]
V, prev, mid = int(V), int(prev), int(mid)

opts, named = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k in ('vertex', 'blob', 'gem'):
        x, _, y = v.partition(',')
        named.append((float(x), float(y)))
    else:
        opts[k] = v
tol = float(opts.get('tol', '0.5'))
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '4'))
engage = opts.get('engage', '1') != '0'

# Two aperture vertices this close are one corner: the clip emits a corner
# twice where an edge meets the rect exactly there, and the pair would
# otherwise read as an ambiguous match.
DEDUPE_R = 0.05
# A named corner must resolve to one vertex: the runner-up must sit at least
# this many pixels further from the target than the winner.
AMBIG_MARGIN = 6.0
# The radius a named corner is looked for in.
FIND_R = 12.0
# The bound the alpha-1 replay must reproduce the canonical present within,
# vertex for vertex: the two are the same float expression evaluated at 1, so
# the only slack is the census's own two printed decimals.
IDENT_TOL = 0.011

AP = re.compile(r'\[portal3-ap f=(\d+) src=(\w+) a=([0-9.]+) slot=(\d+) arg=(\d+) n=(\d+) '
                r'd7=(-?\d+) dw=(-?[0-9.]+) ir0=(-?\d+) far=([0-9A-F]+)\](.*)')
FEED = re.compile(r'\[portal3-feed f=(\d+) src=(\w+) a=([0-9.]+) portals=(\d+) '
                  r'tris=(\d+) emitted=(\d+)\]')
XY = re.compile(r'\(([-0-9.]+),([-0-9.]+)\)')


def read(path):
    """The aperture corners and the fed triangle count of every present in one
    run's census, keyed (src, VBL). Only the base record of each portal carries
    the aperture; its duplicate corners are folded."""
    verts, tris = {}, {}
    try:
        fh = open(path, errors='replace')
    except IOError:
        return None, None
    with fh:
        for ln in fh:
            ln = ln.strip()
            m = FEED.match(ln)
            if m:
                tris[(m.group(2), int(m.group(1)))] = int(m.group(5))
                continue
            m = AP.match(ln)
            if not m or m.group(5) != '1':
                continue
            pts = verts.setdefault((m.group(2), int(m.group(1))), [])
            for x, y in XY.findall(m.group(11)):
                q = (float(x), float(y))
                if all(abs(q[0] - r[0]) > DEDUPE_R or abs(q[1] - r[1]) > DEDUPE_R
                       for r in pts):
                    pts.append(q)
    return verts, tris


def find(pts, tx, ty, radius):
    """The one corner near (tx, ty), or None with the reason."""
    d = sorted((((p[0] - tx) ** 2 + (p[1] - ty) ** 2) ** 0.5, p) for p in pts)
    if not d or d[0][0] > radius:
        return None, "no vertex within %.1f px of (%.1f,%.1f)" % (radius, tx, ty)
    if len(d) > 1 and d[1][0] - d[0][0] < AMBIG_MARGIN:
        return None, ("two vertices within %.1f px of each other at (%.1f,%.1f)"
                      % (AMBIG_MARGIN, tx, ty))
    return d[0][1], ""


def seg_dist(c, a, b):
    """Distance from a corner to the segment between its two canonical
    positions - the line its own motion lies on."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = dx * dx + dy * dy
    t = 0.0 if n == 0 else ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / n
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return ((c[0] - (a[0] + dx * t)) ** 2 + (c[1] - (a[1] + dy * t)) ** 2) ** 0.5


legs = {}
vacuous, bad, rows_out = [], [], []
tris_total = 0
for name, path in ([(a, "%s-i%s.log" % (base, a)) for a in alphas]
                   + ([("suppressed", "%s-s.log" % base)] if suppress else [])):
    verts, tris = read(path)
    if verts is None:
        vacuous.append("leg %s: no log at %s" % (name, path))
        continue
    legs[name] = verts
    for key in (("canon", prev), ("canon", V), ("sub", mid)):
        if key not in verts:
            vacuous.append("leg %s: no %s present at VBL %d in %s"
                           % (name, key[0], key[1], path))
        elif tris.get(key, 0) == 0:
            vacuous.append("leg %s: the %s present at VBL %d fed no portal triangle"
                           % (name, key[0], key[1]))
        else:
            tris_total += tris[key]

if not named:
    vacuous.append("the row names no vertex")
if vacuous:
    print("[%s] game=%s bk2=%s V=%d verts=%d legs=%d tris=%d VACUOUS"
          % (tag, game, bk2, V, len(named), len(legs), tris_total))
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)

# The identity leg, whole-polygon: at alpha 1 the replay evaluates the same
# expression at the tick's own instant, so it must reproduce the canonical
# present corner for corner, not merely within the residual bound.
if "1" in legs:
    c = legs["1"][("canon", V)]
    r = legs["1"][("sub", mid)]
    if len(c) != len(r):
        bad.append("identity: alpha 1 replays %d corners, canon%d has %d"
                   % (len(r), V, len(c)))
    else:
        worst_id = max([max(abs(a[0] - b[0]), abs(a[1] - b[1]))
                        for a, b in zip(c, r)] or [0.0])
        rows_out.append("identity alpha=1 corners=%d worst=%.3f" % (len(c), worst_id))
        if worst_id > IDENT_TOL:
            bad.append("identity: alpha 1 differs from canon%d by %.3f px"
                       % (V, worst_id))

worst = 0.0
engaged = 0
for gx, gy in named:
    # The row names the corner where the replayed tick drew it; its partner one
    # tick back opens the interval. Both are read from the alpha-1 leg's own
    # census, and every other leg's canonical presents are the same run.
    ref = legs.get("1") or legs[alphas[0]]
    c1, why = find(ref[("canon", V)], gx, gy, FIND_R)
    if c1 is None:
        vacuous.append("vertex (%.1f,%.1f) at canon%d: %s" % (gx, gy, V, why))
        continue
    c0, why = find(ref[("canon", prev)], c1[0], c1[1], 3 * FIND_R)
    if c0 is None:
        vacuous.append("vertex (%.1f,%.1f) at canon%d: %s" % (gx, gy, prev, why))
        continue
    step = ((c1[0] - c0[0]) ** 2 + (c1[1] - c0[1]) ** 2) ** 0.5
    rows_out.append("vertex (%.1f,%.1f) canon%d=(%.2f,%.2f) canon%d=(%.2f,%.2f) step=%.2f"
                    % (gx, gy, prev, c0[0], c0[1], V, c1[0], c1[1], step))
    if step < step_floor:
        vacuous.append("vertex (%.1f,%.1f) steps %.2f px over the interval, below the %.2f floor"
                       % (gx, gy, step, step_floor))
        continue
    shown0 = None
    for a in alphas:
        av = float(a)
        ex = (c0[0] + (c1[0] - c0[0]) * av, c0[1] + (c1[1] - c0[1]) * av)
        # The corner is identified by the interval it must lie on, not by the
        # expected point: a replay that holds its tick state sits at the
        # interval's closing end, which is on the same segment, so one rule
        # names the same corner before and after a fix.
        cand = sorted((seg_dist(p, c0, c1), p) for p in legs[a][("sub", mid)])
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("vertex (%.1f,%.1f) at alpha=%s: no corner on the interval"
                           % (gx, gy, a))
            continue
        if len(cand) > 1 and cand[1][0] - cand[0][0] < AMBIG_MARGIN:
            vacuous.append("vertex (%.1f,%.1f) at alpha=%s: two corners on the interval"
                           % (gx, gy, a))
            continue
        ci = cand[0][1]
        if av == 0.0:
            shown0 = ci
        d = ((ci[0] - ex[0]) ** 2 + (ci[1] - ex[1]) ** 2) ** 0.5
        if a in judged:
            worst = max(worst, d)
        rows_out.append("  alpha=%s%s shown=(%.2f,%.2f) expected=(%.2f,%.2f) D=%.2f"
                        % (a, "" if a in judged else " (reported)",
                           ci[0], ci[1], ex[0], ex[1], d))
        if a in judged and d > tol:
            bad.append("vertex (%.1f,%.1f) alpha=%s D=%.2f above the %.2f bound"
                       % (gx, gy, a, d, tol))

    # own: how far the alpha-0 leg stands from the corner the previous tick
    # drew. A replay that carries the interval reads near zero there, one that
    # holds its tick state reads the whole step.
    if shown0 is not None:
        own = ((shown0[0] - c0[0]) ** 2 + (shown0[1] - c0[1]) ** 2) ** 0.5
        rows_out.append("  own=%.2f of step=%.2f" % (own, step))
    # The engagement leg: with the replay's interpolation suppressed
    # the aperture must hold its tick state, the whole step from the previous
    # tick's. A smaller reading means the knob reached nothing.
    if suppress:
        cand = sorted((seg_dist(p, c0, c1), p) for p in legs["suppressed"][("sub", mid)])
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("vertex (%.1f,%.1f) under %s=0: no corner on the interval"
                           % (gx, gy, suppress))
        else:
            cs = cand[0][1]
            own_s = ((cs[0] - c0[0]) ** 2 + (cs[1] - c0[1]) ** 2) ** 0.5
            rows_out.append("  %s=0 own=%.2f of step=%.2f" % (suppress, own_s, step))
            if own_s >= step - IDENT_TOL:
                engaged += 1

if suppress and engage and not engaged:
    vacuous.append("no corner holds its tick state under %s=0: the knob suppressed "
                   "nothing this row can see" % suppress)
head = ("[%s] game=%s bk2=%s V=%d verts=%d legs=%d tris=%d worst=%.2f"
        % (tag, game, bk2, V, len(named), len(legs), tris_total, worst))
for r in rows_out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    else
    python3 - "$TAG" "$JUDGE_DEF" "$GAME" "$bk2" "$V" "$keys" "$OUT/$tag" "$prev" "$mid" \
             "$SUPPRESS" "${ALPHAS[@]}" <<'PYEOF' || fail=1
import sys
from PIL import Image

tag, judge_def, game, bk2, V, keys, base, prev, mid, suppress = sys.argv[1:11]
alphas = sys.argv[11:]
V, prev, mid = int(V), int(prev), int(mid)

opts, gems = {}, []
for tok in keys.split():
    k, _, v = tok.partition('=')
    if k in ('gem', 'blob'):
        x, _, y = v.partition(',')
        gems.append((float(x), float(y)))
    else:
        opts[k] = v
tol = float(opts.get('tol', '0.5'))
judged = set(opts.get('judge', judge_def).split(','))
step_floor = float(opts.get('step', '4'))
engage = opts.get('engage', '1') != '0'

# Two components of one mask are the same blob when their pixels come within
# this Chebyshev distance: a four-point star's arms and a glow ring's fan are
# each contiguous, and the nearest two blobs of any corpus row stand far
# further apart than this.
CLUSTER_R = 6
# A mask this large is not one feeder's primitives; the A/B isolated something
# else and its centroids would be meaningless.
MASK_CAP = 20000
# A named blob must resolve to one component: the runner-up must sit at least
# this many pixels further from the target than the winner.
AMBIG_MARGIN = 6.0
# The radius a named blob's component is looked for in, around the position the
# row names and around each tracked position after it.
FIND_R = 12.0


def mask(a_path, b_path):
    """The pixels this feed painted: the leave-one-out difference of the
    feed-on and feed-off captures of one present."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        raise SystemExit("[%s] %s and %s differ in size" % (tag, a_path, b_path))
    pa, pb = a.load(), b.load()
    w, h = a.size
    px = [(x, y) for y in range(h) for x in range(w) if pa[x, y] != pb[x, y]]
    return px


def components(px):
    """Cluster mask pixels at CLUSTER_R, returning (cx, cy, count) per blob."""
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
    canonical positions — the line the blob's own motion lies on."""
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
          "%s-i%s-off/%d-vk.png" % (base, a, mid), mid) for a in alphas] +
        ([("suppressed", "%s-s-on/%d-vk.png" % (base, mid),
           "%s-s-off/%d-vk.png" % (base, mid), mid)] if suppress else [])):
    px = mask(on, off)
    if len(px) > MASK_CAP:
        raise SystemExit("[%s] leg %s masks %d px, above the %d cap: the "
                         "leave-one-out is not isolating this feeder"
                         % (tag, name, len(px), MASK_CAP))
    legs[name] = components(px)
    maskpx += len(px)
    if not px:
        vacuous.append("leg %s masks no pixel: the feed painted nothing" % name)

worst = 0.0
engaged = 0
rows_out = []
if not gems:
    vacuous.append("the row names no blob")
for gx, gy in gems:
    # The row names the blob where the replayed tick drew it; its partner one
    # tick back opens the interval.
    c1, why = find(legs["canon%d" % V], gx, gy, FIND_R)
    if c1 is None:
        vacuous.append("blob (%.1f,%.1f) at canon%d: %s" % (gx, gy, V, why))
        continue
    c0, why = find(legs["canon%d" % prev], c1[0], c1[1], 3 * FIND_R)
    if c0 is None:
        vacuous.append("blob (%.1f,%.1f) at canon%d: %s" % (gx, gy, prev, why))
        continue
    step = ((c1[0] - c0[0]) ** 2 + (c1[1] - c0[1]) ** 2) ** 0.5
    rows_out.append("blob (%.1f,%.1f) canon%d=(%.1f,%.1f) %dpx canon%d=(%.1f,%.1f) %dpx step=%.2f"
                    % (gx, gy, prev, c0[0], c0[1], c0[2], V, c1[0], c1[1], c1[2], step))
    if step < step_floor:
        vacuous.append("blob (%.1f,%.1f) steps %.2f px over the interval, below the %.2f floor"
                       % (gx, gy, step, step_floor))
        continue
    shown0 = None
    for a in alphas:
        av = float(a)
        ex = (c0[0] + (c1[0] - c0[0]) * av, c0[1] + (c1[1] - c0[1]) * av)
        # The component is identified by the interval it must lie on, not by
        # the expected point: a producer that holds its tick pose sits at the
        # interval's closing end, which is on the same segment, so one rule
        # names the same star before and after the fix.
        cand = sorted(((seg_dist(c, c0, c1), c) for c in legs["inter%s" % a]))
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("blob (%.1f,%.1f) at inter alpha=%s: no component on the interval"
                           % (gx, gy, a))
            continue
        if len(cand) > 1 and cand[1][0] - cand[0][0] < AMBIG_MARGIN:
            vacuous.append("blob (%.1f,%.1f) at inter alpha=%s: two components on the interval"
                           % (gx, gy, a))
            continue
        ci = cand[0][1]
        if float(a) == 0.0:
            shown0 = ci
        d = ((ci[0] - ex[0]) ** 2 + (ci[1] - ex[1]) ** 2) ** 0.5
        if a in judged:
            worst = max(worst, d)
        rows_out.append("  alpha=%s%s shown=(%.1f,%.1f) %dpx expected=(%.1f,%.1f) D=%.2f"
                        % (a, "" if a in judged else " (reported)",
                           ci[0], ci[1], ci[2], ex[0], ex[1], d))
        if a in judged and d > tol:
            bad.append("blob (%.1f,%.1f) alpha=%s D=%.2f above the %.2f bound"
                       % (gx, gy, a, d, tol))

    # own: how far the alpha-0 leg stands from the pose the previous tick drew.
    # A producer that carries its own motion reads near zero, one that holds
    # its tick pose reads the whole step.
    if shown0 is not None:
        own = ((shown0[0] - c0[0]) ** 2 + (shown0[1] - c0[1]) ** 2) ** 0.5
        rows_out.append("  own=%.2f of step=%.2f  area canon%d=%dpx canon%d=%dpx alpha0=%dpx"
                        % (own, step, prev, c0[2], V, c1[2], shown0[2]))
    # The engagement leg: with the path's interpolation suppressed the
    # producer must hold its tick pose, which is the whole step away from the
    # previous tick's. A smaller reading means the knob reached nothing.
    if suppress:
        cand = sorted(((seg_dist(c, c0, c1), c) for c in legs["suppressed"]))
        if not cand or cand[0][0] > FIND_R:
            vacuous.append("blob (%.1f,%.1f) under %s=0: no component on the interval"
                           % (gx, gy, suppress))
        else:
            cs = cand[0][1]
            own_s = ((cs[0] - c0[0]) ** 2 + (cs[1] - c0[1]) ** 2) ** 0.5
            rows_out.append("  %s=0 own=%.2f of step=%.2f %dpx" % (suppress, own_s, step, cs[2]))
            if own_s >= step:
                engaged += 1

if suppress and engage and not engaged:
    vacuous.append("no blob holds its tick pose under %s=0: the knob suppressed "
                   "nothing this row can see" % suppress)
head = ("[%s] game=%s bk2=%s V=%d blobs=%d legs=%d maskpx=%d worst=%.2f"
        % (tag, game, bk2, V, len(gems), len(legs), maskpx, worst))
for r in rows_out:
    print("[%s]   " % tag + r)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[%s]   " % tag + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[%s]   " % tag + b)
sys.exit(1 if bad else 0)
PYEOF
    fi
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "$TAG: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
