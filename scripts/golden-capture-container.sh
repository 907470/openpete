#!/usr/bin/env bash
# scripts/golden-capture-container.sh — run the presentation-golden capture
# legs inside the pinned capture image (docs/linux-capture.Dockerfile).
#
#   scripts/golden-capture-container.sh build            # configure + build
#   scripts/golden-capture-container.sh run -- CMD ARGS  # run CMD in the tree
#   scripts/golden-capture-container.sh identity         # image id + versions
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
# underneath it. When OPENPETE_GOLDEN_REF_DIR is set, its parent directory
# (the store and the state file beside it) is mounted read-write and the
# variable is passed through.
#
# Rootless podman on a btrfs home has no overlay driver; build the image
# there as scripts/release-container.sh describes (podman run + commit).
# Elsewhere this script builds the image when it is absent, from an empty
# context directory.
set -euo pipefail
cd "$(dirname "$0")/.."
TREE="$PWD"

IMAGE="${OPENPETE_CAPTURE_IMAGE:-openpete-linux-capture}"
ENGINE="${OPENPETE_CONTAINER_ENGINE:-}"
if [[ -z "$ENGINE" ]]; then
    for e in podman docker; do command -v "$e" >/dev/null && { ENGINE=$e; break; }; done
fi
[[ -n "$ENGINE" ]] || { echo "[capture-container] no podman or docker found" >&2; exit 64; }

sub="${1:-}"; shift || true
case "$sub" in
    build|identity) ;;
    run) [[ "${1:-}" == "--" ]] && shift
         [[ $# -gt 0 ]] || { echo "[capture-container] run: no command" >&2; exit 64; } ;;
    *) echo "usage: $0 build | run -- CMD [ARGS...] | identity" >&2; exit 64 ;;
esac

if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[capture-container] building $IMAGE"
    ctx="$(mktemp -d)"
    cp docs/linux-capture.Dockerfile "$ctx/Dockerfile"
    "$ENGINE" build -f "$ctx/Dockerfile" -t "$IMAGE" "$ctx"
    rm -rf "$ctx"
fi

mounts=()
common="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
main="${common%/.git}"
if [[ -n "$common" && "$main" != "$common" && "$main" != "$TREE" ]]; then
    mounts+=(-v "$main:$main:ro")
fi
mounts+=(-v "$TREE:$TREE")
env_args=(-e OPENPETE_NO_COMMITS=1 -e OPENPETE_VIBRATION=0)
if [[ -n "${OPENPETE_GOLDEN_REF_DIR:-}" ]]; then
    store_parent="$(cd "$(dirname "$OPENPETE_GOLDEN_REF_DIR")" && pwd)"
    mounts+=(-v "$store_parent:$store_parent")
    env_args+=(-e "OPENPETE_GOLDEN_REF_DIR=$OPENPETE_GOLDEN_REF_DIR")
fi
# The harness knobs a capture run reads; the container sees only what -e passes.
for v in CORPUS_JOBS PRESENTATION_TIER PRESENTATION_GOLDEN_OUT PRESENTATION_GOLDEN_KEEP \
         PRESENTATION_LEG_TIMEOUT OPENPETE_CAP_LODS OPENPETE_FRAME_LIMIT; do
    [[ -n "${!v:-}" ]] && env_args+=(-e "$v=${!v}")
done

uidmap=(--user "$(id -u):$(id -g)")
[[ "$ENGINE" == podman ]] && uidmap=(--userns=keep-id)

run_in() {
    "$ENGINE" run --rm "${mounts[@]}" "${env_args[@]}" "${uidmap[@]}" \
        -w "$TREE" "$IMAGE" "$@"
}

case "$sub" in
    build)
        run_in bash -euo pipefail -c '
            cmake --preset linux-release-container
            cmake --build build/linux-release-container -j'
        ;;
    identity)
        echo "image: $IMAGE"
        "$ENGINE" image inspect --format 'id: {{.Id}}' "$IMAGE"
        "$ENGINE" image inspect --format 'digest: {{.Digest}}' "$IMAGE" 2>/dev/null || true
        echo "engine: $("$ENGINE" --version)"
        run_in bash -c '
            dpkg-query -W -f "\${Package}\t\${Version}\n" \
                mesa-vulkan-drivers libgl1-mesa-dri libegl-mesa0 libllvm15 libvulkan1 glslang-tools gcc-11 g++-11 gcc g++ cmake ninja-build python3
            python3 -c "import PIL, numpy, tomli; print(\"Pillow\t\" + PIL.__version__); print(\"numpy\t\" + numpy.__version__)"
            gcc-11 --version | head -1'
        ;;
    run)
        run_in "$@"
        ;;
esac
