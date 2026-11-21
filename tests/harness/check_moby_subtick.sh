#!/usr/bin/env bash
# check_moby_subtick.sh - sub-tick continuity of the animated mobys.
#
# An intermediate present shows each moby between the pose the previous
# canonical tick drew (alpha 0) and the pose the current one drew (alpha 1).
# The previous pose is rebuilt from the interp snapshot of the moby's
# animation state bytes, so two measurements follow:
#
#   tuple  the rebuilt previous pose (keyframe A, keyframe B, blend byte) at
#          trace VBL v equals the pose the record of the tick before (v - 2)
#          carries; keyframe B is compared only when that blend byte is
#          nonzero. A moby with no previous pose (prevkf=0) starts on its
#          current pose, so that pose is the tuple compared.
#   seam   D = |c_a0(v) - c_a1(v - 2)|, the screen centroid of the moby at
#          alpha 0 against the same moby at alpha 1 one tick earlier: one
#          interval starts where the interval before ended. D carries a
#          floor from the replayed integer record, so it is bounded per moby
#          rather than required to be zero.
#
# The defect class is a leg whose previous tick's record blends (a nonzero
# blend byte) toward keyframe 0 (nextFrame 0) or toward another animation
# (nextId different from id): the moby blending out of one clip into the
# next.
#
# A second bin is the held leg, prevkf=2: the moby switched into a clip whose
# vertex count differs from the one before, where no pose between the two
# meshes exists and the pass shows the previous tick's pose whole until alpha
# 1. Its interval therefore starts exactly where the one before ended, so its
# D is required to be zero outright rather than bounded per moby.
#
# A third bin is the facing reversal: an interval whose moby turns 0x40 byte
# angles (90 degrees) or more on an euler axis while its keyframe pair
# changes. The two channels the sub-tick pass interpolates - the per-vertex
# model-space shape lerp and the euler sweep - cancel each other at the tick
# instant and not between: the shape lerp between a pose and its mirror image
# has in-plane extent proportional to |1 - 2a| and collapses onto the flip's
# invariant axis at alpha 0.5. No pose exists between the two, so the interval
# must be held (prevkf=2), which the bin requires of every such leg. Both the
# euler step and the keyframe change are read off the two canonical ticks (the
# alpha-1 leg of v and of v - 2), so the bin measures the interval rather than
# the engine's verdict on it.
#
# A fourth bin, enabled by the row's wbase= key, is the world bucket base: the
# base a sub-tick present computes for a record's moby (moby_present_wbase,
# the cull's (TRZ << 2) >> 7 less m_DepthOffset at the present's camera and
# at the position the moby is drawn at) must at alpha 0 equal the base the
# cull stored at record +0x18 on the tick before, read off the alpha-1 replay
# at v - 2. A base taken at the newest tick's position words while the
# geometry draws at the interpolated position fails it on every leg whose
# moby moves across a bucket between the ticks.
#
# Input: games/<slug>/corpus/moby-subtick-corpus.manifest, rows
#   <source> | <lo>-<hi> | <key>=<value> ... [| <exclusion>]...
# where <source> is a movie under games/<slug>/bk2, `attract` (the boot with
# no movie: the title screen and the attract demo), `pose:<level>:<pose>`,
# which skips to the level and places the freecam (the pose's spaces are kept),
# or `pose:<level>:<area>:<pose>`, which skips to the integer sub-area.
# The span is the intermediate-capture VBLs, two apart; the [moby-anim] and
# [moby-screen] traces name each present by the VBL they print (the trace
# VBL), which is what the exclusions use. Keys:
#   pairs=<n>   floor on the bracketed legs measured
#   class=<n>   floor on the defect-class legs measured
#   hold=<n>    floor on the held (prevkf=2) legs measured
#   mirror=<n>  floor on the facing-reversal legs measured
#   turn=<n>    floor on the turn legs measured; the key adds a replay at
#               alpha 0.5 and the turn bin: no leg whose euler steps (not
#               held, not a facing reversal, centroid step at least 0.5 px)
#               shows a channel drawn at the current tick through the
#               interval (see TURN_MIN_PX below)
#   dmax=<px>   optional: every leg's D at or below this bound
#   wbase=<n>   floor on the moving legs of the wbase bin (the records whose
#               moby_record_world_offset is nonzero at alpha 0); the key adds
#               the bin: every alpha-0 base equals the stored base at v - 2
#   spin=<n>    pose rows: the freecam engages at VBL lo - 10 instead of at
#               the level's release and turns n yaw units (4096 = one turn)
#               per tick from the row's pose (OPENPETE_FREECAM_AT_VBL,
#               OPENPETE_FREECAM_SPIN), so the span presents a camera in motion
#   still=<n>   floor on the still legs measured; the key adds the still bin:
#               a leg whose moby reads the same world position and angle word
#               on the two canonical ticks ([moby-world] at alpha 1 of v - 2
#               and of v) draws the same pose under the same camera at alpha 0
#               of v and alpha 1 of v - 2, so its D is at most STILL_EPS
#               whatever the camera does between ticks
# An exclusion is a known-open leg, named with its reason:
#   leg <vbl> <moby> <reason>    left out of both checks
#   seam <vbl> <moby> <reason>   left out of the D bound only
# Every exclusion is printed with its reason on each run.
#
# Instrument (stderr), OPENPETE_MOBY_POSTRACE_CLS=0xFFFF, one replay at
# OPENPETE_INTERMEDIATE_ALPHA=0 and one at 1 per row, each split into runs of
# at most 60 intermediate captures:
#   [moby-anim vbl=<v>] m=<moby> ... prevkf=<0|1> ... fa= fb= fap= fbp= w0= w0p=
#   [moby-screen vbl=<v>] leg=inter ... m=<moby> ... c=(x,y)
#   [moby-wbase vbl=<v>] m=<moby> stored=<base|-> wbase=<base|-> woff=<0|1>
#
# Pass criterion, per row: 0 tuple mismatches; every class leg's D at or
# below the largest D the same moby reads on its other legs; every held leg's
# D zero to HOLD_EPS; with dmax, every leg's D at or below it; with still,
# every still leg's D at or below STILL_EPS; with wbase, every alpha-0 base equal
# to the stored base at v - 2. VACUOUS (exit 1) when the bracketed, the
# class, the held, the still or the moving wbase leg count is below the
# row's floor.
#
# Verdict line per row:
#   [moby-subtick] game=<g> bk2=<b> pairs=<n> class=<n> hold=<n>
#                  mismatches=<n> snaps=<n> excluded=<n> verdict=PASS|FAIL
#
# Usage: tests/harness/check_moby_subtick.sh [--game SLUG] [--build-dir=DIR]
#                                      [--filter=SUBSTR]
# --filter runs only the rows whose source contains SUBSTR.
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (binary, disc, bk2 or python3 missing).
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

CHUNK=60
MANIFEST="$(pg_corpus_manifest "$GAME" moby-subtick-corpus.manifest)"
pg_manifest "$MANIFEST" line
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "moby-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "moby-subtick: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/moby-subtick/$GAME"
mkdir -p "$OUT"

# One replay of a row at one alpha, as runs of at most CHUNK captures whose
# logs append to one file. The screenshots are the capture requests that make
# the intermediate presents exist headless; each run's last one is asserted.
run_alpha() {
    local alpha="$1" tag="$2"; shift 2
    local -a src=("${SRC[@]}")
    local dir="$OUT/$tag-a$alpha" log="$OUT/$tag-a$alpha.log"
    local -a vbls=("$@")
    mkdir -p "$dir"
    find "${dir:?}" -maxdepth 1 -name '*.png' -delete
    rm -f "$log" "$log.part"
    # The moby occlusion cull is off: the harness measures pose continuity
    # of every moby, and the pose trace is printed only for mobys the cull
    # keeps, whose set follows the previous present's depth grid.
    PG_CAPTURE_ENV=(OPENPETE_MOBY_POSTRACE_CLS=0xFFFF OPENPETE_MOBY_OCCLUSION=0
                    OPENPETE_INTERMEDIATE_ALPHA="$alpha" "${ROW_ENV[@]}")
    PG_CAPTURE_TIMEOUT=600
    local i n=${#vbls[@]}
    for ((i = 0; i < n; i += CHUNK)); do
        local -a part=("${vbls[@]:i:CHUNK}")
        local last="${part[${#part[@]}-1]}" csv
        csv="$(IFS=,; echo "${part[*]}")"
        pg_capture "$log.part" "$dir/i__$last-vk.png" -- \
            --renderer vk --mute 1 --aspect 4:3 --lod original \
            "${src[@]}" --screenshot-native-only \
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
    IFS='|' read -r src span keys excl <<<"$line"
    [[ -z "$FILTER" || "$src" == *"$FILTER"* ]] || continue
    span="${span// /}"
    # A row names either a movie or a freecam pose: `<bk2>` replays the movie,
    # `pose:<level>:<pose>` skips to the level and places the freecam, whose
    # internal spaces survive the trim; `pose:<level>:<area>:<pose>`, with an
    # integer area, skips to that sub-area (--skip-to-area) instead. A pose
    # row's tag is the level (and area) plus a short hash of the pose, the same
    # slug form the freecam corpus uses.
    area_row=0
    if [[ "$src" == *pose:* ]]; then
        src="${src#"${src%%[![:space:]]*}"}"; src="${src%"${src##*[![:space:]]}"}"
        rest="${src#pose:}"; level="${rest%%:*}"; pose="${rest#*:}"
        area="${pose%%:*}"
        if [[ "$pose" == *:* && "$area" =~ ^[0-9]+$ ]]; then
            pose="${pose#*:}"
            SRC=(--skip-to-area "$level:$area" --freecam-pose "$pose")
            # The scripted freecam waits for the area's release, since the
            # sub-area transition re-seeds the camera from the game's; the
            # run's log order is checked for that below.
            area_row=1
            tag="$level-$area-$(printf '%s' "$pose" | md5sum | cut -c1-6)"
            row="$level:$area"
        else
            SRC=(--skip-to-level "$level" --freecam-pose "$pose")
            tag="$level-$(printf '%s' "$pose" | md5sum | cut -c1-6)"
            row="$level"
        fi
    elif [[ "${src// /}" == attract ]]; then
        src=attract
        SRC=()
        tag=attract
        row=attract
    else
        src="${src// /}"
        movie="$PG_BK2_DIR/$src"
        [[ -f "$movie" ]] || { echo "moby-subtick: SKIP ($movie missing)"; exit 64; }
        SRC=(--bk2 "$movie")
        tag="${src%.bk2}"
        row="$src"
    fi
    rows=$((rows + 1))

    lo="${span%-*}"; hi="${span#*-}"
    ROW_ENV=()
    if [[ "$keys" =~ (^|[[:space:]])spin=(-?[0-9]+) ]]; then
        if [[ "$src" != pose:* ]]; then
            echo "moby-subtick: FAIL (row $row: spin= needs a pose row)"; exit 1
        fi
        ROW_ENV=(OPENPETE_FREECAM_SPIN="${BASH_REMATCH[2]}" OPENPETE_FREECAM_AT_VBL=$((lo - 10)))
    fi
    if (( (hi - lo) % 2 != 0 || hi < lo + 2 )); then
        echo "moby-subtick: FAIL (row $row: span $span is not lo-hi with hi = lo + 2k)"; exit 1
    fi
    vbls=()
    for ((f = lo; f <= hi; f += 2)); do vbls+=("$f"); done

    rc0=0 rc1=0 rch=0
    run_alpha 0 "$tag" "${vbls[@]}" </dev/null || rc0=$?
    run_alpha 1 "$tag" "${vbls[@]}" </dev/null || rc1=$?
    # The turn bin reads a third replay at the interval's midpoint.
    logh=""
    if [[ "$keys" == *turn=* ]]; then
        run_alpha 0.5 "$tag" "${vbls[@]}" </dev/null || rch=$?
        logh="$OUT/$tag-a0.5.log"
        [[ -s "$logh" ]] || rch=1
    fi
    if [[ ! -s "$OUT/$tag-a0.log" || ! -s "$OUT/$tag-a1.log" ]] || (( rc0 || rc1 || rch )); then
        echo "moby-subtick: FAIL (row $row: no instrument output, rc=$rc0/$rc1/$rch; see $OUT)"
        fail=1; continue
    fi
    if (( area_row )); then
        rel="$(grep -n -m1 '\[skip-to-area\] released' "$OUT/$tag-a1.log" | cut -d: -f1)"
        fc="$(grep -n 'freecam: pose set to' "$OUT/$tag-a1.log" | tail -1 | cut -d: -f1)"
        if [[ -z "$rel" || -z "$fc" ]] || (( fc < rel )); then
            echo "moby-subtick: FAIL (row $row: the freecam pose is not placed after the area release; see $OUT/$tag-a1.log)"
            fail=1; continue
        fi
    fi

    python3 - "$OUT/$tag-a0.log" "$OUT/$tag-a1.log" "$GAME" "$row" "$keys" "${excl:-}" "$logh" <<'PYEOF' || fail=1
import re, sys, collections

log0, log1, game, bk2, keys, excl, logh = sys.argv[1:8]
opts = {}
for tok in keys.split():
    k, _, v = tok.partition('=')
    opts[k] = v
min_pairs = int(opts.get('pairs', '1'))
min_class = int(opts.get('class', '0'))
min_hold = int(opts.get('hold', '0'))
min_mirror = int(opts.get('mirror', '0'))
min_turn = int(opts.get('turn', '0'))
min_still = int(opts.get('still', '0'))
min_wbase = int(opts['wbase']) if 'wbase' in opts else None
# A still leg: the moby's world position and angle word are equal on the two
# canonical ticks bounding the interval. Alpha 0 of v and alpha 1 of v - 2 then
# present the same pose under the same camera, so D is the float residue of
# two derivations of one centroid. STILL_EPS bounds it; the replayed record's
# integer translation, stored on the cull's 4-world-unit grid and re-derived
# for a new camera each tick, reads that grid's residue instead (0.19 px per
# unit at view depth 1880).
STILL_EPS = 0.05
# A turn leg: the interval steps the moby's euler on some axis, is neither
# held nor a facing reversal, and moves the screen centroid at least
# TURN_MIN_PX between the two canonical ticks (D = c_1(v) - c_1(v-2)). Every
# channel that sweeps is at the previous tick's value at alpha 0 and halfway
# at alpha 0.5; a channel drawn at the current tick's value through the
# interval shows its whole centroid step R at alpha 0, e0 = c_0(v) - c_1(v-2)
# = R, and half of it again at the midpoint, eh = c_0.5(v) - (c_1(v-2) +
# c_1(v)) / 2 = R / 2. The stepped channel's midpoint progress
# rho = 0.5 + eh.e0 / |e0|^2 reads 1 for that snap and is undefined for a
# sweep (e0 = 0). A leg whose |e0| is at least TURN_MIN_PX and whose rho is
# within TURN_TOL of 1 is a snap; the record replay's translation floor reads
# at most 0.43 px of e0 on the rows' non-turning legs.
TURN_MIN_PX = 0.5
TURN_TOL = 0.25
dmax = float(opts['dmax']) if 'dmax' in opts else None
# A facing reversal: an interval whose euler steps this far on any axis, in
# byte angle units (256 = one turn), while the keyframe pair changes. Read off
# the two canonical ticks, not off the engine's verdict, so the bin measures
# the interval rather than the rule applied to it.
MIRROR_THRESH = 0x40
# A held leg reuses the pose of the tick before, so its centroid is that
# tick's alpha-1 centroid recomputed from the same inputs: equal to the last
# bit of the float format, not merely close.
HOLD_EPS = 0.0005

skip_leg, skip_seam = {}, {}
for field in excl.split('|'):
    words = field.split()
    if not words:
        continue
    if len(words) < 4 or words[0] not in ('leg', 'seam'):
        print("[moby-subtick] game=%s bk2=%s FAIL: malformed exclusion '%s'" % (game, bk2, field.strip()))
        sys.exit(1)
    (skip_leg if words[0] == 'leg' else skip_seam)[(int(words[1]), words[2].lower())] = ' '.join(words[3:])

ani = re.compile(r'\[moby-anim vbl=(\d+)\] m=(\w+) cls=(\w+) prevkf=(\d) balpha=([\d.]+) '
                 r'curr\(frm=(\d+) nfrm=(\d+) prog=(\d+) anim=(\d+)(?: nid=(\d+))?\) '
                 r'fa=(\w+) fb=(\w+) fap=(\w+) fbp=(\w+) w0=(\w+) w0p=(\w+) '
                 r'hdr=(\w+) hdrp=\w+ rot=(\w+)')
wbs = re.compile(r'\[moby-wbase vbl=(\d+)\] m=(\w+) stored=(-?\d+|-) wbase=(-?\d+|-) woff=([01])')
wld = re.compile(r'\[moby-world vbl=(\d+)\] m=(\w+) rot=(\w+) kf=\S+ wpos=\(([-\d,]+)\)')
scr = re.compile(r'\[moby-screen vbl=(\d+)\] leg=inter now=\S+ m=(\w+) cls=\w+ n=\d+ '
                 r'c=\(([-\d.]+),([-\d.]+)\)')

def load(path):
    anim, screen = {}, {}
    for line in open(path, errors='replace'):
        m = wbs.search(line)
        if m:
            WBASE.setdefault(path, {}).setdefault((int(m[1]), m[2].lower()), (m[3], m[4], m[5] == '1'))
            continue
        m = wld.search(line)
        if m:
            WORLD.setdefault(path, {}).setdefault((int(m[1]), m[2].lower()), (m[3].upper(), m[4]))
            continue
        m = ani.search(line)
        if m:
            anim.setdefault((int(m[1]), m[2].lower()), m)
            continue
        m = scr.search(line)
        if m:
            screen.setdefault((int(m[1]), m[2].lower()), (float(m[3]), float(m[4])))
    return anim, screen

WORLD = {}
WBASE = {}
A0, S0 = load(log0)
# The alpha-1 leg is the identity leg: every present there shows the pose its
# own tick drew. The alpha-0 leg does not, because a held leg shows the pose
# of the tick BEFORE it, so the drawn pose of tick v - 2 is read from the
# alpha-1 log and the rebuilt pose at tick v from the alpha-0 one.
A1, S1 = load(log1)

def blend(m, grp):
    return (int(m[grp], 16) >> 8) & 0xFF

def euler(m):
    # rot is the angle word's low three bytes: Z at bit 0, X at bit 8, Y at 16.
    r = int(m[18], 16)
    return (r & 0xFF, (r >> 8) & 0xFF, (r >> 16) & 0xFF)

def arc(a, b):
    d = (b - a) & 0xFF
    return 256 - d if d > 128 else d

def reversal(prv, cur):
    # The interval between two canonical ticks, from the alpha-1 (identity)
    # leg of each: the largest shortest-arc euler step, and whether the two
    # ticks drew a different pair of keyframe streams (header and keyframe A,
    # and keyframe B while both blend). The blend byte alone moving is one
    # keyframe pair at two weights, i.e. ordinary animation.
    step = max(arc(a, b) for a, b in zip(euler(prv), euler(cur)))
    bl, blp = blend(cur, 15), blend(prv, 15)
    kf = (prv[17] != cur[17] or prv[11] != cur[11]
          or (bl != 0 and blp != 0 and prv[12] != cur[12]))
    return step, kf

AH, SH = load(logh) if logh else ({}, {})

pairs = nclass = nhold = nsnap = nmirror = nturn = nturnsnap = nstill = 0
stills = []
W1 = WORLD.get(log1, {})
turns = []
bad, notes, holds = [], [], []
per = collections.defaultdict(lambda: {'cls': [], 'other': 0.0, 'n': 0})
for (v, mob), g in sorted(A0.items()):
    p = A1.get((v - 2, mob))
    if p is None:
        continue
    if (v, mob) in skip_leg:
        notes.append("excluded leg vbl=%d m=%s: %s" % (v, mob, skip_leg[(v, mob)]))
        continue
    fa, fb, bl = p[11], p[12], blend(p, 15)
    snapped = g[4] != '1'
    nsnap += snapped
    fap, fbp, blp = (g[11], g[12], blend(g, 15)) if snapped else (g[13], g[14], blend(g, 16))
    if not (fa == fap and bl == blp and (bl == 0 or fb == fbp)):
        bad.append("vbl=%d m=%s cls=%s%s tuple drawn(a=%s b=%s bl=%02x) rebuilt(a=%s b=%s bl=%02x)"
                   % (v, mob, g[3], " snapped" if snapped else "", fa, fb, bl, fap, fbp, blp))
    # The still bin reads every leg with both centroids, including the legs
    # whose record carries no blend (balpha 1), which the seam bound skips.
    wp, wc = W1.get((v - 2, mob)), W1.get((v, mob))
    if (min_still and wp is not None and wp == wc and g[4] != '2'
            and (v, mob) in S0 and (v - 2, mob) in S1):
        (x0, y0), (x1, y1) = S0[(v, mob)], S1[(v - 2, mob)]
        Ds = ((x0 - x1) ** 2 + (y0 - y1) ** 2) ** 0.5
        nstill += 1
        stills.append((v, mob, g[3], Ds))
        if Ds > STILL_EPS and (v, mob) not in skip_seam:
            bad.append("vbl=%d m=%s cls=%s still D=%.3f above %.2f: a moby that did not move "
                       "or turn between the ticks moves across the tick boundary"
                       % (v, mob, g[3], Ds, STILL_EPS))
    if float(g[5]) >= 1.0 or (v, mob) not in S0 or (v - 2, mob) not in S1:
        continue
    pairs += 1
    (x0, y0), (x1, y1) = S0[(v, mob)], S1[(v - 2, mob)]
    D = ((x0 - x1) ** 2 + (y0 - y1) ** 2) ** 0.5
    in_class = bl != 0 and (int(p[7]) == 0 or (p[10] is not None and p[10] != p[9]))
    nclass += in_class
    held = g[4] == '2'
    nhold += held
    cur = A1.get((v, mob))
    if cur is not None:
        step, kf = reversal(p, cur)
        if step >= MIRROR_THRESH and kf:
            # No pose exists between a shape and its mirror image: the
            # per-vertex model-space lerp collapses the mesh onto the flip's
            # invariant axis at alpha 0.5. The interval must hold.
            nmirror += 1
            if not held:
                bad.append("vbl=%d m=%s cls=%s turns %d byte angles on one axis with its "
                           "keyframe pair changed and reads prevkf=%s: the interval lerps "
                           "a facing reversal" % (v, mob, g[3], step, g[4]))
    gh = AH.get((v, mob))
    if (logh and cur is not None and gh is not None and not held
            and (v, mob) in SH and (v, mob) in S1 and (v, mob) in S0):
        step, kf = reversal(p, cur)
        if step > 0 and not (step >= MIRROR_THRESH and kf) and gh[4] != '2':
            (xp, yp), (xc, yc), (xh, yh) = S1[(v - 2, mob)], S1[(v, mob)], SH[(v, mob)]
            (x0, y0) = S0[(v, mob)]
            dd = ((xc - xp) ** 2 + (yc - yp) ** 2) ** 0.5
            if dd >= TURN_MIN_PX:
                e0 = (x0 - xp, y0 - yp)
                eh = (xh - (xp + xc) / 2, yh - (yp + yc) / 2)
                n0 = (e0[0] ** 2 + e0[1] ** 2) ** 0.5
                rho = 0.5 + (eh[0] * e0[0] + eh[1] * e0[1]) / max(n0 * n0, 1e-12)
                nturn += 1
                snap = n0 >= TURN_MIN_PX and abs(rho - 1.0) <= TURN_TOL
                nturnsnap += snap
                turns.append("turn vbl=%d m=%s cls=%s step=%d |D|=%.3f |e0|=%.3f |eh|=%.3f rho=%.3f%s"
                             % (v, mob, g[3], step, dd, n0, (eh[0] ** 2 + eh[1] ** 2) ** 0.5,
                                rho, " SNAP" if snap else ""))
                if snap:
                    bad.append("vbl=%d m=%s cls=%s turns %d byte angles and shows %.3f px of its "
                               "%.3f px tick step at alpha 0 with midpoint progress %.3f: a channel "
                               "is drawn at the current tick through the interval"
                               % (v, mob, g[3], step, n0, dd, rho))
    if (v, mob) in skip_seam:
        notes.append("excluded seam vbl=%d m=%s D=%.3f: %s" % (v, mob, D, skip_seam[(v, mob)]))
        continue
    if held:
        holds.append((v, mob, g[3], D))
        if D > HOLD_EPS:
            bad.append("vbl=%d m=%s cls=%s held D=%.3f above %.4f: the held pose is not "
                       "the pose the tick before drew" % (v, mob, g[3], D, HOLD_EPS))
        continue
    rec = per[(mob, g[3])]
    if in_class:
        rec['cls'].append((v, D))
    else:
        rec['other'] = max(rec['other'], D)
        rec['n'] += 1
    if dmax is not None and D > dmax:
        bad.append("vbl=%d m=%s cls=%s D=%.3f above the row bound %.3f" % (v, mob, g[3], D, dmax))

# The wbase bin: the alpha-0 base at v against the base the cull stored on
# the tick before, the alpha-1 replay's record at v - 2.
nwb = nwbmove = nwbbad = 0
if min_wbase is not None:
    W0, WS1 = WBASE.get(log0, {}), WBASE.get(log1, {})
    for (v, mob), (_, wb, moving) in sorted(W0.items()):
        p = WS1.get((v - 2, mob))
        if wb == '-' or p is None or p[0] == '-' or (v, mob) in skip_leg:
            continue
        nwb += 1
        nwbmove += moving
        if int(wb) != int(p[0]):
            nwbbad += 1
            bad.append("vbl=%d m=%s%s wbase %s at alpha 0, the cull stored %s at vbl %d"
                       % (v, mob, " moving" if moving else "", wb, p[0], v - 2))

mobys = []
for (mob, c), rec in sorted(per.items()):
    if not rec['cls']:
        continue
    mobys.append("m=%s cls=%s classLegs=%d classMaxD=%.3f otherMaxD=%.3f (n=%d)"
                 % (mob, c, len(rec['cls']), max(d for _, d in rec['cls']), rec['other'], rec['n']))
    if rec['n'] == 0:
        bad.append("m=%s cls=%s has no non-class leg to bound its class legs" % (mob, c))
        continue
    for v, d in rec['cls']:
        if d > rec['other']:
            bad.append("vbl=%d m=%s cls=%s classD=%.3f above the moby's non-class max %.3f"
                       % (v, mob, c, d, rec['other']))

head = ("[moby-subtick] game=%s bk2=%s pairs=%d class=%d hold=%d mirror=%d%s%s%s mismatches=%d snaps=%d excluded=%d"
        % (game, bk2, pairs, nclass, nhold, nmirror,
           (" turn=%d turnsnaps=%d" % (nturn, nturnsnap)) if logh else "",
           (" still=%d stillmax=%.3f" % (nstill, max([s[3] for s in stills] or [0.0]))) if min_still else "",
           (" wbase=%d wbasemove=%d wbasebad=%d" % (nwb, nwbmove, nwbbad)) if min_wbase is not None else "",
           sum(' tuple ' in b for b in bad), nsnap, len(notes)))
for n in notes:
    print("[moby-subtick]   " + n)
if (pairs < min_pairs or nclass < min_class or nhold < min_hold or nmirror < min_mirror
        or nturn < min_turn or nstill < min_still or nwbmove < (min_wbase or 0)):
    print(head + " VACUOUS: floor pairs>=%d class>=%d hold>=%d mirror>=%d turn>=%d still>=%d wbase>=%d"
          % (min_pairs, min_class, min_hold, min_mirror, min_turn, min_still, min_wbase or 0))
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for v, mob, c, d in holds:
    print("[moby-subtick]   held vbl=%d m=%s cls=%s D=%.3f" % (v, mob, c, d))
for m in mobys:
    print("[moby-subtick]   " + m)
for t in turns:
    print("[moby-subtick]   " + t)
for b in bad[:40]:
    print("[moby-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "moby-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
