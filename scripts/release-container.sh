#!/usr/bin/env bash
# scripts/release-container.sh — build and package the Linux release
# inside the Debian 11 base (distribution D6.6; plan
# openpete-distribution-local-recomp).
#
# The symbol floor of the shipped binary is set by the machine that
# compiles it, so the release build is not a dev-box build with different
# flags — it is a different machine. This wraps that: same sources, same
# presets, older libc.
#
#   scripts/release-container.sh [package-dist args...]
#
# Everything after the script name goes to package-dist.sh, e.g.
#   scripts/release-container.sh --appimage
# The release always bundles the fetched clang (`--toolchain .tools/clang`):
# a stranger's machine has no compiler for the first-boot module build.
# Pass `--out` relative to the tree: the packager runs inside the
# container, where only /src (this tree) is mounted.
#
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

IMAGE="${OPENPETE_RELEASE_IMAGE:-openpete-linux-release}"
ENGINE="${OPENPETE_CONTAINER_ENGINE:-}"
if [[ -z "$ENGINE" ]]; then
    for e in podman docker; do command -v "$e" >/dev/null && { ENGINE=$e; break; }; done
fi
[[ -n "$ENGINE" ]] || { echo "[release] no podman or docker found" >&2; exit 64; }

if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[release] building $IMAGE"
    "$ENGINE" build -f docs/linux-release.Dockerfile -t "$IMAGE" .
fi

scripts/setup_linux_clang.sh
scripts/setup_appimagetool.sh

# --userns=keep-id (podman) / --user (docker): the build writes into the
# bind-mounted source tree, and root-owned objects there would be a
# nuisance to clean up afterwards.
uidmap=(--user "$(id -u):$(id -g)")
[[ "$ENGINE" == podman ]] && uidmap=(--userns=keep-id)

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
env_args=(-e OPENPETE_NO_COMMITS=1)
[[ -n "${OPENPETE_DIST_VERSION:-}" ]] && env_args+=(-e "OPENPETE_DIST_VERSION=$OPENPETE_DIST_VERSION")

# The release builds from nothing, in directories only the container uses.
# The host tree's third_party/psxrecomp/build and build/linux-release-shared
# are dev-box builds (a newer glibc floor, objects ninja would call up to
# date); the private directories are mounted over those paths inside the
# container, where package-dist.sh reads them, and emptied first.
PRIV="build/release-container"
rm -rf "$PRIV"
mkdir -p "$PRIV/psxrecomp" "$PRIV/linux-release-shared" \
         third_party/psxrecomp/build build/linux-release-shared
mounts=(-v "$PWD:/src"
        -v "$PWD/$PRIV/psxrecomp:/src/third_party/psxrecomp/build"
        -v "$PWD/$PRIV/linux-release-shared:/src/build/linux-release-shared")
"$ENGINE" run --rm "${mounts[@]}" "${env_args[@]}" "${uidmap[@]}" "$IMAGE" bash -euo pipefail -c '
    cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build \
          -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build third_party/psxrecomp/build -j
    cmake --preset linux-release-shared
    cmake --build build/linux-release-shared -j
    scripts/package-dist.sh linux --toolchain .tools/clang '"${pkg_args[*]}"'
'
