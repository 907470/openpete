cmake_minimum_required(VERSION 3.22)
project(openpete LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenAL has no Config-mode package file in the openal-soft prebuilt, so
# when cross-compiling with MinGW seed the cache vars FindOpenAL.cmake reads.
# Native Linux uses the system OpenAL via FindOpenAL's default search.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(_OAL_ROOT "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32")
    # PsyCross uses `#include <AL/al.h>` form, so point at the parent of
    # AL/ (FindOpenAL.cmake's PATH_SUFFIXES allow either, but the one
    # consumers expect is the prefix that makes the angle-bracket include
    # legal). Header lookup: <AL/al.h> → ${OPENAL_INCLUDE_DIR}/AL/al.h.
    set(OPENAL_INCLUDE_DIR "${_OAL_ROOT}/include"
        CACHE PATH "OpenAL include dir (vendored MinGW prebuilt)" FORCE)
    set(OPENAL_LIBRARY "${_OAL_ROOT}/lib/libOpenAL32.dll.a"
        CACHE FILEPATH "OpenAL import library (vendored MinGW prebuilt)" FORCE)
    unset(_OAL_ROOT)
endif()

# Emscripten ships an OpenAL implementation in its sysroot, linked via the
# `-lopenal` flag rather than a discoverable .a archive. PsyCross calls
# find_package(OpenAL REQUIRED), so seed the cache vars FindOpenAL's
# FindPackageHandleStandardArgs checks (it only verifies they're set and
# not -NOTFOUND). Headers live in the toolchain sysroot, found
# automatically by emcc; OPENAL_INCLUDE_DIR just has to point somewhere
# real for FPHSA. The -lopenal flag also gets appended on the final
# browser target's link line (see games/spyro-1/CMakeLists.txt).
if(EMSCRIPTEN)
    set(OPENAL_INCLUDE_DIR "${CMAKE_SYSROOT}/include"
        CACHE PATH "OpenAL include dir (Emscripten sysroot)" FORCE)
    set(OPENAL_LIBRARY "-lopenal"
        CACHE STRING "OpenAL link flag (Emscripten built-in)" FORCE)
endif()

# SDL3 is built from the third_party/SDL submodule, pinned to a recent
# main commit for Steam Controller (2026) support. See
# notes/plans/openpete-sdl3-upgrade.md. Static link, no shared lib, no
# SDL test apps — produces the SDL3::SDL3-static target consumed by
# openpete_core and PsyCross. Replaces the old find_package(SDL2) +
# SDL2-mingw prebuilt path.
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
# XTEST (X11 synthetic-input extension) is not used by openpete — it only
# polls input, never injects it. Disabling drops the libXtst-dev build
# dependency so a from-source SDL build needs no extra system packages.
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL)

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
# pcsx_redux_gte is a GTE trace-logger reference (diagnostic only, not on
# the runtime path) and requires C++23. MinGW-GCC 10 — the version Ubuntu
# 22.04 ships in `gcc-mingw-w64-x86-64-posix` — caps at C++20, so default
# the option OFF when cross-compiling. Native Linux/macOS keep it ON.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(_OPENPETE_PCSX_REDUX_GTE_DEFAULT OFF)
else()
    set(_OPENPETE_PCSX_REDUX_GTE_DEFAULT ON)
endif()
option(OPENPETE_BUILD_PCSX_REDUX_GTE
       "Build third_party/pcsx_redux_gte as a static lib"
       ${_OPENPETE_PCSX_REDUX_GTE_DEFAULT})
unset(_OPENPETE_PCSX_REDUX_GTE_DEFAULT)
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

# Host-side unit tests (no PSX boot, just pure-function checks).
option(OPENPETE_BUILD_TESTS "Build host-side unit tests" ON)
if(OPENPETE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests/unit)

    # Integration test: gameplay-state-vs-presentation determinism.
    # Asserts that running the headless demo at multiple aspect ratios
    # produces bit-identical gameplay state. Slow (boots two PSX
    # sessions); labeled `slow` so plain `ctest` skips it. Run with
    # `ctest -L slow` or directly via the script.
    add_test(NAME aspect_determinism
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_aspect_determinism.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=8000)
    set_tests_properties(aspect_determinism PROPERTIES
        LABELS "slow;determinism"
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Hor+ presentation symmetry — boots the headless demo at 4:3, 16:9
    # and 21:9, takes a screenshot at the same frame, and pixel-diffs the
    # central 4:3 region. Asserts that widening the GTE projection does
    # not perturb the canonical 4:3 view. Currently RED: cyclorama
    # stretching + sub-pixel projection drift leak into the central
    # region. Closing the leak makes this gate green.
    add_test(NAME aspect_screenshot_symmetry
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_aspect_screenshot.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frame=850)
    set_tests_properties(aspect_screenshot_symmetry PROPERTIES
        LABELS "slow;determinism"
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --record video-capture smoke test. Boots the headless demo with
    # --record and uses ffprobe to assert a playable VFR H.264 stream with
    # even dimensions, the reported frame count, monotonic non-uniform PTS,
    # and window-independent (bars-free) capture at 21:9. Skips cleanly
    # (exit 64) if the disc/binary/ffprobe are absent. See
    # notes/plans/openpete-record-video.md.
    add_test(NAME record_smoke
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_record.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=300)
    set_tests_properties(record_smoke PROPERTIES
        LABELS "slow;tooling"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Channel registry audit — fast static check (no PSX boot). Cross-
    # references manifest/render_channels.toml against the recompiler's
    # address-keyed substitution table; fails on missing or orphaned
    # substitutions. See notes/plans/openpete-determinism-enforcement.md
    # Phase 4.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME channel_registry_audit
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_channel_registry.py)
        set_tests_properties(channel_registry_audit PROPERTIES
            LABELS "determinism"
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        # Channel signature anchors (hook-anchor plan step 2): calibrate +
        # verify that every determinism-critical channel site is uniquely
        # anchorable by an instruction-pattern signature (local or
        # function-scoped) in the reference binary, and that the matcher
        # relocates a spliced window (self-test). Skips cleanly if the regen
        # artifacts (PSX.EXE / discovered_functions.log) aren't present.
        add_test(NAME channel_signatures
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_channel_signatures.py
                         --out ${CMAKE_CURRENT_BINARY_DIR}/channel_signatures.txt)
        set_tests_properties(channel_signatures PROPERTIES
            LABELS "determinism"
            TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # PSX oracle gate — verify openpete gameplay-state RAM matches a
    # DuckStation (real SCPH1001) baseline tick-for-tick over the 20k-tick
    # attract-demo soak (4+ level loads + gameplay). The 3 CD-load pool
    # regions are tolerated only while a load transient heals; every other
    # region must match bit-exact (see check_psx_oracle.sh / the manifest's
    # cd_load_transient flag). Skipped automatically if the baseline trace
    # is missing (regenerate with scripts/capture_psx_oracle_ds.sh, see
    # docs/duckstation-oracle.md). Phase 2 of
    # openpete-determinism-enforcement.md + openpete-psx-oracle-ci.md.
    add_test(NAME psx_oracle_match
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_match PROPERTIES
        LABELS "slow;oracle"
        TIMEOUT 600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1")

    # Matrix determinism gate — broaden coverage from one (aspect,
    # scenario) cell to a sweep across aspects × fovs × scenarios.
    # Same expected-red-vs-regression policy as aspect_determinism.
    # Slow (boots N PSX sessions); only runs under `ctest -L slow`.
    # Phase 6 of openpete-determinism-enforcement.md.
    add_test(NAME determinism_matrix
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=2000)
    set_tests_properties(determinism_matrix PROPERTIES
        LABELS "slow;determinism;matrix"
        TIMEOUT 1800
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Soak determinism gate — long-replay mode for hunting slow-burn
    # / late-frame channel leaks the 2000-frame matrix cells miss.
    # Iterates the full BK2 corpus (minus tas/frame-time-test) at
    # 4:3 vs 16:9, default fov, with --bk2-stop-on-end so each cell
    # exits on movie end. Not part of the default ctest run; opt in
    # via `ctest -L soak`. Substantially slower than matrix.
    add_test(NAME determinism_soak
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --soak)
    set_tests_properties(determinism_soak PROPERTIES
        LABELS "slow;determinism;soak"
        TIMEOUT 7200
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
