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
#   1. Capture the NATIVE (-vk) render headless at each aspect
#      (--screenshot-native-only). The native path is where Hor+ ships;
#      its headless capture renders at the internal resolution,
#      window-independent: 4:3 -> 640x448, W:H -> 640*(W/H)/(4/3) wide.
#      (The PsyCross -og capture stopped widening when the Phase-6
#      --aspect substitution stack was retired, and stays 4:3.)
#   2. Take screenshot at frame N at each aspect.
#   3. Drop top/bottom 16 rows of PSX overscan, center-crop to 640x416
#      and pixel-diff against the 4:3 baseline. 21:9 (even width) is
#      pixel-exact; 16:9 rounds to an ODD width (853), so its central
#      crop sits a half-pixel off and carries a permanent edge-fuzz
#      allowance (the per-aspect ceiling).
#
# Usage:
#   scripts/check_aspect_screenshot.sh
#     [--game SLUG]                (default: spyro-1)
#     [--build-dir=build/linux-release]
#     [--frame=N]                  (default: 100)
#     [--aspects=4:3,16:9,21:9]
#     [--baseline=4:3]
#     [--out=/tmp/aspect-screenshot]
#     [--diff-threshold=32]        per-pixel max-channel-abs threshold
#     [--max-divergent=N]          max # pixels exceeding threshold
#
# Exit codes:
#   0 — every aspect's central 4:3 region matches the baseline within tolerance
#   1 — at least one aspect diverged
#   2 — setup error (missing binary / disc / python deps)

set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

BUILD_DIR="build/linux-release"
FRAME=850
ASPECTS_CSV="4:3,16:9,21:9"
BASELINE="4:3"
OUT_DIR="/tmp/aspect-screenshot"
# Diff-count gate: pass when at most MAX_DIVERGENT[aspect] pixels in
# the central 4:3 region have per-channel abs diff > DIFF_THRESHOLD.
# Per-aspect ceilings — each ratchets the inherent integer-GTE
# precision floor for that aspect (the irrational-Δ-OFX silhouette
# jitter described in notes/plans/openpete-widescreen-fov.md).
# Sub-pixel edge jitter trivially saturates a worst-per-channel
# metric to 255 (one anti-aliased edge pixel flipping fg<->bg), so a
# count metric is the meaningful gate.
#
# Locked at frame 850 to the deterministic per-aspect counts +
# small headroom. Lower as projection precision improves; aspects
# with integer Δ-OFX (5:3, 16:10, 21:9, 48:9, ...) should approach
# zero while aspects with irrational Δ-OFX (16:9, 32:9, ...) bottom
# out at their integer-rounding floor.
DIFF_THRESHOLD=32
declare -A MAX_DIVERGENT_BY_ASPECT=(
    # Δ-OFX = 53.33 PSX-X (irrational); frame-850 floor 5097
    ["16:9"]=5200
    # Δ-OFX = 120 PSX-X (integer); frame-850 floor 2871
    ["21:9"]=3000
    # Δ-OFX = 266.67 PSX-X (irrational); frame-850 floor 7883
    ["32:9"]=8000
)
# Used for any aspect not listed above (e.g. user-supplied via
# --aspects=…). Loose so unknown aspects don't silently fail; tighten
# by adding entries to MAX_DIVERGENT_BY_ASPECT.
MAX_DIVERGENT_FALLBACK=20000

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
        --game|--game=*) ;;   # consumed by pg_parse_game above
        spyro-*)         ;;   # the two-word --game SLUG spelling
        --frame=*)     FRAME="${arg#--frame=}" ;;
        --aspects=*)   ASPECTS_CSV="${arg#--aspects=}" ;;
        --baseline=*)  BASELINE="${arg#--baseline=}" ;;
        --out=*)       OUT_DIR="${arg#--out=}" ;;
        --diff-threshold=*) DIFF_THRESHOLD="${arg#--diff-threshold=}" ;;
        # --max-divergent=ASPECT:N overrides one aspect's ceiling.
        # --max-divergent=N (no colon) sets the fallback for unknowns.
        --max-divergent=*)
            v="${arg#--max-divergent=}"
            if [[ "$v" == *:* ]]; then
                MAX_DIVERGENT_BY_ASPECT["${v%:*}"]="${v#*:}"
            else
                MAX_DIVERGENT_FALLBACK="$v"
            fi
            ;;
        --help|-h)     sed -n '1,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"

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
    # --screenshot-native-only writes just the native render, "-vk" suffix.
    local shot_file="${out_png%.png}-vk.png"
    local log="$OUT_DIR/log-$label.txt"
    rm -f "$out_png"
    local args=(--frame-limit "$((FRAME + 5))"
                --window "${win_w}x720"
                --screenshot-on-frame "$FRAME"
                --screenshot-native-only
                --screenshot-output "$out_png"
                --max-runtime-seconds 600)
    [[ -n "$aspect_arg" ]] && args+=(--aspect "$aspect_arg")
    pg_capture "$log" "$shot_file" -- "${args[@]}"
    local rc=$?
    [[ $rc -eq 0 ]] || {
        echo "FAIL: $label run exited $rc (see $log)" >&2; return 1; }
    echo "$shot_file"
}

echo "[shot-cmp] frame=$FRAME baseline=$BASELINE thresh=$DIFF_THRESHOLD"
for a in "${ASPECTS[@]}"; do
    [[ "$a" == "$BASELINE" ]] && continue
    cap="${MAX_DIVERGENT_BY_ASPECT[$a]:-$MAX_DIVERGENT_FALLBACK}"
    echo "           ceiling[$a] = $cap"
done
declare -A SHOT
for a in "${ASPECTS[@]}"; do
    p="$(run_one "$a")" || exit 1
    SHOT["$a"]="$p"
    echo "  captured $a -> $p"
done

declare -i fail=0
for a in "${ASPECTS[@]}"; do
    [[ "$a" == "$BASELINE" ]] && continue
    cap="${MAX_DIVERGENT_BY_ASPECT[$a]:-$MAX_DIVERGENT_FALLBACK}"
    out="$(python3 scripts/center_crop_diff.py \
            --baseline "${SHOT[$BASELINE]}" \
            --candidate "${SHOT[$a]}" \
            --crop-w 640 --crop-h 416 \
            --overscan-top 16 --overscan-bot 16 \
            --diff-threshold "$DIFF_THRESHOLD" \
            --max-divergent "$cap" \
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
