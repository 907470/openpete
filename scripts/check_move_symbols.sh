#!/usr/bin/env bash

# scripts/check_move_symbols.sh — the move-completeness gate for a
# reorganization commit. A TU dropped from a CMake source list while its
# file moves still builds, tests and renders identically when nothing links
# its symbols; the only trace is the symbol set of the built artifacts. This
# script records the multiset of defined symbols (nm --defined-only, statics
# included) across the core library, the recomp module objects and the game
# exe, and diffs a later build against the record after the declared renames
# are applied.
#
# Usage:
#   scripts/check_move_symbols.sh record                 # before the arc
#   scripts/check_move_symbols.sh check [renames-file]   # after each content commit
#   scripts/check_move_symbols.sh accept [renames-file]  # roll the declared renames into the record
#
# renames-file: one `old new` pair per line, `#` comments. A rename applies
# to the symbol and to its compiler clones (`old.part.0`, `old.constprop.1`).
# `check` fails when the post-rename record differs from the built symbols
# by any symbol (a dropped TU, a missed or undeclared rename), and when a
# renames file declares a rename that changed nothing. `accept` runs
# `check` and rewrites the record so the next commit diffs against it.
#
# Env: OPENPETE_BUILD_DIR (default build/linux-release), OPENPETE_GAME
# (default spyro-1), OPENPETE_MOVE_SYMBOLS_RECORD (default
# $OPENPETE_BUILD_DIR/move_symbols.record).
#
# Exit: 0 clean, 1 usage or missing artifact, 3 the symbol sets differ,
# 8 a declared rename engaged nothing.

set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=${OPENPETE_BUILD_DIR:-build/linux-release}
GAME=${OPENPETE_GAME:-spyro-1}
RECORD=${OPENPETE_MOVE_SYMBOLS_RECORD:-$BUILD_DIR/move_symbols.record}
TAG=${GAME//-/}

CORE_LIB=$BUILD_DIR/core/libopenpete_core.a
EXE=$BUILD_DIR/games/$GAME/openpete-$TAG
MODULE_DIR=$BUILD_DIR/games/$GAME/CMakeFiles/${TAG}_recomp_module.dir

usage() { sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# A symbol whose name embeds __LINE__ (the OP_PROF_* per-site cache,
# `op_prof_aid_<line>`, `op_prof_id_<line>`) renames on every comment edit
# above it; the line is dropped so only the site count is compared. The GCC
# local-symbol suffix (`.<n>`) is dropped with it.
normalize() {
    sed -E 's/ (op_prof_a?id_)[0-9]+(\.[0-9]+)?$/ \1/'
}

symbols() {
    for f in "$CORE_LIB" "$EXE"; do
        [ -e "$f" ] || { echo "check_move_symbols: missing $f (build first)" >&2; exit 1; }
    done
    {
        nm --defined-only "$CORE_LIB" | awk 'NF==3 {print "core " $3}'
        nm --defined-only "$EXE"      | awk 'NF==3 {print "exe " $3}'
        if [ -d "$MODULE_DIR" ]; then
            find "$MODULE_DIR" -name '*.o' -print0 | sort -z | xargs -0 nm --defined-only \
                | awk 'NF==3 {print "module " $3}'
        fi
    } | normalize | LC_ALL=C sort
}

# apply_renames FILE: stdin is a symbol list; each `old new` pair rewrites
# `<scope> old` and `<scope> old.<clone>` to the new name.
apply_renames() {
    local renames=$1
    if [ -z "$renames" ]; then cat; return; fi
    awk -v renames="$renames" '
        BEGIN {
            while ((getline line < renames) > 0) {
                sub(/#.*/, "", line)
                if (split(line, f) == 2) map[f[1]] = f[2]
            }
        }
        {
            name = $2; clone = ""
            i = index(name, ".")
            if (i > 0) { clone = substr(name, i); name = substr(name, 1, i - 1) }
            if (name in map) $2 = map[name] clone
            print
        }' | LC_ALL=C sort
}

declared_renames() {
    local renames=$1
    [ -n "$renames" ] || return 0
    sed 's/#.*//' "$renames" | awk 'NF==2'
}

cmd=${1:-}
case "$cmd" in
record)
    mkdir -p "$(dirname "$RECORD")"
    symbols > "$RECORD"
    echo "check_move_symbols: recorded $(wc -l < "$RECORD") symbols to $RECORD"
    ;;
check|accept)
    renames=${2:-}
    [ -e "$RECORD" ] || { echo "check_move_symbols: no record at $RECORD (run record first)" >&2; exit 1; }
    if [ -n "$renames" ] && [ ! -e "$renames" ]; then
        echo "check_move_symbols: renames file $renames not found" >&2; exit 1
    fi
    now=$(mktemp); expected=$(mktemp); trap 'rm -f "$now" "$expected"' EXIT
    symbols > "$now"
    apply_renames "$renames" < "$RECORD" > "$expected"
    n_renames=$(declared_renames "$renames" | wc -l)
    if [ "$n_renames" -gt 0 ]; then
        # Engagement: every declared old name must have been in the
        # record and every new name must be in the build.
        while read -r old new; do
            if ! grep -q " $old\$\| $old\." "$RECORD"; then
                echo "check_move_symbols: declared rename $old -> $new: $old is not in the record" >&2
                exit 8
            fi
            if ! grep -q " $new\$\| $new\." "$now"; then
                echo "check_move_symbols: declared rename $old -> $new: $new is not in the build" >&2
                exit 8
            fi
        done < <(declared_renames "$renames")
    fi
    if ! diff_out=$(diff "$expected" "$now"); then
        echo "check_move_symbols: FAIL — the built symbol set differs from the record after $n_renames declared rename(s):" >&2
        echo "$diff_out" | grep '^[<>]' | sed 's/^</  missing:/; s/^>/  extra:  /' >&2
        exit 3
    fi
    echo "check_move_symbols: PASS — $(wc -l < "$now") symbols match the record after $n_renames declared rename(s)"
    if [ "$cmd" = accept ]; then
        cp "$now" "$RECORD"
        echo "check_move_symbols: record updated"
    fi
    ;;
*)
    usage
    ;;
esac
