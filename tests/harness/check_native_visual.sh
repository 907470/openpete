#!/usr/bin/env bash
# check_native_visual.sh - native Vulkan renderer visual-regression gate.
#
# Renders the native `-vk` frame HEADLESS - surfaceless Vulkan through the
# software rasterizer (lavapipe/llvmpipe), no GPU and no display - at seven
# attract-demo draw-ticks, and compares each tile's verdict digest (decoded RGB
# payload, mode and size; scripts/presentation-blessed.py) against its row of
# games/spyro-1/native-tick-corpus.manifest. The capture is byte-deterministic
# on the pinned rasterizer, so the compare is exact: any divergence is a
# native-renderer change. A divergent row's reference image is read from the
# local store for its heat diff (tests/harness/presentation-corpus-diff.py).
#
# The verdict is defined over the pinned capture image
# (docs/linux-capture.Dockerfile). Run outside it, the script re-runs itself
# inside the image through scripts/golden-capture-container.sh, which builds
# the engine into build/linux-release-container with the image's toolchain;
# without a container engine or a current image it reports SKIP (64).
#
# Re-bless (an intended native-render change, adjudicated first): promote the
# kept candidate inside the image, which rewrites the changed rows, files the
# tiles into the store and records the capture environment in the header:
#   scripts/golden-capture-container.sh run -- \
#     python3 scripts/presentation-blessed.py promote spyro-1 --tier native-vk \
#     build/native-visual-candidate --all
#
# Usage:
#   tests/harness/check_native_visual.sh
#     [--game SLUG]                 (spyro-1; the manifest is Spyro 1's)
#     [--build-dir=build/linux-release]
#     [--out=build/native-visual-candidate]  candidate directory, kept for a promote
#     [--ticks=T1,T2,...]           a subset of the rows (compared as a subset)
#
# Exit codes:
#   0  - every captured tick matches its row
#   1  - a tick diverged, a row was not captured, or the manifest is absent
#   64 - skipped: outside the capture image without podman/docker or the image,
#        or a missing binary / disc / python+PIL / Vulkan loader
#        (the oracle/record convention, so ctest SKIP_RETURN_CODE 64 works)

set -uo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

BUILD_DIR="build/linux-release"
OUT_DIR="build/native-visual-candidate"
# Attract-demo scenes keyed on CANONICAL DRAW-TICKS (--screenshot-on-tick), not
# VBLs: the VBL-to-tick mapping drifts with host timing (CD-load VSync spins,
# the audio backend), while the sim is tick-deterministic. Scenes: the title
# screen, two views of a grass level, a canyon level, a dusk level, a
# dark-green level and a water scene. The pause-freeze backdrop is not a row:
# its frozen_tex is captured only in the live-present path, a no-op headless.
TICKS="1001,2194,2694,4888,7581,9775,10275"
ALL_TICKS="$TICKS"
# Runaway bound only (VBL units, so it needs headroom over the last tick's
# nominal VBL ~19000 - a drifted mapping must not clip the capture).
FRAME_LIMIT=21000

for arg in "$@"; do
    case "$arg" in
        --build-dir=*)      BUILD_DIR="${arg#--build-dir=}" ;;
        --out=*)            OUT_DIR="${arg#--out=}" ;;
        --ticks=*)          TICKS="${arg#--ticks=}" ;;
        --frame-limit=*)    FRAME_LIMIT="${arg#--frame-limit=}" ;;
        --game|--game=*)    ;;   # consumed by pg_parse_game above
        spyro-*)            ;;   # the two-word --game SLUG spelling
        --help|-h)          sed -n '1,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 64 ;;
    esac
done

MANIFEST="games/spyro-1/native-tick-corpus.manifest"
[[ "$GAME" == "spyro-1" ]] || { echo "SKIP: no native-tick manifest for $GAME" >&2; exit 64; }

if ! pg_in_capture_image; then
    pg_enter_capture_image tests/harness/check_native_visual.sh "$@"
    exit $?
fi

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"

# --- skip (64) when prerequisites are absent -------------------------------
[[ -x "$BIN"  ]] || { echo "SKIP: missing binary $BIN (build the preset first)" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc $DISC" >&2; exit 64; }
command -v python3 >/dev/null || { echo "SKIP: python3 not on PATH" >&2; exit 64; }
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: Pillow (python3-pil) required" >&2; exit 64; }
# A Vulkan loader must exist for the surfaceless device (real GPU not required -
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

# The candidate directory is emptied first, so no tile, record or device line
# of an earlier run can be read as this run's.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
python3 scripts/presentation-blessed.py capture-record spyro-1 --tier native-vk \
    "$OUT_DIR" --build-dir "$BUILD_DIR" \
    || { echo "FAIL: capture record not written" >&2; exit 1; }

# --- capture: one headless run emits every -vk frame -----------------------
LOG="$OUT_DIR/capture.log"
echo "[native-visual] capturing -vk ticks {$TICKS} headless (device: software vk)"
# The screenshot names carry the tick, so the outputs are asserted per tick
# below rather than by name here.
pg_capture "$LOG" "" -- \
       --renderer vk \
       --screenshot-on-tick "$TICKS" \
       --screenshot-output "$OUT_DIR/demo-tick-{n}.png" \
       --frame-limit "$FRAME_LIMIT" \
       --max-runtime-seconds 600
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
pg_note_device "$LOG" "$OUT_DIR"

# Every requested tick must have produced its tile (a partial capture would
# otherwise read as a subset); the -og reference PsyCross also writes is not a
# row and is removed.
declare -i missing=0
IFS=',' read -ra TICK_ARR <<< "$TICKS"
for f in "${TICK_ARR[@]}"; do
    [[ -f "$OUT_DIR/demo-tick-${f}-vk.png" ]] || {
        echo "  FAIL  tick $f: no -vk capture produced"; missing=1; }
done
rm -f "$OUT_DIR"/*-og.png
(( missing )) && exit 1

# An absent manifest is refused by the diff after the capture, so the kept
# candidate can seed it (promote --create).
[[ -f "$MANIFEST" ]] && python3 scripts/presentation-blessed.py environment spyro-1 --tier native-vk --candidate "$OUT_DIR"
SUBSET_ARG=()
[[ "$TICKS" != "$ALL_TICKS" ]] && SUBSET_ARG=(--expect-subset)
python3 tests/harness/presentation-corpus-diff.py "$OUT_DIR" --game spyro-1 \
    --manifest "$MANIFEST" --diff-out "$OUT_DIR/diff" "${SUBSET_ARG[@]}"
exit $?
