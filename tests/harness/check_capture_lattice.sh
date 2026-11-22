#!/usr/bin/env bash
#
# check_capture_lattice.sh - the postfx lattice and the scene compose of a
# headless capture do not depend on --window.
#
# A headless capture composes the scene over the whole capture target; its
# SCENE and COMPOSITE postfx chains must place the PSX raster (op_scene_rect)
# on that target, not on the window's picture panel. Instrument: under
# --raster-quantize 1 each PSX pixel is point-sampled across the capture,
# so the count of adjacent pixel columns identical over every row is the
# capture width minus the PSX columns it shows (cols), and the count of
# adjacent rows identical over every column is the capture height minus the
# PSX lines it shows (rows). Four Spyro 1 legs at one pose (artisans
# freecam, VBL 1900), native-only:
#
#   43-match  --aspect 4:3, window = the capture shape   cols 640-512 = 128,
#                                                         rows 448-224 = 224
#   43-mis    --aspect 4:3 --window 1280x720              cols, rows of 43-match
#   hor-match --aspect 16:9, window = the capture shape   cols and rows above 0
#   hor-mis   --aspect 16:9 --window 1280x960             cols, rows of hor-match
#
# The 4:3 window is wider than the capture (the panel narrows x), the 16:9
# window taller (the panel narrows y), so each axis has a leg that moves
# when the chains read the panel.
#
# Four no-postfx legs (--raster-quantize 0) repeat those shapes; each
# mismatched capture must be pixel-identical to its matching one (0
# differing pixels). The Spyro shadow decal samples the depth pre-pass, so
# a pre-pass rasterized at a window-derived extent moves the decal's edge.
# Every no-postfx leg logs the capture's [depth-prepass] raster line
# (OPENPETE_DEPTH_PREPASS_DEBUG) at the capture's size. Engagement: a
# control leg with every render path but the shadow (43-noshadow) differs
# from 43-plain-match, so the pose's capture carries the decal.
#
# A leg whose exit [render-res] line lacks rq_passes above 0 is VACUOUS
# (the pixelation pass never ran) and fails the run. Runs are sandboxed
# (scripts/lib/sandbox.sh).
#
# Every launch passes --headless; --window only sets the size the renderer
# lays its panels out for, and no window opens: xvfb-guard: no-launch
#
# Usage: tests/harness/check_capture_lattice.sh [OUTDIR]
# Exit: 0 pass, 1 fail or vacuous, 64 SKIP (binary or disc missing).
set -u
export OPENPETE_VIBRATION=0

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=../../scripts/lib/sandbox.sh
source "$ROOT/scripts/lib/sandbox.sh"
BIN="${OPENPETE_BIN:-$ROOT/build/linux-release/openpete}"
DISC="$ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue"
[[ -x "$BIN" && -f "$DISC" ]] || { echo "capture-lattice: SKIP (binary or disc missing)"; exit 64; }
OUT="${1:-${OPENPETE_SANDBOX:?}/lattice}"
mkdir -p "$OUT"
F=1900
FAILS=0

# One headless capture. args: tag flags...
leg() {
  local tag="$1"; shift
  rm -f "${OUT:?}/${tag}"__*.png "${OUT:?}/${tag}.log"
  local sb="${OPENPETE_SANDBOX:?}/lat-out"
  mkdir -p "$sb"
  rm -f "${sb:?}/${tag}"__*.png
  nice -n 10 "$BIN" --game spyro-1 --headless --no-config --disc "$DISC" \
    --renderer vk --lod original --no-card --mute 1 --dither 0 --raster-quantize 1 \
    --screenshot-native-only --skip-to-level artisans --freecam-pose 84992,45582,9948,1024,160 \
    --screenshot-on-frame "$F" --screenshot-output "$sb/${tag}__{n}.png" \
    --frame-limit "$((F + 10))" "$@" >"$OUT/${tag}.log" 2>&1
  local f
  for f in "$sb/${tag}"__*.png; do [[ -f "$f" ]] && mv -f "$f" "$OUT/"; done
}

# Duplicate column and row counts of a leg's -vk as "cols,rows", or -1 when
# the capture or the pixelation pass is missing.
dup() {
  local png="$OUT/$1__${F}-vk.png"
  [[ -f "$png" ]] || { echo -1; return; }
  grep -a '^\[render-res\] why=exit' "$OUT/$1.log" | tail -1 | grep -qE 'rq_passes=[1-9]' || { echo -1; return; }
  python3 -c 'import sys,numpy as np; from PIL import Image
a=np.asarray(Image.open(sys.argv[1]).convert("RGB"))
print("%d,%d" % ((a[:,1:,:]==a[:,:-1,:]).all(axis=(0,2)).sum(), (a[1:,:,:]==a[:-1,:,:]).all(axis=(1,2)).sum()))' "$png"
}

leg 43-match --aspect 4:3 &
leg 43-mis --aspect 4:3 --window 1280x720 &
wait
leg hor-match --aspect 16:9 &
leg hor-mis --aspect 16:9 --window 1280x960 &
wait
export OPENPETE_DEPTH_PREPASS_DEBUG=1
leg 43-plain-match --aspect 4:3 --raster-quantize 0 &
leg 43-plain-mis --aspect 4:3 --raster-quantize 0 --window 1280x720 &
wait
leg hor-plain-match --aspect 16:9 --raster-quantize 0 &
leg hor-plain-mis --aspect 16:9 --raster-quantize 0 --window 1280x960 &
wait
leg 43-noshadow --aspect 4:3 --raster-quantize 0 --render-paths \
  cyclorama,spyro,moby,terrain,flame,particles,tracer,transition,portal,glow,sparkle,moby_shadow,ui
unset OPENPETE_DEPTH_PREPASS_DEBUG

declare -A D
for t in 43-match 43-mis hor-match hor-mis; do
  D[$t]="$(dup "$t")"
  echo "dup $t=${D[$t]}"
  [[ "${D[$t]}" == -1 ]] && { echo "VACUOUS $t: no capture or rq_passes=0 (log $OUT/$t.log)"; FAILS=$((FAILS + 1)); }
done
[[ "${D[43-match]}" == 128,224 ]] || { echo "FAIL 43-match: dup ${D[43-match]} (want 128,224)"; FAILS=$((FAILS + 1)); }
[[ "${D[43-mis]}" == "${D[43-match]}" ]] || { echo "FAIL 43-mis: dup ${D[43-mis]} (want ${D[43-match]})"; FAILS=$((FAILS + 1)); }
[[ "${D[hor-match]}" =~ ^[1-9][0-9]*,[1-9][0-9]*$ ]] || { echo "FAIL hor-match: dup ${D[hor-match]} (want above 0)"; FAILS=$((FAILS + 1)); }
[[ "${D[hor-mis]}" == "${D[hor-match]}" ]] || { echo "FAIL hor-mis: dup ${D[hor-mis]} (want ${D[hor-match]})"; FAILS=$((FAILS + 1)); }

# Differing pixel count of two legs' -vk, or -1 when either capture is
# missing or the shapes differ.
ndiff() {
  local a="$OUT/$1__${F}-vk.png" b="$OUT/$2__${F}-vk.png"
  [[ -f "$a" && -f "$b" ]] || { echo -1; return; }
  python3 -c 'import sys,numpy as np; from PIL import Image
a=np.asarray(Image.open(sys.argv[1]).convert("RGB")); b=np.asarray(Image.open(sys.argv[2]).convert("RGB"))
print(int((a!=b).any(axis=2).sum()) if a.shape==b.shape else -1)' "$a" "$b"
}

# The capture's own pre-pass: a raster line at VBL F whose extent is the
# capture's size.
for t in 43-plain-match 43-plain-mis hor-plain-match hor-plain-mis; do
  wh="$(python3 -c 'import sys; from PIL import Image
w,h=Image.open(sys.argv[1]).size; print("%dx%d" % (w,h))' "$OUT/${t}__${F}-vk.png" 2>/dev/null)"
  grep -aq "^\[depth-prepass\] f=$F .* raster ${wh:-none} in " "$OUT/$t.log" \
    || { echo "FAIL $t: no capture pre-pass raster line at ${wh:-no capture} (log $OUT/$t.log)"; FAILS=$((FAILS + 1)); }
done
for p in 43 hor; do
  n="$(ndiff "$p-plain-match" "$p-plain-mis")"
  echo "plain $p mis-vs-match=$n px"
  [[ "$n" == 0 ]] || { echo "FAIL $p-plain-mis: $n px differ from $p-plain-match (want 0)"; FAILS=$((FAILS + 1)); }
done
n="$(ndiff 43-plain-match 43-noshadow)"
echo "control 43-noshadow-vs-match=$n px"
[[ "$n" =~ ^[1-9][0-9]*$ ]] || { echo "VACUOUS 43-noshadow: $n px (want above 0)"; FAILS=$((FAILS + 1)); }

echo "capture-lattice: $FAILS failure(s)"
(( FAILS == 0 ))
