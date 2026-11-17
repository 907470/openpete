#!/usr/bin/env bash
# check_moby_reveal_stability.sh - the extended-LOD reveal admits the same
# far mobys on every canonical tick of a still scene, and confirming its
# occlusion verdicts on decoded bounds changes no pixel.
#
# Class: under --lod extended the reveal appends records for mobys past the
# cull's distance box, each one gated by the moby occlusion cull. A cached
# verdict at most two VBLs old tests the bounds of the moby's last decode; an
# older one tests a rect rebuilt from the moby's origin and model radius,
# which is wider and passes a moby the decoded bounds cull. Each decode
# refreshes the cache, so without a confirmation on decoded bounds a hidden
# moby is admitted, decoded, culled while its bounds are fresh and admitted
# again once they age: the reveal's record count alternates on a period set
# by the VBL of the last decode, which differs between hosts.
#
# Windowed only: the occlusion grid is built from presented depth, so a
# headless run (no grid) never culls and never reaches the cycle.
#
# Row: spyro-2 summer_forest spawn pose (--skip-to-level, no input), --lod
# extended, 16:9, --render-fps 480 --vsync 0.
#
# Instrument: OPENPETE_REVEAL_CENSUS=1, one line per canonical reveal walk:
#   [reveal-census vbl=<v> key=<feed key> kind=anim|simple] cand=<n> occl=<n>
#                  added=<n> confirmed=<n>
# cand: mobys offered to the occlusion test; occl: culled; added: records
# appended; confirmed: provisional admissions the decoded bounds culled.
# Pixels: --screenshot-intermediate-on-frame captures (deterministic, alpha
# 0.5) at SHOTS, with the confirmation on and off (OPENPETE_OCCL_CONFIRM=0).
# The window is the capture's own extent (768x432 at 16:9): the occlusion grid
# is reduced from the live presents' depth pre-pass, so its verdicts hold for
# a render at that extent, and a capture at another extent can show pixels of
# a moby the live extent hides. A run whose capture extent differs from the
# window is VACUOUS. --window=320x180 reproduces that mismatch (it reads
# nonzero px: the instrument's calibration).
#
# Pass: over the last NTICKS (60) anim lines with key >= LIVE_KEY, `added` takes
# one value; and every capture pair differs in 0 px. Engagement (else
# VACUOUS): cand > 0 and occl > 0 summed over the window; the pixel half
# counts as engaged only when the off leg's `added` differs from the on
# leg's at a key next to a capture (key = shot - 1), so the pair spans a
# tick where the two legs drew different records.
#
# Verdict line:
#   [reveal-stability] game=spyro-2 window=<WxH> shot=<WxH> ticks=<n> added_values=<v,...>
#                      off_added_values=<v,...> cand=<n> occl=<n>
#                      px=<n> px_engaged=<0|1> verdict=PASS|FAIL|VACUOUS
#
# Every launch passes --input-lock --vibration 0 --background 1 and runs on a
# private Xvfb display (scripts/lib/xvfb.sh).
#
# Usage: tests/harness/check_moby_reveal_stability.sh [--build-dir=DIR] [--out=DIR] [--window=WxH]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no xvfb-run, no
# binary, disc or python3 with PIL and numpy).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
WINDOW=768x432
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
        --window=*)    WINDOW="${a#--window=}";;
    esac
done

GAME=spyro-2
pg_resolve "$GAME"
LEVEL=summer_forest
FRAME_LIMIT=2920
LIVE_KEY=2600
NTICKS=60
SHOTS=2900,2902,2904,2906
ROW_TIMEOUT=400

[[ -x "$PG_BIN" ]] || { echo "reveal-stability: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "reveal-stability: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import PIL, numpy' 2>/dev/null ||
    { echo "reveal-stability: SKIP (python3 without PIL and numpy)"; exit 64; }
op_xvfb_prefix reveal-stability 1280x800

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/reveal-stability"
mkdir -p "$OUT"
rm -f "${OUT:?}"/on.log "${OUT:?}"/off.log "${OUT:?}"/on__*.png "${OUT:?}"/off__*.png

# run_leg TAG [ENV=VALUE...]: one windowed run of the row, census on.
run_leg() {
    local tag=$1; shift
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_REVEAL_CENSUS=1 "$@" \
        "${OP_XVFB[@]}" timeout -k 10 "$ROW_TIMEOUT" "$PG_BIN" "${PG_RUN_ARGS[@]}" \
        --no-config --disc "$PG_DISC" --no-card --mute 1 \
        --vibration 0 --input-lock --background 1 \
        --skip-to-level "$LEVEL" --lod extended --window "$WINDOW" \
        --render-fps 480 --vsync 0 \
        --screenshot-intermediate-on-frame "$SHOTS" \
        --screenshot-output "$OUT/${tag}__{n}.png" \
        --frame-limit "$FRAME_LIMIT" </dev/null >"$OUT/$tag.log" 2>&1
}
run_leg on
run_leg off OPENPETE_OCCL_CONFIRM=0
for tag in on off; do
    [[ -s "$OUT/$tag.log" ]] || { echo "reveal-stability: FAIL (no $tag log)"; exit 1; }
    for k in ${SHOTS//,/ }; do
        [[ -s "$OUT/${tag}__$k-vk.png" ]] ||
            { echo "reveal-stability: FAIL (no capture $OUT/${tag}__$k-vk.png)"; exit 1; }
    done
done

read -r nwin values cand occl offvalues engaged <<<"$(python3 - "$OUT" "$LIVE_KEY" "$NTICKS" "$SHOTS" <<'EOF'
import re, sys
out, live, win, shots = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), [int(s) for s in sys.argv[4].split(',')]
pat = re.compile(r'\[reveal-census vbl=\d+ key=(\d+) kind=anim\] cand=(\d+) occl=(\d+) added=(\d+)')
def rows(tag):
    r = {}
    for line in open(f'{out}/{tag}.log', errors='replace'):
        m = pat.search(line)
        if m and int(m.group(1)) >= live:
            r[int(m.group(1))] = tuple(int(x) for x in m.group(2, 3, 4))
    return r
on, off = rows('on'), rows('off')
keys = sorted(on)[-win:]
vals = sorted({on[k][2] for k in keys})
offv = sorted({off[k][2] for k in sorted(off)[-win:]})
eng = int(any(k - 1 in on and k - 1 in off and on[k - 1][2] != off[k - 1][2] for k in shots))
print(len(keys), ','.join(map(str, vals)) or '-', sum(on[k][0] for k in keys), sum(on[k][1] for k in keys),
      ','.join(map(str, offv)) or '-', eng)
EOF
)"
px=$(python3 - "$OUT" "$SHOTS" <<'EOF'
import sys
import numpy as np
from PIL import Image
out, shots = sys.argv[1], sys.argv[2].split(',')
n = 0
for k in shots:
    a = np.asarray(Image.open(f'{out}/on__{k}-vk.png').convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(f'{out}/off__{k}-vk.png').convert('RGB'), dtype=np.int16)
    if a.shape != b.shape:
        n += a.shape[0] * a.shape[1]
        continue
    d = np.abs(a - b).sum(axis=2)
    ys, xs = np.nonzero(d)
    for x, y in list(zip(xs.tolist(), ys.tolist()))[:8]:
        print(f'  shot {k}: px ({x},{y}) differs by {int(d[y, x])}', file=sys.stderr)
    n += int((d > 0).sum())
print(n)
EOF
)

shot_extent=$(grep -o 'vk screenshot [0-9]*x[0-9]*' "$OUT/on.log" | head -1 | awk '{print $3}')
verdict=PASS
if (( nwin < NTICKS || cand == 0 || occl == 0 )); then
    verdict=VACUOUS
elif [[ "$values" == *,* ]] || (( px != 0 )); then
    verdict=FAIL
elif (( engaged == 0 )) || [[ "$shot_extent" != "$WINDOW" ]]; then
    verdict=VACUOUS
fi
echo "[reveal-stability] game=$GAME window=$WINDOW shot=${shot_extent:-none} ticks=$nwin added_values=$values off_added_values=$offvalues cand=$cand occl=$occl px=$px px_engaged=$engaged verdict=$verdict out=$OUT"
[[ "$verdict" == PASS ]]
