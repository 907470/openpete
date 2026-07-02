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
# Demo (attract-mode) corpus — the SAME sim moments as the PsyCross reference
# archive (tests/golden/spyro-1/psycross-ref), minus scenes that can't be
# rendered from the scene buffers alone, but keyed on CANONICAL DRAW-TICKS
# (--screenshot-on-tick), not VBL frames: the frame↔tick mapping drifts with
# host timing (CD-load VSync spins, audio backend), which made frame-keyed
# goldens fork at session-dependent points (the 2026-07-01 gate flake) while
# the sim itself stayed tick-deterministic. Tick↔frame mapping at capture
# (2026-07-02): 1001↔2000, 2194↔4000, 2694↔5000, 4888↔9000, 7581↔14000,
# 9775↔18000, 10275↔19000 — i.e. these ticks are the psycross-ref scenes.
# NOTE: the pause-freeze backdrop (freeze-1185) is intentionally excluded —
# its frozen_tex is captured only in the live-present path, which is a no-op
# headless, so it can't be reproduced by the headless screenshot path.
TICKS="1001,2194,2694,4888,7581,9775,10275"
# Runaway bound only (frame/VBL units, so it needs headroom over the last
# tick's nominal frame ~19000 — a drifted mapping must not clip the capture).
FRAME_LIMIT=21000

for arg in "$@"; do
    case "$arg" in
        --build-dir=*)      BUILD_DIR="${arg#--build-dir=}" ;;
        --out=*)            OUT_DIR="${arg#--out=}" ;;
        --golden-dir=*)     GOLDEN_DIR="${arg#--golden-dir=}" ;;
        --ticks=*)          TICKS="${arg#--ticks=}" ;;
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
echo "[native-visual] capturing -vk ticks {$TICKS} headless (device: software vk)"
"$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
       --renderer vk \
       --screenshot-on-tick "$TICKS" \
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
IFS=',' read -ra TICK_ARR <<< "$TICKS"
declare -i fail=0 checked=0
for f in "${TICK_ARR[@]}"; do
    cand="$OUT_DIR/demo-${f}-vk.png"
    gold="$GOLDEN_DIR/demo-tick-${f}-vk.png"
    if [[ ! -f "$cand" ]]; then
        echo "  FAIL  tick $f: no -vk capture produced ($cand)"; fail=1; continue
    fi
    if [[ "$BLESS" == 1 ]]; then
        cp -f "$cand" "$gold"; echo "  blessed tick $f -> $gold"; continue
    fi
    if [[ ! -f "$gold" ]]; then
        echo "  SKIP-scene tick $f: no golden ($gold) — run with --bless" >&2
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
        echo "  PASS  tick $f  ($out)"
    else
        echo "  FAIL  tick $f diverged from golden"
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
