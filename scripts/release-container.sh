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

"$ENGINE" run --rm -v "$PWD:/src" "${uidmap[@]}" "$IMAGE" bash -euo pipefail -c '
    ./tools/apply_third_party_patches.sh
    cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build \
          -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build third_party/psxrecomp/build -j
    cmake --preset linux-release-shared
    cmake --build build/linux-release-shared -j
    scripts/package-dist.sh linux --toolchain .tools/clang '"$*"'
'
