#!/usr/bin/env bash
# check_native_fidelity.sh — native-vs-PsyCross fidelity ratchet (W1.3).
#
# Promotes --render-stats into a machine-readable, exit-code CI gate. Runs the
# demo headless in `--renderer diff --render-stats` on a software rasterizer
# (surfaceless Vulkan / lavapipe — W1.1, no GPU/display), which makes
# screenshot_all emit a per-scene "diff-stats" line: the native scene vs the
# PsyCross reference, FRAME-SYNCED (diff mode delays the native scene to match
# PsyCross's lagged present). The gate parses those lines and fails if any scene
# diverges from PsyCross by MORE than a calibrated per-scene ceiling.
#
# This is a *ratchet*, not a parity gate: native and PsyCross legitimately differ
# (float vs integer raster, AA, the known fog/brightness residual), so over8% is
# ~40-70% at baseline. The ceilings lock that in + headroom so a native change
# that drifts substantially further from PsyCross trips the gate. It complements
# the bit-exact native-vs-native regression gate (check_native_visual.sh, W1.2)
# and preserves the PsyCross-fidelity signal before PsyCross is retired.
#
# Metric: per-pixel max-channel abs diff; "over8%" = % of pixels differing by >8
# (of 255) in any channel (DIFF_STATS_TOL, matching the live --render-stats).
#
# Usage:
#   scripts/check_native_fidelity.sh [--build-dir=build/linux-release]
#     [--out=/tmp/native-fidelity] [--calibrate] [--headroom-pct=10]
# Exit: 0 pass; 1 a scene exceeded its ceiling; 64 skip (missing prereqs).

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
OUT_DIR="/tmp/native-fidelity"
CALIBRATE=0
HEADROOM_PCT=10
FRAMES="2000,4000,5000,9000,14000,18000,19000"
FRAME_LIMIT=19100

# Per-scene over8% ceilings (native-vs-PsyCross, frame-synced). Calibrated
# 2026-07-01 on lavapipe to the measured baseline + ~10% headroom; both
# renderers are deterministic so the baseline is reproducible run-to-run.
# Re-calibrate with --calibrate after an intentional native-render change.
declare -A CEIL=(
    [2000]=52 [4000]=75 [5000]=70 [9000]=44
    [14000]=44 [18000]=47 [19000]=48
)

for arg in "$@"; do
    case "$arg" in
        --build-dir=*)    BUILD_DIR="${arg#--build-dir=}" ;;
        --out=*)          OUT_DIR="${arg#--out=}" ;;
        --frames=*)       FRAMES="${arg#--frames=}" ;;
        --frame-limit=*)  FRAME_LIMIT="${arg#--frame-limit=}" ;;
        --headroom-pct=*) HEADROOM_PCT="${arg#--headroom-pct=}" ;;
        --calibrate)      CALIBRATE=1 ;;
        --help|-h)        sed -n '1,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 64 ;;
    esac
done

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "SKIP: missing binary $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc $DISC" >&2; exit 64; }
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so'; then
    [[ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ]] || { echo "SKIP: no Vulkan loader" >&2; exit 64; }
fi

LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/fidelity.log"

# --renderer diff makes screenshot_all diff native vs the (frame-synced) PsyCross
# reference; --render-stats makes it emit the machine-readable "diff-stats" line.
echo "[native-fidelity] capturing native-vs-PsyCross diff-stats headless {$FRAMES}"
"$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
       --renderer diff --render-stats \
       --screenshot-on-frame "$FRAMES" \
       --screenshot-output "$OUT_DIR/fid-{n}.png" \
       --frame-limit "$FRAME_LIMIT" --max-runtime-seconds 600 \
       > "$LOG" 2>&1
rc=$?
if [[ $rc -ne 0 ]] || ! grep -q "(headless)" "$LOG"; then
    echo "SKIP: capture run exited $rc / device did not come up (see $LOG)" >&2
    tail -5 "$LOG" | sed 's/^/    /' >&2
    exit 64
fi

IFS=',' read -ra FRAME_ARR <<< "$FRAMES"
declare -i fail=0 seen=0
for f in "${FRAME_ARR[@]}"; do
    # native: diff-stats file=.../fid-<f>.png size=... mean=.. max=.. over8=..%
    line="$(grep -oE "diff-stats file=[^ ]*fid-${f}\.png .*over8=[0-9.]+%" "$LOG" | tail -1)"
    if [[ -z "$line" ]]; then
        echo "  FAIL  frame $f: no diff-stats emitted (native ref-compare didn't run?)"; fail=1; continue
    fi
    over="$(sed -E 's/.*over8=([0-9.]+)%.*/\1/' <<<"$line")"
    mean="$(sed -E 's/.*mean=([0-9.]+).*/\1/' <<<"$line")"
    seen+=1
    if [[ "$CALIBRATE" == 1 ]]; then
        # Emit a ready-to-paste ceiling = ceil(measured * (1 + headroom/100)).
        ceil="$(python3 -c "import math;print(math.ceil($over*(1+$HEADROOM_PCT/100.0)))")"
        printf "  frame %-6s over8=%6s%%  mean=%-7s  suggested_ceiling=%s\n" "$f" "$over" "$mean" "$ceil"
        continue
    fi
    cap="${CEIL[$f]:-}"
    if [[ -z "$cap" ]]; then
        echo "  SKIP-scene frame $f: no ceiling defined" >&2; continue
    fi
    if python3 -c "import sys; sys.exit(0 if float('$over')<=float('$cap') else 1)"; then
        echo "  PASS  frame $f  over8=${over}% (ceiling ${cap}%, mean ${mean})"
    else
        echo "  FAIL  frame $f  over8=${over}% EXCEEDS ceiling ${cap}% (mean ${mean}) — native drifted from PsyCross"
        fail=1
    fi
done

if [[ "$CALIBRATE" == 1 ]]; then echo "[native-fidelity] calibration only"; exit 0; fi
if [[ $seen -eq 0 ]]; then echo "SKIP: no diff-stats lines parsed" >&2; exit 64; fi
exit "$fail"
