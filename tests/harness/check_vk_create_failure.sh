#!/usr/bin/env bash
# check_vk_create_failure.sh - a Vulkan device creation that fails before
# vkCreateDevice tears the partial device down without a fault and the run
# continues on the documented fallback.
#
# Run: one short run per leg.
#   instance  --headless with a native screenshot armed, Vulkan loader
#             pointed at a missing ICD (VK_DRIVER_FILES, VK_ICD_FILENAMES):
#             vkCreateInstance fails on every machine.
#   surface   windowed under SDL's offscreen video driver
#             (SDL_VIDEO_DRIVER=offscreen): SDL_Vulkan_CreateSurface fails
#             where the ICD lacks VK_EXT_headless_surface. Where the ICD
#             offers it the surface exists and the leg reports SKIP.
#
# Instrument: the run's exit status and the log lines below.
#
# Pass criterion, per leg (a leg whose engagement line is missing fails,
# except the surface leg's SKIP):
#   instance  engaged `device creation failed: The Vulkan instance could not
#             be created`; `rhi_create_headless failed` logged; rc 0 (the
#             run continues without the native capture).
#   surface   engaged `device creation failed: The window's Vulkan surface
#             could not be created: ` followed by SDL's reason;
#             `no Vulkan device:` and `continuing on PsyCross's GL window`
#             logged; rc 0 with `openpete-exit: reason=frame-limit`.
#   Both: rc below 128 (no signal).
#
# Verdict line per leg:
#   [vk-create-failure] leg=<instance|surface> rc=<n> verdict=PASS|FAIL|SKIP: <why>
#
# Runtime: about 10 s.
#
# Usage: tests/harness/check_vk_create_failure.sh [--build-dir=DIR] [--game=G]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME=spyro-1
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --game=*) GAME="${a#--game=}";;
    esac
done
pg_resolve "$GAME"

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "vk-create-failure: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/vk-create-failure/$GAME"
mkdir -p "$OUT"
rm -f "${OUT:?}"/instance.log "${OUT:?}"/surface.log "${OUT:?}"/instance__50-vk.png \
    "${OUT:?}"/instance__50-og.png
fail=0

# verdict LEG RC WHY: print the leg's line; an empty WHY is a PASS.
verdict() {
    if [[ -z "$3" ]]; then
        echo "[vk-create-failure] leg=$1 rc=$2 verdict=PASS"
    else
        echo "[vk-create-failure] leg=$1 rc=$2 verdict=FAIL: $3"
        fail=1
    fi
}

# --- instance: no ICD, headless native capture ----------------------------
log="$OUT/instance.log"
env OPENPETE_VIBRATION=0 VK_DRIVER_FILES=/nonexistent-openpete-icd.json \
    VK_ICD_FILENAMES=/nonexistent-openpete-icd.json \
    timeout -s KILL 120 "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
    --disc-cache 0 "${PG_RUN_ARGS[@]}" --renderer vk --mute 1 --no-card \
    --vibration 0 --screenshot-on-frame 50 \
    --screenshot-output "$OUT/instance__{n}.png" --frame-limit 60 \
    >"$log" 2>&1 </dev/null
rc=$?
why=""
grep -qF 'device creation failed: The Vulkan instance could not be created' "$log" ||
    why="no instance-failure engagement line"
(( rc >= 128 )) && why="killed by signal $(( rc - 128 ))"
[[ -z "$why" ]] && ! grep -qF 'rhi_create_headless failed' "$log" && why="no headless fallback line"
[[ -z "$why" && "$rc" != 0 ]] && why="rc $rc, want 0"
verdict instance "$rc" "$why"

# --- surface: offscreen video driver, windowed ----------------------------
log="$OUT/surface.log"
env OPENPETE_VIBRATION=0 OPENPETE_MSGBOX_SKIP=1 SDL_VIDEO_DRIVER=offscreen \
    timeout -s KILL 120 "$PG_BIN" --no-config --disc "$PG_DISC" \
    --disc-cache 0 "${PG_RUN_ARGS[@]}" --renderer vk --mute 1 --no-card \
    --vibration 0 --input-lock --frame-limit 200 \
    >"$log" 2>&1 </dev/null
rc=$?
if (( rc < 128 )) && ! grep -qF 'SDL_Vulkan_CreateSurface failed' "$log"; then
    echo "[vk-create-failure] leg=surface rc=$rc verdict=SKIP: the ICD offers VK_EXT_headless_surface"
else
    why=""
    grep -qF "device creation failed: The window's Vulkan surface could not be created: " "$log" ||
        why="no surface-failure engagement line"
    (( rc >= 128 )) && why="killed by signal $(( rc - 128 ))"
    [[ -z "$why" ]] && ! grep -qF "continuing on PsyCross's GL window" "$log" && why="no GL fallback line"
    [[ -z "$why" && "$rc" != 0 ]] && why="rc $rc, want 0"
    [[ -z "$why" ]] && ! grep -q 'openpete-exit: reason=frame-limit' "$log" && why="no frame-limit exit line"
    verdict surface "$rc" "$why"
fi

(( fail )) && { echo "vk-create-failure: FAIL"; exit 1; }
echo "vk-create-failure: PASS"
exit 0
