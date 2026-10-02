#!/usr/bin/env bash
# check_glint_rot_subtick.sh - sub-tick spin of the gem glint, the glint-rot
# path of the shared feeder harness. See check_feeder_subtick.sh for the
# instrument, the manifest form and the pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path glint-rot "$@"
