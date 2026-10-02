#!/usr/bin/env bash
# check_portal_subtick.sh - sub-tick interpolation of the Spyro 2 and Spyro 3
# homeworld portal preview's aperture, the portal path of the shared feeder
# harness. See check_feeder_subtick.sh for the instrument, the manifest form
# and the pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path portal "$@"
