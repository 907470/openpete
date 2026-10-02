# mingw.sh - locating a Linux-hosted mingw cross compiler.
#
# Sourced by the scripts that cross-compile Windows artifacts on a Linux
# development box (mod DLLs, SDK compile checks). A Linux-hosted llvm-mingw
# tree carries an extensionless driver; the tree the distribution bundles for
# Windows carries only .exe drivers, which this host cannot run.

# mingw_driver_in <toolchain dir> -> prints the driver path, or nothing.
mingw_driver_in() {
    local t="$1" d
    for d in x86_64-w64-mingw32-clang x86_64-w64-mingw32-gcc; do
        [[ -x "$t/bin/$d" ]] && { echo "$t/bin/$d"; return 0; }
    done
    return 1
}

# mingw_find_cc <search root> -> prints the driver path, or nothing.
# A Linux-hosted llvm-mingw* tree under <search root>/toolchains wins over
# x86_64-w64-mingw32-gcc on PATH.
mingw_find_cc() {
    local root="$1" t cc
    for t in "$root"/toolchains/llvm-mingw*; do
        [[ -d "$t" ]] || continue
        cc=$(mingw_driver_in "$t") && { echo "$cc"; return 0; }
    done
    cc=$(command -v x86_64-w64-mingw32-gcc) || return 1
    echo "$cc"
}

# mingw_spyro_root <repo dir> -> the directory the toolchains live beside.
# The toolchains sit beside the main checkout, so a run from a linked worktree
# must resolve the parent of the main worktree, not of the worktree it runs
# in. The common git dir is the main checkout's .git in both cases.
mingw_spyro_root() {
    local repo="$1" common
    common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
    if [[ -n "${common:-}" ]]; then
        dirname -- "$(dirname -- "$common")"
    else
        dirname -- "$repo"
    fi
}
