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
    # legal). Header lookup: <AL/al.h> -> ${OPENAL_INCLUDE_DIR}/AL/al.h.
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

# SDL3 is built from the third_party/SDL submodule, pinned to the
# release-3.4.x branch (currently the release-3.4.16 tag) rather than to
# main: the release branch carries the 2026 Steam Controller support the
# pin was originally taken from main for, and carries it further along -
# input report formats, touchpads and the rumble fix all landed there
# after that commit. Static link, no shared lib, no SDL test apps -
# produces the SDL3::SDL3-static target consumed by openpete_core and
# PsyCross.
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
# XTEST (X11 synthetic-input extension) is not used by openpete - it only
# polls input, never injects it. Disabling drops the libXtst-dev build
# dependency so a from-source SDL build needs no extra system packages.
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL)

# Which games to enable. A game whose generated/ has no recompiled output
# warns and skips itself, so listing one costs nothing until it is regenerated.
set(OPENPETE_GAMES "spyro-1;spyro-2;spyro-3" CACHE STRING "Games to build (semicolon-separated)")

# In-process GLSL->SPIR-V for the mod shader compile service. glslang
# 15.4.0 pinned:
# C++17 under GCC 10 (the MinGW cross compiler's ceiling), targets
# vulkan1.0 like the engine's own glslangValidator -V builds. OFF is a
# degraded build: mods' prebuilt .spv still load, GLSL sources refuse
# with a log pointer.
option(OPENPETE_BUILD_GLSLANG
       "Build third_party/glslang for the mod shader compile service" ON)
if(OPENPETE_BUILD_GLSLANG)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)         # no SPIRV-Tools dep
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glslang)
endif()

# PsyCross integration. Builds the static lib and links it
# into openpete_core; psyq_thunks.cpp routes recompiled PSYQ calls into
# PsyCross. PsyCross's CMake calls find_package(OpenAL REQUIRED), so a
# Linux build needs `libopenal-dev`. Disable with -DOPENPETE_BUILD_PSYCROSS=OFF
# if you need a non-PsyCross link path (the per-game thunks will then
# fall through to the abort-on-call weak stubs in psyq_stubs.c).
option(OPENPETE_BUILD_PSYCROSS "Build third_party/PsyCross as a static lib" ON)
if(OPENPETE_BUILD_PSYCROSS)
    add_subdirectory(third_party/PsyCross)
    # PsyCross walks guest-RAM primitives and ordering tables through
    # type-punned pointers, like the runtime (core/CMakeLists.txt).
    target_compile_options(psycross_static PRIVATE -fno-strict-aliasing)
endif()

# Native render foundation: RHI + Vulkan backend (game-agnostic, under
# core/). Added before core/ so openpete_core can link openpete_rhi.
add_subdirectory(core/rhi)

add_subdirectory(core)

# Host-side unit tests (no PSX boot, just pure-function checks). Testing is
# enabled before the game directories so their per-game ctest rows
# (state_regions_derivation_<game>) register.
option(OPENPETE_BUILD_TESTS "Build host-side unit tests" ON)
if(OPENPETE_BUILD_TESTS)
    enable_testing()
endif()

# Rendered header documentation (Doxyfile at the source root). Not part of
# ALL: `cmake --build --preset <preset> --target openpete_docs` writes
# <build>/docs/html and <build>/docs/xml; without doxygen the target prints
# a SKIP line and succeeds.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(openpete_docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/docs
        COMMAND ${CMAKE_COMMAND} -E env OPENPETE_DOXYGEN_OUT=${CMAKE_BINARY_DIR}/docs
                ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "doxygen: rendering the headers into ${CMAKE_BINARY_DIR}/docs"
        VERBATIM)
else()
    add_custom_target(openpete_docs
        COMMAND ${CMAKE_COMMAND} -E echo
                "openpete_docs: SKIP, doxygen not found (sudo apt install doxygen)"
        VERBATIM)
endif()

# Codebase reference pages for the documentation site (docs/mkdocs.yml).
# Not part of ALL: `cmake --build --preset <preset> --target code_docs`
# rewrites docs/src/developer/reference/ from the doxygen comments, parsed
# by the documentation ratchet over this tree's compile database.
# Stamp-guarded: it reruns when a scanned source or the generator changes.
file(GLOB_RECURSE _code_docs_inputs CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.c ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h ${CMAKE_CURRENT_SOURCE_DIR}/core/*.hpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.c ${CMAKE_CURRENT_SOURCE_DIR}/games/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.h ${CMAKE_CURRENT_SOURCE_DIR}/games/*.hpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.inc)
list(FILTER _code_docs_inputs EXCLUDE REGEX
     "/(generated|third_party|build)/|/games/(spyro-1/spyro-1|spyro-2/spyro-2|spyro-3/spyro-3)/")
set(_code_docs_stamp ${CMAKE_CURRENT_BINARY_DIR}/codedocs.stamp)
add_custom_command(
    OUTPUT  ${_code_docs_stamp}
    COMMAND ${CMAKE_COMMAND} -E env
            OPENPETE_DOCS_DB=${CMAKE_BINARY_DIR}/compile_commands.json
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_code_docs.py
    COMMAND ${CMAKE_COMMAND} -E touch ${_code_docs_stamp}
    DEPENDS ${_code_docs_inputs}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_code_docs.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/doc_ratchet.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/ratchet_lib.py
            ${CMAKE_BINARY_DIR}/compile_commands.json
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating the codebase reference (gen_code_docs.py)"
    VERBATIM)
add_custom_target(code_docs DEPENDS ${_code_docs_stamp})
add_dependencies(code_docs openpete_generated_headers)

foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt")
        add_subdirectory(games/${game})
    else()
        message(WARNING "OPENPETE_GAMES requested '${game}' but games/${game}/CMakeLists.txt is missing - skipping")
    endif()
endforeach()

# surface_dump: each built game writes its OPENPETE_SURFACE_DUMP to
# <build>/surface/<game>.json. Not ALL and absent from a cross build, which
# cannot run the binaries. tools/surface_docs.py baseline turns a dump into
# the committed packaging/surface/<game>.json the docs read.
if(NOT CMAKE_CROSSCOMPILING)
    set(_surface_cmds "")
    set(_surface_deps "")
    foreach(game IN LISTS OPENPETE_GAMES)
        string(REPLACE "-" "" _surface_exe "${game}")
        set(_surface_exe "openpete-${_surface_exe}")
        if(TARGET ${_surface_exe})
            list(APPEND _surface_cmds
                 COMMAND ${CMAKE_COMMAND} -E env
                         OPENPETE_SURFACE_DUMP=${CMAKE_BINARY_DIR}/surface/${game}.json
                         $<TARGET_FILE:${_surface_exe}>)
            list(APPEND _surface_deps ${_surface_exe})
        endif()
    endforeach()
    if(_surface_deps)
        add_custom_target(surface_dump
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/surface
            ${_surface_cmds}
            COMMENT "Writing the surface dumps to ${CMAKE_BINARY_DIR}/surface/"
            VERBATIM)
        add_dependencies(surface_dump ${_surface_deps})
        # The docs coverage fence, per build: every release id of a shipped
        # game has a User docs entry. Warnings only, so a half-written row
        # in one session does not fail another session's build; ctest
        # surface_coverage and the release scripts refuse. Linux only, where
        # the built binaries run, and only where the debug block is compiled
        # (the check reads every row; the shipped presets hide the block).
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT OPENPETE_HIDE_DEBUG_FLAGS)
            add_custom_target(openpete_surface_coverage ALL
                COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/surface_coverage.py
                        check --warn --build-dir ${CMAKE_BINARY_DIR}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "docs coverage fence: every release id has a User docs entry"
                VERBATIM)
            add_dependencies(openpete_surface_coverage ${_surface_deps})
        endif()
    endif()
endif()

if(OPENPETE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/CMakeLists.txt")
    add_subdirectory(tests/unit)

    # RESOURCE_LOCK openpete_game: every test below that launches a game
    # process holds it, so ctest never co-schedules two of them. A second
    # game process invalidates any timing measurement and is refused
    # outright by scripts/fences/bench_guard.py (tests/harness/bench-corpus.py,
    # tests/harness/baseline-terrain-arena.py, tests/harness/check_pacing.sh), and a
    # run that shares the machine also overruns its TIMEOUT. The
    # presentation-labelled per-feature tests keep their parallelism:
    # they carry no timing threshold and run under `ctest -L presentation`.

    # Modding-framework PoC-ladder gate (modtest v0): chain-order matrix,
    # observer-neutrality vs the PSX oracle, raw-tier warning, and the
    # double-gems expected-divergence assertion. Slow (four headless
    # sessions); `ctest -L mods` or tests/harness/check_mods.sh directly.
    add_test(NAME mod_poc_ladder
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mods.sh)
    set_tests_properties(mod_poc_ladder PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # ImGui containment gate: the mods fixture proves the table binds
    # headless, but every guard it installs runs only inside a UI section
    # body, and a body needs an open overlay - so this leg opens one in a
    # real window and reads engagement, the error bracket and the
    # out-of-body refusal off the log. Skips (64) with no display.
    add_test(NAME mod_imgui
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mod_imgui.sh)
    set_tests_properties(mod_imgui PROPERTIES
        LABELS "mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Per-channel material gate: turns every mods/channel-gallery row into
    # an assertion (a row that should paint must, a KNOWN GAP row must
    # not), plus refine-vs-declarative equivalence - including the
    # many-distinct-outcome case, which is the shape that caught
    # per-instance slots being stamped after their intern table was reset.
    # Neither check_mods nor native_visual_regression runs a mod material
    # at all, so this is the only gate that exercises the selector seam.
    # Slow (a dozen headless sessions); `ctest -L mods` or the script.
    add_test(NAME mod_channel_selectors
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mod_channels.sh)
    set_tests_properties(mod_channel_selectors PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # A Windows mod may include the SDK headers and <windows.h>, whose
    # windef.h defines RECT. The SDK's PSYQ types carry a PSX_ prefix to
    # keep clear of it; this cross-compiles one such translation unit and
    # fails on a redefinition. Skips (64) with no mingw cross compiler.
    add_test(NAME sdk_windows_include
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sdk_windows_include.sh
                     --sdk ${CMAKE_BINARY_DIR}/games/spyro-1/sdk)
    set_tests_properties(sdk_windows_include PROPERTIES
        LABELS "mods"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Freecam-corpus gate for the terrain near-LP gapfill classifier -
    # replays recorded problematic freecam spots (holes that must stay
    # filled, walls/arches the fill must not touch) and asserts metric
    # expectations with the feature on vs off. Slow (boots ~7 headless
    # sessions); `ctest -L gapfill` or the script directly.
    add_test(NAME gapfill_corpus
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_gapfill_corpus.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(gapfill_corpus PROPERTIES
        LABELS "slow;gapfill"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Hor+ presentation symmetry - boots the headless demo at 4:3, 16:9
    # and 21:9, takes a screenshot at the same frame, and pixel-diffs the
    # central 4:3 region. Asserts that widening the GTE projection does
    # not perturb the canonical 4:3 view. Currently RED: cyclorama
    # stretching + sub-pixel projection drift leak into the central
    # region. Closing the leak makes this gate green.
    add_test(NAME aspect_screenshot_symmetry
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_aspect_screenshot.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frame=850)
    set_tests_properties(aspect_screenshot_symmetry PROPERTIES
        LABELS "slow;determinism"
        RESOURCE_LOCK openpete_game
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Host disc-read trace: two Spyro 1 boots to VBL 600 (disc cache on and
    # off) under OPENPETE_DISC_TRACE; every row parses, the exe, libcd and
    # woah paths appear, the woah NSF decodes, and the libcd sector sum
    # equals the CdRead thunk's.
    # Exit 64 = no binary or disc.
    add_test(NAME disc_trace
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_disc_trace.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(disc_trace PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 2/3 cutscene speech ring: per row, run to the speech voice's
    # key-off; every ring refill lands behind the guest voice head (0
    # overwrite, 0 stale samples), the OpenAL offset takes no slip step,
    # and each chunk's body correlates >= 0.95 with its payload.
    # Exit 64 = no binary or disc for any row.
    add_test(NAME cutscene_audio
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cutscene_audio.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cutscene_audio PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # CD drive head: the CdlGetlocP head model replayed against every
    # reference answer of the Spyro 2/3 20k goldens (|diff| <= 2 LBA), and
    # the XA decoder head's rate on a Spyro 2 boot run (37609/15000 LBA per
    # VBL +- 0.0005). Exit 64 = no golden, binary or disc for any row.
    add_test(NAME cd_head
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cd_head.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cd_head PROPERTIES
        LABELS "slow;audio;cd"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Disc boot-order profiles: per game, the committed profile equals the
    # generator's output over the committed boot trace, and the loader
    # simulation (tools/disc_boot_order.py) finds the lowest modelled
    # bandwidth at which file order misses no game read and asserts, one
    # step below it, that profile order misses none while file order does.
    # Exit 3 (reported as skipped) = VACUOUS: file order misses nothing even
    # at the lowest bandwidth. The selftest covers the profile parser and
    # the generator's determinism.
    foreach(_dbo_game spyro-1 spyro-2 spyro-3)
        add_test(NAME disc_boot_order_${_dbo_game}
                 COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/disc_boot_order.py
                         check --game ${_dbo_game}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(disc_boot_order_${_dbo_game} PROPERTIES
            LABELS "disc"
            SKIP_RETURN_CODE 3)
    endforeach()
    add_test(NAME disc_boot_order_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_disc_boot_order.py)
    set_tests_properties(disc_boot_order_selftest PROPERTIES LABELS "disc")

    # Sfx pack census: --dump-sfx-pack over the Spyro 1 disc through the
    # disc reader; the counters, the WAV set's name digest and the trace
    # rows are pinned to the retail disc. Exit 64 = no binary or disc.
    add_test(NAME sfx_pack
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sfx_pack.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(sfx_pack PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Terrain-arena allocation legs: an arena's memory type refused
    # (OPENPETE_RHI_FAIL_MEMTYPE) reports each failed attempt and ends in the
    # cache's init-failure path with the run continuing on the CPU terrain
    # walk; the uninjected leg names the type it allocated on. Two headless
    # llvmpipe boots. Exit 64 = no binary or disc.
    add_test(NAME tercache_alloc
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_alloc.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(tercache_alloc PROPERTIES
        LABELS "slow;renderer"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Terrain-arena high-water baseline. Two legs, each a 35-level x 2-pose
    # headless sweep: `regen` re-measures at a capacity no content reaches
    # and must reproduce baselines/terrain-arena/ exactly, with no censored
    # row (a high-water mark equal to its capacity) and no sector claimed
    # back for want of arena room; `stock` repeats the static sweep at the
    # generated caps, where no level at either pose may claim a sector back;
    # `overflow` proves the growth path on a level squeezed below its demand.
    # Its own label: far too long for the per-build ctest run. Exit 64 = no
    # binary or disc.
    add_test(NAME terrain_arena_baseline
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/baseline-terrain-arena.py check
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(terrain_arena_baseline PROPERTIES
        LABELS "slow;terrain-baseline"
        RESOURCE_LOCK openpete_game
        TIMEOUT 7200
        SKIP_RETURN_CODE 64)

    # Cross-level accumulation row: the 40,000-VBL attract demo crosses
    # several level loads. It measures accumulation - one arena replacement
    # fulfilled per level load, and, per arena, the first high-water mark
    # after each edge below that arena's peak in the segment before it -
    # and reports each arena's exit mark against the union of the visited
    # levels' baseline rows as a demand figure, not an assertion: the demo
    # camera traverses a level while the baseline's two poses are static.
    # Without the level-edge replacement the marks never fall and climb
    # toward the caps over a session, far past any single level's demand.
    add_test(NAME terrain_arena_demo
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/baseline-terrain-arena.py demo
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(terrain_arena_demo PROPERTIES
        LABELS "slow;terrain-baseline"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64)

    # Overflow-buffer row: one level whose arenas cannot hold it, against the
    # same level at the generated caps. It asserts that a full arena claims
    # sectors back only until the overflow buffer lands (afail rises, an
    # overflow is installed, afail then stops rising), that the generated caps
    # need neither, and that the squeezed run's image is identical to the
    # stock-cap run's, which is what makes the claim-back invisible.
    add_test(NAME terrain_arena_overflow
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/baseline-terrain-arena.py overflow
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(terrain_arena_overflow PROPERTIES
        LABELS "slow;terrain-baseline"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64)

    # Cognitive-complexity ratchet, the ctest spelling of the per-build
    # check (core/CMakeLists.txt, openpete_complexity_ratchet). Exit 64 =
    # no compile database.
    add_test(NAME complexity_ratchet
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/complexity_ratchet.py
                     check --ctest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(complexity_ratchet PROPERTIES
        LABELS "fence"
        ENVIRONMENT "OPENPETE_COMPLEXITY_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
        SKIP_RETURN_CODE 64)
    # The fence's own tests: the decision function per rule and the hook end
    # to end in a scratch repo.
    add_test(NAME complexity_ratchet_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_complexity_ratchet.py)
    set_tests_properties(complexity_ratchet_selftest PROPERTIES LABELS "fence")
    # Function-documentation ratchet, the ctest spelling of the per-build
    # check (core/CMakeLists.txt, openpete_doc_ratchet). Exit 64 = no
    # compile database.
    add_test(NAME doc_ratchet
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/doc_ratchet.py
                     check --ctest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(doc_ratchet PROPERTIES
        LABELS "fence"
        ENVIRONMENT "OPENPETE_DOCS_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
        SKIP_RETURN_CODE 64)
    # The documentation fence's own tests: the decision function per rule,
    # the touched rule, the per-entry merge, the fixture controls (and a
    # tampered copy that must fail), and the hook end to end in a scratch
    # repository, including a cherry-pick across a landing.
    add_test(NAME doc_ratchet_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_doc_ratchet.py)
    set_tests_properties(doc_ratchet_selftest PROPERTIES LABELS "fence")
    # The baselines' merge driver: the three-way merge by row key, and a
    # cherry-pick across a landing in a scratch repository.
    add_test(NAME ratchet_tsv_merge_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_ratchet_tsv_merge.py)
    set_tests_properties(ratchet_tsv_merge_selftest PROPERTIES LABELS "fence")
    # The golden-image fence: the path rule and the pre-commit dispatcher end
    # to end in a
    # scratch repository (staged image shapes, the bypass, and a complexity
    # refusal through the dispatcher).
    add_test(NAME golden_png_guard_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_golden_png_guard.py)
    set_tests_properties(golden_png_guard_selftest PROPERTIES LABELS "fence")
    # The recomp identity pair of the blessed manifests: the psx.map symbol
    # digest is invariant under the build path, address width and section
    # order; the generated-tree digest ignores stale files and refuses a
    # missing output; rebless-identity refuses outside the capture image and
    # on a dirty recomp input, else rewrites only the two columns.
    add_test(NAME presentation_identity_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_presentation_identity.py)
    set_tests_properties(presentation_identity_selftest PROPERTIES LABELS "fence")

    # The image-side gates below (native_visual_regression, the presentation
    # golden rows and the dragon tier) are defined over the pinned capture
    # image (tests/harness/linux-capture.Dockerfile). Run from a host build
    # tree, each re-enters itself through scripts/golden-capture-container.sh, which
    # rebuilds this source tree with the image's toolchain into
    # build/linux-release-container, overriding the --build-dir passed here,
    # and captures with that binary; a machine without podman/docker or the
    # image reports SKIP (64). The rows share that one build directory, so they
    # carry RUN_SERIAL (the wrapper's lock on it also covers a manual run
    # beside ctest), and each timeout includes the in-image build (~4 min
    # from fresh at nproc 16; priced at 10 min).

    # Native renderer visual-regression gate. Renders the native `-vk`
    # frame headless through the software rasterizer (surfaceless Vulkan, no
    # GPU, no display) at seven attract-demo draw-ticks and compares each
    # tile's verdict digest against its row of
    # games/spyro-1/native-tick-corpus.manifest; the capture is
    # byte-deterministic, so the compare is exact. See
    # tests/golden/spyro-1/native-vk/README.md.
    add_test(NAME native_visual_regression
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_native_visual.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(native_visual_regression PROPERTIES
        LABELS "slow;visual"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        # Headroom over the game's own --max-runtime-seconds 600 so the
        # in-loop self-limit always fires first (equal timeouts raced, and a
        # hung game loop can't fire its own limit), plus the in-image build.
        TIMEOUT 1500
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Presentation-golden image gate (the image-side counterpart to the psx
    # oracle). Captures the native -vk corpus (canonical tick-keyed +
    # deterministic alpha=0.5 intermediate) headless under llvmpipe and compares
    # each tile's verdict digest against its row of the committed manifest
    # tests/golden/<game>/presentation/blessed.tsv; no reference image is read
    # on a pass. A clean tree is pixel-identical, so any divergence is a
    # presentation regression the gameplay oracle can't see. Covers the
    # fanned-out corpus: the skip-to-level bk2s (capped to a few evenly-spaced
    # samples each, 16:9, both LOD modes) plus the debug-freecam poses. Re-bless
    # an intended change with tests/harness/check_presentation_golden.sh --bless
    # or a selective promote; scope a fast subset with --filter=<bk2-substr>.
    # Skips (exit 64) if the capture image, binary, disc, Vulkan loader or PIL
    # are absent. The spyro-1 row also runs the credits-roll interpolation leg
    # (tests/harness/check_credits_interp.sh) so it cannot be skipped
    # separately. The gate takes --game SLUG and derives binary, disc, bk2 dir,
    # manifests and golden from it.
    add_test(NAME presentation_golden
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_presentation_golden.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(presentation_golden PROPERTIES
        LABELS "slow;presentation"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        # The whole corpus replays ~53 bk2 movies + the freecam poses (~35 min),
        # plus the in-image build; headroom so a loaded host doesn't spuriously
        # time out.
        TIMEOUT 4200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # Spyro 2 and Spyro 3: the same gate, scoped by --game, over each game's
    # games/<game>/corpus manifests (~10 min each plus the in-image build).
    foreach(_pg_game spyro2 spyro3)
        string(REPLACE "spyro" "spyro-" _pg_slug ${_pg_game})
        add_test(NAME presentation_golden_${_pg_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_presentation_golden.sh
                         --game ${_pg_slug} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(presentation_golden_${_pg_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            RUN_SERIAL TRUE
            TIMEOUT 2400
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    # Spyro 1 dragon-rescue tier (tests/harness/check_dragon_golden.sh): one
    # pose row per dragon rescue, sampled every 60 VBLs through the cutscene,
    # stock LOD, golden tests/golden/spyro-1/dragons. Labelled `dragons`, not
    # `presentation`, so `ctest -L presentation` and the default gate chains
    # skip it; the harness header states when it runs.
    add_test(NAME dragon_golden_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_dragon_golden.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(dragon_golden_spyro-1 PROPERTIES
        LABELS "slow;dragons"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        TIMEOUT 4200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Skip-target rows: every --skip-to-cutscene (and --skip-to-area) row of
    # games/<slug>/corpus/skip-targets.manifest runs headless; a row passes only when
    # the skip's release line carries the row's engagement values (the cutscene
    # index, the credits sequence, the return level, the level and area words),
    # and a negative control with the fail-safe forced must fail. One row per
    # game with a manifest.
    foreach(_skip_game spyro-1 spyro-2 spyro-3)
        add_test(NAME skip_targets_${_skip_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_skip_targets.sh
                         --game ${_skip_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(skip_targets_${_skip_game} PROPERTIES
            LABELS "slow;skip-targets"
            RESOURCE_LOCK openpete_game
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Option-parse exit codes: --help exits 0, a typed non-finite --fov exits
    # 3, the same value from openpete.toml warns and keeps the default, and
    # fov = 0 parses as the game's projection. Parse-only legs plus one
    # 120-VBL headless boot under the bad config file.
    foreach(_cli_game spyro-1 spyro-2 spyro-3)
        add_test(NAME config_cli_${_cli_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_config_cli.sh
                         --game ${_cli_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(config_cli_${_cli_game} PROPERTIES
            LABELS "tooling"
            RESOURCE_LOCK openpete_game
            TIMEOUT 300
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The surface dump (OPENPETE_SURFACE_DUMP): each game writes its flags,
    # registry rows, menu rows, key actions and vocabularies with their
    # declared defaults, and the selfcheck parses every flag at its declared
    # default against the default parse. A broken default (the negative
    # control) must fail it. Parse-only; no disc.
    foreach(_surface_game spyro-1 spyro-2 spyro-3)
        add_test(NAME surface_dump_${_surface_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_surface_dump.sh
                         --game ${_surface_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(surface_dump_${_surface_game} PROPERTIES
            LABELS "tooling"
            TIMEOUT 120
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The User docs' surface macros: each committed macro is what
    # tools/surface_docs.py's converter writes for the literal it expands
    # to, and a per-kind negative control (a mutated def, binding, config
    # key or flag spelling) changes the expansion. Then every TOML block
    # and command line on the User pages, macros expanded, passes
    # --config-report, and a refused value is refused. No disc.
    add_test(NAME docs_surface_identity
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/surface_docs.py identity)
    set_tests_properties(docs_surface_identity PROPERTIES
        LABELS "tooling"
        TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # The docs coverage fence (scripts/fences/surface_coverage.py): every
    # release flag, row, menu row, key action and directive of a shipped
    # game has a docs-map entry or resolves through one, every release value
    # declares a default, and the parser, --help, schema and dump agree.
    # The selftest feeds one negative control per refusal kind.
    add_test(NAME surface_coverage
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/surface_coverage.py
                     check --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(surface_coverage PROPERTIES
        LABELS "fence"
        TIMEOUT 120
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME surface_coverage_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_surface_coverage.py
                     --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(surface_coverage_selftest PROPERTIES
        LABELS "fence"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME docs_surface_snippets
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/surface_docs.py snippets
                     --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(docs_surface_snippets PROPERTIES
        LABELS "tooling"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The 3D glyph text gate: the settings menu, the OSD toasts and the
    # performance readout publish glyph vertices from the game's own font,
    # decoded from guest RAM (Spyro 1) or read from the disc (Spyro 2 and 3,
    # in every level). Rows in games/<slug>/corpus/text-font-corpus.manifest.
    # Windowed (the producer is inert headless); exits 64 without a display.
    foreach(_font_game spyro-1 spyro-2 spyro-3)
        add_test(NAME text_font_${_font_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_text_font.sh
                         --game ${_font_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(text_font_${_font_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Terrain triangle-swing census under --lod original, where the near/HQ
    # classifier keeps the game's foreground band (--lod extended pins the
    # highest tier on every front face by design). Replays a camera-swing bk2
    # headless and reads the terrain triangle count of every canonical walk
    # over a fixed VBL window; fails on any adjacent-walk jump of 1000
    # triangles or more, the signature of a near/HQ subdivide routed without
    # the band. Engagement floor: at least 300 walks in the window and a
    # nonzero mean near-face count. Spyro 3 is the game whose draw
    # bins the HP list by distance; the check skips for the others.
    add_test(NAME nearhq_swing_spyro3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_nearhq_swing.sh
                     --game spyro-3 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(nearhq_swing_spyro3 PROPERTIES
        LABELS "presentation;terrain"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Player sub-tick continuity. Per row of
    # games/<slug>/corpus/player-subtick-corpus.manifest, two headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0 and at alpha 1, and
    # the player model's screen centroid and bounding box read per present:
    # the alpha-0 present of one tick interval must equal the alpha-1 present
    # of the interval before it to 0.01 px, and the alpha-1 present must equal
    # the tick present exactly. A part placed from the wrong tick's data fails
    # the first and passes the second. Fails a leg whose present is not
    # bracketed, so a row cannot pass on tick presents alone.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME player_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_player_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(player_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;player"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Moby sub-tick continuity. Per row of
    # games/<slug>/corpus/moby-subtick-corpus.manifest, two headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0 and at alpha 1, and
    # every animated moby's keyframe pose and screen centroid read per
    # present: the previous pose rebuilt at each leg must equal the pose the
    # record of the tick before carries, and a leg where that record blends
    # out of a clip must seam no worse than the same moby's other legs. Fails
    # as vacuous when the row measures fewer bracketed or blending legs than
    # its floor.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME moby_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(moby_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Vulkan validation errors on the boot-to-level path. One headless run per
    # game under --renderer vk with VK_LAYER_KHRONOS_validation loaded, from
    # boot through --skip-to-level to a native screenshot, asserting zero
    # `Validation Error` lines. API misuse the driver tolerates changes no
    # pixel and no gameplay byte, so neither the presentation golden nor the
    # oracle can see it. Skips when the binary or the disc is missing; fails as
    # vacuous when the layer did not load or the render path drew nothing.
    foreach(_vkval_game spyro-1 spyro-2 spyro-3)
        add_test(NAME vk_validation_${_vkval_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vk_validation.sh
                         --game ${_vkval_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(vk_validation_${_vkval_game} PROPERTIES
            LABELS "presentation;validation"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Runahead prediction contract. Per row of
    # games/<slug>/corpus/runahead-corpus.manifest, two headless replays of one bk2:
    # the native capture at the row's VBL under --runahead <depth>, and the
    # capture at that VBL plus 2*<depth> without runahead. --runahead presents
    # the state <depth> ticks ahead of the committed timeline, so the two are
    # the same picture and any pixel between them is a canonical presentation
    # latch that a speculative tick wrote. Fails as vacuous when the run never
    # armed runahead, retired no led prediction, or had its speculation
    # adopted into the real timeline before the row's VBL.
    foreach(_ra_game spyro-1 spyro-2 spyro-3)
        add_test(NAME runahead_ident_${_ra_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_runahead_ident.sh
                         --game ${_ra_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(runahead_ident_${_ra_game} PROPERTIES
            LABELS "presentation;runahead"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # 2D-UI carry identity across sub-tick presents. Per row of
    # games/<slug>/corpus/carry-subtick-corpus.manifest, three headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0.25, 0.5 and 0.75,
    # and every interpolated carry triangle checked for a crossed pair: one
    # whose partner is another entry's predecessor, so the interpolation sweeps
    # the element between two different sprites for the length of a tick. Fails
    # as vacuous when the row interpolates fewer triangles than its floor.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME carry_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_carry_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(carry_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;carry"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Sub-tick interpolation of the gem-glint stars. Per row of
    # games/<slug>/corpus/glint-subtick-corpus.manifest, six headless replays of one
    # bk2 - the two canonical ticks of an interval and the intermediate present
    # inside it, each captured with the glint feed on and off - and the
    # leave-one-out mask's per-star centroid compared against
    # (1-alpha)*canon(V-2) + alpha*canon(V). Spyro 1 has no such routine and
    # registers no row. Fails as vacuous when a leg's mask is empty, when a
    # named gem resolves ambiguously, or when its canonical motion over the
    # interval is below the row's floor.
    # Sub-tick interpolation of Zoe's zap. Per row of
    # games/<slug>/corpus/zap-subtick-corpus.manifest, three headless replays pinning
    # the intermediate present at alpha 0, 0.5 and 1, with the record's own
    # projected head and tail traced per present and each required to sit on
    # the alpha blend of the two canonical ticks. Spyro 1 has no such routine
    # and registers no row. Fails as vacuous when a leg feeds nothing, when the
    # legs pair different records, or when the canonical motion is below the
    # row's floor.
    foreach(_zap_game spyro-2 spyro-3)
        add_test(NAME zap_subtick_${_zap_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_zap_subtick.sh
                         --game ${_zap_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(zap_subtick_${_zap_game} PROPERTIES
            LABELS "presentation;zap"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The blended-world draw order on the identity instant: the game render's
    # own block against the sub-tick replay's, run for run, keyed by provenance
    # and blend class. The classes composite in emission order and the blend
    # equations do not commute once a channel clamps, so a replay that emits
    # its producers in a different order than the guest linked them draws a
    # picture the canonical present never produced. Fails as vacuous when
    # either leg draws no blended run.
    foreach(_bwo_game spyro-2)
        add_test(NAME blendorder_ident_${_bwo_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_blendorder_ident.sh
                         --game ${_bwo_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(blendorder_ident_${_bwo_game} PROPERTIES
            LABELS "presentation;blendorder"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME glint_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_glint_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(glint_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;glint"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME glow_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_glow_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(glow_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;glow"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Spyro 2 has no shadow row: its two candidate movies carry only shadows
    # whose moby is static in the world or none at all over the window, and a
    # blob that cannot move measures nothing. The harness SKIPs there for want
    # of the manifest until a movie with a moving shadowed moby is recorded.
    foreach(_subtick_game spyro-1 spyro-3)
        add_test(NAME shadow_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_shadow_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(shadow_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;shadow"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The remaining render paths of the shared feeder harness. Each is one
    # channel of tests/harness/check_feeder_subtick.sh, with its own manifest per
    # game; the game list per path is the set whose rows engage and pass
    # today, and each manifest records why the games left out have no row.
    foreach(_subtick_path_game
            "terrain;spyro-1" "terrain_tanim;spyro-1" "terrain_tanim;spyro-3"
            "terrain_canim;spyro-1" "terrain_canim;spyro-2"
            "cyclorama;spyro-2" "cyclorama;spyro-3"
            "particles;spyro-1" "particles;spyro-2" "particles;spyro-3"
            "flame;spyro-2" "flame;spyro-3"
            "tracer;spyro-2" "tracer;spyro-3"
            "moby_mask;spyro-1"
            "glint_ext;spyro-2" "glint_ext;spyro-3"
            "glint_rot;spyro-2" "glint_rot;spyro-3")
        list(GET _subtick_path_game 0 _sp)
        list(GET _subtick_path_game 1 _sg)
        add_test(NAME ${_sp}_subtick_${_sg}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_${_sp}_subtick.sh
                         --game ${_sg} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(${_sp}_subtick_${_sg} PROPERTIES
            LABELS "presentation;subtick;${_sp}"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Sub-tick interpolation of the homeworld portal preview's aperture. Per
    # row of games/<slug>/corpus/portal-subtick-corpus.manifest, four headless replays
    # of one bk2 - the intermediate present at alpha 0, 0.5 and 1, and once
    # more with the replay's own interpolation suppressed - with the feeder's
    # emitted aperture polygon read out of each run's census and every named
    # corner required to sit on the alpha blend of the two canonical ticks the
    # same run recorded. The statistic is the polygon rather than a pixel mask:
    # the aperture spans most of the frame. Spyro 1 draws its portals as a
    # re-projected dome and registers no row. Fails as vacuous when a present
    # feeds no portal triangle, when a named corner resolves ambiguously, or
    # when its canonical motion over the interval is below the row's floor.
    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME portal_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_portal_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(portal_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;portal"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Vertex-stream field resolution on a stock boot: one headless 206-VBL
    # boot with a native screenshot armed, and every frame word the moby walk
    # decodes required to name an address. An unresolved field there is the
    # walk decoding a record the game never queued and reading the frame word
    # from unmapped memory. Spyro 1 only: it is the one game whose reader
    # routes frame words through the handle resolver that feeds the census
    # (games/spyro-1/renderer/moby_game.c, s1_vstream_init), so on the others
    # the run decodes no field and the case could only read as vacuous.
    add_test(NAME vstream_boot_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vstream_boot.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(vstream_boot_spyro-1 PROPERTIES
        LABELS "presentation;vstream"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Glitch-mark rings. One headless bk2 replay with the mark armed on a walk
    # ordinal, then tools/glitch_mark_read.py over the dump: no SWING (a walk
    # whose terrain triangle count left and came back), no STAMP-BACK (a
    # present composing an older terrain build than its predecessor) and no
    # GAP. Headless composes no presents, so the present ring is empty and the
    # walk ring is what the row measures. Engagement: the full 600-row ring,
    # with nothing lost to recycling. Spyro 1 holds the window measured clean.
    add_test(NAME glitch_mark_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_glitch_mark.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(glitch_mark_spyro-1 PROPERTIES
        LABELS "presentation;terrain"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Capture survival under contention. Runs the presentation golden's own
    # legs at CORPUS_JOBS=nproc for three rounds and keeps each round's log.
    # A golden divergence (exit 1) is a rendering verdict and passes here;
    # what fails is a leg that died - an exit outside {0,1}, a MISSING tile,
    # or a "capture incomplete" line. Engagement: compared > 0 per round.
    # Registered for the games that have a golden to run.
    foreach(_contention_game spyro-1 spyro-3)
        add_test(NAME contention_${_contention_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_contention.sh
                         --game ${_contention_game} --rounds 3
                         --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(contention_${_contention_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            # Three whole-corpus runs: the golden's own timeout, tripled, with
            # headroom for the load this deliberately creates.
            TIMEOUT 14400
            SKIP_RETURN_CODE 64
            # Serial: the point is the load the corpus itself makes, so a
            # concurrent ctest row would be measuring something else.
            RUN_SERIAL TRUE
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Freeze-backdrop source frame, windowed. Per row of
    # games/<slug>/corpus/freeze-backdrop-corpus.manifest, one windowed replay
    # of a bk2 that enters the pause menu, counting the composed presents the
    # live path accepted as the backdrop's source frame. When that count is
    # zero nothing is ever snapshotted and the pause backdrop is a flat fill of
    # the clear colour. The source frame is chosen in the live compose, which
    # no headless run executes, so the presentation golden cannot see the class
    # and this check drives a real window. Skips (exit 64) without a display,
    # a binary, a disc or the bk2; a missing manifest fails (exit 1), so a row
    # exists only for a game that has the manifest. The corpus directory is a
    # configure dependency: adding a game's manifest adds its row on the next
    # build.
    foreach(_freeze_game spyro-1 spyro-2 spyro-3)
        set(_freeze_corpus ${CMAKE_CURRENT_SOURCE_DIR}/games/${_freeze_game}/corpus)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_freeze_corpus})
        if(NOT EXISTS ${_freeze_corpus}/freeze-backdrop-corpus.manifest)
            continue()
        endif()
        add_test(NAME freeze_backdrop_${_freeze_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_freeze_backdrop.sh
                         --game ${_freeze_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(freeze_backdrop_${_freeze_game} PROPERTIES
            LABELS "presentation;freeze;windowed"
            TIMEOUT 3600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Paused repaint draws the retained terrain: one windowed row per game
    # pauses at a pinned VBL (OPENPETE_PAUSE_AT_VBL), steps one tick, resumes,
    # and compares every paused present's terrain-cache draw count with the
    # count of the scene shown before it. The paused repaint returns under
    # --headless and eager production needs a window, so no headless capture
    # reaches the path. Skips (exit 64) without a display, a binary or a disc.
    foreach(_pr_game spyro-1 spyro-2 spyro-3)
        add_test(NAME pause_repaint_${_pr_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_pause_repaint.sh
                         --game ${_pr_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(pause_repaint_${_pr_game} PROPERTIES
            LABELS "presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1200
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Eager canonical presents under scene-ring pressure: one windowed row per
    # game stalls the present thread (OPENPETE_PRESENT_STALL_MS) until the
    # ring refuses publishes, and fails on a canonical present with no
    # retained terrain, a retained-terrain state overwritten while its scene
    # is unretired, or a canonical present composing fewer triangles of a
    # render path than its scene was published with. Eager production needs a
    # window, so no headless capture reaches the path. Skips (exit 64) without
    # a display, a binary or a disc.
    foreach(_rp_game spyro-1 spyro-2 spyro-3)
        add_test(NAME eager_ring_pressure_${_rp_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_eager_ring_pressure.sh
                         --game ${_rp_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(eager_ring_pressure_${_rp_game} PROPERTIES
            LABELS "presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1200
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Retained-terrain GPU cache hole census, windowed on the machine's own
    # GPU. Per freecam-corpus row, two legs of one build on one device
    # differing only in --terrain-cache, and a count of the pixels the cached
    # leg renders as the framebuffer clear where the CPU walk feed renders
    # terrain. The class is an exact-bit tie in the fragment key contest, so it
    # is device-dependent: a software rasteriser cannot produce it and the
    # headless presentation golden, which runs llvmpipe with the cache off,
    # cannot separate a build carrying it from a build that does not. Skips
    # (exit 64) without a display, a windows-mingw exe, a disc or PIL.
    foreach(tcgh_game spyro-1 spyro-2 spyro-3)
        string(REPLACE "-" "" tcgh_slug ${tcgh_game})
        add_test(NAME tercache_gpu_holes_${tcgh_slug}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_gpu_holes.sh
                         --game ${tcgh_game})
        set_tests_properties(tercache_gpu_holes_${tcgh_slug} PROPERTIES
            LABELS "slow;presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            # Two windowed legs per corpus row, each booting to a pinned
            # post-load VBL; the Spyro 1 corpus is the long one.
            TIMEOUT 21600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Retained-terrain GPU cache vs CPU terrain walk, whole-frame A/B over a
    # bk2 camera swing, windowed on the machine's own GPU. Four legs of one
    # build (cache on and off, under --lod extended and --lod original) at
    # --render-fps 0; the extended pair may differ by no more than the
    # original pair plus a margin at every sampled VBL, the original pair
    # being the in-run control. Refuses a verdict unless the cache-on legs
    # report retained sectors and the cache-off legs report none. Skips (exit
    # 64) without a display, a windows-mingw exe, a disc, the bk2 or PIL.
    add_test(NAME tercache_cache_ab_spyro3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_cache_ab.sh
                     --game spyro-3)
    set_tests_properties(tercache_cache_ab_spyro3 PROPERTIES
        LABELS "slow;presentation;terrain;windowed"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Multi-Spyro sub-tick records:
    # the spyro-coop mod draws N dragons through the game's own player
    # renderers; each corpus case asserts the OPENPETE_SPYRO_RECDBG hook-call
    # counts, the hook-vs-ring byte identity and the staging census with the
    # mod loaded. The mod is out of tree: SPYRO_COOP_DIR names its source dir,
    # else the test skips (exit 64).
    add_test(NAME spyro_records
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_spyro_records.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(spyro_records PROPERTIES
        LABELS "slow;presentation;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The pacing analysis suite's own maths, against synthetic inputs with
    # known answers. Fast, no PSX boot, no display: a wrong Allan deviation or
    # a mis-set gate tolerance is worse than none, because it looks
    # authoritative and nobody re-derives it.
    add_test(NAME pacing_stats
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pacing/test_pacestats.py)
    set_tests_properties(pacing_stats PROPERTIES
        LABELS "pacing"
        TIMEOUT 300
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The standing pacing gate: does the sim clock hold its grid, does it
    # hold BECAUSE presents shed (proved by fault injection, not by a machine
    # fast enough never to exercise the mechanism), and does it hold in a level
    # rather than only on the attract demo. Exit 3 = vacuous (nothing engaged)
    # and fails the test deliberately: a gate that cannot reach its own subject
    # must never read as a pass. 64 = SKIP (no binary / disc / display).
    #
    # NOT in the default set: it runs four 120 s legs and needs a display for
    # three of them. Run with `ctest -L pacing`.
    add_test(NAME pacing_strict
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_pacing.sh)
    set_tests_properties(pacing_strict PROPERTIES
        LABELS "slow;pacing"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --record video-capture smoke test. Boots the headless demo with
    # --record and uses ffprobe to assert a playable VFR H.264 stream with
    # even dimensions, the reported frame count, monotonic non-uniform PTS,
    # and window-independent (bars-free) capture at 21:9. Skips cleanly
    # (exit 64) if the disc/binary/ffprobe are absent.
    add_test(NAME record_smoke
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_record.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=300)
    set_tests_properties(record_smoke PROPERTIES
        LABELS "slow;tooling"
        RESOURCE_LOCK openpete_game
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --input-lock gate. Replays one key-and-pointer injection script three
    # ways on the headless build: filed with the physical devices under the
    # lock (nothing may change), filed with them without the lock (the
    # negative control: free camera, menu, motion totals and the quit binding
    # all respond), and marked as injected under the lock (still flows).
    # Exit 2 when the negative control did not engage.
    add_test(NAME input_lock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_input_lock.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(input_lock PROPERTIES
        LABELS "slow;input"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Performance benchmark pipeline smoke - replays a few bk2 movies through
    # the profiler (--profile-out) and asserts the trace/summary/rollup pipeline
    # runs and parses. NOT a perf-threshold gate (CI hardware varies); the
    # baseline regression check (tests/harness/bench-corpus.py --baseline) is a
    # manually-run tool. Registered only when spyro-1 is configured: the
    # $<TARGET_FILE> expression fails generation on an absent target.
    if(TARGET openpete-spyro1)
        add_test(NAME bench_corpus_smoke
                 COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/bench-corpus.py
                         --exe $<TARGET_FILE:openpete-spyro1>
                         --movies artisans-bridge,stone-hill-entry
                         --frame-limit 4000 --max-runtime-seconds 90
                         --skip-ticks 30
                         --out-dir ${CMAKE_BINARY_DIR}/bench-smoke --run-id ctest)
        set_tests_properties(bench_corpus_smoke PROPERTIES
            LABELS "slow;bench"
            RESOURCE_LOCK openpete_game
            TIMEOUT 400
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # Channel registry audit - fast static check (no PSX boot). Cross-
    # references games/spyro-1/manifest/render_channels.toml against the recompiler's
    # address-keyed substitution table; fails on missing or orphaned
    # substitutions.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME channel_registry_audit
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_channel_registry.py)
        set_tests_properties(channel_registry_audit PROPERTIES
            LABELS "determinism"
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        # Channel signature anchors: calibrate +
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

    # No PSX RAM dump in the tree (scripts/fences/no-ram-dump.py). The
    # hook half refuses `git add`/`git commit` of a dump; this half scans
    # `git ls-files` by content (has_ram trace, 2 MB image, PS-X EXE) so a
    # commit made outside the hook is caught before it is pushed. The scan
    # runs its own negative controls first and refuses PASS unless they
    # were refused.
    add_test(NAME no_ram_dumps
             COMMAND python3
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/no-ram-dump.py tree
                     ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(no_ram_dumps PROPERTIES
        LABELS "fence;oracle"
        TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Recompiler GTE data-register side effects: mtc2/lwc2 to $30 must
    # recompute LZCR, to $28 must unpack IR1-3. Recompiles a probe binary
    # with PSXRecomp and runs the emitted C against a harness. Skips when
    # PSXRecomp is not built.
    add_test(NAME recomp_gte_sideeffects
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_recomp_gte.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --cc=${CMAKE_C_COMPILER})
    set_tests_properties(recomp_gte_sideeffects PROPERTIES
        LABELS "recompiler;determinism"
        TIMEOUT 120
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # PSX oracle gate - verify openpete gameplay-state RAM matches a
    # DuckStation (real SCPH1001) baseline tick-for-tick over the 20k-tick
    # attract-demo soak (4+ level loads + gameplay). The 3 CD-load pool
    # regions are tolerated only while a load transient heals; every other
    # region must match bit-exact (see check_psx_oracle.sh / the manifest's
    # cd_load_transient flag). The golden is generated, not committed: the
    # script captures it from the pinned DuckStation fork on the first run
    # (about fifteen minutes, hence the timeout) and skips when the fork is
    # not built (see docs/src/developer/duckstation-oracle.md).
    add_test(NAME psx_oracle_match
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_match PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1")

    # One row per game with a recipe; the Spyro 1 row keeps its name above.
    foreach(_g spyro-2 spyro-3)
        string(REPLACE "-" "" _tag "${_g}")
        add_test(NAME psx_oracle_match_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh --game ${_g})
        set_tests_properties(psx_oracle_match_${_g} PROPERTIES
            LABELS "slow;oracle"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/${_g}/openpete-${_tag}")
    endforeach()

    # The windowed oracle row: the same golden verified with the native
    # renderer drawing into a real window under llvmpipe, the eager present
    # thread active. Headless never runs that path, so a presentation-side
    # write into a hashed row is reachable only here. Paced to the display,
    # hence the reduced VBL count (~3k records, the first level load
    # included); FAIL unless the present thread presented; SKIP without a
    # display or the lvp ICD.
    add_test(NAME psx_oracle_windowed
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_windowed PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1;OPENPETE_ORACLE_WINDOWED=1;OPENPETE_FRAME_LIMIT=6000")

    # The Spyro 3 windowed row. Same contract as the row above on the Spyro 3
    # golden: --render-fps 60 is above the 29.913 Hz tick rate, so every
    # non-drawing VBL plans a sub-tick present and the run's re-extract count
    # is nonzero; the row fails if it is zero. The frame limit reaches VBL
    # 4000, past the demo's first level load (the bulk reads at VBLs
    # 2903-3475 in the golden's load schedule), so the sub-tick leg is
    # verified across a load transient and not only in steady gameplay.
    add_test(NAME psx_oracle_windowed_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh
                     --game spyro-3 -- --render-fps 60)
    set_tests_properties(psx_oracle_windowed_spyro-3 PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-3/openpete-spyro3;OPENPETE_ORACLE_WINDOWED=1;OPENPETE_FRAME_LIMIT=4000")

    # Matrix determinism gate - broaden coverage from one (aspect,
    # scenario) cell to a sweep across aspects × fovs × scenarios.
    # Divergence in an open channel is expected; in a gated or verified
    # channel it is a regression.
    # Slow (boots N PSX sessions); only runs under `ctest -L slow`.
    add_test(NAME determinism_matrix
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=2000)
    set_tests_properties(determinism_matrix PROPERTIES
        LABELS "slow;determinism;matrix"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Soak determinism gate - long-replay mode for hunting slow-burn
    # / late-frame channel leaks the 2000-frame matrix cells miss.
    # Iterates the full BK2 corpus (minus tas/frame-time-test) at
    # 4:3 vs 16:9, default fov, with --bk2-stop-on-end so each cell
    # exits on movie end. Not part of the default ctest run; opt in
    # via `ctest -L soak`. Substantially slower than matrix.
    add_test(NAME determinism_soak
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --soak)
    set_tests_properties(determinism_soak PROPERTIES
        LABELS "slow;determinism;soak"
        RESOURCE_LOCK openpete_game
        TIMEOUT 7200
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Savestate restore-at-yield gate. Per corpus movie, three headless legs
    # (clean, --state-stress 1, --state-stress-rewind 25) whose
    # --gameplay-hash-log outputs must agree byte-for-byte, plus the runahead
    # stanzas: --runahead 2 must leave the COMMITTED timeline identical to
    # clean, the read-only prediction oracle must report >= 50 checks with
    # zero mismatches (a driver that never speculates would pass the compare
    # trivially), and a portal entry must still complete its level load under
    # speculation. Slow (~20 headless sessions); `ctest -L savestate` or the
    # script directly. Skips (exit 64) if the binary / disc / movies are
    # absent.
    add_test(NAME state_roundtrip
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_state_roundtrip.sh)
    set_tests_properties(state_roundtrip PROPERTIES
        LABELS "slow;determinism;savestate"
        RESOURCE_LOCK openpete_game
        # Each leg self-limits at `timeout 600`; headroom for ~20 of them.
        TIMEOUT 7200
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Sub-tick present gameplay-state gate. The psx oracle replays canonical
    # ticks only, so an intermediate present that writes gameplay-readable RAM
    # and mis-restores it is invisible to it. Per movie, two headless legs of
    # the same binary - intermediate re-extracts armed vs not - whose
    # gameplay-hash logs must be identical, with a per-present bracket check
    # that names the region a leak came from, and the canonical leg compared
    # against the blessed baseline under tests/state-baselines/. Refuses to
    # pass at zero sub-tick presents (exit 3, VACUOUS). Runs the `quick`
    # corpus here; `--corpus full` is the complete sweep. Skips (exit 64) if
    # the binary or disc are absent.
    add_test(NAME subtick_state
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_state.py)
    set_tests_properties(subtick_state PROPERTIES
        LABELS "slow;determinism"
        RESOURCE_LOCK openpete_game
        # Four headless legs, each self-limited at --max-runtime-seconds 280.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Retained-terrain GPU cache vs CPU terrain walk, pixel-exact, headless on
    # llvmpipe. Per measured freecam row, two legs of one build at the same
    # pose and pinned VBL differing only in the feed (--terrain-cache 0 against
    # the cache forced on); the two feeds are one scene, so the pass criterion
    # is 0 differing pixels. Refuses a verdict unless the cache leg reports
    # retained sectors and the walk leg reports none and reports terrain
    # triangles. Skips (exit 64) without a binary, a disc, PIL, or measured
    # rows for the game.
    add_test(NAME cache_walk_ab_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cache_walk_ab.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cache_walk_ab_spyro-1 PROPERTIES
        LABELS "slow;presentation;terrain"
        RESOURCE_LOCK openpete_game
        # Four headless boots to a post-load VBL, each self-limited at
        # --max-runtime-seconds 840.
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # CPU terrain walk vs retained terrain cache, per-pose residual against a
    # committed baseline. Per row of games/spyro-1/corpus/walk-cache.manifest, two
    # headless legs at the freecam corpus's canonical VBL (the walk, and the
    # cache forced on under software vk); the count of pixels whose RGB differ
    # must equal the row of tests/golden/spyro-1/walk-cache-baseline.tsv, in
    # either direction. VACUOUS when a capture is missing, the walk leg draws
    # no terrain triangle or the cache leg retains no sector. Skips (exit 64)
    # without a binary, a disc, a Vulkan loader or PIL.
    add_test(NAME terrain_walk_cache_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-1 PROPERTIES
        LABELS "presentation;terrain;spyro-1"
        # Three rows of two concurrent boots, each self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same check on Spyro 2, whose row is a bk2 row
    # (games/spyro-2/corpus/walk-cache.manifest): the plain boot's attract demo at
    # one VBL on the deterministic sub-tick present, walk against cache.
    add_test(NAME terrain_walk_cache_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-2 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-2 PROPERTIES
        LABELS "presentation;terrain;spyro-2"
        # One row of two concurrent boots, each self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME terrain_walk_cache_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-3 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-3 PROPERTIES
        LABELS "presentation;terrain;spyro-3"
        # One row of two concurrent boots, self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
