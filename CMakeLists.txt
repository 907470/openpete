cmake_minimum_required(VERSION 3.22)
project(openpete LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# When cross-compiling with MinGW, point find_package(SDL2) at the vendored
# prebuilt under third_party/SDL2-mingw/. Native Linux uses the system SDL2.
# Phase 0b TODO: replace the prebuilt with the SDL submodule under
# third_party/SDL/ once we wire its CMake build into our tree.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(SDL2_DIR "${CMAKE_SOURCE_DIR}/third_party/SDL2-mingw/x86_64-w64-mingw32/lib/cmake/SDL2"
        CACHE PATH "SDL2 cmake config dir")
endif()

find_package(SDL2 REQUIRED)

# Which games to enable. Phase 0b only spyro-1 is wired up.
set(OPENPETE_GAMES "spyro-1" CACHE STRING "Games to build (semicolon-separated)")

# PsyCross integration (Phase 1c+). Builds the static lib and links it
# into openpete_core; psyq_thunks.cpp routes recompiled PSYQ calls into
# PsyCross. PsyCross's CMake calls find_package(OpenAL REQUIRED), so a
# Linux build needs `libopenal-dev`. Disable with -DOPENPETE_BUILD_PSYCROSS=OFF
# if you need a non-PsyCross link path (the per-game thunks will then
# fall through to the abort-on-call weak stubs in psyq_stubs.c).
option(OPENPETE_BUILD_PSYCROSS "Build third_party/PsyCross as a static lib" ON)
if(OPENPETE_BUILD_PSYCROSS)
    add_subdirectory(third_party/PsyCross)
endif()

# Vendored pcsx-redux GTE — third reference engine for the GTE trace logger
# (PsyCross is the second). Adds a static lib `pcsx_redux_gte` exposing one
# C-callable: pcsxr_gte_run(cmd, pre_data[32], pre_ctrl[32], post_data[32],
# post_ctrl[32]).
option(OPENPETE_BUILD_PCSX_REDUX_GTE
       "Build third_party/pcsx_redux_gte as a static lib" ON)
if(OPENPETE_BUILD_PCSX_REDUX_GTE)
    add_subdirectory(third_party/pcsx_redux_gte)
endif()

add_subdirectory(core)

foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt")
        add_subdirectory(games/${game})
    else()
        message(WARNING "OPENPETE_GAMES requested '${game}' but games/${game}/CMakeLists.txt is missing — skipping")
    endif()
endforeach()
