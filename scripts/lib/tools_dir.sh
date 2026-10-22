# tools_dir.sh - locating the fetched-tools directory (.tools/).
#
# Sourced by the scripts that fetch or bundle pinned downloads (the Linux
# clang, appimagetool and its runtime). .tools/ is gitignored and lives in
# the main checkout; a linked worktree has none of its own, so a run from a
# worktree resolves the main checkout's through the common git directory.
# OPENPETE_TOOLS_DIR overrides the lookup. Where git cannot answer (inside
# the release container, whose /src/.git of a worktree points outside the
# mount), the repository's own .tools/ is used; the container mounts the
# resolved directory there.

# openpete_tools_dir <repo dir> -> prints the absolute .tools/ path (which
# may not exist yet).
openpete_tools_dir() {
    local repo="$1" common
    if [[ -n "${OPENPETE_TOOLS_DIR:-}" ]]; then
        realpath -m -- "$OPENPETE_TOOLS_DIR"
        return 0
    fi
    common=$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
    if [[ -n "$common" && -f "$(dirname -- "$common")/CMakePresets.json" ]]; then
        echo "$(dirname -- "$common")/.tools"
    else
        echo "$(realpath -m -- "$repo")/.tools"
    fi
}
