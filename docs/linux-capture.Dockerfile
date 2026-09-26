# The presentation-golden capture image: the environment whose rasterizer
# and compiler a verdict digest depends on.
#
# Ubuntu 22.04 (jammy), pinning the Mesa llvmpipe/lavapipe rasterizer, its
# LLVM, the Vulkan loader, the C/C++ compiler, CMake, Ninja, and the Python
# image stack to the versions of the blessing host, so a tile captured here
# reproduces a tile captured there. This is not the release image
# (docs/linux-release.Dockerfile): that base is chosen for a glibc symbol
# floor, and its Mesa (20.3) exposes lavapipe at Vulkan 1.0, below the 1.2
# instance and device the engine creates.
#
# Verdict-relevant pins:
#   mesa-vulkan-drivers  lavapipe ICD (lvp_icd.x86_64.json) the capture forces
#   libllvm15            the llvmpipe JIT; its version is in the device name
#   libvulkan1           the loader the capture refuses to run without
#   gcc-11 / g++-11      compile the engine; float codegen enters every tile;
#   gcc / g++            the cc and c++ the build's mod-compile step invokes
#   glslang-tools        glslangValidator compiles the renderer's shaders to
#                        SPIR-V, which lavapipe executes
#   Pillow, numpy (pip)  PNG decode for the diff, digest and identity legs;
#                        numpy for the credits leg. The blessing host carries
#                        them from pip (Pillow 12.0.0, numpy 2.2.6), not from
#                        the python3-pil / python3-numpy packages.
# The remaining -dev packages are the engine's build dependencies (SDL3
# refuses to configure without X11 or Wayland headers; OpenAL, ALSA,
# PulseAudio, sndio and FFmpeg are probed as on the host). The version each
# one resolved to is recorded by `dpkg-query -W` in the wrapper's
# `identity` subcommand.
#
#   scripts/golden-capture-container.sh image     # this image, labelled with the recipe hash
#   scripts/golden-capture-container.sh build     # engine, preset linux-release-container
#   scripts/golden-capture-container.sh run -- tests/harness/check_presentation_golden.sh \
#       --build-dir=build/linux-release-container --game spyro-1
FROM docker.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git make pkg-config file \
        python3=3.10.6-1~22.04.1 python3-pip \
        gcc-11=11.4.0-1ubuntu1~22.04.3 g++-11=11.4.0-1ubuntu1~22.04.3 \
        cpp-11=11.4.0-1ubuntu1~22.04.3 gcc=4:11.2.0-1ubuntu1 g++=4:11.2.0-1ubuntu1 \
        cmake=3.22.1-1ubuntu1.22.04.2 cmake-data=3.22.1-1ubuntu1.22.04.2 \
        ninja-build=1.10.1-1 \
        mesa-vulkan-drivers=23.2.1-1ubuntu3.1~22.04.4 \
        libllvm15=1:15.0.7-0ubuntu0.22.04.3 \
        libvulkan1=1.3.204.1-2 \
        glslang-tools=11.8.0+1.3.204.0-1 \
        libopenal-dev libasound2-dev libpulse-dev libsndio-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
        libxi-dev libxss-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols libdecor-0-dev \
        libdrm-dev libgbm-dev libgl-dev libegl-dev libgles-dev \
        libegl1-mesa-dev libdbus-1-dev libudev-dev libibus-1.0-dev \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
        libswresample-dev \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir Pillow==12.0.0 numpy==2.2.6 tomli==2.4.0

# The engine initialises PsyCross, which opens a hidden SDL window with a GL
# context even under --headless. The image has no display server: SDL's
# offscreen video driver gives that window an EGL context on Mesa's
# llvmpipe (libegl-mesa0 / libgl1-mesa-dri, the same Mesa build as the ICD).
# The native -vk capture is surfaceless Vulkan and does not use the window.
ENV SDL_VIDEO_DRIVER=offscreen

# The tree is bind-mounted and owned by the host uid.
RUN git config --system --add safe.directory '*'

# The image carries the recipe it was built from. A process that finds it is
# inside the capture image, and its SHA-256 is the recipe hash a manifest
# header records (scripts/presentation-blessed.py, Environment). The build
# context is a directory holding this file alone, named Dockerfile.
COPY Dockerfile /etc/openpete/capture.Dockerfile

WORKDIR /src
