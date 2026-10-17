#!/usr/bin/env bash
#
# swrender.sh - pin a scripted run's GL and Vulkan to Mesa's CPU rasterizers.
# SOURCED, not executed.
#
# Usage:
#   source scripts/lib/swrender.sh
#   sw_render_env
#
# Exports, for every child the caller starts afterwards:
#   VK_ICD_FILENAMES        the lavapipe ICD, when installed: the Vulkan loader
#                           then enumerates no other device.
#   LIBGL_ALWAYS_SOFTWARE=1 the Mesa GL loader skips hardware DRI drivers (a
#                           host with a /dev/dri render node would otherwise
#                           load radeonsi, iris, ...) and loads swrast_dri.so.
#   GALLIUM_DRIVER=llvmpipe swrast_dri.so carries several software-winsys
#                           drivers and picks d3d12 when /dev/dxg exists (WSL),
#                           which renders on the host GPU; this selects
#                           llvmpipe on every host.
# MESA_LOADER_DRIVER_OVERRIDE is unset: Mesa ships no llvmpipe_dri.so, so the
# value llvmpipe fails to load and the loader falls back to swrast_dri.so with
# its default driver choice.
#
# A caller-set GALLIUM_DRIVER is kept (a deliberate driver A/B passes its own);
# VK_ICD_FILENAMES is always replaced, since the presentation golden digests
# pin one Vulkan rasterizer.

# sw_render_env: export the software-rasterizer selection described above.
sw_render_env() {
    local lvp="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
    if [[ -e "$lvp" ]]; then
        export VK_ICD_FILENAMES="$lvp"
    fi
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER="${GALLIUM_DRIVER:-llvmpipe}"
    unset MESA_LOADER_DRIVER_OVERRIDE
}
