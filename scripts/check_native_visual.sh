#!/usr/bin/env bash
# check_native_visual.sh — native Vulkan renderer visual-regression gate (W1.2).
#
# Renders the native `-vk` frame HEADLESS — surfaceless Vulkan through a software
# rasterizer (lavapipe/llvmpipe), no GPU and no display (plan W1.1) — at a curated
# set of (scene, frame) pairs, and compares each against a stored golden with a
# tolerance metric. This is the automated safety net the derive-from-gamestate
# renderer structurally needs: it catches native-renderer regressions and, when a
# transient-state seam is missed, the next undiscovered one.
#
# Native-vs-native: the golden and the candidate are the same native pipeline on
# the same software rasterizer, so the diff is ~0. The tolerance only absorbs any
# minor run-to-run FP/order noise; it never masks a real render change. (For
# native-vs-PsyCross *fidelity* — a different question — capture in `--renderer
# ab`/`diff`, where the native scene is frame-delayed to match PsyCross's lagged
# present; those images live in tests/golden/spyro-1/psycross-ref.)
#
# Usage:
#   scripts/check_native_visual.sh
#     [--build-dir=build/linux-release]
#     [--bless]                     (re)generate the goldens from the current build
#     [--out=/tmp/native-visual]
#     [--diff-threshold=N]          per-pixel max-channel-abs threshold (default 16)
#     [--max-divergent=N]           max # pixels over threshold (default 400)
#
# Exit codes:
#   0  — every scene matches its golden within tolerance
#   1  — at least one scene diverged (a real regression, or re-bless intended)
#   64 — skipped: missing binary / disc / goldens / python+PIL / Vulkan loader
#        (matches the oracle/record convention so ctest SKIP_RETURN_CODE 64 works)

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
OUT_DIR="/tmp/native-visual"
GOLDEN_DIR="tests/golden/spyro-1/native-vk"
BLESS=0
DIFF_THRESHOLD=16
MAX_DIVERGENT=400
# Demo (attract-mode) corpus — the SAME (scene, frame) pairs as the PsyCross
# reference archive (tests/golden/spyro-1/psycross-ref), minus scenes that can't
# be rendered from the scene buffers alone. NOTE: the pause-freeze backdrop
# (freeze-1185) is intentionally excluded — its frozen_tex is captured only in
# the live-present path, which is a no-op headless, so it can't be reproduced by
# the headless screenshot path.
FRAMES="2000,4000,5000,9000,14000,18000,19000"
FRAME_LIMIT=19100

for arg in "$@"; do
    case "$arg" in
        --build-dir=*)      BUILD_DIR="${arg#--build-dir=}" ;;
        --out=*)            OUT_DIR="${arg#--out=}" ;;
        --golden-dir=*)     GOLDEN_DIR="${arg#--golden-dir=}" ;;
        --frames=*)         FRAMES="${arg#--frames=}" ;;
        --frame-limit=*)    FRAME_LIMIT="${arg#--frame-limit=}" ;;
        --bless)            BLESS=1 ;;
        --diff-threshold=*) DIFF_THRESHOLD="${arg#--diff-threshold=}" ;;
        --max-divergent=*)  MAX_DIVERGENT="${arg#--max-divergent=}" ;;
        --help|-h)          sed -n '1,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 64 ;;
    esac
done

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

# --- skip (64) when prerequisites are absent -------------------------------
[[ -x "$BIN"  ]] || { echo "SKIP: missing binary $BIN (build the preset first)" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc $DISC" >&2; exit 64; }
command -v python3 >/dev/null || { echo "SKIP: python3 not on PATH" >&2; exit 64; }
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: Pillow (python3-pil) required" >&2; exit 64; }
# A Vulkan loader must exist for the surfaceless device (real GPU not required —
# a software rasterizer is enough and is what we force below).
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so'; then
    [[ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ]] || {
        echo "SKIP: no Vulkan loader (libvulkan.so.1)" >&2; exit 64; }
fi

# Force the software rasterizer: deterministic, no GPU, no display. Prefer an
# explicit lavapipe ICD if present (bypasses any real ICDs that WSL/CI may also
# expose); otherwise fall back to the Mesa driver override.
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

mkdir -p "$OUT_DIR"
[[ "$BLESS" == 1 ]] && mkdir -p "$GOLDEN_DIR"

# --- capture: one headless run emits every -vk frame -----------------------
LOG="$OUT_DIR/capture.log"
echo "[native-visual] capturing -vk frames {$FRAMES} headless (device: software vk)"
"$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
       --renderer vk \
       --screenshot-on-frame "$FRAMES" \
       --screenshot-output "$OUT_DIR/demo-{n}.png" \
       --frame-limit "$FRAME_LIMIT" \
       --max-runtime-seconds 600 \
       > "$LOG" 2>&1
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "SKIP: capture run exited $rc (see $LOG)" >&2
    tail -5 "$LOG" | sed 's/^/    /' >&2
    exit 64
fi
if ! grep -q "(headless)" "$LOG"; then
    echo "SKIP: native surfaceless device did not come up (see $LOG)" >&2
    tail -5 "$LOG" | sed 's/^/    /' >&2
    exit 64
fi

# --- compare (or bless) each scene -----------------------------------------
IFS=',' read -ra FRAME_ARR <<< "$FRAMES"
declare -i fail=0 checked=0
for f in "${FRAME_ARR[@]}"; do
    cand="$OUT_DIR/demo-${f}-vk.png"
    gold="$GOLDEN_DIR/demo-${f}-vk.png"
    if [[ ! -f "$cand" ]]; then
        echo "  FAIL  frame $f: no -vk capture produced ($cand)"; fail=1; continue
    fi
    if [[ "$BLESS" == 1 ]]; then
        cp -f "$cand" "$gold"; echo "  blessed frame $f -> $gold"; continue
    fi
    if [[ ! -f "$gold" ]]; then
        echo "  SKIP-scene frame $f: no golden ($gold) — run with --bless" >&2
        continue
    fi
    # Golden and candidate are the same native output size; compare full-frame
    # (no overscan trim: both goldens carry identical bands). Derive dims so a
    # future resolution change is a loud comparator error, not a silent crop.
    read -r gw gh < <(python3 -c "from PIL import Image;i=Image.open('$gold');print(i.width,i.height)")
    out="$(python3 scripts/center_crop_diff.py \
            --baseline "$gold" --candidate "$cand" \
            --crop-w "$gw" --crop-h "$gh" \
            --diff-threshold "$DIFF_THRESHOLD" \
            --max-divergent "$MAX_DIVERGENT" \
            --diff-out "$OUT_DIR/diff-${f}.png" 2>&1)"
    if [[ $? -eq 0 ]]; then
        echo "  PASS  frame $f  ($out)"
    else
        echo "  FAIL  frame $f diverged from golden"
        echo "$out" | sed 's/^/        /'
        fail=1
    fi
    checked+=1
done

if [[ "$BLESS" == 1 ]]; then
    echo "[native-visual] bless complete"
    exit 0
fi
if [[ $checked -eq 0 ]]; then
    echo "SKIP: no goldens present — run with --bless to generate them" >&2
    exit 64
fi
exit "$fail"
