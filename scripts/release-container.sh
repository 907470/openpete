#!/usr/bin/env bash
# scripts/release-container.sh - build and package the Linux release
# inside the Debian 11 base. The surface dumps it runs execute in the
# container, whose HOME is not the developer's (sandbox-guard: intentional
# the release build's surface dump writes no per-user file and runs in the container).
#
# The symbol floor of the shipped binary is set by the machine that
# compiles it, so the release build is not a dev-box build with different
# flags - it is a different machine. This wraps that: same sources, same
# presets, older libc.
#
#   OPENPETE_DIST_VERSION=vX.Y.Z scripts/release-container.sh [package-dist args...]
#   scripts/release-container.sh --test [package-dist args...]
#
# It packages a tagged release: it refuses unless OPENPETE_DIST_VERSION names
# a tag, HEAD is that tag's commit, and the tracked tree is clean. --test
# (first argument) drops those three refusals and packages any HEAD, tracked
# changes included, under a version ending in -test: OPENPETE_DIST_VERSION
# when it ends so (anything else is refused, so a test artifact never
# carries a release name), else <short HEAD>[-dirty]-test. The surface
# compare below still runs and prints every difference, but a difference
# does not stop a test build: master moves its surface between releases, and
# the release commit updates the baseline. Everything else below runs in
# both modes. Inside the
# container, after the build and before packaging, each shipped game's
# binary dumps its surface (OPENPETE_SURFACE_DUMP, which exits before the
# recomp module attaches), and the release subset of that dump must equal the
# committed baseline packaging/surface/<game>.json
# (scripts/fences/surface_coverage.py compare).
#
# Everything after the script name goes to package-dist.sh, e.g.
#   scripts/release-container.sh --appimage
# The release always bundles the fetched clang (`--toolchain /tools/clang`):
# a stranger's machine has no compiler for the first-boot module build.
# Pass `--out` relative to the tree: the packager runs inside the
# container, where only /src (this tree) is mounted.
#
# The image build and the container run each first wait out another job's
# timing window (scripts/lib/timing_gate.sh); with
# OPENPETE_TIMING_GATE_TIMEOUT=<seconds> set the script exits 75 at the limit
# without launching.
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
# just pinned downloads), so the container does no network work of its own
# beyond its own apt layer. The main checkout's .tools/
# (scripts/lib/tools_dir.sh; a linked worktree has none of its own) is
# mounted read-only at /tools, and the container reads it through
# OPENPETE_TOOLS_DIR.
set -euo pipefail
cd "$(dirname "$0")/.."

TEST=0
if [[ "${1:-}" == --test ]]; then TEST=1; shift; fi

# The image is tagged with a hash of its Dockerfile, so an edited Dockerfile
# names an image that does not exist yet and is built, rather than silently
# reusing the one an older Dockerfile made.
DOCKERFILE=packaging/linux-release.Dockerfile
IMAGE="${OPENPETE_RELEASE_IMAGE:-openpete-linux-release:$(sha256sum "$DOCKERFILE" | cut -c1-12)}"
# shellcheck source=scripts/lib/container.sh
source scripts/lib/container.sh
# shellcheck source=scripts/lib/timing_gate.sh
source scripts/lib/timing_gate.sh
ctr_engine release
ENGINE="$CTR_ENGINE"

if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[release] building $IMAGE"
    op_timing_gate "release-container image" >&2 || exit $?
    "$ENGINE" build -f "$DOCKERFILE" -t "$IMAGE" docs
fi

scripts/setup_linux_clang.sh
scripts/setup_appimagetool.sh
# shellcheck source=scripts/lib/tools_dir.sh
source scripts/lib/tools_dir.sh
TOOLS=$(openpete_tools_dir .)

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
if [[ $TEST -eq 1 ]]; then
    if [[ -z "${OPENPETE_DIST_VERSION:-}" ]]; then
        OPENPETE_DIST_VERSION=$(git rev-parse --short HEAD)
        git diff --quiet HEAD -- || OPENPETE_DIST_VERSION+=-dirty
        OPENPETE_DIST_VERSION+=-test
    fi
    [[ "$OPENPETE_DIST_VERSION" == *-test ]] \
        || { echo "[release] --test: OPENPETE_DIST_VERSION '$OPENPETE_DIST_VERSION'" \
                  "does not end in -test" >&2; exit 64; }
    echo "[release] test build $OPENPETE_DIST_VERSION: tag, HEAD and clean-tree checks skipped"
else
    [[ -n "${OPENPETE_DIST_VERSION:-}" ]] \
        || { echo "[release] set OPENPETE_DIST_VERSION=vX.Y.Z, the release tag" >&2; exit 64; }
    tag_sha=$(git rev-parse -q --verify "${OPENPETE_DIST_VERSION}^{commit}" 2>/dev/null) \
        || { echo "[release] refused: no tag or commit $OPENPETE_DIST_VERSION" >&2; exit 1; }
    [[ "$(git rev-parse HEAD)" == "$tag_sha" ]] \
        || { echo "[release] refused: HEAD is not $OPENPETE_DIST_VERSION ($tag_sha)" >&2; exit 1; }
    git diff --quiet HEAD -- \
        || { echo "[release] refused: tracked changes in the tree" >&2; exit 1; }
fi
env_args=(-e OPENPETE_NO_COMMITS=1 -e "OPENPETE_DIST_VERSION=$OPENPETE_DIST_VERSION"
          -e OPENPETE_TOOLS_DIR=/tools -e OPENPETE_VIBRATION=0 -e OPENPETE_IN_RELEASE_CONTAINER=1
          -e "OPENPETE_RELEASE_TEST=$TEST")

# The release builds from nothing, in directories only the container uses.
# The host tree's third_party/psxrecomp/build and build/linux-release-launcher
# are dev-box builds (a newer glibc floor, objects ninja would call up to
# date); the private directories are mounted over those paths inside the
# container, where package-dist.sh reads them, and emptied first.
PRIV="build/release-container"
rm -rf "$PRIV"
mkdir -p "$PRIV/psxrecomp" "$PRIV/linux-release-launcher" \
         third_party/psxrecomp/build build/linux-release-launcher
# A linked worktree's untracked build inputs (generated C, the decomp build
# directories, the psxrecomp build) are absolute symlinks into the main
# checkout, so the main checkout is mounted read-only at its own path, and a
# private directory replacing a symlinked path is mounted at the symlink's
# target, which the container runtime refuses to mount through.
main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
mounts=(-v "$PWD:/src" -v "$TOOLS:/tools:ro")
[[ "$main" == "$PWD" ]] || mounts+=(-v "$main:$main:ro")
for d in third_party/psxrecomp/build:psxrecomp \
         build/linux-release-launcher:linux-release-launcher; do
    dst=/src/${d%%:*}
    [[ -L "${d%%:*}" ]] && dst=$(realpath "${d%%:*}")
    mounts+=(-v "$PWD/$PRIV/${d#*:}:$dst")
done
op_timing_gate "release-container build" >&2 || exit $?
"$ENGINE" "${CTR_ENGINE_ARGS[@]}" run --rm "${mounts[@]}" "${env_args[@]}" "${uidmap[@]}" "$IMAGE" bash -euo pipefail -c '
    cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build \
          -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build third_party/psxrecomp/build -j
    cmake --preset linux-release-launcher
    cmake --build build/linux-release-launcher -j
    surf=$(mktemp -d)
    for game in $(python3 scripts/fences/surface_coverage.py shipped); do
        exe=build/linux-release-launcher/openpete
        OPENPETE_SURFACE_DUMP=$surf/$game.json "$exe" --game "$game" >/dev/null
        "$exe" --game "$game" --help >"$surf/$game.help" 2>&1
        python3 scripts/fences/surface_coverage.py compare --game "$game" \
            --dump "$surf/$game.json" --help-file "$surf/$game.help" \
            --baseline "packaging/surface/$game.json" && continue
        [[ $OPENPETE_RELEASE_TEST == 1 ]] || exit 1
        echo "[release] test build: the $game surface differs from" \
             "packaging/surface/$game.json (above); a release of this HEAD" \
             "refuses until the release commit updates the baseline" >&2
    done
    # The bundled clang carries glibc and Linux uapi headers for source
    # mods (scripts/setup_linux_clang.sh fetches them on the host, pinned
    # to this image'"'"'s package versions). Every one must be byte-equal to
    # the file this image'"'"'s libc6-dev or linux-libc-dev installed, the
    # multiarch x86_64-linux-gnu/ prefix dropped, and no other file may be
    # staged: a Dockerfile snapshot bump that moves either package refuses
    # here until the pins follow it.
    sys=/tools/clang/sysroot/include
    n=0
    while read -r f; do
        rel=${f#/usr/include/}; rel=${rel#x86_64-linux-gnu/}
        cmp -s "$f" "$sys/$rel" \
            || { echo "[release] staged header differs from this image: $f" >&2; exit 1; }
        n=$((n + 1))
    done < <(dpkg -L libc6-dev linux-libc-dev | grep "^/usr/include/" \
             | while read -r p; do [[ -f $p ]] && echo "$p"; done)
    staged=$(find "$sys" -type f | wc -l)
    [[ $n -eq $staged ]] \
        || { echo "[release] staged headers: $staged files, this image: $n" >&2; exit 1; }
    echo "[release] sysroot headers match the image ($n files)"
    scripts/package-dist.sh linux --toolchain /tools/clang '"${pkg_args[*]}"'
'

# The release archive, on the host: the container's home is not this
# machine's, so package-dist.sh skips it there (OPENPETE_IN_RELEASE_CONTAINER).
out=dist
for ((i = 0; i < ${#pkg_args[@]}; i++)); do
    [[ "${pkg_args[$i]}" == --out ]] && out="${pkg_args[$((i + 1))]}"
done
# shellcheck source=scripts/lib/release_archive.sh
source scripts/lib/release_archive.sh
arts=()
for a in "$out/openpete-${OPENPETE_DIST_VERSION}-linux-x86_64.tar.xz" \
         "$out/OpenPete-${OPENPETE_DIST_VERSION}-x86_64.AppImage"; do
    [[ -f "$a" ]] && arts+=("$a")
done
if [[ ${#arts[@]} -gt 0 ]]; then
    release_archive_store "$OPENPETE_DIST_VERSION" "$(git rev-parse --short=9 HEAD)" "${arts[@]}"
else
    echo "[release] NOTE: no package under $out to archive" >&2
fi
