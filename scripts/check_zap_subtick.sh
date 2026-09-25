#!/usr/bin/env bash
# check_zap_subtick.sh — sub-tick interpolation of Zoe's zap.
#
# The zap routine links its own triangles into the 3D ordering table and the
# feeder reads them back out of the polybuf, so what a sub-tick present has to
# work with is the guest's own int16 screen corners, built under the tick's
# camera from the tick's record. A replay that re-emits them verbatim draws the
# ribbon where the tick drew it for the whole interior of the tick, and the
# next tick moves it the whole way at once.
#
# This is a sibling of check_glint_subtick.sh rather than a bin inside it. That
# script measures a compact star by the centroid of its leave-one-out pixel
# mask, which works because the star is a small blob that only translates. The
# zap is a ribbon whose painted extent grows by more than a factor of ten
# across its own live window, as the chain lengthens and as the routine redraws
# a random displacement per point inside each call, so a mask centroid moves
# with the shape rather than with the record and cannot be compared against an
# interpolation. The instrument here is the record's own two endpoints instead,
# which is a trace rather than an image, so the capture loop, the parser and
# the pass criterion all differ; sharing a file would have meant two disjoint
# pipelines under one name.
#
# Instrument (stderr), OPENPETE_ZAP_TRACE=1, one line per present that fed the
# producer:
#   [zap-screen vbl=<v> leg=canon|inter alpha=<a>] live=<n> rec=<addr>
#     head=(x,y) tail=(x,y)
# On a canonical present head and tail are the routine's own projection of the
# record's two world points. On a sub-tick present they are those points moved
# by the rebase, which is where the ribbon's ends were actually drawn. The line
# ends ` width=<px> wn=<n>`: the ribbon's mean full width, each widened
# corner's distance from its own chain point measured across the drawn
# head-to-tail line, over `wn` corners (the head and tail pairs carry none).
#
# The width cell prices that statistic the way check_feeder_subtick.sh prices
# its area cell: the residual from the lerp of the two canonical widths over
# the distance a held width stands from that lerp, LERPED at or below 0.25,
# SNAP at or above 0.75, `own` the alpha-0 width's distance from canon(V-2).
# The routine redraws each point's jitter per call and the width follows it,
# so the canonical width steps by up to a pixel between ticks whatever the
# record does; a row whose width step is below `wstep` cannot separate a
# lerp from that noise and declares `wengage=0`. A row that judges the width
# runs one more replay, the alpha-0 present under OPENPETE_ZAP_WIDTH=0, where
# the width must hold the tick's (own at least 0.75 of the step).
#
# A sub-tick present replays the set the most recent canonical tick captured
# and shows it between the pose of the tick before (alpha 0) and the pose that
# tick drew (alpha 1), so the present at VBL V+1 covers the interval the ticks
# at V-2 and V drew. One replay per alpha yields all three legs: the trace
# prints the canonical presents of the same run.
#
# Input: games/<slug>/corpus/zap-subtick-corpus.manifest, rows
#   <source> | <V> | <key>=<value> ...
# <source> is a movie under games/<slug>/bk2, or any name not ending in .bk2,
# which is the plain boot — the title screen and the attract demo, which need
# no input — replayed with no movie. V is the canonical VBL whose primitives
# the intermediate present at V+1 replays; V-2 opens the interval. Keys:
#   tol=<px>     bound on each endpoint, default 0.5
#   step=<px>    floor on the larger endpoint's canonical motion over the
#                interval. A record that does not move between the two ticks
#                passes every alpha whatever the producer does, so a row below
#                this floor is VACUOUS rather than a pass. Default 4.
#   wtol=<r>     bound on the width cell's alpha-0.5 ratio; absent, the width
#                is reported and not judged.
#   wstep=<px>   floor on the canonical width step, default 1.0.
#   wengage=0    the width cell is reported only: its step is below the floor.
#
# Pass criterion, per row: on every alpha, the intermediate head and the
# intermediate tail each within `tol` of `(1-alpha)*canon(V-2) + alpha*canon(V)`
# for that endpoint. VACUOUS (exit 1) when any leg reports live=0 or is
# missing, when the legs disagree on which record is paired, or when the
# canonical motion is below the floor.
#
# Verdict line per row:
#   [zap-subtick] game=<g> row=<r> V=<v> legs=<n> live=<n> step=<px>
#                 worst=<px> verdict=PASS|FAIL
#
# Usage: scripts/check_zap_subtick.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (binary, disc, bk2 or python3 missing).
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
MANIFEST="$(pg_corpus_manifest "$GAME" zap-subtick-corpus.manifest)"
pg_manifest "$MANIFEST" line
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "zap-subtick: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "zap-subtick: SKIP (python3 missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/zap-subtick/$GAME"
mkdir -p "$OUT"

# One replay at one alpha. The intermediate screenshot request is what makes
# the sub-tick present exist at all under --headless, and it also arms the
# native renderer for the whole run, so the canonical presents of the same run
# trace too. The log and the screenshot are deleted before the run and the
# screenshot asserted after it.
run_alpha() {
    local alpha="$1" tag="$2" mid="$3" flim="$4" sfx="${5:-}"
    local dir="$OUT/$tag-a$alpha$sfx" log="$OUT/$tag-a$alpha$sfx.log"
    mkdir -p "$dir"
    find "$dir" -maxdepth 1 -name '*.png' -delete
    PG_CAPTURE_ENV=(OPENPETE_ZAP_TRACE=1 OPENPETE_INTERMEDIATE_ALPHA="$alpha")
    [[ -n "$sfx" ]] && PG_CAPTURE_ENV+=(OPENPETE_ZAP_WIDTH=0)
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$dir/$mid-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original \
        "${MOVIE_ARG[@]}" --screenshot-native-only \
        --screenshot-intermediate-on-frame "$mid" \
        --screenshot-output "$dir/{n}.png" \
        --frame-limit "$flim" --max-runtime-seconds 800
}

fail=0 rows=0
while IFS= read -r line; do
    line="${line%%#*}"
    [[ -n "${line// /}" ]] || continue
    IFS='|' read -r src vfield keys <<<"$line"
    src="${src// /}"; V="${vfield// /}"
    # A row whose first field does not end in .bk2 is the plain boot, which
    # needs no input and replays with no movie.
    if [[ "$src" == *.bk2 ]]; then
        MOVIE="$PG_BK2_DIR/$src"
        [[ -f "$MOVIE" ]] || { echo "zap-subtick: SKIP ($MOVIE missing)"; exit 64; }
        MOVIE_ARG=(--bk2 "$MOVIE")
    else
        MOVIE_ARG=()
    fi
    rows=$((rows + 1))
    tag="${src%.bk2}"

    if (( V < 4 )); then
        echo "zap-subtick: FAIL (row $src: V=$V has no interval below it)"; exit 1
    fi
    mid=$((V + 1)) flim=$((V + 11))

    logs=() rc=0
    for alpha in "${ALPHAS[@]}"; do
        run_alpha "$alpha" "$tag" "$mid" "$flim" </dev/null || rc=$?
        logs+=("$OUT/$tag-a$alpha.log")
    done
    missing=0
    # The width engagement leg, for a row that judges the width.
    slog=""
    if [[ " $keys " == *" wtol="* && " $keys " != *" wengage=0 "* ]]; then
        run_alpha 0 "$tag" "$mid" "$flim" -s </dev/null || rc=$?
        slog="$OUT/$tag-a0-s.log"
        [[ -s "$slog" ]] || missing=1
    fi
    for l in "${logs[@]}"; do [[ -s "$l" ]] || missing=1; done
    if (( missing || rc )); then
        echo "zap-subtick: FAIL (row $src: no instrument output, rc=$rc; see $OUT)"
        fail=1; continue
    fi

    python3 - "$GAME" "$src" "$V" "$keys" "$slog" "${ALPHAS[@]}" -- "${logs[@]}" <<'PYEOF' || fail=1
import re, sys

game, row, V, keys, slog = sys.argv[1:6]
rest = sys.argv[6:]
sep = rest.index('--')
alphas, logs = rest[:sep], rest[sep + 1:]
V = int(V)
prev, mid = V - 2, V + 1

opts = {}
for tok in keys.split():
    k, _, v = tok.partition('=')
    opts[k] = v
tol = float(opts.get('tol', '0.5'))
step_floor = float(opts.get('step', '4'))
wtol = float(opts['wtol']) if 'wtol' in opts else None
wstep_floor = float(opts.get('wstep', '1.0'))
wengage = opts.get('wengage', '1') != '0'

rx = re.compile(r'\[zap-screen vbl=(\d+) leg=(\w+) alpha=([\d.]+)\] live=(\d+) '
                r'rec=(\w+) head=\(([-\d.]+),([-\d.]+)\) tail=\(([-\d.]+),([-\d.]+)\)'
                r'(?: width=([\d.]+) wn=(\d+))?')


def load(path):
    """First trace line per (vbl, leg) of one replay."""
    out = {}
    for line in open(path, errors='replace'):
        m = rx.search(line)
        if m:
            out.setdefault((int(m[1]), m[2]),
                           (int(m[4]), m[5], float(m[6]), float(m[7]),
                            float(m[8]), float(m[9]),
                            float(m[10]) if m[10] else None))
    return out


vacuous, bad, notes = [], [], []
legs = 0
live_min = None
recs = set()

# The canonical ends of the interval come from the alpha-1 replay, whose
# canonical presents are the same ones every other replay draws.
base = load(logs[-1])
c0 = base.get((prev, 'canon'))
c1 = base.get((V, 'canon'))
if c0 is None or c1 is None:
    vacuous.append("no canonical trace at VBL %d or %d" % (prev, V))
else:
    legs += 2
    for c in (c0, c1):
        recs.add(c[1])
        live_min = c[0] if live_min is None else min(live_min, c[0])
    hstep = ((c1[2] - c0[2]) ** 2 + (c1[3] - c0[3]) ** 2) ** 0.5
    tstep = ((c1[4] - c0[4]) ** 2 + (c1[5] - c0[5]) ** 2) ** 0.5
    notes.append("canon%d head=(%.2f,%.2f) tail=(%.2f,%.2f)" % (prev, c0[2], c0[3], c0[4], c0[5]))
    notes.append("canon%d head=(%.2f,%.2f) tail=(%.2f,%.2f)" % (V, c1[2], c1[3], c1[4], c1[5]))
    notes.append("canonical motion head=%.2f tail=%.2f" % (hstep, tstep))
    if max(hstep, tstep) < step_floor:
        vacuous.append("canonical motion %.2f px is below the %.2f floor"
                       % (max(hstep, tstep), step_floor))

worst = 0.0
if not vacuous:
    for a, path in zip(alphas, logs):
        av = float(a)
        leg = load(path).get((mid, 'inter'))
        if leg is None:
            vacuous.append("no intermediate trace at VBL %d, alpha=%s" % (mid, a))
            continue
        legs += 1
        recs.add(leg[1])
        live_min = min(live_min, leg[0])
        ex_h = (c0[2] + (c1[2] - c0[2]) * av, c0[3] + (c1[3] - c0[3]) * av)
        ex_t = (c0[4] + (c1[4] - c0[4]) * av, c0[5] + (c1[5] - c0[5]) * av)
        dh = ((leg[2] - ex_h[0]) ** 2 + (leg[3] - ex_h[1]) ** 2) ** 0.5
        dt = ((leg[4] - ex_t[0]) ** 2 + (leg[5] - ex_t[1]) ** 2) ** 0.5
        worst = max(worst, dh, dt)
        notes.append("alpha=%s head=(%.2f,%.2f) want=(%.2f,%.2f) D=%.2f | "
                     "tail=(%.2f,%.2f) want=(%.2f,%.2f) D=%.2f"
                     % (a, leg[2], leg[3], ex_h[0], ex_h[1], dh,
                        leg[4], leg[5], ex_t[0], ex_t[1], dt))
        if dh > tol:
            bad.append("alpha=%s head D=%.2f above the %.2f bound" % (a, dh, tol))
        if dt > tol:
            bad.append("alpha=%s tail D=%.2f above the %.2f bound" % (a, dt, tol))

# The width cell.
if not vacuous and c0[6] is not None and c1[6] is not None:
    wst = c1[6] - c0[6]
    wl = "width canon%d=%.3f canon%d=%.3f step=%.3f" % (prev, c0[6], V, c1[6], wst)
    judge_w = wtol is not None and wengage
    if judge_w and abs(wst) < wstep_floor:
        vacuous.append("width step %.3f below the %.2f floor" % (abs(wst), wstep_floor))
    wshown = {}
    for a, path in zip(alphas, logs):
        leg = load(path).get((mid, 'inter'))
        if leg is None or leg[6] is None:
            continue
        av = float(a)
        wshown[a] = leg[6]
        ex = c0[6] + wst * av
        wl += " a%s=%.3f(D=%.3f)" % (a, leg[6], abs(leg[6] - ex))
        if av >= 1.0:
            if abs(leg[6] - c1[6]) > 0.0005:
                bad.append("alpha 1 width %.3f is not canon(V) %.3f" % (leg[6], c1[6]))
            continue
        if abs(wst) < 1e-9:
            continue
        r = abs(leg[6] - ex) / ((1.0 - av) * abs(wst))
        wl += " r%s=%.2f %s" % (a, r, "LERPED" if r <= 0.25 else ("SNAP" if r >= 0.75 else "PARTIAL"))
        if judge_w and a == '0.5' and r > wtol:
            bad.append("alpha=0.5 width ratio %.2f above the %.2f bound" % (r, wtol))
    if '0' in wshown:
        wl += " own=%.3f" % abs(wshown['0'] - c0[6])
    if not judge_w:
        wl += " (reported, not judged)"
    notes.append(wl)
    if judge_w and slog:
        s = load(slog).get((mid, 'inter'))
        if s is None or s[6] is None:
            vacuous.append("no width trace in the OPENPETE_ZAP_WIDTH=0 leg")
        else:
            own_s = abs(s[6] - c0[6])
            notes.append("OPENPETE_ZAP_WIDTH=0 own=%.3f of step=%.3f" % (own_s, abs(wst)))
            if own_s < 0.75 * abs(wst):
                vacuous.append("the width holds no tick state under OPENPETE_ZAP_WIDTH=0: "
                               "the knob suppressed nothing this row can see")

if live_min is not None and live_min <= 0:
    vacuous.append("a leg reports live=0: the producer fed nothing")
if len(recs) > 1:
    vacuous.append("the legs pair different records: %s" % ", ".join(sorted(recs)))

head = ("[zap-subtick] game=%s row=%s V=%d legs=%d live=%s worst=%.2f"
        % (game, row, V, legs, live_min if live_min is not None else "n/a", worst))
for n in notes:
    print("[zap-subtick]   " + n)
if vacuous:
    print(head + " VACUOUS")
    for v in vacuous:
        print("[zap-subtick]   " + v)
    sys.exit(1)
print(head + " verdict=%s" % ("FAIL" if bad else "PASS"))
for b in bad:
    print("[zap-subtick]   " + b)
sys.exit(1 if bad else 0)
PYEOF
done <"$MANIFEST"

if (( rows == 0 )); then
    echo "zap-subtick: FAIL ($MANIFEST has no rows)"; exit 1
fi
exit $fail
