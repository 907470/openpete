#!/usr/bin/env bash
# check_moby_jump_census.sh - the sub-tick moby jump verdicts scored against
# the write-attribution truth.
#
# A moby step between two ticks is either motion, which the sub-tick view
# interpolates, or a jump (a placement by the game's code), which it holds at
# tick N-1 until alpha 1 (cut_obj_jump_reason). The truth is
# games/<game>/corpus/moby-jump-truth.tsv: every step of 900 world units or
# more over its rows, labelled from the writer of the position stores (an
# OPENPETE_WRITE_CENSUS attribution). Each row is replayed headless with
# OPENPETE_JUMP_CENSUS=900, which prints, on every capture that rolls a moby
# that far, each rule's own verdict (the cutscene sampler's, the placement
# table's, the load regime's, the distance bound in force) and the verdict in
# force. A line is matched to its truth step by (row, VBL, object).
#
# Scored steps are the truth's jump and motion steps; sampler steps (the
# sampler's own rule decides) and cut steps (the camera cut decides) are
# counted, not scored. A census line for a class change counts as a hold
# (the class guard holds it). Per configuration:
#   false holds     motion steps the verdict holds
#   misses          jump steps outside the named residual it interpolates
#   residual misses jump steps of a residual bin (no marker known) it
#                   interpolates
# The configurations are the markers off and on, each with the bound in force
# and with no bound; the verdict in force decides the exit code.
#
# Usage:
#   tests/harness/check_moby_jump_census.sh [--game SLUG] [--filter SUBSTR]
#       [--jobs N] [--out DIR] [--score-only]
#   --game        spyro-1 (default), spyro-2 or spyro-3
#   --filter      run only truth rows whose name contains SUBSTR
#   --jobs        concurrent game processes (default 2)
#   --out         capture directory (default build/moby-jump-census/<game>)
#   --score-only  score the captures already in --out
# OPENPETE_MOBY_JUMP_BOUND passes through to the binary to price a bound.
#
# Exit: 0 = PASS (no false hold, no miss outside the residual, engaged),
#       2 = FAIL (a false hold, a miss, a missing capture, or zero matched
#           steps),
#       64 = SKIP (binary, disc or truth file missing).
set -u
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

GAME="spyro-1" FILTER="" JOBS=2 OUT="" SCORE_ONLY=0
while (( $# )); do
  case "$1" in
    --game) GAME="$2"; shift 2;;
    --game=*) GAME="${1#--game=}"; shift;;
    --filter) FILTER="$2"; shift 2;;
    --jobs) JOBS="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --score-only) SCORE_ONLY=1; shift;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 64;;
    *) echo "check_moby_jump_census: unknown argument '$1'" >&2; exit 64;;
  esac
done
pg_resolve "$GAME" || exit 64
OUT="${OUT:-build/moby-jump-census/$GAME}"
TRUTH="games/$GAME/corpus/moby-jump-truth.tsv"
[[ -f "$TRUTH" ]] || { echo "[jump-census] SKIP: no truth file $TRUTH"; exit 64; }

LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

declare -A LAUNCH
while IFS='|' read -r name _start _interval _freeze _pins launch; do
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  LAUNCH[$name]="$(echo "${launch:-}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
done < "$PG_CORPUS_MANIFEST"

# Replay one truth row and keep its census lines. args: name frame-limit
run_row() {
  local name="$1" flim="$2"
  local cen="$OUT/$name.cen" log="$OUT/$name.log"
  rm -f "$cen" "$log" "$cen.rc"
  local bk2="$PG_BK2_DIR/${name%%@*}.bk2"
  local -a bk2_arg=(); [[ -f "$bk2" && "$name" != attract ]] && bk2_arg=(--bk2 "$bk2")
  local -a ROW_ENV=() ROW_ARGS=(); pg_split_launch "${LAUNCH[$name]:-}"
  env "${ROW_ENV[@]}" OPENPETE_JUMP_CENSUS=900 timeout 1500 "$PG_BIN" --headless --no-config \
      --disc "$PG_DISC" --disc-cache 0 --renderer vk --mute 1 --aspect 16:9 --lod original \
      "${PG_RUN_ARGS[@]}" "${bk2_arg[@]}" --render-fps 120 --frame-limit "$flim" \
      --max-runtime-seconds 1400 "${ROW_ARGS[@]}" > "$log" 2>&1
  local rc=$?
  grep '^\[jump-census ' "$log" > "$cen"
  echo "$rc" > "$cen.rc"
  [[ $rc -eq 0 ]] && rm -f "$log"
}

if (( ! SCORE_ONLY )); then
  [[ -x "$PG_BIN" ]] || { echo "[jump-census] SKIP: no binary at $PG_BIN"; exit 64; }
  [[ -f "$PG_DISC" ]] || { echo "[jump-census] SKIP: no disc at $PG_DISC"; exit 64; }
  mkdir -p "$OUT"
  while IFS=$'\t' read -r kind name flim _; do
    [[ "$kind" == row ]] || continue
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    rm -f "$OUT/$name.cen" "$OUT/$name.cen.rc"
  done < "$TRUTH"
  while IFS=$'\t' read -r kind name flim _; do
    [[ "$kind" == row ]] || continue
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    while (( $(jobs -rp | wc -l) >= JOBS )); do wait -n; done
    run_row "$name" "$flim" &
  done < "$TRUTH"
  wait
fi

python3 - "$TRUTH" "$OUT" "$FILTER" "$GAME" <<'PY'
import collections, os, re, sys
truth_path, out, filt, game = sys.argv[1:5]
rows, steps = {}, {}
for line in open(truth_path):
    if line.startswith('#') or not line.strip():
        continue
    f = line.rstrip('\n').split('\t')
    if f[0] == 'row':
        if not filt or filt in f[1]:
            rows[f[1]] = int(f[2])
    elif f[0] == 'step' and f[1] in rows:
        steps[(f[1], int(f[2]), f[3])] = {'cls': f[4], 'label': f[8], 'bin': f[9], 'writer': f[10]}
pat = re.compile(r'\[jump-census vbl=(\d+)\] obj=([0-9a-f]+) cls=(\w+) st=(\w+)->(\w+) d=([\d.]+) '
                 r'cutpath=(-?\d) table=(\d) load=(\d) bound=(\d+) verdict=(\w+) why=(\w+)')
missing, lines = [], {}
for r in rows:
    p = os.path.join(out, r + '.cen')
    rc = open(p + '.rc').read().strip() if os.path.exists(p + '.rc') else 'none'
    if not os.path.exists(p) or rc != '0':
        missing.append(f'{r} (rc={rc})')
        continue
    for l in open(p):
        m = pat.match(l)
        if m:
            lines[(r, int(m.group(1)), m.group(2))] = m
bound = max([int(m.group(10)) for m in lines.values()] or [0])
def verdict(m, markers, b):
    cp = int(m.group(7))
    if m.group(12) == 'class':
        return 1
    if cp >= 0:
        return cp
    if markers and (m.group(8) == '1' or m.group(9) == '1'):
        return 1
    return 1 if b > 0 and float(m.group(6)) > b else 0
configs = [(f'markers {"on" if mk else "off"}, bound {b}', mk, b)
           for mk in (0, 1) for b in sorted({bound, 0}, reverse=True)]
lab = collections.Counter(s['label'] for s in steps.values())
matched = {k: v for k, v in steps.items() if k in lines}
mlab = collections.Counter(s['label'] for s in matched.values())
print(f'[jump-census] game={game} rows={len(rows)} truth steps={len(steps)} '
      + ' '.join(f'{k}={lab[k]}' for k in sorted(lab)))
print(f'[jump-census] engagement: census lines={len(lines)} matched truth steps={len(matched)} '
      + ' '.join(f'{k}={mlab[k]}' for k in sorted(mlab)))
samp = [k for k, s in matched.items() if s['label'] == 'sampler']
print(f'[jump-census] sampler steps decided by the sampler rule: '
      f'{sum(1 for k in samp if int(lines[k].group(7)) >= 0)}/{len(samp)}')
unl = collections.Counter(f'{m.group(12)}/{m.group(11)}' for k, m in lines.items() if k not in steps)
print(f'[jump-census] census lines with no truth step: {sum(unl.values())} ('
      + ' '.join(f'{k}={v}' for k, v in sorted(unl.items())) + ')')
def score(markers, b, use_force=False):
    c = collections.Counter(); res = collections.Counter(); fh = collections.Counter()
    for k, s in matched.items():
        if s['label'] not in ('jump', 'motion'):
            continue
        m = lines[k]
        v = (m.group(11) == 'jump') if use_force else verdict(m, markers, b)
        if s['label'] == 'motion':
            c['motion'] += 1
            if v: c['false_holds'] += 1; fh[f"{s['cls']}:{s['writer']}"] += 1
        else:
            c['jump'] += 1
            if v: c['held'] += 1
            elif s['bin'].startswith('residual'):
                c['residual_misses'] += 1; res[s['bin']] += 1
            else:
                c['misses'] += 1; res[s['bin']] += 1
    return c, res, fh
for name, mk, b in configs:
    c, res, fh = score(mk, b)
    print(f"[jump-census] {name}: false_holds={c['false_holds']}/{c['motion']} "
          f"misses={c['misses']} residual_misses={c['residual_misses']} held={c['held']}/{c['jump']}")
c, res, fh = score(0, 0, use_force=True)
print(f"[jump-census] in force: false_holds={c['false_holds']}/{c['motion']} misses={c['misses']} "
      f"residual_misses={c['residual_misses']} held={c['held']}/{c['jump']}")
for k, v in sorted(res.items()):
    print(f'[jump-census]   missed {k}: {v}')
for k, v in fh.most_common(8):
    print(f'[jump-census]   false hold {k}: {v}')
if missing:
    print('[jump-census] FAIL: missing or failed captures: ' + ', '.join(missing))
    sys.exit(2)
if not matched:
    print('[jump-census] FAIL: VACUOUS, no truth step matched a census line')
    sys.exit(2)
if c['false_holds'] or c['misses']:
    print('[jump-census] verdict: FAIL')
    sys.exit(2)
print('[jump-census] verdict: PASS')
sys.exit(0)
PY
