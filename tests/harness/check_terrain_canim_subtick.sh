#!/usr/bin/env bash
# check_terrain_canim_subtick.sh - sub-tick interpolation of the terrain env-anim colour
# channel, the terrain-canim path of the shared feeder harness. See check_feeder_subtick.sh
# for the instrument, the manifest form and the pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path terrain-canim "$@"
