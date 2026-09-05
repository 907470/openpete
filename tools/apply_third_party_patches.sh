#!/usr/bin/env bash
# tools/apply_third_party_patches.sh — re-apply local patches to vendored
# git submodules. Run once after `git submodule update --init --recursive`
# or any time a submodule pin changes.
#
# Each patch is resolved through three checks, in order:
#
#   1. patches/<sub>/APPLIED_AT maps the patch to the submodule commit that
#      introduced it. If that commit is an ancestor of the submodule HEAD the
#      patch is already in the pinned history and is skipped. This is the only
#      check that survives context drift: a later commit editing the same
#      region leaves the change present but makes the patch text unapplicable
#      in both directions.
#   2. `git apply --check` succeeds -> the patch is missing from the pin and
#      is applied to the work tree.
#   3. `git apply --reverse --check` succeeds -> already applied, skip.
#
# A patch that fails all three is a genuinely unpatched pin (or a conflict)
# and aborts the script with a non-zero exit naming the patch.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# Commit recorded for this patch in patches/<sub>/APPLIED_AT, or empty.
recorded_commit() {
    local ledger="$1" patch_name="$2"
    [[ -f "$ledger" ]] || return 0
    awk -v name="$patch_name" '
        /^[[:space:]]*(#|$)/ { next }
        $1 == name { print $2; exit }
    ' "$ledger"
}

apply_patches() {
    local sub="$1"
    local patches_dir="$ROOT/patches/$sub/PATCHES"
    local ledger="$ROOT/patches/$sub/APPLIED_AT"
    local sub_dir="$ROOT/third_party/$sub"

    if [[ ! -d "$patches_dir" ]]; then
        echo "no patches for $sub"
        return
    fi
    if [[ ! -d "$sub_dir/.git" && ! -f "$sub_dir/.git" ]]; then
        echo "skip: $sub not initialised (run \`git submodule update --init\`)"
        return
    fi

    pushd "$sub_dir" > /dev/null
    local failed=()
    for p in "$patches_dir"/*.patch; do
        [[ -e "$p" ]] || continue
        local name commit
        name=$(basename "$p")
        commit=$(recorded_commit "$ledger" "$name")

        if [[ -n "$commit" ]] \
           && git cat-file -e "$commit^{commit}" 2>/dev/null \
           && git merge-base --is-ancestor "$commit" HEAD 2>/dev/null; then
            echo "in history $sub: $name (${commit:0:9})"
        elif git apply --check "$p" 2>/dev/null; then
            echo "applying $sub: $name"
            git apply "$p"
        elif git apply --reverse --check "$p" 2>/dev/null; then
            echo "already applied $sub: $name"
        else
            echo "FAIL $sub: $name does not apply, is not reverse-applicable," \
                 "and its APPLIED_AT commit is absent from the pinned history" >&2
            failed+=("$name")
        fi
    done
    popd > /dev/null

    if (( ${#failed[@]} )); then
        echo "error: $sub is unpatched for: ${failed[*]}" >&2
        echo "the submodule pin predates these patches, or the work tree conflicts with them" >&2
        return 1
    fi
}

apply_patches PsyCross
