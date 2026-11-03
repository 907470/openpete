#!/usr/bin/env bash
# check_moby_occlusion.sh - moby decode census by model kind at freecam poses,
# extended LOD against original LOD, with the image held byte-identical.
#
# The moby occlusion cull skips the decode of a record whose bounds lie wholly
# behind nearer world depth. A record kind without bounds is decoded on every
# present however hidden it is, which costs time and changes no pixel, so
# neither the presentation golden nor the oracle can see it. This harness
# reads the work instead. Per row of games/<game>/corpus/moby-occlusion-corpus.manifest
# it runs three headless legs at the row's freecam pose:
#   ext   --lod extended
#   ref   --lod extended with OPENPETE_MOBY_SIMPLE_OCCL=0 (the simple-kind
#         occlusion test off: the decode set of a build without it)
#   orig  --lod original
# Each leg captures one tick present and 21 sub-tick presents at alpha 0.5
# (--screenshot-intermediate-on-frame); the occlusion grid and the verdicts
# the sub-tick replays hold come from those presents, so a canonical-only
# run never culls a moby.
#
# Instrument (stderr, OPENPETE_MOBY_KIND_CENSUS=1), one line per pass:
#   [moby-kind vbl=<V> leg=canon|sub kind=anim|simple] recs=<n> occl=<n>
#       verts=<n> tris=<n>
# and one per extended-LOD reveal walk (kind=anim-reveal|simple-reveal: recs
# is the records appended, occl the candidates the occlusion test culled).
# The census of a leg is the median of its sub-tick decode lines inside the
# VBL span of its intermediate captures; culled is the sum of occl over every
# line of that span.
#
# Checks: every row requires each screenshot of the ext leg byte-identical to
# the ref leg's; the row's manifest checks add count criteria (see the
# manifest header).
# Engagement (VACUOUS, exit 1): a leg with fewer than MIN_BRACKETED bracketed
# intermediate captures, fewer than MIN_LINES sub-tick census lines of a kind
# a check reads (both kinds on ext and ref, the simple kind on orig for a
# max_ratio row), or a missing screenshot; a row marked engage whose ext leg
# culls no record in the span.
#
# Usage: tests/harness/check_moby_occlusion.sh [--game SLUG] [--build-dir=DIR]
#            [--filter SUBSTR] [--keep DIR]
#   --filter  run only rows whose name contains SUBSTR
#   --keep    copy every leg's log and screenshots to DIR/<row>/<leg>/
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc or manifest missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

FILTER="" KEEP=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[i]}" in
        --build-dir=*) PG_BUILD_DIR="${args[i]#--build-dir=}";;
        --filter) FILTER="${args[i+1]:-}"; i=$((i + 1));;
        --filter=*) FILTER="${args[i]#--filter=}";;
        --keep) KEEP="${args[i+1]:-}"; i=$((i + 1));;
        --keep=*) KEEP="${args[i]#--keep=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
MANIFEST="games/$GAME/corpus/moby-occlusion-corpus.manifest"
[[ -f "$MANIFEST" ]] || { echo "moby-occlusion: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] || { echo "moby-occlusion: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "moby-occlusion: SKIP (python3 missing)"; exit 64; }

# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

CANON_VBL=2473
INTER_VBLS="$(seq -s, 2491 2 2531)"
FRAME_LIMIT=2533
MIN_BRACKETED=15
MIN_LINES=8

OUT="${PG_BUILD_DIR:-build/linux-release}/moby-occlusion/$GAME"
mkdir -p "$OUT"

# One leg. args: row level pose leg; writes $OUT/<row>__<leg>.log and its PNGs.
run_leg() {
    local row="$1" level="$2" pose="$3" leg="$4"
    local tag="$OUT/${row}__${leg}" lod=extended
    [[ "$leg" == orig ]] && lod=original
    rm -f "${tag:?}.log" "${tag:?}"__*.png
    local -a outs=("${tag}__${CANON_VBL}-vk.png")
    local v
    for v in ${INTER_VBLS//,/ }; do outs+=("${tag}__${v}-vk.png"); done
    PG_CAPTURE_ENV=(OPENPETE_MOBY_KIND_CENSUS=1 OPENPETE_INTERMEDIATE_ALPHA=0.5)
    [[ "$leg" == ref ]] && PG_CAPTURE_ENV+=(OPENPETE_MOBY_SIMPLE_OCCL=0)
    PG_CAPTURE_TIMEOUT=600
    local outlist
    outlist="$(IFS=,; echo "${outs[*]}")"
    pg_capture "$tag.log" "$outlist" -- \
        --renderer vk --aspect 16:9 --lod "$lod" --no-card --mute 1 \
        --skip-to-level "$level" --freecam-pose "$pose" \
        --screenshot-native-only --screenshot-on-frame "$CANON_VBL" \
        --screenshot-intermediate-on-frame "$INTER_VBLS" \
        --screenshot-output "${tag}__{n}.png" --frame-limit "$FRAME_LIMIT"
    local rc=$?
    echo "moby-occlusion: leg $row/$leg rc=$rc"
    if [[ -n "$KEEP" ]]; then
        mkdir -p "$KEEP/$row/$leg"
        cp -f "$tag.log" "${tag}"__*.png "$KEEP/$row/$leg/" 2>/dev/null
    fi
}

rows=()
while IFS='|' read -r name level pose checks; do
    name="$(echo "$name" | tr -d ' \t')"
    [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    level="$(echo "$level" | tr -d ' \t')"
    pose="$(echo "$pose" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    checks="$(echo "${checks:-}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    rows+=("$name|$checks")
    for leg in ext ref orig; do run_leg "$name" "$level" "$pose" "$leg"; done
done < "$MANIFEST"
(( ${#rows[@]} )) || { echo "moby-occlusion: FAIL (no manifest row selected)"; exit 1; }

python3 - "$OUT" "$CANON_VBL" "$INTER_VBLS" "$MIN_BRACKETED" "$MIN_LINES" "${rows[@]}" <<'PYEOF'
import filecmp, os, re, statistics, sys

out, canon, inter = sys.argv[1], int(sys.argv[2]), [int(x) for x in sys.argv[3].split(',')]
min_br, min_lines = int(sys.argv[4]), int(sys.argv[5])
rows = [r.split('|', 1) for r in sys.argv[6:]]

kind_re = re.compile(r'\[moby-kind vbl=(\d+) leg=(\w+) kind=([\w-]+)\] recs=(\d+) occl=(\d+) verts=(\d+) tris=(\d+)')
inter_re = re.compile(r'wrote intermediate .* on frame (\d+) \(target=(\d+), paired=\d+, bracketed=(\d+)')

def leg_census(path):
    """Median sub-tick census per kind inside the intermediate captures' VBL span."""
    frames, bracketed = [], 0
    lines = []
    for ln in open(path, errors='replace'):
        m = inter_re.search(ln)
        if m:
            frames.append(int(m.group(1)))
            bracketed += int(m.group(3))
            continue
        m = kind_re.search(ln)
        if m:
            lines.append(m.groups())
    res = {'bracketed': bracketed}
    if not frames:
        return res
    lo, hi = min(frames), max(frames)
    # Every record any pass of the window culled, reveal walks included.
    res['culled'] = sum(int(g[4]) for g in lines if lo <= int(g[0]) <= hi)
    for kind in ('anim', 'simple'):
        sel = [g for g in lines if g[1] == 'sub' and g[2] == kind and lo <= int(g[0]) <= hi]
        res[kind + '_n'] = len(sel)
        for i, key in enumerate(('recs', 'occl', 'verts', 'tris')):
            vals = [int(g[3 + i]) for g in sel]
            res[kind + '_' + key] = statistics.median(vals) if vals else None
    return res

def shots(row, leg):
    return [os.path.join(out, '%s__%s__%d-vk.png' % (row, leg, v)) for v in [canon] + inter]

verdict = 0
print('%-22s %-5s %4s %5s %6s %6s | %4s %5s %6s %6s | %9s %6s' %
      ('row', 'leg', 'aRec', 'aOccl', 'aVerts', 'aTris', 'sRec', 'sOccl', 'sVerts', 'sTris', 'bracketed', 'culled'))
for name, checks in rows:
    cen, notes, fail, vac = {}, [], [], []
    for leg in ('ext', 'ref', 'orig'):
        log = os.path.join(out, '%s__%s.log' % (name, leg))
        if not os.path.exists(log):
            vac.append('%s: no log' % leg); continue
        c = cen[leg] = leg_census(log)
        f = lambda k: '-' if c.get(k) is None else '%g' % c[k]
        print('%-22s %-5s %4s %5s %6s %6s | %4s %5s %6s %6s | %9d %6d' %
              (name, leg, f('anim_recs'), f('anim_occl'), f('anim_verts'), f('anim_tris'),
               f('simple_recs'), f('simple_occl'), f('simple_verts'), f('simple_tris'),
               c['bracketed'], c.get('culled', 0)))
        if c['bracketed'] < min_br:
            vac.append('%s: %d bracketed intermediate captures < %d' % (leg, c['bracketed'], min_br))
        # The ext and ref legs carry every check; the orig leg only the ratio.
        need = ('anim', 'simple') if leg != 'orig' else (('simple',) if 'max_ratio=' in checks else ())
        for kind in need:
            if c.get(kind + '_n', 0) < min_lines:
                vac.append('%s: %d %s sub-tick census lines < %d' % (leg, c.get(kind + '_n', 0), kind, min_lines))
        missing = [p for p in shots(name, leg) if not os.path.exists(p)]
        if missing:
            vac.append('%s: %d screenshots missing' % (leg, len(missing)))
    # Image identity: ext against the leg with the simple-kind test off.
    if 'ext' in cen and 'ref' in cen:
        diff = [os.path.basename(a) for a, b in zip(shots(name, 'ext'), shots(name, 'ref'))
                if os.path.exists(a) and os.path.exists(b) and not filecmp.cmp(a, b, shallow=False)]
        if diff:
            fail.append('%d ext screenshots differ from ref (%s)' % (len(diff), ', '.join(diff[:4])))
        else:
            notes.append('images ext==ref (%d)' % len(shots(name, 'ext')))
    st = lambda leg: cen.get(leg, {}).get('simple_tris')
    for chk in checks.split():
        if chk.startswith('max_ratio='):
            r = float(chk.split('=', 1)[1])
            e, o = st('ext'), st('orig')
            if e is None or o is None:
                vac.append('max_ratio: no simple census')
            elif e > r * max(o, 1):
                fail.append('ext simple tris %g > %g x orig %g' % (e, r, o))
            else:
                notes.append('ext simple tris %g <= %g x orig %g' % (e, r, o))
        elif chk == 'unchanged':
            e, f0 = st('ext'), st('ref')
            if e is None or f0 is None:
                vac.append('unchanged: no simple census')
            elif e != f0:
                fail.append('ext simple tris %g != ref %g' % (e, f0))
            else:
                notes.append('ext simple tris %g == ref' % e)
        elif chk == 'engage':
            occl = cen.get('ext', {}).get('culled', 0)
            if occl <= 0:
                vac.append('engage: ext leg culled no record')
            else:
                notes.append('ext culled %d records over the window' % occl)
        elif chk == 'report':
            pass
        else:
            vac.append('unknown check %s' % chk)
    v = 'VACUOUS' if vac else ('FAIL' if fail else 'PASS')
    if v != 'PASS':
        verdict = 1
    print('[moby-occlusion] row=%s verdict=%s %s' % (name, v, '; '.join(vac + fail + notes)))
print('[moby-occlusion] verdict=%s' % ('PASS' if verdict == 0 else 'FAIL'))
sys.exit(verdict)
PYEOF
