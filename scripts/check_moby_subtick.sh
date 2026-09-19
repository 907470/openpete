#!/usr/bin/env bash
# check_moby_subtick.sh — sub-tick continuity of the animated mobys.
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
# Input: games/<slug>/moby-subtick-corpus.manifest, rows
#   <bk2> | <lo>-<hi> | <key>=<value> ... [| <exclusion>]...
# The span is the intermediate-capture VBLs, two apart; the [moby-anim] and
# [moby-screen] traces name each present by the VBL they print (the trace
# VBL), which is what the exclusions use. Keys:
#   pairs=<n>   floor on the bracketed legs measured
#   class=<n>   floor on the defect-class legs measured
#   hold=<n>    floor on the held (prevkf=2) legs measured
#   dmax=<px>   optional: every leg's D at or below this bound
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
#
# Pass criterion, per row: 0 tuple mismatches; every class leg's D at or
# below the largest D the same moby reads on its other legs; every held leg's
# D zero to HOLD_EPS; with dmax, every leg's D at or below it. VACUOUS
# (exit 1) when the bracketed, the class or the held leg count is below the
# row's floor.
#
# Verdict line per row:
#   [moby-subtick] game=<g> bk2=<b> pairs=<n> class=<n> hold=<n>
#                  mismatches=<n> snaps=<n> excluded=<n> verdict=PASS|FAIL
#
# Usage: scripts/check_moby_subtick.sh [--game SLUG] [--build-dir=DIR]
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
MANIFEST="games/$GAME/moby-subtick-corpus.manifest"
[[ -f "$MANIFEST" ]] || { echo "moby-subtick: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "moby-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "moby-subtick: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/moby-subtick/$GAME"
mkdir -p "$OUT"

# One replay of a row at one alpha, as runs of at most CHUNK captures whose
# logs append to one file. The screenshots are the capture requests that make
# the intermediate presents exist headless; each run's last one is asserted.
run_alpha() {
    local alpha="$1" tag="$2" movie="$3"; shift 3
    local dir="$OUT/$tag-a$alpha" log="$OUT/$tag-a$alpha.log"
    local -a vbls=("$@")
    mkdir -p "$dir"
    find "$dir" -maxdepth 1 -name '*.png' -delete
    rm -f "$log" "$log.part"
    PG_CAPTURE_ENV=(OPENPETE_MOBY_POSTRACE_CLS=0xFFFF
                    OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    PG_CAPTURE_TIMEOUT=600
    local i n=${#vbls[@]}
    for ((i = 0; i < n; i += CHUNK)); do
        local -a part=("${vbls[@]:i:CHUNK}")
        local last="${part[${#part[@]}-1]}" csv
        csv="$(IFS=,; echo "${part[*]}")"
        pg_capture "$log.part" "$dir/i__$last-vk.png" -- \
            --renderer vk --mute 1 --aspect 4:3 --lod original \
            --bk2 "$movie" --screenshot-native-only \
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
    IFS='|' read -r bk2 span keys excl <<<"$line"
    bk2="${bk2// /}"; span="${span// /}"
    movie="$PG_BK2_DIR/$bk2"
    [[ -f "$movie" ]] || { echo "moby-subtick: SKIP ($movie missing)"; exit 64; }
    tag="${bk2%.bk2}"
    rows=$((rows + 1))

    lo="${span%-*}"; hi="${span#*-}"
    if (( (hi - lo) % 2 != 0 || hi < lo + 2 )); then
        echo "moby-subtick: FAIL (row $bk2: span $span is not lo-hi with hi = lo + 2k)"; exit 1
    fi
    vbls=()
    for ((f = lo; f <= hi; f += 2)); do vbls+=("$f"); done

    rc0=0 rc1=0
    run_alpha 0 "$tag" "$movie" "${vbls[@]}" </dev/null || rc0=$?
    run_alpha 1 "$tag" "$movie" "${vbls[@]}" </dev/null || rc1=$?
    if [[ ! -s "$OUT/$tag-a0.log" || ! -s "$OUT/$tag-a1.log" ]] || (( rc0 || rc1 )); then
        echo "moby-subtick: FAIL (row $bk2: no instrument output, rc=$rc0/$rc1; see $OUT)"
        fail=1; continue
    fi

    python3 - "$OUT/$tag-a0.log" "$OUT/$tag-a1.log" "$GAME" "$bk2" "$keys" "${excl:-}" <<'PYEOF' || fail=1
import re, sys, collections

log0, log1, game, bk2, keys, excl = sys.argv[1:7]
opts = {}
for tok in keys.split():
    k, _, v = tok.partition('=')
    opts[k] = v
min_pairs = int(opts.get('pairs', '1'))
min_class = int(opts.get('class', '0'))
min_hold = int(opts.get('hold', '0'))
dmax = float(opts['dmax']) if 'dmax' in opts else None
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
                 r'fa=(\w+) fb=(\w+) fap=(\w+) fbp=(\w+) w0=(\w+) w0p=(\w+)')
scr = re.compile(r'\[moby-screen vbl=(\d+)\] leg=inter now=\S+ m=(\w+) cls=\w+ n=\d+ '
                 r'c=\(([-\d.]+),([-\d.]+)\)')

def load(path):
    anim, screen = {}, {}
    for line in open(path, errors='replace'):
        m = ani.search(line)
        if m:
            anim.setdefault((int(m[1]), m[2].lower()), m)
            continue
        m = scr.search(line)
        if m:
            screen.setdefault((int(m[1]), m[2].lower()), (float(m[3]), float(m[4])))
    return anim, screen

A0, S0 = load(log0)
# The alpha-1 leg is the identity leg: every present there shows the pose its
# own tick drew. The alpha-0 leg does not, because a held leg shows the pose
# of the tick BEFORE it, so the drawn pose of tick v - 2 is read from the
# alpha-1 log and the rebuilt pose at tick v from the alpha-0 one.
A1, S1 = load(log1)

def blend(m, grp):
    return (int(m[grp], 16) >> 8) & 0xFF

pairs = nclass = nhold = nsnap = 0
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
    if float(g[5]) >= 1.0 or (v, mob) not in S0 or (v - 2, mob) not in S1:
        continue
    pairs += 1
    (x0, y0), (x1, y1) = S0[(v, mob)], S1[(v - 2, mob)]
    D = ((x0 - x1) ** 2 + (y0 - y1) ** 2) ** 0.5
    in_class = bl != 0 and (int(p[7]) == 0 or (p[10] is not None and p[10] != p[9]))
    nclass += in_class
    held = g[4] == '2'
    nhold += held
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

head = ("[moby-subtick] game=%s bk2=%s pairs=%d class=%d hold=%d mismatches=%d snaps=%d excluded=%d"
        % (game, bk2, pairs, nclass, nhold, sum(' tuple ' in b for b in bad), nsnap, len(notes)))
for n in notes:
    print("[moby-subtick]   " + n)
if pairs < min_pairs or nclass < min_class or nhold < min_hold:
    print(head + " VACUOUS: floor pairs>=%d class>=%d hold>=%d" % (min_pairs, min_class, min_hold))
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for v, mob, c, d in holds:
    print("[moby-subtick]   held vbl=%d m=%s cls=%s D=%.3f" % (v, mob, c, d))
for m in mobys:
    print("[moby-subtick]   " + m)
for b in bad[:40]:
    print("[moby-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "moby-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
