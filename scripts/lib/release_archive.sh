# scripts/lib/release_archive.sh - keep every package this machine produces,
# tagged or not, so a bug report names the exact artifact its tester ran.
#
# release_archive_store <version> <commit> <artifact>...
#
# Copies each artifact into
#   ${XDG_DATA_HOME:-~/.local/share}/openpete/release-archive/<version>-<commit>/
# (OPENPETE_RELEASE_ARCHIVE_DIR names the root instead) and appends to two
# files there, both in sha256sum's "<hex>  <name>" form:
#   SHA256SUMS      the artifact's own SHA-256;
#   EXE-SHA256SUMS  the SHA-256 of the executable inside it, named
#                   "<artifact>:<path inside>", read from the artifact's
#                   <artifact>.exe.sha256 sidecar that package-dist.sh writes
#                   (the hash a bug report's manifest.json records as
#                   exe_sha256, since the writer hashes the running binary).
# A line already present is not repeated. Nothing is uploaded.
# Sourced by package-dist.sh (outside a container) and release-container.sh
# (on the host, after the container exits).

release_archive_root() {
    if [[ -n "${OPENPETE_RELEASE_ARCHIVE_DIR:-}" ]]; then
        echo "$OPENPETE_RELEASE_ARCHIVE_DIR"
    else
        echo "${XDG_DATA_HOME:-$HOME/.local/share}/openpete/release-archive"
    fi
}

release_archive_line() {  # release_archive_line <file> <line> - append once
    touch "$1"
    grep -qxF -- "$2" "$1" || echo "$2" >> "$1"
}

release_archive_store() {  # release_archive_store <version> <commit> <artifact>...
    local version="$1" commit="$2"; shift 2
    local dir
    dir="$(release_archive_root)/${version}-${commit}"
    mkdir -p "$dir"
    local a name sha
    for a in "$@"; do
        [[ -f "$a" ]] || { echo "[archive] no artifact $a" >&2; return 1; }
        name=$(basename "$a")
        cp -f "$a" "$dir/$name"
        sha=$(sha256sum "$dir/$name" | cut -d' ' -f1)
        release_archive_line "$dir/SHA256SUMS" "$sha  $name"
        if [[ -f "$a.exe.sha256" ]]; then
            local exe_sha inner
            read -r exe_sha inner < "$a.exe.sha256"
            release_archive_line "$dir/EXE-SHA256SUMS" "$exe_sha  $name:$inner"
        else
            echo "[archive] NOTE: $a has no .exe.sha256 sidecar; EXE-SHA256SUMS lacks it" >&2
        fi
        echo "[archive] $name -> $dir"
    done
}
