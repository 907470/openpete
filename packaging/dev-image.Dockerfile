# The developer image: the build environment of scripts/container.sh.
#
# Two stages. `developer` builds the engine, the games, the fences, the
# documentation site, PSXRecomp and the DuckStation oracle fork; `mingw`
# adds the Windows cross toolchain. The install steps are
# packaging/dev-image.sh, one call per stage, over the package names in
# packaging/dev-image-packages.txt (the native apt line of building.md).
# Every input is pinned: the base by its multi-arch index digest, apt by a
# snapshot.ubuntu.com timestamp, clang-19 by version, pip by version.
#
# This is not the release image (packaging/linux-release.Dockerfile, Debian
# 11 for the glibc symbol floor of a shipped binary) and not the capture
# image (tests/harness/linux-capture.Dockerfile, whose recipe hash a
# presentation-golden verdict records).
#
#   scripts/container.sh shell        # builds both stages on first use
#
# The build context is a directory holding dev-image.sh and
# dev-image-packages.txt alone. When rootless podman stores images with the
# vfs driver, scripts/lib/container.sh replays this file (FROM, ENV,
# WORKDIR, COPY, RUN) with podman run and commit instead of podman build.
FROM docker.io/library/ubuntu:22.04@sha256:b1066385161d28ddf6bc7e7b28a9170eec11484c821d1a5150d176cbde41d7f7 AS developer
ENV DEBIAN_FRONTEND=noninteractive
# PsyCross opens a hidden SDL window with a GL context under --headless;
# the offscreen driver gives it an EGL context on Mesa llvmpipe.
ENV SDL_VIDEO_DRIVER=offscreen
COPY dev-image.sh dev-image-packages.txt /opt/openpete-dev-image/
RUN /opt/openpete-dev-image/dev-image.sh base
WORKDIR /

FROM developer AS mingw
RUN /opt/openpete-dev-image/dev-image.sh mingw
