#!/usr/bin/env bash
# scripts/golden-capture-container.sh — run the presentation-golden capture
# legs inside the pinned capture image (docs/linux-capture.Dockerfile).
#
#   scripts/golden-capture-container.sh image            # build the image
#   scripts/golden-capture-container.sh build            # configure + build
#   scripts/golden-capture-container.sh run -- CMD ARGS  # run CMD in the tree
#   scripts/golden-capture-container.sh gate -- CMD ARGS # build, then run CMD
#   scripts/golden-capture-container.sh identity         # image id + versions
#
# `gate` is the entry the presentation gates take from the host
# (scripts/presentation-game.sh pg_enter_capture_image): it exits 64 (SKIP)
# when no container engine or no current image is present, instead of building
# the image, then builds the engine and runs CMD. `build` and `gate` hold an
# exclusive lock on build/linux-release-container.build-lock for their whole run, so
# two gates (ctest -j, a manual run beside ctest) never configure or build into
# the shared container build directory at once; the second waits.
#
# The image is labelled with the SHA-256 of the recipe it was built from and
# carries the recipe at /etc/openpete/capture.Dockerfile. An image whose label
# differs from the tracked recipe's hash is stale: `image`, `build` and `run`
# rebuild it, `gate` skips. OPENPETE_CAPTURE_IMAGE_DIGEST passes the image's
# digest into the container, where the capture record reads it.
#
# `build` configures the `linux-release-container` preset fresh into
# build/linux-release-container with the image's gcc-11, never the host's
# build/linux-release: CMake caches CMAKE_C_COMPILER as a path and does not
# re-identify it on a reconfigure, so a shared build directory would mix the
# two toolchains' objects. It runs no recompiler and does not rebuild
# third_party/psxrecomp/build; the engine's generated sources come from the
# manifest tomls.
#
# Mounts. The tree is mounted read-write at its own absolute path, so the
# absolute paths CMake records and the symlinks a worktree carries resolve
# identically on both sides. When the tree is a git worktree, the main
# checkout (which holds the common git directory, the generated recomp
# sources and the disc images the worktree links to) is mounted read-only
# underneath it, its .git directory read-write. The reference store's parent
# directory (the store, $OPENPETE_GOLDEN_REF_DIR or the default under
# ${XDG_DATA_HOME:-~/.local/share}/openpete, and the state file beside it) is
# mounted read-write and the store's path is passed in, so an in-container
# promote files its blobs on the host.
#
# OPENPETE_CAPTURE_TREE names another source tree to mount and run in (a
# materialize worktree at a past revision, which carries no copy of this
# script); the image is built from this checkout's recipe either way.
#
# Rootless podman on a btrfs home has no overlay driver; build the image
# there as scripts/release-container.sh describes (podman run + commit, the
# commit passing --change "LABEL org.openpete.capture-recipe=<sha256>").
# Elsewhere this script builds the image when it is absent or stale, from a
# context directory holding the recipe alone.
set -euo pipefail
cd "$(dirname "$0")/.."
RECIPE_DIR="$PWD"
if [[ -n "${OPENPETE_CAPTURE_TREE:-}" ]]; then
    cd "$OPENPETE_CAPTURE_TREE" || { echo "[capture-container] no tree at $OPENPETE_CAPTURE_TREE" >&2; exit 64; }
fi
TREE="$PWD"

IMAGE="${OPENPETE_CAPTURE_IMAGE:-openpete-linux-capture}"
ENGINE="${OPENPETE_CONTAINER_ENGINE:-}"
if [[ -z "$ENGINE" ]]; then
    for e in podman docker; do command -v "$e" >/dev/null && { ENGINE=$e; break; }; done
fi
[[ -n "$ENGINE" ]] || { echo "[capture-container] no podman or docker found" >&2; exit 64; }

sub="${1:-}"; shift || true
case "$sub" in
    image|build|identity) ;;
    run|gate) [[ "${1:-}" == "--" ]] && shift
         [[ $# -gt 0 ]] || { echo "[capture-container] $sub: no command" >&2; exit 64; } ;;
    *) echo "usage: $0 image | build | run -- CMD [ARGS...] | gate -- CMD [ARGS...] | identity" >&2; exit 64 ;;
esac

RECIPE_HASH="$(sha256sum "$RECIPE_DIR/docs/linux-capture.Dockerfile" | cut -d' ' -f1)"
LABEL=org.openpete.capture-recipe
image_label="$("$ENGINE" image inspect --format "{{index .Labels \"$LABEL\"}}" "$IMAGE" 2>/dev/null || true)"
if [[ "$image_label" != "$RECIPE_HASH" ]]; then
    if [[ "$sub" == gate ]]; then
        if [[ -z "$image_label" ]] && ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
            echo "[capture-container] SKIP: no capture image $IMAGE; build it with: $0 image"
        else
            echo "[capture-container] SKIP: $IMAGE was built from another recipe" \
                 "(${image_label:-unlabelled}, tracked ${RECIPE_HASH:0:12}); rebuild it with: $0 image"
        fi
        exit 64
    fi
    echo "[capture-container] building $IMAGE from recipe ${RECIPE_HASH:0:12}"
    ctx="$(mktemp -d)"
    cp "$RECIPE_DIR/docs/linux-capture.Dockerfile" "$ctx/Dockerfile"
    "$ENGINE" build --label "$LABEL=$RECIPE_HASH" -f "$ctx/Dockerfile" -t "$IMAGE" "$ctx"
    rm -rf "$ctx"
fi
[[ "$sub" == image ]] && exit 0

mounts=()
common="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
main="${common%/.git}"
if [[ -n "$common" && "$main" != "$common" && "$main" != "$TREE" ]]; then
    # The common git directory stays writable beneath the read-only checkout:
    # the capture record hashes the working copy into git objects, and a
    # promote stages its manifest in the worktree's index.
    mounts+=(-v "$main:$main:ro" -v "$common:$common")
fi
mounts+=(-v "$TREE:$TREE")
image_digest="$("$ENGINE" image inspect --format '{{.Digest}}' "$IMAGE" 2>/dev/null || true)"
[[ -z "$image_digest" || "$image_digest" == "<none>" ]] && \
    image_digest="$("$ENGINE" image inspect --format '{{.Id}}' "$IMAGE")"
env_args=(-e OPENPETE_NO_COMMITS=1 -e OPENPETE_VIBRATION=0
          -e "OPENPETE_CAPTURE_IMAGE_DIGEST=$image_digest")
# The image has no passwd entry for the mapped uid, so HOME is unset inside
# the container and the engine's per-user directories ($HOME/.config/openpete)
# and PulseAudio's runtime cookie resolve relative to the working directory,
# the tree. A home under the container build directory keeps them out of it.
home="$TREE/build/linux-release-container/home"
mkdir -p "$home/run"
chmod 700 "$home/run"
env_args+=(-e "HOME=$home" -e "XDG_CONFIG_HOME=$home/.config"
           -e "XDG_CACHE_HOME=$home/.cache" -e "XDG_DATA_HOME=$home/.local/share"
           -e "XDG_RUNTIME_DIR=$home/run")
# The reference store resolves as scripts/presentation-blessed.py resolves it,
# and the container gets it by an explicit path: the container's home is not
# the host's, so the default would otherwise name a directory inside the
# container that vanishes with it.
store="${OPENPETE_GOLDEN_REF_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/openpete/golden-ref}"
mkdir -p "$store"
store="$(cd "$store" && pwd -P)"
store_parent="$(dirname "$store")"
mounts+=(-v "$store_parent:$store_parent")
env_args+=(-e "OPENPETE_GOLDEN_REF_DIR=$store")
# The harness knobs a capture run reads; the container sees only what -e passes.
for v in CORPUS_JOBS PRESENTATION_TIER PRESENTATION_GOLDEN_OUT PRESENTATION_GOLDEN_KEEP \
         PRESENTATION_LEG_TIMEOUT OPENPETE_CAP_LODS OPENPETE_FRAME_LIMIT; do
    [[ -n "${!v:-}" ]] && env_args+=(-e "$v=${!v}")
done

uidmap=(--user "$(id -u):$(id -g)")
[[ "$ENGINE" == podman ]] && uidmap=(--userns=keep-id)

# The lock descriptor (9) is closed for the engine: rootless podman leaves
# helper processes (slirp4netns, a session dbus-daemon) running after the
# container exits, and an inherited descriptor would hold the lock with them.
run_in() {
    "$ENGINE" run --rm "${mounts[@]}" "${env_args[@]}" "${uidmap[@]}" \
        -w "$TREE" "$IMAGE" "$@" 9>&-
}

# Serialise every configure and build into build/linux-release-container: the
# lock is held by this host-side process until it exits.
lock_build_dir() {
    mkdir -p build
    exec 9>build/linux-release-container.build-lock
    if ! flock -n 9; then
        echo "[capture-container] build/linux-release-container is locked by another run; waiting"
        flock 9
    fi
}

build_engine() {
    run_in bash -euo pipefail -c '
        cmake --preset linux-release-container
        cmake --build build/linux-release-container -j'
}

case "$sub" in
    build)
        lock_build_dir
        build_engine
        ;;
    gate)
        lock_build_dir
        build_engine || { echo "[capture-container] FAIL: in-image engine build"; exit 1; }
        run_in "$@"
        ;;
    identity)
        echo "image: $IMAGE"
        "$ENGINE" image inspect --format 'id: {{.Id}}' "$IMAGE"
        "$ENGINE" image inspect --format 'digest: {{.Digest}}' "$IMAGE" 2>/dev/null || true
        echo "engine: $("$ENGINE" --version)"
        run_in bash -c '
            dpkg-query -W -f "\${Package}\t\${Version}\n" \
                mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 libllvm15 libvulkan1 glslang-tools spirv-tools gcc-11 g++-11 gcc g++ cmake ninja-build python3
            python3 -c "import PIL, numpy, tomli; print(\"Pillow\t\" + PIL.__version__); print(\"numpy\t\" + numpy.__version__)"
            gcc-11 --version | head -1'
        ;;
    run)
        run_in "$@"
        ;;
esac
