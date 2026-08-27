#!/usr/bin/env bash
# setup_mingw_ffmpeg.sh — cross-build static x264 + FFmpeg for the windows-mingw
# target, so the Windows build can link --record video capture (runtime/record.c).
#
# WHY this exists: the windows-mingw preset is a cross-compile, and no prebuilt
# *static* mingw FFmpeg dev libraries are published (BtbN/gyan "static" packages
# ship only the .exe tools, not lib/include). The native (linux) build links the
# host libav via pkg-config; the cross build has no target libav to link, so
# core/CMakeLists.txt compiled --record out for cross-compiles. This script
# produces the missing static libs into third_party/ffmpeg-mingw/, after which
# the windows-mingw build links them and --record works on Windows.
#
# Output prefix: third_party/ffmpeg-mingw/{lib,include}  (gitignored; ~30 MB).
# Re-run is idempotent-ish (it re-clones/re-extracts into the build dir). The
# libs are STATIC + GPL (x264) — the resulting openpete-spyro1.exe is GPL.
#
# Requires: x86_64-w64-mingw32-gcc toolchain, nasm, git, curl, tar.
set -euo pipefail

HOST=x86_64-w64-mingw32
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/third_party/ffmpeg-mingw"
BUILD="$ROOT/third_party/ffmpeg-mingw-build"
# 6.1 (not 7.x): record.c uses the pre-7.0 channel-layout API
# (AVCodecContext.channels/channel_layout, swr_alloc_set_opts), which FFmpeg 7.0
# REMOVED. The native/linux build links Ubuntu's 4.x libav (same old API), so
# keeping mingw on the 6.x era lets record.c compile unchanged on both. (A future
# migration to the AVChannelLayout API would be needed to move past 6.x.)
FFMPEG_VER=6.1.1
JOBS="$(nproc)"

command -v "${HOST}-gcc" >/dev/null || { echo "error: ${HOST}-gcc not found (install mingw-w64)"; exit 1; }
command -v nasm >/dev/null || { echo "error: nasm not found (sudo apt install nasm)"; exit 1; }

mkdir -p "$BUILD"

echo "==> x264 (static, $HOST)"
cd "$BUILD"
rm -rf x264
git clone --depth 1 https://code.videolan.org/videolan/x264.git
cd x264
./configure --host="$HOST" --cross-prefix="${HOST}-" --prefix="$PREFIX" \
            --enable-static --disable-cli --disable-opencl --enable-pic
make -j"$JOBS"
make install

echo "==> FFmpeg $FFMPEG_VER (static, $HOST, +libx264)"
cd "$BUILD"
rm -rf "ffmpeg-$FFMPEG_VER"
[ -f "ffmpeg-$FFMPEG_VER.tar.xz" ] || curl -sSL -o "ffmpeg-$FFMPEG_VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
tar xf "ffmpeg-$FFMPEG_VER.tar.xz"
cd "ffmpeg-$FFMPEG_VER"
# PKG_CONFIG_LIBDIR isolates pkg-config to our cross prefix so it finds the x264
# we just built (and never the host's libs). --disable-autodetect keeps the cross
# build from linking any host external library.
PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" ./configure \
    --prefix="$PREFIX" --enable-cross-compile --cross-prefix="${HOST}-" \
    --arch=x86_64 --target-os=mingw32 \
    --pkg-config=pkg-config --pkg-config-flags=--static \
    --enable-gpl --enable-libx264 --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-network --disable-devices \
    --disable-autodetect --disable-debug \
    --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib"
make -j"$JOBS"
make install

echo
echo "==> done. static libs in $PREFIX/lib :"
ls "$PREFIX"/lib/*.a
echo
echo "Reconfigure the windows-mingw preset to pick these up:"
echo "  cmake --preset windows-mingw"
# The knob named below is a compile definition, not a child environment
# variable — wslenv-guard: no-launch.
echo "Then build; --record will be enabled (OPENPETE_HAS_LIBAV=1)."
