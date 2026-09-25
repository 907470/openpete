#!/usr/bin/env bash
# check_vk_validation.sh — Vulkan validation errors on the boot-to-level path.
#
# The Vulkan validation layer reports API misuse the driver is free to tolerate:
# a fragment shader reading a varying its vertex stage never wrote, an object
# still alive at vkDestroyDevice, a barrier naming the wrong layout. None of it
# changes a pixel on the driver this runs against, so no image golden and no
# gameplay oracle can see it, and a defect of the class survives every other
# gate in the tree.
#
# One headless run per game under --renderer vk with the layer loaded, from
# boot through a level load to a native screenshot. The screenshot is the
# capture request that makes the native renderer draw a real level headless:
# pipelines the terrain, moby and 2D feeders create only after a level load are
# built, used and destroyed inside the run, so the shutdown path is measured
# with them alive. Objects created only by a path this run never takes (mod
# pipelines, the retained terrain cache under --terrain-cache) need their own
# row.
#
# Engagement, both required, else VACUOUS exit 3:
#   layer=on  the engine printed `validation layer enabled`, so the layer is
#             loaded into the instance and its messages reach the log. Without
#             it every run reads zero errors and every build passes.
#   the screenshot exists, so the native render path ran to a present.
# msgs= counts the layer's messages of every severity, warnings included, and
# is reported rather than bounded: a run may legitimately draw no warning.
#
# Criterion: `Validation Error` lines = 0. Any is a FAIL, with the first three
# quoted.
#
# Verdict line per game:
#   [vk-validation] game=<g> level=<l> vbl=<n> layer=on|off msgs=<n>
#                   errors=<n> verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_vk_validation.sh [--game SLUG] [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# The level each game loads and the VBL the screenshot is taken at. The level
# is the game's first, the one every other row of the presentation corpus
# reaches through; the VBL is the freecam corpus's post-load default, by which
# --skip-to-level has finished loading and the level is drawing.
case "$GAME" in
    spyro-1) LEVEL=artisans;       VBL=1900;;
    spyro-2) LEVEL=summer_forest;  VBL=1900;;
    spyro-3) LEVEL=sunrise_spring; VBL=1900;;
    *) echo "vk-validation: SKIP ($GAME has no level row)"; exit 64;;
esac

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "vk-validation: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/vk-validation/$GAME"
mkdir -p "$OUT"
LOG="$OUT/$GAME.log"
SHOT="$OUT/boot__$VBL-vk.png"

# pg_capture deletes the log and the named output before the run and asserts
# the output after it, so a stale screenshot cannot stand in for this run's
#.
PG_CAPTURE_TIMEOUT=600
rc=0
pg_capture "$LOG" "$SHOT" -- \
    --renderer vk --mute 1 --aspect 4:3 --lod original \
    --skip-to-level "$LEVEL" --screenshot-native-only \
    --screenshot-on-frame "$VBL" --screenshot-output "$OUT/boot__{n}.png" \
    --frame-limit $((VBL + 10)) </dev/null || rc=$?

if [[ ! -s "$LOG" ]]; then
    echo "[vk-validation] game=$GAME level=$LEVEL vbl=$VBL verdict=VACUOUS: the run wrote no log (rc=$rc)"
    exit 3
fi

layer=off
grep -q '^\[rhi/vk\] validation layer enabled' "$LOG" && layer=on
msgs=$(grep -c '^\[rhi/vk\] validation: ' "$LOG")
errors=$(grep -c 'Validation Error' "$LOG")

head="[vk-validation] game=$GAME level=$LEVEL vbl=$VBL layer=$layer msgs=$msgs errors=$errors"

if [[ "$layer" != on ]]; then
    echo "$head verdict=VACUOUS: the validation layer did not load, so no build can fail this check"
    echo "[vk-validation]   install VK_LAYER_KHRONOS_validation (vulkan-validationlayers)"
    exit 3
fi
if [[ ! -f "$SHOT" ]]; then
    echo "$head verdict=VACUOUS: no screenshot at VBL $VBL, so the native render path did not run (rc=$rc); see $LOG"
    exit 3
fi

echo "$head verdict=$( ((errors)) && echo FAIL || echo PASS)"
if ((errors)); then
    grep 'Validation Error' "$LOG" | head -3 | sed 's/^/[vk-validation]   /'
    echo "[vk-validation]   full log: $LOG"
    exit 1
fi
exit 0
