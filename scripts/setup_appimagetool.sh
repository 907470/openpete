#!/usr/bin/env bash
# scripts/setup_appimagetool.sh - fetch the pinned appimagetool used to
# build the Linux AppImage.
#
# The tool is a release artifact, not source we build: pin it by version
# AND sha256 so a release is reproducible and a compromised upstream
# download fails loudly instead of packaging silently.
#
# Lands in .tools/ (gitignored, like the other fetched dev tools).
# package-dist.sh picks it up from there, or from $OPENPETE_APPIMAGETOOL.
#
# Idempotent - re-running with the tool already present verifies and exits.
# --check runs only that verification: exit 0 when both files are current,
# 1 (naming the file) when one is missing or stale, with no download.
#
# .tools/ is the main checkout's, also from a linked worktree
# (scripts/lib/tools_dir.sh; OPENPETE_TOOLS_DIR overrides).
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/tools_dir.sh
source scripts/lib/tools_dir.sh
TOOLS=$(openpete_tools_dir .)
CHECK=0
[[ "${1:-}" == --check ]] && CHECK=1

VERSION="1.9.1"
SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
DEST="$TOOLS/appimagetool-x86_64.AppImage"
URL="https://github.com/AppImage/appimagetool/releases/download/${VERSION}/appimagetool-x86_64.AppImage"

# The type-2 runtime is the ~1 MB launcher stub prepended to the squashfs.
# appimagetool downloads it from a rolling "continuous" tag when it is not
# handed one, which would make every package silently depend on the
# network and on whatever that tag points at today. Pin it and pass it in.
RT_VERSION="20251108"
RT_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
RT_DEST="$TOOLS/appimage-runtime-x86_64"
RT_URL="https://github.com/AppImage/type2-runtime/releases/download/${RT_VERSION}/runtime-x86_64"

fetch() {  # fetch <url> <dest> <sha256> <exec?>
    if [[ -f "$2" ]] && echo "$3  $2" | sha256sum -c --status -; then
        echo "[setup-appimagetool] $2 already current"
        return 0
    fi
    if [[ $CHECK -eq 1 ]]; then
        echo "[setup-appimagetool] $2 missing or not sha256 $3" >&2
        STALE=1
        return 0
    fi
    echo "[setup-appimagetool] downloading $(basename "$2")"
    curl --fail --retry 5 -L -o "$2.tmp" "$1"
    echo "$3  $2.tmp" | sha256sum -c -
    [[ -z "${4:-}" ]] || chmod +x "$2.tmp"
    mv "$2.tmp" "$2"
}

STALE=0
[[ $CHECK -eq 1 ]] || mkdir -p "$TOOLS"
fetch "$URL" "$DEST" "$SHA256" exec
fetch "$RT_URL" "$RT_DEST" "$RT_SHA256"
[[ $STALE -eq 0 ]] || exit 1
echo "[setup-appimagetool] ready"
