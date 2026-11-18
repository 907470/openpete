#!/usr/bin/env bash
#
# render-res-rows.sh - headless instruments of --resolution on one game.
#
# Every leg is one headless capture at one pose. Each leg's log must carry
# the [render-res] engagement line with composes above 0, else the leg is
# VACUOUS and the run fails. Checks:
#
#   model     the resample model (tests/harness/render_res_model.py) on the
#             leg's -base.png and -vk.png: copy both axes (WxH = C), nearest
#             (WxH = C/2), area at ratio 2 (WxH = 2C), sharp/area at N = 1
#             and 2 at 4:3 native-only (C = 640 wide), N = 1 and 2 beside a
#             --psycross-native-raster reference (C = the 512-wide raster),
#             and N = 2 on a native-only Hor+ capture at --aspect 16:9 (crop
#             above 0). Each has a shift control (OPENPETE_RENDER_RES_SHIFT,
#             1 when both axes fetch whole texels, else 0.5) the model must
#             fail.
#   og        the reference legs write the PsyCross -og capture beside -vk
#             for adjudication by eye; listed only.
#   rq        pixelation: under N = 2 with integer vertex precision, the
#             implicit default, --raster-quantize 0 and 1 are pixel-identical
#             with rq_passes=0; under WxH = C the implicit default has
#             rq_passes=0 and equals --raster-quantize 0, an explicit 1 has
#             rq_passes above 0 and differs; at window the implicit default
#             has rq_passes above 0 and differs from --raster-quantize 0.
#   snap      OPENPETE_RENDER_RES_SNAP_CENSUS at N = 1 and 2 on the Hor+ pose
#             with integer vertex precision: in-frame integer-x vertices
#             above 0, off-pixel vertices 0.
#   default   a run with no --resolution prints composes above 0 and
#             base_composes=0 (the base path never ran).
#
# Runs are sandboxed (scripts/lib/sandbox.sh); captures are written inside
# the sandbox root and moved to OUTDIR.
#
# Usage: tests/harness/render-res-rows.sh [--game G] OUTDIR
# Exit: 0 every check passed, 1 a check failed or a leg was vacuous,
#       64 SKIP (binary or disc missing).
set -u
export OPENPETE_VIBRATION=0

GAME=spyro-1
while [[ $# -gt 1 ]]; do
  case "$1" in
    --game) GAME="$2"; shift 2 ;;
    *) break ;;
  esac
done
OUT="${1:?usage: render-res-rows.sh [--game G] OUTDIR}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../scripts/lib/sandbox.sh
source "$ROOT/scripts/lib/sandbox.sh"
BIN="${OPENPETE_BIN:-$ROOT/build/linux-release/openpete}"
case "$GAME" in
  spyro-1) CUE=SCUS-94228.cue; POSE=(--skip-to-level artisans --freecam-pose 84992,45582,9948,1024,160); F=1900; C43=640x448; CHALF=320x224; CDBL=1280x896 ;;
  spyro-2) CUE=SCUS-94425.cue; POSE=(--skip-to-level summer_forest); F=2073; C43=640x432; CHALF=320x216; CDBL=1280x864 ;;
  spyro-3) CUE=SCUS-94467.cue; POSE=(--skip-to-level sunrise_spring); F=1700; C43=640x432; CHALF=320x216; CDBL=1280x864 ;;
  *) echo "render-res-rows: unknown game $GAME"; exit 1 ;;
esac
DISC="$ROOT/games/$GAME/$GAME/build/$CUE"
[[ -x "$BIN" && -f "$DISC" ]] || { echo "render-res-rows: SKIP (binary or disc missing)"; exit 64; }
MODEL="$ROOT/tests/harness/render_res_model.py"
mkdir -p "$OUT"
FAILS=0

# One headless capture. args: tag env-assignments... -- flags...
leg() {
  local tag="$1"; shift
  local -a envs=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
  shift
  rm -f "${OUT:?}/${tag}"__*.png "${OUT:?}/${tag}.log"
  local sb="${OPENPETE_SANDBOX:?}/rr-out"
  mkdir -p "$sb"
  rm -f "${sb:?}/${tag}"__*.png
  env "${envs[@]}" nice -n 10 "$BIN" --game "$GAME" --headless --no-config --disc "$DISC" \
    --renderer vk --lod original --no-card --mute 1 --dither 0 "${POSE[@]}" \
    --screenshot-on-frame "$F" --screenshot-output "$sb/${tag}__{n}.png" \
    --frame-limit "$((F + 10))" "$@" >"$OUT/${tag}.log" 2>&1
  local rc=$?
  # Captures land inside the sandbox root (the game refuses a real
  # per-user location) and move to OUTDIR.
  local f
  for f in "$sb/${tag}"__*.png; do [[ -f "$f" ]] && mv -f "$f" "$OUT/"; done
  if [[ ! -f "$OUT/${tag}__${F}-vk.png" ]]; then
    echo "FAIL $tag: no -vk capture (rc=$rc, log $OUT/${tag}.log)"; FAILS=$((FAILS + 1)); return 1
  fi
  local line; line="$(grep -a '^\[render-res\] why=' "$OUT/${tag}.log" | tail -1)"
  if [[ -z "$line" ]] || ! grep -qE 'composes=[1-9]' <<<"$line"; then
    echo "VACUOUS $tag: no [render-res] line with composes above 0"; FAILS=$((FAILS + 1)); return 1
  fi
  return 0
}

capline() { grep -a '^\[render-res\] why=capture' "$OUT/$1.log" | tail -1; }
exitline() { grep -a '^\[render-res\] why=exit' "$OUT/$1.log" | tail -1; }
rqof() { exitline "$1" | sed -n 's/.*rq_passes=\([0-9]*\).*/\1/p'; }

# Pixel compare of two legs' -vk. args: a b want(same|differ)
cmp_legs() {
  local n
  n="$(python3 -c 'import sys,numpy as np; from PIL import Image
a=np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int); b=np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
print(-1 if a.shape!=b.shape else int((np.abs(a-b).max(axis=2)>0).sum()))' "$OUT/$1__${F}-vk.png" "$OUT/$2__${F}-vk.png")"
  if [[ "$3" == same && "$n" == 0 ]] || [[ "$3" == differ && "$n" != 0 && "$n" != -1 ]]; then
    echo "PASS cmp $1 vs $2: differing=$n (want $3)"
  else
    echo "FAIL cmp $1 vs $2: differing=$n (want $3)"; FAILS=$((FAILS + 1))
  fi
}

# Resample model on a leg and its shift control: 1 texel when both axes
# fetch whole texels (copy, nearest, area), else 0.5. args: tag flags...
model_row() {
  local tag="$1" sh=0.5; shift
  leg "$tag" OPENPETE_RENDER_RES_DUMP_BASE=1 -- "$@" || return
  [[ "$(capline "$tag")" =~ filter=x:(copy|nearest|area),y:(copy|nearest|area)\  ]] && sh=1
  leg "${tag}-shift" OPENPETE_RENDER_RES_DUMP_BASE=1 OPENPETE_RENDER_RES_SHIFT="$sh" -- "$@" || return
  python3 "$MODEL" --base "$OUT/${tag}__${F}-base.png" --out "$OUT/${tag}__${F}-vk.png" \
    --line "$(capline "$tag")" || FAILS=$((FAILS + 1))
  python3 "$MODEL" --base "$OUT/${tag}-shift__${F}-base.png" --out "$OUT/${tag}-shift__${F}-vk.png" \
    --line "$(capline "$tag")" --expect-fail || FAILS=$((FAILS + 1))
}

echo "== $GAME: resample model =="
model_row copy --aspect 4:3 --screenshot-native-only --resolution "$C43"
model_row nearest --aspect 4:3 --screenshot-native-only --resolution "$CHALF"
model_row area2 --aspect 4:3 --screenshot-native-only --resolution "$CDBL"
model_row n1 --aspect 4:3 --screenshot-native-only --resolution 1
model_row n2 --aspect 4:3 --screenshot-native-only --resolution 2
model_row n1og --aspect 4:3 --psycross-native-raster 1 --resolution 1
model_row n2og --aspect 4:3 --psycross-native-raster 1 --resolution 2
model_row n2hor --aspect 16:9 --screenshot-native-only --resolution 2
for t in n1og n2og; do [[ -f "$OUT/${t}__${F}-og.png" ]] && echo "og $OUT/${t}__${F}-og.png beside $OUT/${t}__${F}-vk.png"; done

echo "== $GAME: pixelation =="
VP=(--aspect 4:3 --screenshot-native-only --vertex-precision integer)
leg rqN-imp -- "${VP[@]}" --resolution 2
leg rqN-0 -- "${VP[@]}" --resolution 2 --raster-quantize 0
leg rqN-1 -- "${VP[@]}" --resolution 2 --raster-quantize 1
leg rqE-imp -- "${VP[@]}" --resolution "$C43"
leg rqE-0 -- "${VP[@]}" --resolution "$C43" --raster-quantize 0
leg rqE-1 -- "${VP[@]}" --resolution "$C43" --raster-quantize 1
leg rqW-imp -- "${VP[@]}" --resolution window
leg rqW-0 -- "${VP[@]}" --resolution window --raster-quantize 0
for t in rqN-imp rqN-0 rqN-1 rqE-imp rqE-0 rqE-1 rqW-imp rqW-0; do echo "rq_passes $t=$(rqof "$t")"; done
for t in rqN-imp rqN-0 rqN-1 rqE-imp rqE-0; do
  [[ "$(rqof "$t")" == 0 ]] || { echo "FAIL $t: rq_passes $(rqof "$t") (want 0)"; FAILS=$((FAILS + 1)); }
done
for t in rqE-1 rqW-imp; do
  [[ "$(rqof "$t")" =~ ^[1-9] ]] || { echo "FAIL $t: rq_passes $(rqof "$t") (want above 0)"; FAILS=$((FAILS + 1)); }
done
cmp_legs rqN-imp rqN-0 same
cmp_legs rqN-imp rqN-1 same
cmp_legs rqE-imp rqE-0 same
cmp_legs rqE-imp rqE-1 differ
cmp_legs rqW-imp rqW-0 differ

echo "== $GAME: N width census =="
for n in 1 2; do
  leg "snap$n" OPENPETE_RENDER_RES_SNAP_CENSUS=1 -- --aspect 16:9 --screenshot-native-only --vertex-precision integer --resolution "$n"
  s="$(grep -a '^\[render-res-snap\]' "$OUT/snap$n.log" | tail -1)"
  echo "snap$n: $s"
  if [[ "$s" =~ int_x=([0-9]+)\ outside=[0-9]+\ off_pixel=([0-9]+) ]] && (( BASH_REMATCH[1] > 0 && BASH_REMATCH[2] == 0 )); then
    echo "PASS snap$n"
  else
    echo "FAIL snap$n"; FAILS=$((FAILS + 1))
  fi
done

echo "== $GAME: default path =="
leg default -- --aspect 16:9 --screenshot-native-only
d="$(exitline default)"
echo "default: $d"
if [[ "$d" =~ base_composes=0 ]] && ! grep -qa '^\[render-res\] why=capture' "$OUT/default.log"; then
  echo "PASS default: base path never ran"
else
  echo "FAIL default: base path ran"; FAILS=$((FAILS + 1))
fi

echo "render-res-rows $GAME: $FAILS failure(s)"
(( FAILS == 0 ))
