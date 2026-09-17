#!/bin/bash
# P0 field census across all levels: one boot per level (sectors decode only at
# a render, so the census needs a present — the screenshot forces one).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
S="${FIELD_CENSUS_CACHE:-$REPO/build/field-census}"; mkdir -p "$S"; S="$(cd "$S" && pwd)"
BIN=$REPO/build/linux-release/games/spyro-1/openpete-spyro1
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe

LEVELS="artisans stone_hill dark_hollow town_square toasty sunny_flight \
peace_keepers dry_canyon cliff_town ice_cavern doctor_shemp night_flight \
magic_crafters alpine_ridge high_caves wizard_peak blowhard crystal_flight \
beast_makers terrace_village misty_bog tree_tops metalhead wild_flight \
dream_weavers dark_passage lofty_castle haunted_towers jacques icy_flight \
gnorc_gnexus gnorc_cove twilight_harbor gnasty_gnorc gnastys_loot"

JOBS=${JOBS:-6}
for lvl in $LEVELS; do
  [ -s "$S/$lvl.log" ] && grep -q "fc-hdr" "$S/$lvl.log" && continue
  (
    fr=2101; [ "$lvl" = icy_flight ] && fr=4001   # flight-level load oddity
    cd $REPO && OPENPETE_TERRAIN_FIELDCENSUS=1 \
    $BIN --headless --disc $DISC --disc-cache 0 --renderer vk \
      --mute 1 --aspect 4:3 --skip-to-level $lvl --lod original \
      --render-paths terrain --screenshot-native-only \
      --screenshot-on-frame $fr --screenshot-output $S/$lvl-{n}.png \
      --frame-limit $((fr+10)) --max-runtime-seconds 380 \
      > /dev/null 2> $S/$lvl.log
    echo "$lvl: $(grep -c fc-lpf $S/$lvl.log) lp faces, $(grep -c fc-hpf $S/$lvl.log) hp faces"
  ) &
  while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do wait -n; done
done
wait
echo census-complete
