# MinGW-w64 cross toolchain for building Windows x64 binaries from Linux/WSL2.
# See notes/plans/openpete-pc-port.md for rationale.
#
# Use:
#   cmake -B build/win -G Ninja \
#         --toolchain cmake/toolchains/mingw-w64.cmake \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER       x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER     x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER      x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH   /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Single-file .exe: statically link libgcc, libstdc++, and winpthread so we
# don't have to ship .dll alongside. SDL2 itself remains a runtime .dll
# (libSDL2-2.0.dll) — that's the prebuilt's ABI.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
