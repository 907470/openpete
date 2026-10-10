#!/usr/bin/env bash
# scripts/release-container.sh - build and package the Linux release
# inside the Debian 11 base.
#
# The symbol floor of the shipped binary is set by the machine that
# compiles it, so the release build is not a dev-box build with different
# flags - it is a different machine. This wraps that: same sources, same
# presets, older libc.
#
#   OPENPETE_DIST_VERSION=vX.Y.Z scripts/release-container.sh [package-dist args...]
#
# It packages a tagged release: it refuses unless OPENPETE_DIST_VERSION names
# a tag, HEAD is that tag's commit, and the tracked tree is clean. Inside the
# container, after the build and before packaging, each shipped game's
# binary dumps its surface (OPENPETE_SURFACE_DUMP, which exits before the
# recomp module attaches), and the release subset of that dump must equal the
# committed baseline packaging/surface/<game>.json
# (scripts/fences/surface_coverage.py compare).
#
# Everything after the script name goes to package-dist.sh, e.g.
#   scripts/release-container.sh --appimage
# The release always bundles the fetched clang (`--toolchain .tools/clang`):
# a stranger's machine has no compiler for the first-boot module build.
# Pass `--out` relative to the tree: the packager runs inside the
# container, where only /src (this tree) is mounted.
#
# Engine detection and the uid mapping come from scripts/lib/container.sh.
# Rootless podman on a btrfs home has no overlay driver, and `podman build`
# overlay-mounts its context even when the context is empty. There, set
# `driver = "vfs"` in ~/.config/containers/storage.conf.d/*.conf, build
# the image once by hand (run the Dockerfile's RUN steps in a
# `podman run --name relbase debian:bullseye` container, then
# `podman commit --change 'WORKDIR /src' relbase $IMAGE`); this script
# skips its own build when the image already exists.
#
# The bundled clang and appimagetool are fetched on the HOST (they are
# just pinned downloads, and .tools/ is bind-mounted in), so the
# container does no network work of its own beyond its own apt layer.
set -euo pipefail
cd "$(dirname "$0")/.."

# The image is tagged with a hash of its Dockerfile, so an edited Dockerfile
# names an image that does not exist yet and is built, rather than silently
# reusing the one an older Dockerfile made.
DOCKERFILE=packaging/linux-release.Dockerfile
IMAGE="${OPENPETE_RELEASE_IMAGE:-openpete-linux-release:$(sha256sum "$DOCKERFILE" | cut -c1-12)}"
# shellcheck source=scripts/lib/container.sh
source scripts/lib/container.sh
ctr_engine release
ENGINE="$CTR_ENGINE"

if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[release] building $IMAGE"
    "$ENGINE" build -f "$DOCKERFILE" -t "$IMAGE" docs
fi

scripts/setup_linux_clang.sh
scripts/setup_appimagetool.sh

# --userns=keep-id (podman) / --user (docker): the build writes into the
# bind-mounted source tree, and root-owned objects there would be a
# nuisance to clean up afterwards.
ctr_uidmap
uidmap=("${CTR_UIDMAP[@]}")

# The SDK ships in every release, so it is staged unless the caller opts out
# with --no-mod-sdk. Leaving it to be remembered per invocation is how an
# artifact reaches the dist gate with no sdk/ in it: the gate treats a missing
# SDK as a hard failure, which is the right verdict but arrives an hour into
# packaging and a full re-pack later.
pkg_args=("$@")
if [[ " ${pkg_args[*]} " == *" --no-mod-sdk "* ]]; then
    pkg_args=("${pkg_args[@]/--no-mod-sdk}")
elif [[ " ${pkg_args[*]} " != *" --with-mod-sdk "* ]]; then
    pkg_args+=(--with-mod-sdk)
fi

# The container builds and never commits: the ratchet fences' hook-arming
# checks (scripts/fences/{complexity,doc}_ratchet.py check --armed) do not apply.
# OPENPETE_DIST_VERSION names the artifact and sets the exe's version line
# (CMake configure and package-dist.sh both read it); the container sees
# only the variables passed with -e.
# The version is read on the host when the caller names none: in a git
# worktree /src/.git is a file pointing at a gitdir outside the mount, so
# `git describe` inside the container fails and the artifact would be named
# `dev`.
# The version is the tag create-release.sh made, passed by the caller and
# checked against the tree on the host: inside the container /src/.git of a
# worktree points outside the mount and git cannot answer.
[[ -n "${OPENPETE_DIST_VERSION:-}" ]] \
    || { echo "[release] set OPENPETE_DIST_VERSION=vX.Y.Z, the release tag" >&2; exit 64; }
tag_sha=$(git rev-parse -q --verify "${OPENPETE_DIST_VERSION}^{commit}" 2>/dev/null) \
    || { echo "[release] refused: no tag or commit $OPENPETE_DIST_VERSION" >&2; exit 1; }
[[ "$(git rev-parse HEAD)" == "$tag_sha" ]] \
    || { echo "[release] refused: HEAD is not $OPENPETE_DIST_VERSION ($tag_sha)" >&2; exit 1; }
git diff --quiet HEAD -- \
    || { echo "[release] refused: tracked changes in the tree" >&2; exit 1; }
env_args=(-e OPENPETE_NO_COMMITS=1 -e "OPENPETE_DIST_VERSION=$OPENPETE_DIST_VERSION")

# The release builds from nothing, in directories only the container uses.
# The host tree's third_party/psxrecomp/build and build/linux-release-launcher
# are dev-box builds (a newer glibc floor, objects ninja would call up to
# date); the private directories are mounted over those paths inside the
# container, where package-dist.sh reads them, and emptied first.
PRIV="build/release-container"
rm -rf "$PRIV"
mkdir -p "$PRIV/psxrecomp" "$PRIV/linux-release-launcher" \
         third_party/psxrecomp/build build/linux-release-launcher
mounts=(-v "$PWD:/src"
        -v "$PWD/$PRIV/psxrecomp:/src/third_party/psxrecomp/build"
        -v "$PWD/$PRIV/linux-release-launcher:/src/build/linux-release-launcher")
"$ENGINE" run --rm "${mounts[@]}" "${env_args[@]}" "${uidmap[@]}" "$IMAGE" bash -euo pipefail -c '
    cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build \
          -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build third_party/psxrecomp/build -j
    cmake --preset linux-release-launcher
    cmake --build build/linux-release-launcher -j
    surf=$(mktemp -d)
    for game in $(python3 scripts/fences/surface_coverage.py shipped); do
        exe=build/linux-release-launcher/games/$game/openpete-${game//-/}
        OPENPETE_SURFACE_DUMP=$surf/$game.json "$exe" >/dev/null
        "$exe" --help >"$surf/$game.help" 2>&1
        python3 scripts/fences/surface_coverage.py compare --game "$game" \
            --dump "$surf/$game.json" --help-file "$surf/$game.help" \
            --baseline "packaging/surface/$game.json"
    done
    scripts/package-dist.sh linux --toolchain .tools/clang '"${pkg_args[*]}"'
'
