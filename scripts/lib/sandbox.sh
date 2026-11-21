#!/usr/bin/env bash
#
# sandbox.sh - point every game run of a harness at a scratch root.
# SOURCED, not executed; sourcing it does the work.
#
# Under OPENPETE_SANDBOX=<root> the game resolves its config, state and data
# directories to <root>/config, <root>/state and <root>/data on every
# platform (core/config/paths.c), so a run writes no log, hang file, bug
# bundle, pipeline cache, card, savestate, screenshot, recording or settings
# save into the player's directories, and reads no player config. The game
# refuses to start (exit 66) when a write path it resolves or is handed lies
# in a real per-user location or beside the executable.
#
# Sourcing exports OPENPETE_SANDBOX as a fresh directory owned by this shell,
# <parent>/<pid>.<random>, the parent being $OPENPETE_SANDBOX_PARENT, else
# $TMPDIR/openpete-harness-sandbox. A nested harness whose OPENPETE_SANDBOX
# already names a live directory under the parent keeps it. The root is
# removed on exit when the sourcing script has no EXIT trap yet; a script
# that installs its own trap afterwards calls op_sandbox_cleanup from it.
# Every source also removes the roots whose owning process is gone.
#
# Each root's state/openpete/cache is a link to one shared directory,
# $OPENPETE_SANDBOX_CACHE, else $XDG_CACHE_HOME/openpete-harness/cache
# (~/.cache/openpete-harness/cache), so the Vulkan pipeline cache stays warm
# across runs. The link is a Linux symbolic link: a Windows exe given a root
# on a Windows drive (OPENPETE_SANDBOX_PARENT=/mnt/c/...) gets a plain
# directory there instead and runs with a cold pipeline cache.
#
# OPENPETE_CARD_DIR is exported as <root>/cards for the harnesses that make
# per-run card directories inside it (scripts/lib/game.sh pg_capture).
#
# A binary that predates OPENPETE_SANDBOX (a bisect checkout, a kept playtest
# build) resolves its per-user directories from XDG_CONFIG_HOME,
# XDG_STATE_HOME and XDG_DATA_HOME, else $HOME. op_contain CMD... runs one
# launch with those set to <root>/xdg/{config,state,data} of the current
# OPENPETE_SANDBOX (its pipeline cache linked to the shared one), and
# op_sandbox_xdg ROOT fills OP_SANDBOX_XDG_ENV with the same assignments for
# an `env` line; pg_capture (scripts/lib/game.sh) adds them to every run.
# They lie apart from <root>/{config,state,data} because a sandboxed build
# refuses a write path inside the directories its XDG variables name. They
# are set per launch, never exported: ccache, git, podman and the harness's
# own stores (the golden store, the bench results) read the same variables.
# Windows ignores XDG; a pre-sandbox Windows exe still writes %APPDATA% and
# %LOCALAPPDATA%.
#
# op_sandbox_run_root prints a fresh per-run root under OPENPETE_SANDBOX
# (with the cache link), for a harness that isolates its runs from each
# other: `OPENPETE_SANDBOX="$(op_sandbox_run_root)" "$BIN" ...`.
#
# A Windows exe receives the variable through scripts/lib/wslenv.sh (as a
# Windows path). scripts/fences/sandbox_guard.py refuses a launcher that does
# not source this file (directly or through a library that does).
#
# Timing window: sourcing this file and each op_sandbox_run_root call wait
# while another job holds the timing window (scripts/lib/timing_gate.sh), so
# a sandboxed launch never starts beside a timing leg; a harness that takes a
# run root per launch is gated per launch, one that launches from the shared
# root only when it starts.

# shellcheck source=scripts/lib/timing_gate.sh
source "$(dirname "${BASH_SOURCE[0]}")/timing_gate.sh"

# op_sandbox_cache_dir: the shared pipeline-cache directory, created.
op_sandbox_cache_dir() {
    local c="${OPENPETE_SANDBOX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/openpete-harness/cache}"
    mkdir -p "$c" && echo "$c"
}

# op_sandbox_link_cache ROOT: make ROOT/state/openpete and link its cache to
# the shared directory; on a Windows drive (/mnt/<letter>/) make a directory.
op_sandbox_link_cache() {
    local root="$1" cache
    mkdir -p "$root/state/openpete" || return 1
    [[ -e "$root/state/openpete/cache" ]] && return 0
    if [[ "$root" == /mnt/[a-z]/* ]]; then
        mkdir -p "$root/state/openpete/cache"
        return
    fi
    cache="$(op_sandbox_cache_dir)" || return 1
    ln -s "$cache" "$root/state/openpete/cache"
}

# op_sandbox_xdg ROOT: set OP_SANDBOX_XDG_ENV to the XDG config, state and
# data assignments under ROOT/xdg, made, with ROOT/xdg/state/openpete/cache
# linked to the shared pipeline cache.
op_sandbox_xdg() {
    local x="$1/xdg"
    op_sandbox_link_cache "$x" || return 1
    mkdir -p "$x/config" "$x/data" || return 1
    OP_SANDBOX_XDG_ENV=("XDG_CONFIG_HOME=$x/config" "XDG_STATE_HOME=$x/state" "XDG_DATA_HOME=$x/data")
}

# op_contain CMD [ARG...]: run CMD with the XDG bases under the current
# OPENPETE_SANDBOX (op_sandbox_xdg); returns its status.
op_contain() {
    op_sandbox_xdg "${OPENPETE_SANDBOX:?}" || return 1
    env "${OP_SANDBOX_XDG_ENV[@]}" "$@"
}

op_sandbox() {
    local parent="${OPENPETE_SANDBOX_PARENT:-${TMPDIR:-/tmp}/openpete-harness-sandbox}"
    case "${OPENPETE_SANDBOX:-}" in
        "$parent"/*)
            if [[ -d "$OPENPETE_SANDBOX" ]]; then
                export OPENPETE_SANDBOX
                export OPENPETE_CARD_DIR="${OPENPETE_CARD_DIR:-$OPENPETE_SANDBOX/cards}"
                mkdir -p "$OPENPETE_CARD_DIR"
                return 0
            fi ;;
    esac
    mkdir -p "$parent" || return 1
    local d pid
    for d in "$parent"/*; do
        [[ -d "$d" ]] || continue
        pid="${d##*/}"; pid="${pid%%.*}"
        [[ "$pid" =~ ^[0-9]+$ && ! -d "/proc/$pid" ]] && rm -rf "${d:?}"
    done
    OPENPETE_SANDBOX="$(mktemp -d "$parent/$$.XXXXXX")" || return 1
    export OPENPETE_SANDBOX
    OP_SANDBOX_ROOT="$OPENPETE_SANDBOX"
    op_sandbox_link_cache "$OPENPETE_SANDBOX" || return 1
    export OPENPETE_CARD_DIR="$OPENPETE_SANDBOX/cards"
    mkdir -p "$OPENPETE_CARD_DIR" || return 1
    [[ -z "$(trap -p EXIT)" ]] && trap op_sandbox_cleanup EXIT
    return 0
}

# op_sandbox_run_root: print a fresh per-run root under OPENPETE_SANDBOX,
# with the shared cache linked in and an empty cards directory.
op_sandbox_run_root() {
    local r
    op_timing_gate >&2 || return 1
    r="$(mktemp -d "${OPENPETE_SANDBOX:?}/run.XXXXXX")" || return 1
    op_sandbox_link_cache "$r" || return 1
    mkdir -p "$r/cards" || return 1
    echo "$r"
}

# Remove the root this shell made, if it made one; the cache link goes with
# it, the shared cache stays.
op_sandbox_cleanup() {
    [[ -n "${OP_SANDBOX_ROOT:-}" ]] && rm -rf "${OP_SANDBOX_ROOT:?}"
    return 0
}

op_timing_gate || {
    echo "sandbox: a timing window is open; not launching" >&2
    exit 75
}
op_sandbox || {
    echo "sandbox: cannot make a sandbox root under ${OPENPETE_SANDBOX_PARENT:-${TMPDIR:-/tmp}/openpete-harness-sandbox}" >&2
    exit 1
}
