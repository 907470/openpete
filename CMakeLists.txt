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

# PsyCross integration. Phase 1b builds the static lib but does NOT link
# it into per-game binaries — generated PSYQ stubs satisfy the link first.
# Phase 1c flips the default ON and replaces stubs with real PsyCross
# thunks. PsyCross's CMake calls find_package(OPENAL REQUIRED), so this
# requires `libopenal-dev` (Linux). We default OFF so a fresh checkout
# builds without the dev headers.
option(OPENPETE_BUILD_PSYCROSS "Build third_party/PsyCross as a static lib" OFF)
if(OPENPETE_BUILD_PSYCROSS)
    add_subdirectory(third_party/PsyCross)
endif()

add_subdirectory(core)

foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt")
        add_subdirectory(games/${game})
    else()
        message(WARNING "OPENPETE_GAMES requested '${game}' but games/${game}/CMakeLists.txt is missing — skipping")
    endif()
endforeach()
