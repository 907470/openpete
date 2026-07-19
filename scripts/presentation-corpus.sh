#!/usr/bin/env bash
#
# presentation-corpus.sh — capture the native -vk presentation corpus.
#
# For every bk2 in the manifest (filtered), boot the game headless under
# software Vulkan (llvmpipe, deterministic) and capture two legs on the
# native-only path:
#   canonical   VBL-keyed frames                -> <name>__canon__<frame>-vk.png
#   intermediate deterministic alpha=0.5 frames  -> <name>__inter__<frame>-vk.png
# Both legs are gameplay-neutral display captures; the same-binary depth kill
# switch (OPENPETE_DEPTH_AFFINE) selects legacy vs candidate depth so golden and
# candidate differ ONLY by the change under test.
#
# Usage:
#   scripts/presentation-corpus.sh <off|on> <outdir> [bk2-filter]
#     off|on     OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir     where the -vk PNGs are written (created)
#     bk2-filter substring; only manifest rows whose name contains it are run
#                (default: cliff-town-vista, the single-bk2 bring-up scope)
#
# Exit: 0 = all captures produced, 1 = a capture is missing (hard failure),
#       64 = SKIP (binary / disc / Vulkan loader / manifest missing).
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/.."

AFFINE="${1:-off}"
OUTDIR="${2:-}"
FILTER="${3:-cliff-town-vista}"

# Capture aspect. The golden is ALWAYS widescreen so it catches Hor+ artifacts
# the 4:3 view can't show; override for a 4:3 spot-check if ever needed.
CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"

# --build-dir=DIR override (ctest passes this); else the default release tree.
BUILD_DIR="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD_DIR="${a#--build-dir=}";; esac; done

BIN="${OPENPETE_BIN:-$BUILD_DIR/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MANIFEST="scripts/presentation-corpus.manifest"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on> <outdir> [bk2-filter]" >&2; exit 64; fi
case "$AFFINE" in off|0) AFFINE_ENV=0;; on|1) AFFINE_ENV=1;; *)
  echo "presentation-corpus: first arg must be off|on (got '$AFFINE')" >&2; exit 64;; esac

if [[ ! -x "$BIN" ]]; then echo "[presentation-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[presentation-corpus] SKIP: no disc at $DISC"; exit 64; fi
if [[ ! -f "$MANIFEST" ]]; then echo "[presentation-corpus] SKIP: no manifest $MANIFEST"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[presentation-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

# Deterministic software raster (see the plan's pinned-rasterizer requirement).
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"
export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"

mkdir -p "$OUTDIR"
echo "[presentation-corpus] bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT filter='$FILTER' -> $OUTDIR"

run_one() {  # name frame_limit leg flag targets
  local name="$1" flim="$2" leg="$3" flag="$4" targets="$5"
  local out="$OUTDIR/${name}__${leg}__{n}.png"
  local log; log="$(mktemp)"
  # Always capture at 16:9 so the golden catches widescreen (Hor+) artifacts —
  # the revealed side world and the reveal-edge seams a 4:3 dump can't see. The
  # headless native-only capture honors --aspect (openpete_native_widescreen_factor
  # surfaces the fixed aspect when there is no window) and renders the widened
  # view into a dedicated capture target, so both legs are true Hor+ (853x448).
  timeout 300 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 --renderer vk \
    --aspect "$CAP_ASPECT" \
    --bk2 "games/spyro-1/bk2/${name}.bk2" --screenshot-native-only \
    "$flag" "$targets" --screenshot-output "$out" \
    --frame-limit "$flim" --max-runtime-seconds 280 >"$log" 2>&1
  local rc=$?
  # Assert every requested target produced a -vk PNG (fail loudly, never silently
  # capture a partial corpus that would read as "all clean" downstream).
  local miss=0 t
  IFS=',' read -ra TS <<<"$targets"
  for t in "${TS[@]}"; do
    if [[ ! -f "$OUTDIR/${name}__${leg}__${t}-vk.png" ]]; then
      echo "  MISSING ${name}__${leg}__${t}-vk.png (rc=$rc)"; miss=1; fi
  done
  if [[ $miss -ne 0 ]]; then echo "  --- last log lines ---"; tail -5 "$log" | sed 's/^/  /'; fi
  rm -f "$log"
  return $miss
}

# Authoritative movie length = the number of input-frame lines ('|'-prefixed) in
# the bk2's Input Log.txt — matches the binary's own bk2-end frame exactly. The
# frame-limit is one cycle past that (render the last input's result, then stop),
# and the sample grid runs up to it, so nothing is hardcoded to a bk2's length.
bk2_frames() {
  python3 - "$1" <<'PY'
import zipfile, sys
try:
    z = zipfile.ZipFile(sys.argv[1])
    print(sum(1 for l in z.read('Input Log.txt').decode('latin-1').splitlines()
              if l.startswith('|')))
except Exception:
    print(0)
PY
}

fail=0 ran=0
while IFS='|' read -r name start interval; do
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  [[ "$name" == *"$FILTER"* ]] || continue
  start="$(echo "$start" | tr -d ' \t')"
  interval="$(echo "$interval" | tr -d ' \t')"; interval="${interval:-100}"
  bk2="games/spyro-1/bk2/${name}.bk2"
  if [[ ! -f "$bk2" ]]; then echo "  SKIP row: no bk2 $bk2"; continue; fi
  fc="$(bk2_frames "$bk2")"
  if ! [[ "$fc" =~ ^[0-9]+$ ]] || [[ "$fc" -le "$start" ]]; then
    echo "  SKIP row $name: bad/short bk2 length ($fc) vs start ($start)"; continue; fi
  # Extend each grid ONE point PAST the movie end (round the last input frame up
  # to the next grid point: canonical %interval, intermediate the +half offset),
  # so the final settled frame after the last input is captured; run to there.
  half=$((interval / 2))
  istart=$((start + half))
  canon_end=$(( start  + interval * ( (fc - start)  / interval + 1 ) ))
  inter_end=$(( istart + interval * ( (fc - istart) / interval + 1 ) ))
  flim=$(( (canon_end > inter_end ? canon_end : inter_end) + 1 ))
  canon="$(seq -s, "$start"  "$interval" "$canon_end")"
  inter="$(seq -s, "$istart" "$interval" "$inter_end")"
  ncanon=$(awk -F, '{print NF}' <<<"$canon"); ninter=$(awk -F, '{print NF}' <<<"$inter")
  # The binary caps ONE --screenshot list at OPENPETE_MAX_SCREENSHOT_FRAMES (64);
  # a longer movie needs a bigger interval (log it, never silently truncate).
  if [[ "$ncanon" -gt 64 || "$ninter" -gt 64 ]]; then
    echo "  FAIL row $name: $ncanon canon / $ninter inter samples exceed the 64-frame"
    echo "        screenshot cap for a $fc-frame movie — raise this row's interval."
    fail=1; continue; fi
  ran=$((ran+1))
  echo "[$name] movie=$fc frame-limit=$flim  canon=${start}/${interval} ($ncanon)  inter=$((start+half))/${interval} ($ninter)"
  run_one "$name" "$flim" canon --screenshot-on-frame "$canon" || fail=1
  run_one "$name" "$flim" inter --screenshot-intermediate-on-frame "$inter" || fail=1
done < "$MANIFEST"

if [[ $ran -eq 0 ]]; then echo "[presentation-corpus] SKIP: no manifest rows matched '$FILTER'"; exit 64; fi
if [[ $fail -ne 0 ]]; then echo "[presentation-corpus] FAIL: missing captures"; exit 1; fi
echo "[presentation-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images across $ran bk2"
exit 0
