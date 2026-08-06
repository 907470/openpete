#!/bin/bash
# Terrain face-set join gate: for each scene, one native run emits BOTH the
# .s prim capture (OPENPETE_OT_DUMP_FRAMES — terrain-filtered because the walk
# filter runs before the dump row under --render-paths terrain) and the native
# face universe + verdict trail (OPENPETE_TERRAIN_FARDBG + [fardbg-drop]);
# terrain-face-join.py then reports the disagreement buckets.
#
# Expected-clean baseline (see the terrain-LOD-fidelity plan):
#   native-missing  = r=nclip (integer-SXY NCLIP vs float, screen-edge and
#                     degenerate horizon slivers) + single-digit r=hpdrop /
#                     r=admit threshold flips (float m3 vs GTE SZ3)
#   ONSCREEN extra  = the same NCLIP quantisation class in reverse
#   offscreen extra = .s clip-code trivial-reject; native GPU-clips
#   subdivband      = og subdivides faces with 4*sum(SZ3) < 0x2000; native
#                     draws whole (near/HQ path covers its own subset)
#   LP/back extra   = native either-half acceptance vs .s per-half verdict
# Anything OUTSIDE those buckets is a model regression — investigate before
# shipping terrain LOD changes.
#
# Usage: scripts/terrain-face-join.sh            # default 9-scene sweep
#        SCENES="artisans:2101" FORCE=1 scripts/terrain-face-join.sh
set -u
cd "$(dirname "$0")/.."
BIN=build/linux-release/games/spyro-1/openpete-spyro1
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
CACHE="${FACE_JOIN_CACHE:-build/face-join-cache}"
mkdir -p "$CACHE"
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.x86_64.json}"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

SCENES="${SCENES:-artisans:2101 stone_hill:2101 dark_hollow:2101 toasty:2101 dream_weavers:2101 haunted_towers:2101 gnorc_cove:2101 blowhard:2101 cliff_town:2101}"

for sc in $SCENES; do
  lvl=${sc%%:*}; fr=${sc##*:}
  ot=$CACHE/$lvl-ot.txt; log=$CACHE/$lvl-fardbg.log
  if [ ! -s "$ot" ] || [ ! -s "$log" ] || [ "${FORCE:-0}" = 1 ]; then
    OPENPETE_TERRAIN_FARDBG=$fr OPENPETE_OT_DUMP_FRAMES=$fr-$fr \
    OPENPETE_OT_DUMP_PATH=$ot \
    $BIN --headless --disc $DISC --run-recomp 0x80012204 --renderer vk \
      --mute 1 --aspect 4:3 --skip-to-level $lvl --lod original \
      --render-paths terrain --screenshot-native-only \
      --screenshot-on-frame $fr --screenshot-output $CACHE/$lvl-{n}.png \
      --frame-limit $((fr+10)) --max-runtime-seconds 380 \
      > /dev/null 2> $log
  fi
  echo "== $lvl =="
  python3 scripts/terrain-face-join.py --ot $ot --fardbg $log --frame $fr
done
