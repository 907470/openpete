#!/usr/bin/env bash
# check_tracer_subtick.sh — sub-tick interpolation of the tracer channel, the
# tracer path of the shared feeder harness. See check_feeder_subtick.sh for the
# instrument, the manifest form and the pass criterion.
exec "$(dirname "$0")/check_feeder_subtick.sh" --path tracer "$@"
