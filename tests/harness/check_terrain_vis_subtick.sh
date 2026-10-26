#!/usr/bin/env bash
# check_terrain_vis_subtick.sh - the sector set a sub-tick present draws under
# --terrain-visibility game, the terrain-vis path of the shared feeder harness.
# See check_feeder_subtick.sh for the instrument, the manifest form and the
# pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path terrain-vis "$@"
