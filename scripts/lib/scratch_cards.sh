#!/usr/bin/env bash
#
# scratch_cards.sh - point every game run of a harness at scratch memory
# cards. SOURCED, not executed; sourcing it does the work.
#
# A run given no card flag opens card1.mcr and card2.mcr in the directory
# core/host/memcard.c resolves: --card-dir, else OPENPETE_CARD_DIR, else the
# per-user state directory (~/.local/state/openpete/cards). The last is the
# player's own cards: a harness run there reads whatever an earlier run or
# the player left (a save, a modset sidecar) and writes into it. Sourcing
# this file exports OPENPETE_CARD_DIR as a fresh empty directory owned by
# this shell, so every run it starts, and every child script, boots on two
# freshly formatted cards. A run that wants a card keeps it: --card1, --card2
# and --card-dir beat the variable, and --no-card removes both cards.
#
# The directory lives under $TMPDIR/openpete-harness-cards, named
# <pid>.<random> after the shell that made it. A nested harness whose
# OPENPETE_CARD_DIR already names a live directory there keeps it. The
# directory is removed on exit when the sourcing script has no EXIT trap yet;
# a script that installs its own trap afterwards replaces that removal unless
# the trap calls op_scratch_cards_cleanup, so every source also deletes the
# directories whose owning process is gone.

op_scratch_cards() {
    local parent="${TMPDIR:-/tmp}/openpete-harness-cards"
    case "${OPENPETE_CARD_DIR:-}" in
        "$parent"/*) [[ -d "$OPENPETE_CARD_DIR" ]] && return 0 ;;
    esac
    mkdir -p "$parent" || return 1
    local d pid
    for d in "$parent"/*; do
        [[ -d "$d" ]] || continue
        pid="${d##*/}"; pid="${pid%%.*}"
        [[ "$pid" =~ ^[0-9]+$ && ! -d "/proc/$pid" ]] && rm -rf "${d:?}"
    done
    OPENPETE_CARD_DIR="$(mktemp -d "$parent/$$.XXXXXX")" || return 1
    export OPENPETE_CARD_DIR
    OP_SCRATCH_CARD_DIR="$OPENPETE_CARD_DIR"
    [[ -z "$(trap -p EXIT)" ]] && trap op_scratch_cards_cleanup EXIT
    return 0
}

# Remove the scratch card directory this shell made, if it made one. A
# script that installs its own EXIT trap calls this from it.
op_scratch_cards_cleanup() {
    [[ -n "${OP_SCRATCH_CARD_DIR:-}" ]] && rm -rf "${OP_SCRATCH_CARD_DIR:?}"
    return 0
}

op_scratch_cards || {
    echo "scratch_cards: cannot make a scratch card directory under ${TMPDIR:-/tmp}" >&2
    exit 1
}
