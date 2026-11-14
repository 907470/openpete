#!/usr/bin/env bash
# mesa_capture.sh - the driver ISA dump of the terrain pipelines, run on a Mesa
# Vulkan host (copied there and started by isa_record.sh).
#
# Usage: mesa_capture.sh <root> <spv-dir> <out-dir>
#   root     the host's run directory: <root>/bin/openpete (+ spyro1-recomp.so),
#            <root>/lib (shared libraries for LD_LIBRARY_PATH),
#            <root>/disc1/SCUS-94228.cue
# Environment:
#   DRVDBG  the driver dump knobs, required: RADV
#           "RADV_DEBUG=shaders,shaderstats,nocache", ANV "INTEL_DEBUG=vs";
#           further KEY=VALUE words are passed to the run as well
#   NICE    a prefix for the run, e.g. "nice -n 19" (default none)
#   ARMS    a subset of "gpl gplaff mono" (default all)
#
# Boots Spyro 1 headless once per arm with the terrain .spv of <spv-dir>
# (OPENPETE_SHADER_DIR) and one compile worker (OPENPETE_COMPILE_POOL=1), so
# each `pipe-create` log line precedes the dumps of its compile:
#   gpl     GPL libraries + LTO links (default), AFFINE_UV=0
#   gplaff  the same with --texture-mapping affine (AFFINE_UV=1)
#   mono    OPENPETE_PIPELINE_LIBRARY=0, monolithic pipelines
# Writes <out-dir>/drv-<arm>.err (the dump) and .out, both deleted first; the
# run's own files (log, pipeline cache) go to <out-dir>/sandbox-<arm>
# (OPENPETE_SANDBOX).
set -u
DRVDBG=${DRVDBG:?set DRVDBG}
NICE=${NICE:-}
ROOT=${1:?root}; SPV=${2:?spv dir}; OUT=${3:?out dir}
B="$ROOT/bin"; D="$ROOT/disc1/SCUS-94228.cue"
mkdir -p "$OUT"
read -r -a drv <<<"$DRVDBG"
read -r -a nice <<<"$NICE"
run() {
    local tag=$1 extra=$2; shift 2
    local -a ex=()
    [[ -n "$extra" ]] && read -r -a ex <<<"$extra"
    rm -f "${OUT:?}/drv-$tag.err" "${OUT:?}/drv-$tag.out" "${OUT:?}/shot-$tag-"*.png
    env "${drv[@]}" SDL_VIDEO_DRIVER=offscreen OPENPETE_VIBRATION=0 OPENPETE_COMPILE_POOL=1 \
        OPENPETE_SANDBOX="$OUT/sandbox-$tag" \
        OPENPETE_SHADER_DIR="$SPV" MESA_SHADER_CACHE_DISABLE=true \
        LD_LIBRARY_PATH="$ROOT/lib" "$@" \
        "${nice[@]}" timeout 1200 "$B/openpete" --game spyro-1 --headless --no-config --disc "$D" \
        --renderer vk --aspect 4:3 --lod original --no-card --mute 1 --vibration 0 \
        --screenshot-on-frame 300 --screenshot-output "$OUT/shot-$tag-{n}.png" --frame-limit 310 \
        "${ex[@]}" > "$OUT/drv-$tag.out" 2> "$OUT/drv-$tag.err"
    echo "$tag rc=$? terrain-creates=$(grep -a -c 'pipe-create.*terrain' "$OUT/drv-$tag.err")"
    test -s "$OUT/drv-$tag.err" || echo "$tag: no dump"
}
for a in ${ARMS:-gpl gplaff mono}; do
    case $a in
        gpl)    run gpl "" ;;
        gplaff) run gplaff "--texture-mapping affine" ;;
        mono)   run mono "" OPENPETE_PIPELINE_LIBRARY=0 ;;
        *)      echo "mesa_capture: unknown arm $a" >&2; exit 64 ;;
    esac
    grep -a -m1 "pipeline features" "$OUT/drv-$a.err"
done
