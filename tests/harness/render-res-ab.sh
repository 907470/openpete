#!/usr/bin/env bash
#
# render-res-ab.sh - the in-binary live A/B of --resolution (windowed, Xvfb).
#
# One windowed process per pair renders three legs of one scene through the
# live compose (OPENPETE_RENDER_RES_AB): A at the configured value, B at the
# variable's, A' as A again. A' must equal A, else the sample is VOID; no
# valid sample is VACUOUS. Every run is at wf = 1 exactly: AUTO aspect in a
# window of the game's display shape (Spyro 1 640x448, Spyro 2/3 640x432),
# native-only, so the full-extent and panel mappings compute the same floats
# and pairs (a) and (b) must differ in zero pixels. The harness refuses a
# run whose engagement lines show wf other than 1 or V other than the whole
# window.
#
#   (a) window against WxH = window, --ssaa 1 and 2, --dither 0 and 1
#   (b) window --ssaa 2 against 2 x window at ssaa 1, --dither 0 and 1
#   (c) WxH = window / 2 at --dither 0: the resample model on B's legs
#   negative: B = window, zero differing pixels
#   shift: (a) and (b) with OPENPETE_RENDER_RES_SHIFT=1 must differ
#
# Usage: tests/harness/render-res-ab.sh [--game G] OUTDIR
# Exit: 0 pass, 1 fail, VOID-only or vacuous, 64 SKIP (no Xvfb, binary or disc).
set -u
export OPENPETE_VIBRATION=0

GAME=spyro-1
while [[ $# -gt 1 ]]; do
  case "$1" in
    --game) GAME="$2"; shift 2 ;;
    *) break ;;
  esac
done
OUT="${1:?usage: render-res-ab.sh [--game G] OUTDIR}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../scripts/lib/sandbox.sh
source "$ROOT/scripts/lib/sandbox.sh"
# shellcheck source=../../scripts/lib/xvfb.sh
source "$ROOT/scripts/lib/xvfb.sh"
BIN="${OPENPETE_BIN:-$ROOT/build/linux-release/openpete}"
case "$GAME" in
  spyro-1) CUE=SCUS-94228.cue; POSE=(--skip-to-level artisans --freecam-pose 84992,45582,9948,1024,160); F=1900; WIN=640x448; HALF=320x224; DBL=1280x896 ;;
  spyro-2) CUE=SCUS-94425.cue; POSE=(--skip-to-level summer_forest); F=2073; WIN=640x432; HALF=320x216; DBL=1280x864 ;;
  spyro-3) CUE=SCUS-94467.cue; POSE=(--skip-to-level sunrise_spring); F=1700; WIN=640x432; HALF=320x216; DBL=1280x864 ;;
  *) echo "render-res-ab: unknown game $GAME"; exit 1 ;;
esac
DISC="$ROOT/games/$GAME/$GAME/build/$CUE"
op_xvfb_prefix render-res-ab 1600x1200
[[ -x "$BIN" && -f "$DISC" ]] || { echo "render-res-ab: SKIP (binary or disc missing)"; exit 64; }
mkdir -p "$OUT"
FAILS=0 SAMPLES=0

# One windowed A/B run. args: tag want(same|differ) ab-spec env-assignments... -- flags...
pair() {
  local tag="$1" want="$2" spec="$3"; shift 3
  local -a envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  shift
  local sb="${OPENPETE_SANDBOX:?}/ab-out"
  mkdir -p "$sb"
  rm -f "${sb:?}/${tag}"__*.png "${OUT:?}/${tag}"__*.png "${OUT:?}/${tag}.log"
  env "${envs[@]}" OPENPETE_RENDER_RES_AB="$spec" nice -n 10 "${OP_XVFB[@]}" \
    "$BIN" --game "$GAME" --no-config --disc "$DISC" --renderer vk --lod original --no-card --mute 1 \
    --input-lock --vibration 0 --window "$WIN" --aspect auto --perf-stats off --osd 0 --version-osd 0 \
    --screenshot-native-only "${POSE[@]}" --screenshot-on-frame "$F" \
    --screenshot-output "$sb/${tag}__{n}.png" --frame-limit "$((F + 10))" "$@" >"$OUT/${tag}.log" 2>&1
  local f
  for f in "$sb/${tag}"__*.png; do [[ -f "$f" ]] && mv -f "$f" "$OUT/"; done
  local bad
  bad="$(grep -a '^\[render-res\] why=ab' "$OUT/${tag}.log" | grep -vE "V=0,0,${WIN} .* wf=1\.0000 " | head -1)"
  if [[ -n "$bad" ]] || ! grep -qa '^\[render-res\] why=ab' "$OUT/${tag}.log"; then
    echo "REFUSED $tag: a leg is not at wf = 1 over the whole window: ${bad:-no ab engagement line}"
    FAILS=$((FAILS + 1)); return 1
  fi
  local s; s="$(grep -a '^\[render-res-ab\]' "$OUT/${tag}.log" | tail -1)"
  if [[ "$s" == *VOID* || -z "$s" ]]; then echo "VOID $tag: ${s:-no sample}"; return 1; fi
  SAMPLES=$((SAMPLES + 1))
  local n="${s##*A_vs_B=}"
  if [[ "$want" == same && "$n" == 0 ]] || [[ "$want" == differ && "$n" =~ ^[1-9] ]]; then
    echo "PASS $tag: A_vs_B=$n (want $want)"
  else
    echo "FAIL $tag: A_vs_B=$n (want $want)"; FAILS=$((FAILS + 1))
  fi
}

for d in 0 1; do
  pair "a-s1-d$d" same "$WIN" -- --ssaa 1 --dither "$d"
  pair "a-s2-d$d" same "$WIN,ssaa=2" -- --ssaa 2 --dither "$d"
  pair "b-d$d" same "$DBL,ssaa=1" -- --ssaa 2 --dither "$d"
done
pair neg same window -- --ssaa 1 --dither 0
pair a-shift differ "$WIN" OPENPETE_RENDER_RES_SHIFT=1 -- --ssaa 1 --dither 0
pair b-shift differ "$DBL,ssaa=1" OPENPETE_RENDER_RES_SHIFT=1 -- --ssaa 2 --dither 0
pair c-d0 differ "$HALF" OPENPETE_RENDER_RES_DUMP_BASE=1 -- --ssaa 1 --dither 0
line="$(grep -a '^\[render-res\] why=ab value=' "$OUT/c-d0.log" | grep -v 'value=window' | head -1)"
python3 "$ROOT/tests/harness/render_res_model.py" --base "$OUT/c-d0__${F}-abB-base.png" \
  --out "$OUT/c-d0__${F}-abB.png" --line "$line" || FAILS=$((FAILS + 1))

echo "render-res-ab $GAME: samples=$SAMPLES failures=$FAILS"
(( SAMPLES == 0 )) && { echo "VACUOUS: no valid sample"; exit 1; }
(( FAILS == 0 ))
