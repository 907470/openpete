#!/usr/bin/env bash
# check_aspect_screenshot.sh — pixel-identity gate on the central 4:3 region.
#
# Hor+ widescreen widens the GTE projection symmetrically: extra content
# fills in to the left and right of the canonical PSX 4:3 frustum, while
# the central 4:3 region must be pixel-identical to a 4:3 render of the
# same frame. If a presentation feature (cyclorama matrix scaling, sky
# stretching, FOV leak, etc.) perturbs anything inside the central 4:3
# slice, the test will fail with a non-zero pixel diff.
#
# Method:
#   1. Pin --window so all aspects share an identical 4:3 sub-grid:
#      4:3   →  960x720
#      16:9  → 1280x720
#      21:9  → 1680x720
#      Any AR W:H → window width = 720 * W/H (must divide evenly).
#      Window height 720 = 3× PSX 240, so host:PSX is exactly 3.0
#      regardless of AR (avoids sub-pixel drift between aspects).
#   2. Take screenshot at frame N at each aspect.
#   3. Drop top/bottom 24 rows of PSX overscan, center-crop to 960x672
#      and pixel-diff against the 4:3 baseline.
#
# Currently RED: the cyclorama uses an aspect-aware matrix scaling
# during the render pass that perturbs the central 4:3 column too.
# Closing that channel will turn this gate green.
#
# Usage:
#   scripts/check_aspect_screenshot.sh
#     [--build-dir=build/linux-release]
#     [--frame=N]                  (default: 100)
#     [--aspects=4:3,16:9,21:9]
#     [--baseline=4:3]
#     [--out=/tmp/aspect-screenshot]
#     [--tolerance=0]              max per-channel abs diff that still passes
#
# Exit codes:
#   0 — every aspect's central 4:3 region matches the baseline within tolerance
#   1 — at least one aspect diverged
#   2 — setup error (missing binary / disc / python deps)

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
FRAME=850
ASPECTS_CSV="4:3,16:9,21:9"
BASELINE="4:3"
OUT_DIR="/tmp/aspect-screenshot"
TOLERANCE=0

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
        --frame=*)     FRAME="${arg#--frame=}" ;;
        --aspects=*)   ASPECTS_CSV="${arg#--aspects=}" ;;
        --baseline=*)  BASELINE="${arg#--baseline=}" ;;
        --out=*)       OUT_DIR="${arg#--out=}" ;;
        --tolerance=*) TOLERANCE="${arg#--tolerance=}" ;;
        --help|-h)     sed -n '1,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "missing binary: $BIN (run cmake --build --preset linux-release)" >&2; exit 2; }
[[ -f "$DISC" ]] || { echo "missing disc: $DISC" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not on PATH" >&2; exit 2; }
python3 -c 'import PIL' 2>/dev/null || { echo "Pillow (python3-pil) required" >&2; exit 2; }

mkdir -p "$OUT_DIR"
IFS=',' read -ra ASPECTS <<< "$ASPECTS_CSV"

# Window width derivation: with window height pinned to 480, set
# window width = 480 * AR_w / AR_h. That keeps host-pixels-per-PSX-pixel
# at exactly 2.0 horizontally for *every* aspect (Hor+ widens the GL
# ortho extent by AR/(4/3), matching the wider window proportionally).
# The central 640 host pixels are then the canonical 4:3 sub-region.
window_w_for() {
    local label="$1"
    local w h
    w="${label%:*}"; h="${label#*:}"
    # Window height fixed at 720 ⇒ host:PSX = 3.0 vertically. Width
    # = 720 * W/H. We require an even integer (any AR with H not
    # dividing 720*W must be approximated, breaking pixel identity).
    python3 -c "
w,h=$w,$h
v=720.0*w/h
iv=int(round(v))
if abs(iv-v) > 1e-9 or iv%2:
    import sys; sys.exit('aspect '+'$label'+' does not produce even integer pixel grid at 720p')
print(iv)
"
}

run_one() {
    local label="$1"
    local aspect_arg=""
    [[ "$label" != "4:3" ]] && aspect_arg="$label"
    local win_w; win_w="$(window_w_for "$label")"
    local out_png="$OUT_DIR/shot-$label-${FRAME}.png"
    local log="$OUT_DIR/log-$label.txt"
    rm -f "$out_png"
    local args=(--headless --disc "$DISC" --run-recomp 0x80012204
                --frame-limit "$((FRAME + 5))"
                --window "${win_w}x720"
                --screenshot-on-frame "$FRAME"
                --screenshot-output "$out_png"
                --max-runtime-seconds 600)
    [[ -n "$aspect_arg" ]] && args+=(--aspect "$aspect_arg")
    "$BIN" "${args[@]}" > "$log" 2>&1
    local rc=$?
    [[ $rc -eq 0 && -f "$out_png" ]] || {
        echo "FAIL: $label run exited $rc (see $log)" >&2; return 1; }
    echo "$out_png"
}

echo "[shot-cmp] frame=$FRAME baseline=$BASELINE tolerance=$TOLERANCE"
declare -A SHOT
for a in "${ASPECTS[@]}"; do
    p="$(run_one "$a")" || exit 1
    SHOT["$a"]="$p"
    echo "  captured $a -> $p"
done

declare -i fail=0
for a in "${ASPECTS[@]}"; do
    [[ "$a" == "$BASELINE" ]] && continue
    out="$(python3 scripts/center_crop_diff.py \
            --baseline "${SHOT[$BASELINE]}" \
            --candidate "${SHOT[$a]}" \
            --crop-w 960 --crop-h 672 \
            --overscan-top 24 --overscan-bot 24 \
            --tolerance "$TOLERANCE" \
            --diff-out "$OUT_DIR/diff-$BASELINE-vs-$a.png" 2>&1)"
    rc=$?
    if [[ $rc -eq 0 ]]; then
        echo "  PASS  $a central 4:3 matches $BASELINE  ($out)"
    else
        echo "  FAIL  $a central 4:3 diverges from $BASELINE"
        echo "$out" | sed 's/^/        /'
        fail=1
    fi
done

exit "$fail"
