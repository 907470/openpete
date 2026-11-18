#!/usr/bin/env bash
#
# render-res-backdrop.sh - the pause backdrop across a --resolution change
# (windowed, Xvfb).
#
# Spyro 1's pause-menu-text-moby-swim movie holds the pause screen at VBL
# 1750, where the compose draws the frozen world backdrop. One windowed run
# per transition renders three legs of that present through the live
# compose (OPENPETE_RENDER_RES_AB): A at the configured value, B at the
# variable's, A' as A again. Each leg re-ensures the targets, so B and A'
# each change the backdrop target's size while the game is paused and no
# world present can re-snapshot it.
#
# Transitions: window -> 2 -> window at 640x448 (wf = 1), 2 -> window -> 2
# at 1280x720 (16:9 AUTO, wf' > wf), 2 -> 960x540 -> 2 at 1280x720.
#
# Pass, per run: every leg's backdrop line shows the freeze bit set and the
# quad drawn from a valid record; the B leg draws frozen_prev (the size
# changed under it); A' equals A bit for bit (the backdrop survived two size
# changes; else the sample is VOID and fails); B's image differs from A's by
# a mean of at most 3 LSB per channel over the window (the same backdrop
# under another resample; the pause HUD itself is resampled too).
#
# Usage: tests/harness/render-res-backdrop.sh OUTDIR
# Exit: 0 pass, 1 fail, 64 SKIP (no Xvfb, binary, disc or movie).
set -u
export OPENPETE_VIBRATION=0

OUT="${1:?usage: render-res-backdrop.sh OUTDIR}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../scripts/lib/sandbox.sh
source "$ROOT/scripts/lib/sandbox.sh"
# shellcheck source=../../scripts/lib/xvfb.sh
source "$ROOT/scripts/lib/xvfb.sh"
BIN="${OPENPETE_BIN:-$ROOT/build/linux-release/openpete}"
DISC="$ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue"
MOVIE="$ROOT/games/spyro-1/bk2/pause-menu-text-moby-swim.bk2"
F=1750
op_xvfb_prefix render-res-backdrop 1600x1200
[[ -x "$BIN" && -f "$DISC" && -f "$MOVIE" ]] || { echo "render-res-backdrop: SKIP (binary, disc or movie missing)"; exit 64; }
mkdir -p "$OUT"
FAILS=0

# args: tag window configured-value ab-value
run() {
  local tag="$1" win="$2" val="$3" ab="$4"
  local sb="${OPENPETE_SANDBOX:?}/bd-out"
  mkdir -p "$sb"
  rm -f "${sb:?}/${tag}"__*.png "${OUT:?}/${tag}"__*.png "${OUT:?}/${tag}.log"
  OPENPETE_RENDER_RES_AB="$ab" "${OP_XVFB[@]}" "$BIN" --game spyro-1 --no-config --disc "$DISC" \
    --renderer vk --lod original --no-card --mute 1 --input-lock --vibration 0 --window "$win" \
    --aspect auto --perf-stats off --osd 0 --version-osd 0 --screenshot-native-only \
    --resolution "$val" --bk2 "$MOVIE" --screenshot-on-frame "$F" \
    --screenshot-output "$sb/${tag}__{n}.png" --frame-limit "$((F + 10))" >"$OUT/${tag}.log" 2>&1
  local f
  for f in "$sb/${tag}"__*.png; do [[ -f "$f" ]] && mv -f "$f" "$OUT/"; done
  local log="$OUT/${tag}.log" ok=1
  grep -a '^\[render-res-ab\] backdrop' "$log" | sed "s/^/  $tag: /"
  [[ "$(grep -ac '^\[render-res-ab\] backdrop .*freeze_bit=1 drawn=1 .*valid=1' "$log")" == 3 ]] \
    || { echo "FAIL $tag: a leg drew no valid backdrop"; ok=0; }
  grep -qa '^\[render-res-ab\] backdrop leg=-abB .*record=frozen_prev' "$log" \
    || { echo "FAIL $tag: the B leg did not draw the displaced backdrop (frozen_prev)"; ok=0; }
  local s; s="$(grep -a '^\[render-res-ab\] sample\|^\[render-res-ab\] VOID' "$log" | tail -1)"
  [[ "$s" == *"A_vs_A2=0 "* ]] || { echo "FAIL $tag: ${s:-no sample} (A' must equal A)"; ok=0; }
  local m
  m="$(python3 -c 'import sys,numpy as np; from PIL import Image
a=np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int); b=np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
print("%.3f" % (np.abs(a-b).mean() if a.shape==b.shape else 999))' "$OUT/${tag}__${F}-abA.png" "$OUT/${tag}__${F}-abB.png" 2>/dev/null)"
  echo "  $tag: mean |A-B| = ${m:-none} LSB (bound 3)"
  python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) <= 3.0 else 1)' "${m:-999}" \
    || { echo "FAIL $tag: mean |A-B| above the bound"; ok=0; }
  (( ok )) && echo "PASS $tag" || FAILS=$((FAILS + 1))
}

run w-n2-w 640x448 window 2
run n2-w-n2 1280x720 2 window
run n2-e-n2 1280x720 2 960x540
echo "render-res-backdrop: $FAILS failure(s)"
(( FAILS == 0 ))
