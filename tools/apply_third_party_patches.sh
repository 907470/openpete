#!/usr/bin/env bash
# tools/apply_third_party_patches.sh — re-apply local patches to vendored
# git submodules. Run once after `git submodule update --init --recursive`
# or any time a submodule pin changes.
#
# Idempotency: each patch is checked with `git apply --check` before
# applying; if it would fail-clean, we assume already-applied and skip.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

apply_patches() {
    local sub="$1"
    local patches_dir="$ROOT/patches/$sub/PATCHES"
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
    for p in "$patches_dir"/*.patch; do
        [[ -e "$p" ]] || continue
        if git apply --check "$p" 2>/dev/null; then
            echo "applying $sub: $(basename "$p")"
            git apply "$p"
        else
            echo "skip $sub: $(basename "$p") (already applied or conflicts)"
        fi
    done
    popd > /dev/null
}

apply_patches PsyCross
