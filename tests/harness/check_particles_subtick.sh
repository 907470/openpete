#!/usr/bin/env bash
# check_particles_subtick.sh — sub-tick interpolation of the particles channel, the
# particles path of the shared feeder harness. See check_feeder_subtick.sh for the
# instrument, the manifest form and the pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path particles "$@"
