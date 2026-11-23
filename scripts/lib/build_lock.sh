#!/usr/bin/env bash
#
# build_lock.sh - hold a build directory shared while a gate reads it.
# SOURCED, not executed.
#
# op_build_read_lock PATH [LABEL] waits while a build holds the build
# directory of PATH (a binary, or the directory itself), then holds it
# shared until this shell ($$) exits, so no build replaces the binary
# during the gate and no gate starts on a binary a build is replacing.
# Returns 0 at once when PATH lies outside a build directory or this
# shell's process tree already holds it. Call it after any build the
# harness itself runs into that directory: a build under the hold refuses.
# See scripts/lib/build_lock.py.

op_build_read_lock() {
    python3 "$(dirname "${BASH_SOURCE[0]}")/build_lock.py" read --for-pid "$$" \
        --path "$1" --label "${2:-${0##*/}}"
}
